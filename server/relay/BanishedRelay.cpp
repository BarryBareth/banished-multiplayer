// BanishedRelay.exe - the Banished Multiplayer relay server for Windows: a C++ port of tools/bmp_relay.py (same
// protocol, same limits), no Python needed. Double-click it: it listens on TCP 7780 and prints the address to give
// the other players. Ctrl+C or closing the window stops it.
//   BanishedRelay.exe [--port 7780] [--bind 0.0.0.0] [--max-rooms 500] [--per-ip 12]
//
// Protocol (one text line each, '\n'-terminated) - see docs/RELAY.md:
//   host  -> relay  RELAY HOST <room> [<token>]  relay -> host   RELAY OK <room> | RELAY ERR <reason>
//   guest -> relay  RELAY JOIN <room>            relay -> guest  RELAY OK <room> | RELAY ERR <reason>
//                                                relay -> host   RELAY CONN <id>
//   guest -> relay  <line>                       relay -> host   RELAY FROM <id> <line>
//   host  -> relay  RELAY TO <id> <line>         relay -> guest  <line>
//   host  -> relay  RELAY KICK <id>              (guest disconnected)
//   guest disconnects                            relay -> host   RELAY DROP <id>
//   host disconnects                             every guest of the room is disconnected
// One thread per connection, a lock around the rooms; sends to a connection are serialized by its own lock.
#define WIN32_LEAN_AND_MEAN
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <winhttp.h>
#include <windows.h>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Limits {
    size_t line = 1024 * 1024;          // bytes per line (save transfer lines are ~60 KB)
    int perIp = 12;                     // concurrent connections per IP address
    int roomsPerIp = 3;
    int handshake = 15;                 // seconds to send RELAY HOST/JOIN
    int idle = 30 * 60;                 // seconds without any line before a connection is dropped
    uint64_t rate = 4ull * 1024 * 1024; // bytes per second per connection (average over 10 s)
    int maxRooms = 500;
    int maxGuests = 16;
    int sendTimeout = 30;               // seconds a send may block (a stuck receiver is dropped)
};
static Limits L;

static std::mutex g_logMx;
static void Log(const char* fmt, ...) {
    char msg[2048];
    va_list a;
    va_start(a, fmt);
    vsnprintf(msg, sizeof msg, fmt, a);
    va_end(a);
    time_t t = time(nullptr);
    tm lt;
    localtime_s(&lt, &t);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &lt);
    std::lock_guard<std::mutex> lk(g_logMx);
    printf("%s %s\n", ts, msg);
    fflush(stdout);
}

struct Stats { uint64_t bytesIn = 0, bytesOut = 0, connections = 0, refusedIp = 0, roomsOpened = 0, guestsJoined = 0; };
static Stats g_stats;
static std::mutex g_mx;  // rooms, per-IP counts, stats

struct Conn {
    SOCKET s;
    std::string ip;
    std::mutex sendMx;
    std::atomic<bool> closed{false};
    std::string buf;  // received bytes not yet returned as lines
    ULONGLONG window = GetTickCount64();
    uint64_t used = 0;
    void Close() {
        if (!closed.exchange(true)) shutdown(s, SD_BOTH);  // wakes this connection's thread; it closes the socket
    }
};
using ConnPtr = std::shared_ptr<Conn>;

static void Send(const ConnPtr& c, const std::string& line) {
    if (!c || c->closed) return;
    std::string data = line + "\n";
    std::lock_guard<std::mutex> lk(c->sendMx);
    size_t off = 0;
    while (off < data.size()) {
        int n = send(c->s, data.data() + off, (int)std::min<size_t>(data.size() - off, 1 << 20), 0);
        if (n <= 0) { c->Close(); return; }  // includes the send timeout: a receiver that stopped reading
        off += n;
    }
    std::lock_guard<std::mutex> g(g_mx);
    g_stats.bytesOut += data.size();
}

// One line (without '\n' / '\r') within `timeout` seconds; false = closed, timed out, too long or over the traffic cap.
static bool ReadLine(const ConnPtr& c, int timeout, std::string& out) {
    DWORD ms = (DWORD)timeout * 1000;
    setsockopt(c->s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&ms, sizeof ms);
    for (;;) {
        size_t nl = c->buf.find('\n');
        if (nl != std::string::npos) {
            out = c->buf.substr(0, nl);
            c->buf.erase(0, nl + 1);
            if (!out.empty() && out.back() == '\r') out.pop_back();
            return true;
        }
        if (c->buf.size() > L.line) return false;
        char tmp[65536];
        int n = recv(c->s, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        c->buf.append(tmp, n);
        ULONGLONG now = GetTickCount64();
        if (now - c->window > 10000) c->window = now, c->used = 0;
        c->used += n;
        {
            std::lock_guard<std::mutex> g(g_mx);
            g_stats.bytesIn += n;
        }
        if (c->used > L.rate * 10) return false;
    }
}

struct Room {
    std::string code, token, ip;
    ConnPtr host;
    std::map<int, ConnPtr> guests;
    int nextId = 1;
};
using RoomPtr = std::shared_ptr<Room>;
static std::map<std::string, RoomPtr> g_rooms;
static std::map<std::string, int> g_perIp;

static std::string Upper(std::string s) {
    for (char& ch : s) ch = (char)toupper((unsigned char)ch);
    return s;
}
static std::vector<std::string> Split(const std::string& s, char sep, int maxParts) {
    std::vector<std::string> v;
    size_t a = 0;
    while ((int)v.size() < maxParts - 1) {
        size_t b = s.find(sep, a);
        if (b == std::string::npos) break;
        v.push_back(s.substr(a, b - a));
        a = b + 1;
    }
    v.push_back(s.substr(a));
    return v;
}
static bool AllDigits(const std::string& s) {
    if (s.empty()) return false;
    for (char ch : s) if (!isdigit((unsigned char)ch)) return false;
    return true;
}

static void RunHost(const ConnPtr& c, std::string code, const std::string& token) {
    code = Upper(code);
    RoomPtr room;
    std::string err;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto old = g_rooms.find(code);
        if (old != g_rooms.end() && !token.empty() && old->second->token == token) {  // the same host is back
            RoomPtr r = old->second;
            g_rooms.erase(old);
            r->host->Close();
            for (auto& [id, g] : r->guests) g->Close();
            Log("room %s reopened by its host (%d stale guests disconnected)", code.c_str(), (int)r->guests.size());
        }
        int mine = 0;
        for (auto& [k, r] : g_rooms) mine += r->ip == c->ip;
        bool alnum = code.size() >= 3 && code.size() <= 16;
        for (char ch : code) alnum = alnum && isalnum((unsigned char)ch);
        if (g_rooms.count(code)) err = "room code already in use";
        else if ((int)g_rooms.size() >= L.maxRooms) err = "the relay is full - try again later";
        else if (mine >= L.roomsPerIp) err = "too many games from your address";
        else if (!alnum) err = "bad room code";
        else {
            room = std::make_shared<Room>();
            room->code = code, room->token = token, room->ip = c->ip, room->host = c;
            g_rooms[code] = room;
            g_stats.roomsOpened++;
        }
    }
    if (!room) { Send(c, "RELAY ERR " + err); return; }
    Log("room %s opened by %s", code.c_str(), c->ip.c_str());
    Send(c, "RELAY OK " + code);
    std::string line;
    while (ReadLine(c, L.idle, line)) {
        if (line.compare(0, 9, "RELAY TO ") == 0) {
            auto p = Split(line, ' ', 4);  // RELAY TO <id> <line>
            ConnPtr g;
            if (p.size() >= 3 && AllDigits(p[2])) {
                std::lock_guard<std::mutex> lk(g_mx);
                auto it = room->guests.find(atoi(p[2].c_str()));
                if (it != room->guests.end()) g = it->second;
            }
            if (g) Send(g, p.size() == 4 ? p[3] : "");
        } else if (line.compare(0, 11, "RELAY KICK ") == 0) {
            std::string gid = line.substr(11);
            ConnPtr g;
            if (AllDigits(gid)) {
                std::lock_guard<std::mutex> lk(g_mx);
                auto it = room->guests.find(atoi(gid.c_str()));
                if (it != room->guests.end()) g = it->second, room->guests.erase(it);
            }
            if (g) g->Close();
        }
    }
    std::vector<ConnPtr> left;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_rooms.find(code);
        if (it != g_rooms.end() && it->second == room) g_rooms.erase(it);  // (a reopened room belongs to the new host)
        for (auto& [id, g] : room->guests) left.push_back(g);
    }
    for (auto& g : left) g->Close();
    Log("room %s closed (%d guests disconnected)", code.c_str(), (int)left.size());
}

static void RunGuest(const ConnPtr& c, const std::string& codeIn) {
    std::string code = Upper(codeIn);
    RoomPtr room;
    int gid = 0;
    std::string err;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        auto it = g_rooms.find(code);
        if (it == g_rooms.end()) err = "no game with that room code - check the code and that the host is waiting";
        else if ((int)it->second->guests.size() >= L.maxGuests) err = "the game is full";
        else {
            room = it->second;
            gid = room->nextId++;
            room->guests[gid] = c;
            g_stats.guestsJoined++;
        }
    }
    if (!room) { Send(c, "RELAY ERR " + err); return; }
    Log("room %s: guest %d joined from %s", code.c_str(), gid, c->ip.c_str());
    Send(c, "RELAY OK " + code);
    Send(room->host, "RELAY CONN " + std::to_string(gid));
    std::string line;
    while (ReadLine(c, L.idle, line)) Send(room->host, "RELAY FROM " + std::to_string(gid) + " " + line);
    bool wasIn;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        wasIn = room->guests.erase(gid) > 0;
    }
    if (wasIn) Send(room->host, "RELAY DROP " + std::to_string(gid));
    Log("room %s: guest %d left", code.c_str(), gid);
}

static void Handle(SOCKET s, std::string ip) {
    auto c = std::make_shared<Conn>();
    c->s = s, c->ip = ip;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_perIp[ip] >= L.perIp) {
            g_stats.refusedIp++;
            closesocket(s);
            return;
        }
        g_perIp[ip]++;
        g_stats.connections++;
    }
    DWORD sendMs = (DWORD)L.sendTimeout * 1000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&sendMs, sizeof sendMs);
    BOOL nodelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&nodelay, sizeof nodelay);
    std::string first;
    if (ReadLine(c, L.handshake, first)) {
        auto p = Split(first, ' ', 5);
        if ((p.size() == 3 || p.size() == 4) && p[0] == "RELAY" && p[1] == "HOST") RunHost(c, p[2], p.size() == 4 ? p[3] : "");
        else if (p.size() == 3 && p[0] == "RELAY" && p[1] == "JOIN") RunGuest(c, p[2]);
        else Send(c, "RELAY ERR expected RELAY HOST/JOIN");
    }
    c->Close();
    closesocket(s);
    std::lock_guard<std::mutex> lk(g_mx);
    if (--g_perIp[ip] <= 0) g_perIp.erase(ip);
}

static void Report() {
    for (;;) {
        Sleep(60000);
        std::lock_guard<std::mutex> lk(g_mx);
        int guests = 0, conns = 0;
        for (auto& [k, r] : g_rooms) guests += (int)r->guests.size();
        for (auto& [k, n] : g_perIp) conns += n;
        Log("stats: %d rooms, %d guests, %d connections from %d addresses; total in %.1f MB out %.1f MB, refused %llu",
            (int)g_rooms.size(), guests, conns, (int)g_perIp.size(), g_stats.bytesIn / 1e6, g_stats.bytesOut / 1e6,
            (unsigned long long)g_stats.refusedIp);
    }
}

// what to tell the players: this PC's LAN addresses (for the router's port forward) and its public address
static std::vector<std::string> LanAddresses() {
    std::vector<std::string> out;
    ULONG size = 16 * 1024;
    std::vector<uint8_t> mem(size);
    auto* a = (IP_ADAPTER_ADDRESSES*)mem.data();
    if (GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr, a, &size) != NO_ERROR)
        return out;
    for (; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            char ip[64];
            auto* sin = (sockaddr_in*)u->Address.lpSockaddr;
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof ip);
            if (strncmp(ip, "169.254.", 8) != 0) out.push_back(ip);
        }
    }
    return out;
}
static std::string PublicAddress() {  // asks api.ipify.org (only to print it; skipped with --no-lookup)
    std::string body;
    HINTERNET s = WinHttpOpen(L"BanishedRelay", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return "";
    WinHttpSetTimeouts(s, 5000, 5000, 5000, 5000);
    HINTERNET c = WinHttpConnect(s, L"api.ipify.org", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", L"/", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        char tmp[128];
        DWORD got = 0;
        while (WinHttpReadData(r, tmp, sizeof tmp, &got) && got && body.size() < 64) body.append(tmp, got);
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    for (char ch : body) if (!isdigit((unsigned char)ch) && ch != '.' && ch != ':') return "";
    return body;
}

int main(int argc, char** argv) {
    int port = 7780;
    std::string bind = "0.0.0.0";
    bool lookup = true;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--port") port = atoi(next());
        else if (a == "--bind") bind = next();
        else if (a == "--max-rooms") L.maxRooms = atoi(next());
        else if (a == "--per-ip") L.perIp = atoi(next());
        else if (a == "--no-lookup") lookup = false;
        else {
            printf("usage: BanishedRelay.exe [--port 7780] [--bind 0.0.0.0] [--max-rooms 500] [--per-ip 12] [--no-lookup]\n");
            return 1;
        }
    }
    SetConsoleTitleA("Banished Multiplayer relay");
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((u_short)port);
    inet_pton(AF_INET, bind.c_str(), &addr.sin_addr);
    if (ls == INVALID_SOCKET || ::bind(ls, (sockaddr*)&addr, sizeof addr) != 0 || listen(ls, SOMAXCONN) != 0) {
        printf("Can't listen on %s:%d (error %d) - is another relay already running on that port?\n", bind.c_str(), port, WSAGetLastError());
        printf("Press Enter to close.\n");
        getchar();
        return 1;
    }
    Log("relay listening on %s:%d (max %d rooms, %d connections per address)", bind.c_str(), port, L.maxRooms, L.perIp);
    printf("\n  The other players choose \"Relay server\" in the Multiplayer window and enter this server's address\n"
           "  under \"Relay address\":\n");
    std::string pub = lookup ? PublicAddress() : "";
    if (!pub.empty()) printf("      over the internet:  %s:%d\n", pub.c_str(), port);
    for (auto& ip : LanAddresses()) printf("      on this network:    %s:%d\n", ip.c_str(), port);
    printf("\n  Over the internet, players can only reach this PC if TCP port %d is forwarded to it on your router\n"
           "  (to the \"on this network\" address above) and Windows Firewall allows it (answer Allow when Windows asks).\n"
           "  Everyone who connects sees this address. Close this window or press Ctrl+C to stop the relay.\n\n", port);
    std::thread(Report).detach();
    for (;;) {
        sockaddr_storage from{};
        int len = sizeof from;
        SOCKET s = accept(ls, (sockaddr*)&from, &len);
        if (s == INVALID_SOCKET) continue;
        char ip[64] = "?";
        if (from.ss_family == AF_INET) inet_ntop(AF_INET, &((sockaddr_in*)&from)->sin_addr, ip, sizeof ip);
        std::thread(Handle, s, std::string(ip)).detach();
    }
}
