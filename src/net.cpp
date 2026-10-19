// Lockstep networking (see net.h). One client protocol, three transports:
//   kClientTcp   - connect to an external relay (tools/relay.py) or to another game hosting over TCP
//   kHost        - this game runs the relay itself (in-process); remote players arrive over TCP and/or Steam P2P,
//                  the local player is relay peer 0 and talks to it by function call
//   kClientSteam - joined a friend's Steam lobby; lines travel as reliable P2P packets to the lobby owner (host)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <shlobj.h>
#include "net.h"
#include "common.h"
#include "commands.h"
#include "gameload.h"
#include "autotest.h"
#include "steam.h"
#include "towns.h"
#include "update.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <random>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace net {

enum Mode { kOff, kClientTcp, kHost, kClientSteam };
static Mode g_mode = kOff;
static bool g_active;
static std::string g_host = "127.0.0.1";
static int g_port = 7777;
static int g_listenPort;                     // host: also accept TCP players on this port (0 = Steam only)
static int g_autoStart;                      // host: START automatically once this many players are present
static std::string g_hostSave;               // host: save (file stem) to send to everybody
static uint64_t g_delay = 30;
static SOCKET g_sock = INVALID_SOCKET;
static std::mutex g_mx;
static std::deque<std::string> g_inbox;      // lines for this client (from the relay / host)
static int g_me = -1, g_players = 0;
static bool g_started;
static std::map<int, int64_t> g_turns;       // player -> highest tick it promised commands for
static int64_t g_sentTurn = -1;
static std::map<uint64_t, uint64_t> g_myHash;
// Sim-heap signature (HEAP lines, beside HASH): the heaps drift apart before any draw differs (2026-10-14 friend
// desync: offsets drifted >1300 ticks before the draws did, the cause had left the ring). A mismatch only dumps the
// ring once per session - no resync; old versions ignore HEAP lines.
static std::map<uint64_t, uint64_t> g_myHeap;
static bool g_heapDumped;
static void HeapDrift(uint64_t tick, const char* who) {
    if (g_heapDumped) return;
    g_heapDumped = true;
    Log("!! net: sim-heap drift at tick %llu (%s) - no gameplay difference yet; ring dumped for diagnosis", tick, who);
    ProbeDumpRecentDraws(("HEAP DRIFT at tick " + std::to_string(tick)).c_str());
}
static uint64_t g_cmdsIn, g_desyncs;
static int g_netSpeed = -1;     // -1 = not set yet (adopt the local value)
static int g_netPaused = 0;
static int g_wroteSpeed = -1, g_wrotePaused = -1;  // values SyncSpeed last wrote into the game
static std::string g_saveName;               // save every player loads (sent by the host), "" = harness decides
static std::string g_status = "";            // shown in the window title
static std::string g_notice;                 // shown instead of the status for a few seconds (invite: room code copied)
static DWORD g_noticeUntil;
static void CopyText(const std::string& s) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, s.size() + 1)) {
        memcpy(GlobalLock(h), s.c_str(), s.size() + 1);
        GlobalUnlock(h);
        SetClipboardData(CF_TEXT, h);
    }
    CloseClipboard();
}
static bool g_wasResync;          // the session being loaded is a resync -> "Back in sync" when it runs
static uint64_t g_statusExpires;  // GetTickCount64 when a temporary status gives way to the normal one
static uint64_t g_steamHost;                 // kClientSteam: lobby owner
static bool g_loadPending, g_sessionInGame;
static std::string g_recvName;               // test: store the received save under this name instead
static bool g_steamHostAuto;                 // bmp_net.txt steamhost=1: host through Steam as soon as the menu is up (self-test)
// Sessions / resync. Every (re)load of the shared save starts a new epoch: "START <players> <epoch>". After it, each
// player sends "SYNCED <epoch>"; its TURN/CMD/HASH lines are ignored until then (they belong to the old session).
static int g_epoch;
static std::map<int, int> g_peerEpoch;       // player -> last epoch it confirmed (SYNCED)
static int64_t g_resyncAt = -1;              // RESYNC <tick>: stop the sim at this tick (host saves there)
static uint64_t g_startSessions = UINT64_MAX; // ProbeSessionCount() when START arrived: frozen until a new session
static int64_t g_testResyncAt = -1;
static std::deque<int64_t> g_testResyncMore;  // resyncat=T1,T2,...: later ones (session ticks restart at 0 after a reload)          // bmp_net.txt resyncat=T (host): trigger a resync at tick T (test)
static int g_joinDelay;                      // bmp_net.txt joindelay=S (client): connect S seconds late (test)
static std::map<int, std::string> g_names;   // roster from the host: "NAMES id=name;id=name"
// How players meet (bmp_settings.txt, set in the main-menu overlay): Steam lobby + P2P, direct TCP to the host
// (the host forwards a port), or a relay server (tools/bmp_relay.py) that host and guests both connect out to.
enum Method { kViaSteam, kViaDirect, kViaRelay };
static int g_method = kViaSteam;
static int g_hostPort = 7777;                // direct: the host listens here
static std::string g_relayAddr;  // relay server "host:port" (bmp_settings.txt relay=; no default - see docs/RELAY.md)
static std::string g_playerName;             // name without Steam
static std::string g_room;                   // relay room code (host: its own; guest: the joined one)
static std::string g_joinAddr;               // guest (TCP): "host:port" to connect to ("" = g_host:g_port from bmp_net.txt)
static std::string g_helloName;              // guest (TCP): name sent in HELLO ("" = slotN, tests)
static std::string g_testName;               // test (bmp_net.txt name@N=): overrides the player name
static int g_hostVia = -1;                   // host: how players reach us (Method), -1 = not hosting
static SOCKET g_relaySock = INVALID_SOCKET;  // host: connection to the relay server
static SOCKET g_listenSock = INVALID_SOCKET; // host: direct TCP listener

// Every player must run the identical DLL - any code difference can desync. The build id = release number + a hash of
// this DLL's code section as stored on disk (in memory it is relocated); HELLO carries it, the host turns away others.
static constexpr const char* kModVersion = "0.9.3";
static std::string g_fakeVersion;
static int64_t g_testDropAt = -1;
static bool g_testCorruptSave;  // bmp_net.txt corruptsave=1 (test): the first save received counts as corrupt
static int64_t g_testStallAt = -1;
static int64_t g_testDumpAt = -1;   // bmp_net.txt dumpat=T (test): write the desync record at session tick T  // bmp_net.txt stallat=T (test): this game freezes for 6 s at session tick T  // bmp_net.txt dropat=T (test, guest): cut the connection once at session tick T  // bmp_net.txt fakeversion=X (test: pretend to run another build)
static const std::string& BuildId() {
    if (!g_fakeVersion.empty()) return g_fakeVersion;
    static std::string id = [] {
        uint64_t h = 0xcbf29ce484222325ull;
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&BuildId, &self);
        wchar_t path[MAX_PATH];
        FILE* f = nullptr;
        if (self && GetModuleFileNameW(self, path, MAX_PATH) && !_wfopen_s(&f, path, L"rb") && f) {
            std::vector<uint8_t> file;
            fseek(f, 0, SEEK_END);
            file.resize(ftell(f));
            fseek(f, 0, SEEK_SET);
            fread(file.data(), 1, file.size(), f);
            fclose(f);
            auto* dos = (IMAGE_DOS_HEADER*)file.data();
            auto* nt = (IMAGE_NT_HEADERS64*)(file.data() + dos->e_lfanew);
            auto* sec = IMAGE_FIRST_SECTION(nt);
            for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
                if (memcmp(sec->Name, ".text", 6) || sec->PointerToRawData + sec->SizeOfRawData > file.size()) continue;
                for (DWORD k = 0; k < sec->SizeOfRawData; k++) h = (h ^ file[sec->PointerToRawData + k]) * 0x100000001b3ull;
            }
        }
        char s[48];
        sprintf_s(s, "%s-%08x", kModVersion, (uint32_t)(h ^ (h >> 32)));
        return std::string(s);
    }();
    return id;
}
// a desync: keep the last in-sim draws for diagnosis (once per detection - host and client paths may both see it)
static void DumpDraws(const std::string& why) {
    static uint64_t last;
    if (GetTickCount64() - last < 5000) return;
    last = GetTickCount64();
    ProbeDumpRecentDraws(why.c_str());
}
// "HELLO @<build id>[!<companion mod state>]#<token> <name>" - the mod state (CompanionMod: 1 missing, 2 another
// build) is only there when the player's bmpShare.pkm isn't the one this DLL expects; the host tells its player. The
// token is random per game start: the same name with the same token is that player reconnecting, the same name with
// another token is somebody else (shown as "Name (2)").
static const std::string& HelloToken() {
    static std::string t;
    if (t.empty()) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        char b[24];
        sprintf_s(b, "%016llx", (unsigned long long)(q.QuadPart * 0x9E3779B97F4A7C15ull ^ GetCurrentProcessId() ^ ((uint64_t)GetTickCount64() << 32)));
        t = b;
    }
    return t;
}
static std::string Hello(const std::string& name) {
    int mod = CompanionModKnown();
    return "HELLO @" + BuildId() + (mod != kModOk ? "!" + std::to_string(mod) : "") + "#" + HelloToken() + " " + name;
}
static bool g_relayUp;                       // host: the relay confirmed the room
// Separate towns: every player starts the same brand-new map with an own town ("START n 0 NEW a,b,c,d,e TOWNS ids")
static bool g_separate;                      // host's choice
static std::string g_newArgs;                // "seed,size,start,terrain,climate"
static bool g_sharedNew;                     // host: hosting one shared town on a new map ("M|" - START ... NEW without TOWNS)
static std::string g_loadNewArgs;            // client: new map to start (instead of loading a save)
static bool g_separateSession;               // client: the session is a separate-towns game
static std::map<int, int> g_townOf;          // player id -> town index
// The save remembers each town's player by name (towns BPNM): record the current owners whenever towns are handed out.
static void PublishTownPlayers() {
    for (auto& [id, t] : g_townOf)
        if (g_names.count(id)) towns::SetTownPlayer(t, g_names[id]);
}
// The town the save remembers for this player name, if no one else has it in `byTown` (-1 = free); else -1.
static int SavedTownOf(const std::string& name, const std::vector<int>& byTown, int count) {
    for (int t = 0; t < count; t++)
        if (!name.empty() && towns::TownPlayer(t) == name && (t >= (int)byTown.size() || byTown[t] == -1)) return t;
    return -1;
}
// A free town nobody is remembered for (an old save, or a town that was never played); else -1.
static int UnclaimedTown(const std::vector<int>& byTown, int count) {
    for (int t = 0; t < count; t++)
        if (towns::TownPlayer(t).empty() && (t >= (int)byTown.size() || byTown[t] == -1)) return t;
    return -1;
}
static bool g_pickStarts = true;             // host: joining players choose where their town starts
static std::vector<TradeOffer> g_offers;      // offers to me
static int g_offerSeq;
static std::string g_tradeNews;
static std::mutex g_uiMx;
static std::deque<std::pair<int, std::string>> g_uiActions;  // UiDo queue (overlay -> main thread)

bool Active() { return g_active; }
bool Started() { return g_started; }
// Input delay = a time budget, not a tick count: commands are scheduled latencyMs ahead at the current shared speed
// (speed = sim ticks per 60 Hz frame). 150 ms = 9 ticks at 1x (was a fixed 30 = half a second), 90 at 10x (30 was only
// 50 ms there: stalls on internet latency). bmp_net.txt delay=N (tests) forces a fixed tick count.
static int g_latencyMs = 150;
static bool g_fixedDelay;
uint64_t Delay() {
    if (g_fixedDelay) return g_delay;
    int64_t speed = g_netSpeed > 0 ? g_netSpeed : 1;
    int64_t d = (speed * 60 * g_latencyMs + 999) / 1000;
    return (uint64_t)(d < 3 ? 3 : d > 600 ? 600 : d);
}
// The delay shrinks when the speed drops: a command must still land after every tick already promised by TURN.
uint64_t CommandSlot(uint64_t tick) {
    int64_t slot = (int64_t)(tick + Delay());
    return (uint64_t)(slot > g_sentTurn ? slot : g_sentTurn + 1);
}
const std::string& SaveName() { return g_saveName; }

static void HostReceive(int peerKind, uint64_t key, const std::string& line);
static void ResetSession();
static void HostCheckReady();

// ---------------------------------------------------------------- utilities
static void SendAll(SOCKET s, const std::string& data) {
    const char* p = data.data();
    int left = (int)data.size();
    while (left > 0) {
        int n = send(s, p, left, 0);
        if (n <= 0) return;
        p += n;
        left -= n;
    }
}

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static std::string B64Encode(const uint8_t* d, size_t n) {
    std::string o;
    o.reserve((n + 2) / 3 * 4);
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        o += kB64[v >> 18 & 63];
        o += kB64[v >> 12 & 63];
        o += i + 1 < n ? kB64[v >> 6 & 63] : '=';
        o += i + 2 < n ? kB64[v & 63] : '=';
    }
    return o;
}
static void B64Decode(const std::string& s, std::vector<uint8_t>& out) {
    uint32_t v = 0;
    int bits = 0;
    for (char c : s) {
        const char* p = strchr(kB64, c);
        if (!p || !c) continue;
        v = v << 6 | (uint32_t)(p - kB64);
        bits += 6;
        if (bits >= 8) { bits -= 8; out.push_back((uint8_t)(v >> bits)); }
    }
}
static uint64_t Fnv(const std::vector<uint8_t>& d) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : d) { h ^= b; h *= 0x100000001b3ull; }
    return h;
}
static bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? n : 0);
    size_t got = n > 0 ? fread(out.data(), 1, n, f) : 0;
    fclose(f);
    return got == out.size();
}
static std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }
static std::string Narrow(const std::wstring& s) {
    std::string o;
    for (wchar_t c : s) o += (char)(c < 128 ? c : '_');
    return o;
}

static void Push(const std::string& line) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_inbox.push_back(line);
}

// ---------------------------------------------------------------- client side: outgoing
static void SendLine(const std::string& s) {
    switch (g_mode) {
    case kClientTcp:
        if (g_sock != INVALID_SOCKET) SendAll(g_sock, s + "\n");
        break;
    case kClientSteam:
        if (g_steamHost) steam::Send(g_steamHost, s.data(), (uint32_t)s.size());
        break;
    case kHost:
        HostReceive(0, 0, s);  // local player = relay peer 0
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------- TCP client transport
static void ReceiveLoop(SOCKET s, std::function<void(const std::string&)> sink, std::function<void()> onClose) {
    std::string buf;
    std::vector<char> tmp(65536);
    for (;;) {
        int n = recv(s, tmp.data(), (int)tmp.size(), 0);
        if (n <= 0) { onClose(); return; }
        buf.append(tmp.data(), n);
        size_t pos, from = 0;
        while ((pos = buf.find('\n', from)) != std::string::npos) {
            sink(buf.substr(from, pos - from));
            from = pos + 1;
        }
        buf.erase(0, from);
    }
}

// "host:port" (DNS names allowed) -> connected TCP socket, or INVALID_SOCKET
static SOCKET TcpConnect(const std::string& addr, int defaultPort) {
    std::string host = addr, port = std::to_string(defaultPort);
    size_t c = addr.rfind(':');
    if (c != std::string::npos && addr.find(':') == c) { host = addr.substr(0, c); port = addr.substr(c + 1); }
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) return INVALID_SOCKET;
    SOCKET s = INVALID_SOCKET;
    for (addrinfo* r = res; r; r = r->ai_next) {
        s = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        if (connect(s, r->ai_addr, (int)r->ai_addrlen) == 0) break;
        closesocket(s);
        s = INVALID_SOCKET;
    }
    freeaddrinfo(res);
    if (s != INVALID_SOCKET) {
        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
    }
    return s;
}

// Guest connection thread. A dropped connection is retried for about a minute: the host sees the HELLO again, treats it
// as a rejoin (same name -> the old player id and town) and resyncs everybody. Leaving on purpose (ResetSession) bumps
// g_connGen, which ends this thread instead.
static std::atomic<int> g_connGen;
static void Connect() {
    const int gen = g_connGen;
    if (g_joinDelay) Sleep(g_joinDelay * 1000);
    const std::string addr = g_joinAddr.empty() ? g_host + ":" + std::to_string(g_port) : g_joinAddr;
    const std::string room = g_room;
    const std::string hello = Hello(!g_testName.empty() ? g_testName : g_helloName.empty() ? "slot" + std::to_string(g_slot) : g_helloName);
    int attempts = g_joinAddr.empty() ? 60 : 3;  // tests wait for the host to come up; players get an answer quickly
    bool reconnecting = false;
    int quickDrops = 0;
    for (;;) {
        SOCKET s = INVALID_SOCKET;
        for (int attempt = 0; attempt < attempts && s == INVALID_SOCKET; attempt++) {
            if (g_connGen != gen) return;
            s = TcpConnect(addr, room.empty() ? 7777 : 7780);
            if (s == INVALID_SOCKET) Sleep(reconnecting ? 3000 : 1000);
        }
        if (g_connGen != gen) { if (s != INVALID_SOCKET) closesocket(s); return; }
        if (s == INVALID_SOCKET) {
            Log("!! net: could not reach %s", addr.c_str());
            Push(reconnecting ? "STATUS the connection to the host is gone - press Leave to return to the menu"
                 : room.empty() ? "STATUS could not reach " + addr + " - check the address, and that the host forwarded the port"
                                : "STATUS could not reach the relay " + addr);
            return;
        }
        g_sock = s;
        if (!room.empty()) SendAll(s, "RELAY JOIN " + room + "\n");
        SendAll(s, hello + "\n");
        Log("net: %s %s%s", reconnecting ? "reconnected to" : "connected to", addr.c_str(),
            room.empty() ? "" : (" (relay room " + room + ")").c_str());
        if (reconnecting) Push("STATUS reconnected - the host is resyncing the game...");
        uint64_t since = GetTickCount64();
        ReceiveLoop(s, Push, [] {});
        if (g_connGen != gen) return;  // we left
        closesocket(s);
        g_sock = INVALID_SOCKET;
        // a relay without our room (the host is still reconnecting to it) answers and hangs up at once: those count
        // as failed tries too, so a host that is really gone ends in "gone" after about a minute
        quickDrops = GetTickCount64() - since < 5000 ? quickDrops + 1 : 0;
        if (reconnecting && quickDrops > 20) {
            Log("!! net: the host did not come back");
            Push("STATUS the connection to the host is gone - press Leave to return to the menu");
            return;
        }
        Log("!! net: connection to the host lost - reconnecting");
        Push("STATUS lost the connection to the host - reconnecting...");
        reconnecting = true;
        attempts = 20;  // ~1 minute
        Sleep(quickDrops ? 3000 : 1000);
    }
}

// ---------------------------------------------------------------- host: in-process relay
enum PeerKind { kLocal, kTcp, kSteam, kRelay };  // kRelay: key = the relay's guest id (key 0 = relay control lines)
struct Peer {
    PeerKind kind;
    uint64_t key;      // socket (kTcp) or Steam ID (kSteam)
    std::string name;
    bool ready = false; // has the host's save (READY)
    int epoch = 0;      // last SYNCED epoch
    int mod = 0;        // their companion mod (CompanionMod state, from HELLO / MOD)
    std::string token;  // HELLO token: tells a reconnect from another player of the same name
    ULONGLONG heard = GetTickCount64();  // last line from them (guests send PING every 2 s besides the game's traffic)
};
static std::map<int, Peer> g_peers;          // relay player id -> peer
static uint64_t g_seq;
static std::map<uint64_t, std::map<int, std::string>> g_hashes, g_heaps;
struct RelayIn { PeerKind kind; uint64_t key; std::string line; bool closed; };
static std::mutex g_relayMx;
static std::deque<RelayIn> g_relayIn;        // from TCP receive threads (handled on the main thread)
static bool g_hostStarted;
static bool g_waitReady;                     // host: save sent, waiting for READY from every player
static int g_hostEpoch;                      // host: epoch of the last START
enum ResyncPhase { kResyncNone, kResyncWaitTick, kResyncSaving };
static ResyncPhase g_resyncPhase;
static int64_t g_resyncTick;
static std::string g_resyncReason;
static FILETIME g_resyncSaveTime;
static DWORD g_lastResyncMs;
static void HostResync(const std::string& reason, bool now = false);
static uint64_t g_resyncStarted;             // GetTickCount64 when the current resync began
static uint64_t g_sharedSession;             // ProbeSessionCount() of the shared game while it runs

static void PeerSend(const Peer& p, const std::string& line) {
    switch (p.kind) {
    case kLocal: Push(line); break;
    case kTcp: SendAll((SOCKET)p.key, line + "\n"); break;
    case kSteam: steam::Send(p.key, line.data(), (uint32_t)line.size()); break;
    case kRelay: if (g_relaySock != INVALID_SOCKET) SendAll(g_relaySock, "RELAY TO " + std::to_string(p.key) + " " + line + "\n"); break;
    }
}
static void Broadcast(const std::string& line) {
    for (auto& [id, p] : g_peers) PeerSend(p, line);
}
static void BroadcastNames() {
    std::string s = "NAMES ";
    for (auto& [id, p] : g_peers) {
        std::string n = p.name;
        for (auto& ch : n) if (ch == ';' || ch == '=' || ch == '\n') ch = '_';
        s += std::to_string(id) + "=" + n + ";";
    }
    Broadcast(s);
}
static int FindPeer(PeerKind kind, uint64_t key) {
    for (auto& [id, p] : g_peers)
        if (p.kind == kind && p.key == key) return id;
    return -1;
}
static std::map<std::string, ULONGLONG> g_recentlyLeft;  // host: name / token of players who left -> when (reconnects)
static void RemovePeer(int id, const char* why) {
    auto it = g_peers.find(id);
    if (it == g_peers.end()) return;
    Log("net(host): player %d (%s) left: %s", id, it->second.name.c_str(), why);
    g_recentlyLeft[it->second.name] = g_recentlyLeft["#" + it->second.token] = GetTickCount64();
    if (it->second.kind == kTcp) closesocket((SOCKET)it->second.key);
    if (it->second.kind == kSteam) steam::Close(it->second.key);
    if (it->second.kind == kRelay && g_relaySock != INVALID_SOCKET) SendAll(g_relaySock, "RELAY KICK " + std::to_string(it->second.key) + "\n");
    g_peers.erase(it);
    char line[32];
    sprintf_s(line, "LEFT %d", id);
    Broadcast(line);
    BroadcastNames();
}

// The host's player hears when a friend's bmpShare.pkm is missing or from another build (20 s notice).
static void ModNote(const std::string& who, int mod) {
    std::string name = who.empty() ? std::string("A player") : who;
    Log("net(host): %s's companion mod: %s", name.c_str(), mod == kModMissing ? "MISSING" : mod == kModOther ? "ANOTHER build" : "ok");
    if (mod == kModMissing)
        Push("NOTE " + name + " doesn't have bmpShare.pkm (the mod's second file): they can play, but without the "
             "game-styled multiplayer windows. It goes in Banished\\WinData.");
    else if (mod == kModOther)
        Push("NOTE " + name + " has a bmpShare.pkm that doesn't match the mod's dinput8.dll: some multiplayer windows "
             "won't open for them. They need both files from the same release.");
}

static void HostReceive(int kind, uint64_t key, const std::string& line) {
    int pid = kind == kLocal ? 0 : FindPeer((PeerKind)kind, key);
    std::string k = line.substr(0, line.find(' '));
    std::string rest = line.size() > k.size() ? line.substr(k.size() + 1) : "";
    if (k == "HELLO") {
        if (pid >= 0) return;
        std::string version = "an older version";  // "HELLO @<build id> <name>"; older DLLs send just the name
        if (rest.compare(0, 1, "@") == 0) {
            size_t sp = rest.find(' ');
            version = rest.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
            rest = sp == std::string::npos ? "" : rest.substr(sp + 1);
        }
        std::string token;
        if (size_t hash = version.find('#'); hash != std::string::npos) {
            token = version.substr(hash + 1);
            version.resize(hash);
        }
        int theirMod = kModOk;
        if (size_t bang = version.find('!'); bang != std::string::npos) {
            theirMod = atoi(version.c_str() + bang + 1);
            version.resize(bang);
        }
        if (version != BuildId()) {
            Log("!! net(host): %s tried to join with mod %s, mine is %s - refused", rest.c_str(), version.c_str(),
                BuildId().c_str());
            PeerSend(Peer{(PeerKind)kind, key, rest}, "BADVERSION " + BuildId() + " " + version);
            Push("MSG " + (rest.empty() ? std::string("a player") : rest) + " has a different version of the mod (" + version +
                 ", yours " + BuildId() + ") - you both need the same dinput8.dll");
            return;
        }
        // a player coming back (dropped connection; the host may not have noticed yet): the old entry goes, so the lowest
        // free id - normally the old one, and with it the old town - is theirs again
        bool back = false;  // a reconnect, not a new player
        for (auto it = g_peers.begin(); it != g_peers.end();) {
            int oldId = it->first;
            // the same game (also when it got a "(2)" name), or the same name from a game that went quiet (crashed and
            // restarted: a new token); the same name from a game that is still talking is somebody else
            bool quiet = GetTickCount64() - it->second.heard > 6000;
            bool same = it->second.kind != kLocal &&
                        ((!token.empty() && it->second.token == token) ||
                         (!rest.empty() && it->second.name == rest && (quiet || token.empty() || it->second.token.empty())));
            if (same) rest = it->second.name, back = true;
            ++it;
            if (same) RemovePeer(oldId, "reconnected");
        }
        // somebody else already has this name (another player, or the host): number this one
        auto taken = [](const std::string& n) {
            for (auto& [i, p] : g_peers) if (p.name == n) return true;
            return false;
        };
        if (!rest.empty() && taken(rest)) {
            std::string base = rest;
            for (int k = 2; taken(rest); k++) rest = base + " (" + std::to_string(k) + ")";
            Log("net(host): another player is already called %s - this one is %s", base.c_str(), rest.c_str());
        }
        auto recent = [](const std::string& k) { auto r = g_recentlyLeft.find(k); return r != g_recentlyLeft.end() && GetTickCount64() - r->second < 120000; };
        back = back || (!token.empty() && recent("#" + token)) || (!rest.empty() && recent(rest));
        int id = 1;
        while (g_peers.count(id)) id++;
        g_peers[id] = Peer{(PeerKind)kind, key, rest};
        g_peers[id].token = token;
        char w[48];
        sprintf_s(w, "WELCOME %d %d", id, (int)g_peers.size());
        PeerSend(g_peers[id], w);
        Log("net(host): player %d joined (%s), %d players", id, rest.c_str(), (int)g_peers.size());
        g_peers[id].mod = theirMod;
        if (theirMod != kModOk) ModNote(rest, theirMod);
        g_peers[id].epoch = -1;  // not part of any session yet
        BroadcastNames();
        if (g_hostStarted) HostResync(rest + (back ? " reconnected" : " joined"));  // join in progress: everybody reloads
        return;
    }
    if (pid < 0) return;
    g_peers[pid].heard = GetTickCount64();
    if (k == "PING") return;
    if (k == "MOD") {  // their companion mod state changed after HELLO (found out later)
        int m = atoi(rest.c_str());
        if (pid > 0 && m != g_peers[pid].mod) {
            g_peers[pid].mod = m;
            ModNote(g_peers[pid].name, m);
        }
        return;
    }
    if (k == "CMD") {
        Broadcast("CMD " + std::to_string(pid) + " " + std::to_string(++g_seq) + " " + rest);
    } else if (k == "TURN" || k == "CTL") {
        Broadcast(k + " " + std::to_string(pid) + " " + rest);
    } else if (k == "TRADE") {  // offers/answers between players: relay as-is with the sender
        Broadcast("TRADE " + std::to_string(pid) + " " + rest);
    } else if (k == "RESYNCREQ") {  // a player asked for a resync (UI button)
        HostResync(g_peers[pid].name + " asked for a resync");
    } else if (k == "SYNCED") {
        g_peers[pid].epoch = atoi(rest.c_str());
        Broadcast("SYNCED " + std::to_string(pid) + " " + rest);
    } else if (k == "READY") {
        g_peers[pid].ready = true;
        Log("net(host): player %d has the save", pid);
        HostCheckReady();
    } else if (k == "FAIL") {
        Log("!! net(host): player %d (%s) failed: %s", pid, g_peers[pid].name.c_str(), rest.c_str());
        Broadcast("MSG " + g_peers[pid].name + ": " + rest);
        RemovePeer(pid, "could not get the save");
        HostCheckReady();
    } else if (k == "HASH") {
        if (g_peers[pid].epoch != g_hostEpoch) return;  // from an old session
        unsigned long long tick = strtoull(rest.c_str(), nullptr, 10);
        auto& seen = g_hashes[tick];
        seen[pid] = rest.substr(rest.find(' ') + 1);
        size_t inSession = 0;
        for (auto& [id, p] : g_peers) inSession += p.epoch == g_hostEpoch;
        if (seen.size() >= inSession) {
            bool ok = true;
            for (auto& [p, h] : seen) ok &= h == seen.begin()->second;
            if (!ok || tick % 6000 == 0) Log("net(host): tick %llu hashes %s", tick, ok ? "MATCH" : "DESYNC");
            if (!ok) DumpDraws("DESYNC at tick " + std::to_string(tick));
            g_hashes.erase(tick);
            if (!ok) HostResync("towns went out of sync");  // automatic recovery
        }
        Broadcast("HASH " + std::to_string(pid) + " " + rest);
    } else if (k == "HEAP") {
        if (g_peers[pid].epoch != g_hostEpoch) return;
        unsigned long long tick = strtoull(rest.c_str(), nullptr, 10);
        auto& seen = g_heaps[tick];
        seen[pid] = rest.substr(rest.find(' ') + 1);
        size_t inSession = 0;
        for (auto& [id, p] : g_peers) inSession += p.epoch == g_hostEpoch;
        if (seen.size() >= inSession) {
            bool ok = true;
            for (auto& [p, h] : seen) ok &= h == seen.begin()->second;
            if ((!ok && !g_heapDumped) || tick % 6000 == 0) Log("net(host): tick %llu sim heaps %s", tick, ok ? "MATCH" : "DRIFTED");
            if (!ok) HeapDrift(tick, "host compare");
            g_heaps.erase(tick);
        }
        if (g_heaps.size() > 64) g_heaps.erase(g_heaps.begin());
        Broadcast("HEAP " + std::to_string(pid) + " " + rest);
    }
}

static void TcpClientThread(SOCKET s) {
    ReceiveLoop(s,
        [s](const std::string& l) { std::lock_guard<std::mutex> lk(g_relayMx); g_relayIn.push_back({kTcp, (uint64_t)s, l, false}); },
        [s] { std::lock_guard<std::mutex> lk(g_relayMx); g_relayIn.push_back({kTcp, (uint64_t)s, "", true}); });
}

// Host: connection out to the relay server; guests' lines arrive as "RELAY FROM <id> <line>". A lost connection is
// retried for about a minute with the same room code; the token lets the relay hand the room back to us even if it
// hasn't noticed yet that the old connection is dead (relays older than that answer "expected RELAY HOST/JOIN" to
// the token: then without it). Guests reconnect by themselves; the main thread drops the relay players meanwhile.
static void RelayHostThread(std::string addr, std::string room) {
    const int gen = g_connGen;
    static std::string token;
    if (token.empty()) { std::random_device rd; char t[17]; sprintf_s(t, "%08x%08x", rd(), rd()); token = t; }
    auto in = [](uint64_t key, const std::string& line, bool closed) {
        std::lock_guard<std::mutex> lk(g_relayMx);
        g_relayIn.push_back({kRelay, key, line, closed});
    };
    bool useToken = true, wasUp = false;
    for (int failures = 0; g_connGen == gen;) {
        SOCKET s = TcpConnect(addr, 7780);
        if (s == INVALID_SOCKET) {
            if (!wasUp || ++failures > 20) { in(0, "RELAY ERR could not reach the relay " + addr, false); return; }
            Sleep(3000);
            continue;
        }
        g_relaySock = s;
        SendAll(s, "RELAY HOST " + room + (useToken ? " " + token : "") + "\n");
        bool oldRelay = false, up = false;
        ReceiveLoop(s,
            [&](const std::string& l) {
                if (l.compare(0, 11, "RELAY FROM ") == 0) {
                    size_t sp = l.find(' ', 11);
                    if (sp != std::string::npos) in(strtoull(l.c_str() + 11, nullptr, 10), l.substr(sp + 1), false);
                } else if (l.compare(0, 11, "RELAY DROP ") == 0) {
                    in(strtoull(l.c_str() + 11, nullptr, 10), "", true);
                } else if (useToken && l.find("expected RELAY HOST") != std::string::npos) {
                    oldRelay = true;  // this relay doesn't know tokens yet
                } else {
                    if (l.compare(0, 9, "RELAY OK ") == 0) up = true;
                    in(0, l, false);  // RELAY OK / ERR / CONN
                }
            },
            [] {});
        closesocket(s);
        g_relaySock = INVALID_SOCKET;
        if (g_connGen != gen) return;  // we stopped hosting
        if (oldRelay) { useToken = false; continue; }
        if (!up && !wasUp) return;     // refused (the ERR line went to the main thread)
        if (up) failures = 0;
        wasUp = true;
        in(0, "RELAY LOST", false);
        Sleep(2000);
    }
}

static void ListenThread() {
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    g_listenSock = ls;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)g_listenPort);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(ls, (sockaddr*)&a, sizeof a) || listen(ls, 8)) {
        Log("!! net(host): cannot listen on port %d (error %d)", g_listenPort, WSAGetLastError());
        return;
    }
    Log("net(host): listening for TCP players on port %d", g_listenPort);
    for (;;) {
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) return;
        int one = 1;
        setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof one);
        std::thread(TcpClientThread, c).detach();
    }
}

// newest .sav in the save folder (what the host most likely wants to share)
static std::vector<std::string> ListSaves() {
    struct Item { std::string stem; FILETIME t; };
    std::vector<Item> items;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((gameload::SaveDir() + L"\\*.sav").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            std::string stem = Narrow(n.substr(0, n.size() - 4));
            if (_stricmp(stem.c_str(), "Multiplayer") != 0) items.push_back({stem, fd.ftLastWriteTime});
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(items.begin(), items.end(), [](const Item& x, const Item& y) { return CompareFileTime(&x.t, &y.t) > 0; });
    std::vector<std::string> out;
    for (auto& i : items) out.push_back(i.stem);
    return out;
}

// Everybody (host included) loads Multiplayer.sav = a byte copy of `src`. Sends it to every remote player and waits
// for their READY; HostCheckReady then broadcasts START <players> <epoch>.
static bool HostSendSave(const std::wstring& src) {
    std::vector<uint8_t> bytes;
    if (!ReadFileBytes(src, bytes) || bytes.empty()) {
        Log("!! net(host): cannot read %s", Narrow(src).c_str());
        g_status = "cannot read save " + Narrow(src);
        return false;
    }
    std::wstring dst = gameload::SaveDir() + L"\\Multiplayer.sav";
    if (_wcsicmp(src.c_str(), dst.c_str()) != 0) CopyFileW(src.c_str(), dst.c_str(), FALSE);
    char head[96];
    sprintf_s(head, "FILE Multiplayer %zu %016llx", bytes.size(), Fnv(bytes));
    std::vector<std::string> lines{head};
    for (size_t i = 0; i < bytes.size(); i += 45000) lines.push_back("DATA " + B64Encode(bytes.data() + i, std::min<size_t>(45000, bytes.size() - i)));
    lines.push_back("FILEEND");
    for (auto& [id, p] : g_peers) {
        p.ready = p.kind == kLocal;
        if (p.kind != kLocal)
            for (auto& l : lines) PeerSend(p, l);
    }
    g_saveName = "Multiplayer";
    Log("net(host): sent %s (%zu bytes) to %d players", Narrow(src).c_str(), bytes.size(), (int)g_peers.size() - 1);
    g_waitReady = true;
    HostCheckReady();  // nobody to wait for when hosting alone
    return true;
}

static void HostStart() {
    if (g_hostStarted) return;
    g_hostStarted = true;
    steam::SetLobbyData("started", "1");
    if (g_sharedNew) {  // no file: everybody generates the same new map with one town they all play
        for (auto& [id, p] : g_peers) p.epoch = 0;
        Broadcast("START " + std::to_string(g_peers.size()) + " 0 NEW " + g_newArgs);
        g_sharedNew = false;  // later (re)loads go through the host's save like any shared game
        return;
    }
    if (g_separate) {  // no file: everybody generates the same new map, each player gets a town
        std::string ids;
        for (auto& [id, p] : g_peers) { p.epoch = 0; ids += (ids.empty() ? "" : ",") + std::to_string(id); }
        Broadcast("START " + std::to_string(g_peers.size()) + " 0 NEW " + g_newArgs + " TOWNS " + ids + (g_pickStarts ? " PICK" : ""));
        return;
    }
    if (!g_hostSave.empty()) {
        if (!HostSendSave(gameload::SaveDir() + L"\\" + Widen(g_hostSave) + L".sav")) { g_hostStarted = false; return; }
        g_status = "sending the save to the other players...";
    } else {
        for (auto& [id, p] : g_peers) p.epoch = 0;
        Broadcast("START " + std::to_string(g_peers.size()) + " 0");  // harness: everybody loads its configured save
    }
}

static void HostCheckReady() {
    if (!g_waitReady) return;
    for (auto& [id, p] : g_peers)
        if (!p.ready) return;
    g_waitReady = false;
    int epoch = g_hostEpoch + (g_resyncReason.empty() ? 0 : 1);  // first start = epoch 0, every resync +1
    g_hostEpoch = epoch;
    if (epoch == 0) for (auto& [id, p] : g_peers) p.epoch = 0;  // epoch 0 needs no SYNCED
    g_resyncReason.clear();
    Log("net(host): every player has the save -> START epoch %d", epoch);
    std::string towns;
    if (g_separate) {  // the save holds every town: keep each player's town, a new player gets a new (unsettled) one
        std::vector<int> byTown;
        for (auto& [id, t] : g_townOf) {
            if ((int)byTown.size() <= t) byTown.resize(t + 1, -1);
            byTown[t] = id;
        }
        bool added = false;
        for (auto& [id, p] : g_peers) {
            if (g_townOf.count(id)) continue;
            // a returning player gets the town the save remembers for their name, anyone else a town nobody had
            int t = SavedTownOf(p.name, byTown, towns::Count());
            if (t < 0) t = UnclaimedTown(byTown, towns::Count());
            if (t >= 0) {
                if ((int)byTown.size() <= t) byTown.resize(t + 1, -1);
                byTown[t] = id;
                Log("net(host): %s gets town %d (%s)", p.name.c_str(), t, towns::TownPlayer(t).empty() ? "free" : "theirs in the save");
            } else {
                byTown.push_back(id);  // a new town, settled by its player
                added = true;
            }
        }
        for (size_t i = 0; i < byTown.size(); i++) towns += (i ? "," : "") + std::to_string(byTown[i]);
        towns = " TOWNS " + towns + (g_pickStarts || added ? " PICK" : "");
    }
    Broadcast("START " + std::to_string(g_peers.size()) + " " + std::to_string(epoch) + towns);
}

// Resync / join in progress: every player stops at tick T (no commands are lost: all with slot <= T are known), the
// host saves its game there, sends the file, and everybody - host included - reloads it as a new session.
// now = the host's game itself was replaced (a save loaded from the game's menu): save it at the next tick of that
// game, whatever turns the old game had promised.
static void HostResync(const std::string& reason, bool now) {
    if (!g_hostStarted || g_resyncPhase != kResyncNone || g_waitReady) return;
    if (!now && GetTickCount() - g_lastResyncMs < 10000 && reason.find("joined") == std::string::npos &&
        reason.find("reconnected") == std::string::npos) return;  // no storms
    g_lastResyncMs = GetTickCount();
    g_resyncReason = reason;
    g_resyncTick = now ? (int64_t)ProbeTick() + 1 : std::max<int64_t>(g_sentTurn + 1, (int64_t)ProbeTick() + 1);
    g_resyncStarted = GetTickCount64();
    g_resyncPhase = kResyncWaitTick;
    Log("net(host): RESYNC at tick %lld (%s)", g_resyncTick, reason.c_str());
    Broadcast("RESYNC " + std::to_string(g_resyncTick) + " " + reason);
}

static void HostResyncFrame() {
    // normally at the agreed tick; if the host's game can't get there (paused, or a different game whose ticks restarted)
    // save where it is after 15 s - everybody reloads that save anyway, a stuck resync froze the whole session
    bool late = GetTickCount64() - g_resyncStarted > 15000;
    if (g_resyncPhase == kResyncWaitTick && ProbeGameLoop() && ((int64_t)ProbeTick() >= g_resyncTick || late)) {
        if (late && (int64_t)ProbeTick() < g_resyncTick) Log("!! net(host): resync tick %lld not reached (at %llu) - saving now", g_resyncTick, ProbeTick());
        GetSystemTimeAsFileTime(&g_resyncSaveTime);
        if (gameload::BeginAutoSave()) g_resyncPhase = kResyncSaving;
    } else if (g_resyncPhase == kResyncSaving && !gameload::SaveBusy()) {
        std::wstring autoSav = gameload::SaveDir() + L"\\Auto.sav";
        WIN32_FILE_ATTRIBUTE_DATA fa;
        if (!GetFileAttributesExW(autoSav.c_str(), GetFileExInfoStandard, &fa) || CompareFileTime(&fa.ftLastWriteTime, &g_resyncSaveTime) < 0)
            return;  // the file write may still be in flight
        std::vector<uint8_t> probe;
        if (!ReadFileBytes(autoSav, probe) || probe.empty()) return;
        Sleep(200);  // let the writer finish
        g_resyncPhase = kResyncNone;
        if (!HostSendSave(autoSav)) g_resyncReason.clear();
    }
}

static std::string PlayerName() {
    if (!g_testName.empty()) return g_testName;
    if (!g_playerName.empty()) return g_playerName;
    if (steam::Init()) return steam::PersonaName();
    char n[128] = "";
    DWORD len = sizeof n;
    return GetUserNameA(n, &len) && n[0] ? std::string(n) : std::string("player");
}

static void BecomeHost() {
    g_mode = kHost;
    g_active = true;
    g_peers.clear();
    g_peers[0] = Peer{kLocal, 0, PlayerName()};
    g_names[0] = g_peers[0].name;
    Push("WELCOME 0 1");
    if (g_listenPort) std::thread(ListenThread).detach();
}

// ---------------------------------------------------------------- client: received file
static std::string g_fileName;
static size_t g_fileSize;
static uint64_t g_fileHash;
static std::vector<uint8_t> g_fileBytes;
// Frame (after the inbox): leave the session and show this - a failed save transfer used to leave the guest stuck in
// the host's lobby, ignoring the host's next invite ("already in a session")
static std::string g_leaveNote;
static uint64_t g_switchLobby;  // a Steam invite that arrived while waiting in another lobby: leave, then join it

static void FileDone() {
    if (g_testCorruptSave && !g_fileBytes.empty()) g_fileBytes.pop_back(), g_testCorruptSave = false;
    if (g_fileBytes.size() != g_fileSize || Fnv(g_fileBytes) != g_fileHash) {
        Log("!! net: save transfer corrupt (%zu of %zu bytes)", g_fileBytes.size(), g_fileSize);
        g_status = "save transfer failed";
        SendLine("FAIL save transfer corrupt");
        g_leaveNote = "couldn't receive the host's save - ask them to invite you again";
        return;
    }
    std::wstring path = gameload::SaveDir() + L"\\" + Widen(g_fileName) + L".sav";
    std::vector<uint8_t> existing;
    if (ReadFileBytes(path, existing) && existing == g_fileBytes) {
        Log("net: save '%s' already present (identical)", g_fileName.c_str());
    } else {
        int dirErr = SHCreateDirectoryExW(nullptr, gameload::SaveDir().c_str(), nullptr);  // whole chain (fresh installs)
        FILE* f = nullptr;
        errno_t err = _wfopen_s(&f, path.c_str(), L"wb");
        size_t wrote = f ? fwrite(g_fileBytes.data(), 1, g_fileBytes.size(), f) : 0;
        if (f) fclose(f);
        if (err || !f || wrote != g_fileBytes.size()) {
            Log("!! net: cannot write %s (errno %d, dir result %d, wrote %zu)", Narrow(path).c_str(), (int)err, dirErr, wrote);
            char buf[160];
            sprintf_s(buf, "could not write the save to Documents/Banished/Save (error %d)", (int)err);
            g_status = buf;
            SendLine(std::string("FAIL ") + buf);
            return;
        }
        Log("net: received save '%s' (%zu bytes)", g_fileName.c_str(), g_fileBytes.size());
    }
    g_saveName = g_fileName;
    SendLine("READY");
    g_fileBytes.clear();
    g_fileBytes.shrink_to_fit();
}

// ---------------------------------------------------------------- Steam lobby flow
static void JoinSteamLobby(uint64_t lobby) {
    if (g_mode == kClientSteam && steam::Lobby() == lobby) { Log("net: invite to the lobby I'm in - ignored"); return; }
    if (g_mode != kOff && g_mode != kHost && !g_sessionInGame) {  // waiting in another lobby: switch over
        Log("net: invite while waiting in another session - switching to lobby %llu", lobby);
        g_switchLobby = lobby;
        return;
    }
    if (g_mode != kOff) {
        Log("net: ignoring lobby invite, already in a session");
        g_notice = "you were invited to another game - leave this one first to join it";
        g_noticeUntil = GetTickCount() + 10000;
        return;
    }
    if (!steam::Init()) return;
    Log("net: joining Steam lobby %llu", lobby);
    g_status = "joining the game...";
    steam::JoinLobby(lobby);
}

static void OnSteamEvent(steam::Event e, uint64_t a) {
    switch (e) {
    case steam::kLobbyCreated:
        steam::SetLobbyData("bmp", "1");
        Log("net: lobby %llu created: owner=%llu (me %llu), members=%d", a, steam::LobbyOwner(), steam::MySteamId(), steam::LobbyMembers());
        steam::OpenInviteOverlay();
        break;
    case steam::kLobbyEntered:
        if (g_mode == kHost) break;  // our own lobby
        g_steamHost = steam::LobbyOwner();
        Log("net: in lobby %llu, host is %llu", a, g_steamHost);
        g_mode = kClientSteam;
        g_active = true;
        SendLine(Hello(steam::PersonaName()));
        g_status = "connected, waiting for host to start";
        break;
    case steam::kLobbyFailed:
        g_status = "Steam lobby failed";
        break;
    case steam::kJoinRequested:
        JoinSteamLobby(a);
        break;
    case steam::kP2PRequest:
        if (g_mode == kHost) steam::Accept(a);  // also mid-game: (re)joining triggers a resync
        else if (g_mode == kClientSteam && a == g_steamHost) steam::Accept(a);
        break;
    case steam::kP2PFailed:
        if (g_mode == kHost) { int id = FindPeer(kSteam, a); if (id > 0) RemovePeer(id, "P2P connection failed"); }
        else if (g_mode == kClientSteam && a == g_steamHost) { Log("!! net: lost connection to host"); g_status = "lost connection to host"; }
        break;
    case steam::kMemberChange:
        if (g_mode == kHost && g_hostStarted) {
            int id = FindPeer(kSteam, a);
            if (id > 0 && steam::LobbyMembers() < (int)g_peers.size()) RemovePeer(id, "left the lobby");
        }
        break;
    }
}

static void PollSteam() {
    std::string data;
    uint64_t from;
    while (steam::Receive(data, from)) {
        size_t s = 0, e;
        do {
            e = data.find('\n', s);
            std::string line = data.substr(s, e == std::string::npos ? std::string::npos : e - s);
            if (!line.empty()) {
                if (g_mode == kHost) HostReceive(kSteam, from, line);
                else if (g_mode == kClientSteam && from == g_steamHost) Push(line);
            }
            s = e + 1;
        } while (e != std::string::npos);
    }
}

// ---------------------------------------------------------------- hotkeys + window title
static HWND g_wnd;
static BOOL CALLBACK FindMainWindow(HWND w, LPARAM) {
    DWORD pid;
    GetWindowThreadProcessId(w, &pid);
    wchar_t cls[16];
    if (pid == GetCurrentProcessId() && IsWindowVisible(w) && GetClassNameW(w, cls, 16) && wcscmp(cls, L"#32770") != 0) {
        g_wnd = w;
        return FALSE;
    }
    return TRUE;
}
static void UpdateTitle() {
    static std::string shown = "\x01";
    std::string s = g_status;
    if (s == shown) return;
    if (!g_wnd) EnumWindows(FindMainWindow, 0);
    if (!g_wnd) return;
    shown = s;
    std::wstring t = L"Banished";
    if (!s.empty()) t += L"  |  MULTIPLAYER: " + Widen(s);
    SetWindowTextW(g_wnd, t.c_str());
}

static bool Pressed(int vk) {
    static bool down[256];
    bool now = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool edge = now && !down[vk];
    down[vk] = now;
    return edge;
}

static void HostStatus() {
    int friends = (int)g_peers.size() - 1;
    std::string what = g_hostSave == "new map" ? "a new map" : _stricmp(g_hostSave.c_str(), "Auto") == 0 ? "the autosave" : "'" + g_hostSave + "'";
    std::string n = friends == 1 ? "1 other player" : std::to_string(friends) + " other players";
    if (g_hostStarted) g_status = "Hosting " + what + (friends ? " with " + n : " - waiting for players");
    else g_status = "Hosting " + what + (friends ? " - " + n + " joined" : " - nobody has joined yet");
}

static void DoHost(const std::string& arg) {
    if (g_mode != kOff) return;
    if (!gameload::InMenu()) { g_status = "go to the main menu first"; return; }
    std::string save = arg;
    g_separate = arg.compare(0, 2, "N|") == 0;
    g_sharedNew = arg.compare(0, 2, "M|") == 0;  // one shared town on a new map: everybody generates it, no file
    if (g_sharedNew) {
        g_newArgs = arg.substr(2);
        save = "new town";
    } else if (g_separate) {
        g_newArgs = arg.substr(2);
        size_t bar = g_newArgs.find('|');
        g_pickStarts = bar == std::string::npos || g_newArgs.substr(bar + 1) != "0";
        if (bar != std::string::npos) g_newArgs.resize(bar);
        save = "new map";
    }
    else if (arg.compare(0, 2, "S|") == 0) save = arg.substr(2);
    if (save.empty()) { g_status = "no saves to host"; return; }
    if (g_method == kViaSteam && !steam::Init()) { g_status = "Steam not available - choose Direct or Relay"; return; }
    if (g_method == kViaRelay && g_relayAddr.empty()) { g_status = "enter the relay server's address first (Relay address below) - the mod's GitHub page explains how to run one"; return; }
    g_hostSave = save;
    g_hostVia = g_method;
    if (g_method == kViaDirect) g_listenPort = g_hostPort;
    BecomeHost();
    if (g_method == kViaSteam) {
        steam::CreateLobby(8);
        Log("net: hosting '%s' through Steam", g_hostSave.c_str());
    } else if (g_method == kViaDirect) {
        Log("net: hosting '%s' directly on TCP port %d", g_hostSave.c_str(), g_hostPort);
    } else {
        static const char kChars[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";  // no 0/O/1/I
        g_room.clear();
        uint64_t r = GetTickCount64() * 6364136223846793005ull + GetCurrentProcessId();
        for (int i = 0; i < 6; i++) { r = r * 6364136223846793005ull + 1442695040888963407ull; g_room += kChars[(r >> 33) % 32]; }
        g_relayUp = false;
        std::thread(RelayHostThread, g_relayAddr, g_room).detach();
        Log("net: hosting '%s' through the relay %s, room %s", g_hostSave.c_str(), g_relayAddr.c_str(), g_room.c_str());
    }
    HostStatus();
}

// Join a game hosted over TCP: arg "D|host:port" (direct) or "R|ROOMCODE" (relay from the settings)
static void DoJoin(const std::string& arg) {
    if (g_mode != kOff) return;
    if (!gameload::InMenu()) { g_status = "go to the main menu first"; return; }
    std::string v = arg.size() > 2 ? arg.substr(2) : "";
    while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back();
    while (!v.empty() && isspace((unsigned char)v.front())) v.erase(0, 1);
    if (v.empty()) { g_status = arg[0] == 'R' ? "enter the room code" : "enter the host's address"; return; }
    if (arg[0] == 'R') {
        if (g_relayAddr.empty()) { g_status = "enter the relay server's address first (Relay address below) - the mod's GitHub page explains how to run one"; return; }
        g_joinAddr = g_relayAddr;
        g_room = v;
        for (auto& ch : g_room) ch = (char)toupper((unsigned char)ch);
    } else {
        g_joinAddr = v;
        g_room.clear();
    }
    g_helloName = PlayerName();
    g_mode = kClientTcp;
    g_active = true;
    g_status = arg[0] == 'R' ? "joining room " + g_room + "..." : "connecting to " + g_joinAddr + "...";
    Log("net: joining %s%s", g_joinAddr.c_str(), g_room.empty() ? "" : (" room " + g_room).c_str());
    std::thread(Connect).detach();
}

static void DoLeave() {
    Log("net: leaving the multiplayer session");
    ResetSession();
}

static void UiActions() {
    std::deque<std::pair<int, std::string>> acts;
    {
        std::lock_guard<std::mutex> lk(g_uiMx);
        acts.swap(g_uiActions);
    }
    for (auto& [a, arg] : acts) {
        switch (a) {
        case kUiHost: DoHost(arg); break;
        case kUiInvite:  // Steam: the overlay's invite list; relay / direct have no Steam lobby -> what friends need to join
            if (g_mode != kHost) break;
            if (g_hostVia == kViaSteam) { steam::OpenInviteOverlay(); break; }
            if (g_hostVia == kViaRelay && !g_room.empty()) {
                CopyText(g_room);
                g_notice = "Room code " + g_room + " copied - players choose Relay server and paste it under Join a game";
            } else {
                g_notice = "Players choose Direct (IP) and join your public IP address with port " + std::to_string(g_hostPort);
            }
            g_noticeUntil = GetTickCount() + 10000;
            Log("net: invite -> %s", g_notice.c_str());
            break;
        case kUiStart: if (g_mode == kHost && !g_hostStarted) { if (!arg.empty()) g_hostSave = arg; HostStart(); } break;
        case kUiResync:
            if (g_mode == kHost) HostResync("requested by the host");
            else if (g_active) SendLine("RESYNCREQ");
            break;
        case kUiLeave: if (g_mode != kOff) DoLeave(); break;
        case kUiJoin: DoJoin(arg); break;
        }
    }
}

static void Hotkeys() {
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    bool mine = pid == GetCurrentProcessId();
    bool chord = mine && (GetAsyncKeyState(VK_CONTROL) & 0x8000) && (GetAsyncKeyState(VK_SHIFT) & 0x8000);
    bool h = Pressed('H'), n = Pressed('N'), i = Pressed('I'), s = Pressed('S');
    if (g_steamHostAuto && g_mode == kOff && gameload::InMenu()) { g_steamHostAuto = false; h = chord = true; }
    if (!chord) return;
    if (h && g_mode == kOff) {
        auto saves = ListSaves();
        DoHost(saves.empty() ? "" : "S|" + saves[0]);
    } else if (n && g_mode == kHost && !g_hostStarted) {
        auto saves = ListSaves();
        auto it = std::find(saves.begin(), saves.end(), g_hostSave);
        if (!saves.empty()) g_hostSave = (it == saves.end() || it + 1 == saves.end()) ? saves[0] : *(it + 1);
        HostStatus();
    } else if (i && g_mode == kHost) {
        steam::OpenInviteOverlay();
    } else if (s && g_mode == kHost && !g_hostStarted) {
        HostStart();
    }
}

// ---------------------------------------------------------------- init / config
static void LoadSettings();

void Init() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    steam::SetEventHandler(OnSteamEvent);
    LoadSettings();
    Log("net: mod build %s", BuildId().c_str());
    update::Start(kModVersion);
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wcscpy_s(wcsrchr(path, L'\\') + 1, 64, L"bmp_net.txt");
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"r");
    if (!f) return;
    bool relay = false;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[64], val[192];
        if (sscanf_s(line, " %63[^= ] = %191[^\r\n]", key, (unsigned)sizeof key, val, (unsigned)sizeof val) != 2) continue;
        std::string k = key, v = val;
        size_t at = k.find('@');
        if (at != std::string::npos) {
            if (atoi(k.c_str() + at + 1) != g_slot) continue;
            k = k.substr(0, at);
        }
        if (k == "relay") {
            size_t c = v.find(':');
            g_host = v.substr(0, c);
            if (c != std::string::npos) g_port = atoi(v.c_str() + c + 1);
            relay = true;
        } else if (k == "host") {
            g_listenPort = atoi(v.c_str());
        } else if (k == "players") {
            g_autoStart = atoi(v.c_str());
        } else if (k == "save") {
            g_hostSave = v;
        } else if (k == "resyncat") {
            g_testResyncAt = atoll(v.c_str());
            for (size_t cpos = v.find(','); cpos != std::string::npos; cpos = v.find(',', cpos + 1))
                g_testResyncMore.push_back(atoll(v.c_str() + cpos + 1));
        } else if (k == "dumpat") {
            g_testDumpAt = atoll(v.c_str());
        } else if (k == "stallat") {
            g_testStallAt = atoll(v.c_str());
        } else if (k == "dropat") {
            g_testDropAt = atoll(v.c_str());
        } else if (k == "corruptsave") {
            g_testCorruptSave = v == "1";
        } else if (k == "fakeversion") {
            g_fakeVersion = v;
        } else if (k == "joindelay") {
            g_joinDelay = atoi(v.c_str());
        } else if (k == "recvname") {
            g_recvName = v;
        } else if (k == "name") {  // test: this instance's player name (name@N=...; default slotN / the settings name)
            g_testName = v;
        } else if (k == "pick") {
            g_pickStarts = v != "0";
        } else if (k == "separate") {
            g_separate = true;
            g_newArgs = v;
        } else if (k == "sharednew") {  // test: host one shared town on a new map (seed,size,start,terrain,climate)
            g_sharedNew = true;
            g_newArgs = v;
        } else if (k == "relayhost") {  // test: host through this relay server (room from "room")
            g_relayAddr = v;
            g_method = kViaRelay;
        } else if (k == "room") {
            g_room = v;
        } else if (k == "steamhost") {
            g_steamHostAuto = v != "0";
        } else if (k == "delay") {
            g_delay = strtoull(v.c_str(), nullptr, 10);
            g_fixedDelay = true;
        }
    }
    fclose(f);
    if (g_method == kViaRelay && !g_room.empty() && !relay) {  // test host through a relay
        g_hostVia = kViaRelay;
        BecomeHost();
        std::thread(RelayHostThread, g_relayAddr, g_room).detach();
        Log("net: ACTIVE hosting through the relay %s room %s (auto-start at %d players)", g_relayAddr.c_str(), g_room.c_str(), g_autoStart);
    } else if (g_listenPort) {
        BecomeHost();
        Log("net: ACTIVE hosting on TCP port %d (auto-start at %d players, save '%s') delay=%llu", g_listenPort,
            g_autoStart, g_hostSave.c_str(), g_delay);
    } else if (relay) {
        g_mode = kClientTcp;
        g_active = true;
        Log("net: ACTIVE relay=%s:%d delay=%llu ticks", g_host.c_str(), g_port, g_delay);
        std::thread(Connect).detach();
    }
}

void SendCommand(uint64_t slot, const std::string& line) {
    char head[32];
    sprintf_s(head, "CMD %llu ", slot);
    SendLine(head + line);
}

static void ResetSession() {
    Log("net: session over, back to single player");
    g_connGen++;  // a guest connection thread stops instead of reconnecting
    if (g_mode == kClientTcp && g_sock != INVALID_SOCKET) closesocket(g_sock);
    g_sock = INVALID_SOCKET;
    if (g_relaySock != INVALID_SOCKET) { closesocket(g_relaySock); g_relaySock = INVALID_SOCKET; }
    if (g_listenSock != INVALID_SOCKET) { closesocket(g_listenSock); g_listenSock = INVALID_SOCKET; }
    g_room.clear();
    g_joinAddr.clear();
    g_helloName.clear();
    g_hostVia = -1;
    g_relayUp = false;
    g_listenPort = 0;
    for (auto& [id, p] : g_peers) if (p.kind == kTcp) closesocket((SOCKET)p.key);
    steam::LeaveLobby();
    g_mode = kOff;
    g_active = g_started = g_hostStarted = g_loadPending = g_sessionInGame = false;
    g_peers.clear();
    g_turns.clear();
    g_myHash.clear();
    g_myHeap.clear();
    g_heapDumped = false;
    g_hashes.clear();
    g_heaps.clear();
    g_me = -1;
    g_players = 0;
    g_sentTurn = -1;
    g_epoch = g_hostEpoch = 0;
    g_peerEpoch.clear();
    g_resyncAt = -1;
    g_startSessions = UINT64_MAX;
    g_resyncPhase = kResyncNone;
    g_resyncReason.clear();
    g_seq = 0;
    g_waitReady = false;
    g_netSpeed = -1;
    g_netPaused = 0;
    g_wroteSpeed = g_wrotePaused = -1;
    g_saveName.clear();
    g_steamHost = 0;
    g_status.clear();
    g_wasResync = false;
    g_statusExpires = 0;
    g_names.clear();
    g_hostSave.clear();
    g_separate = false;
    g_sharedNew = false;
    g_newArgs.clear();
    g_loadNewArgs.clear();
    g_separateSession = false;
    g_townOf.clear();
    g_offers.clear();
    g_tradeNews.clear();
    towns::Configure(1, 0);
    towns::SetPlayers(1);
    std::lock_guard<std::mutex> lk(g_mx);
    g_inbox.clear();
}

// A save with several towns hosted as an ordinary save (no TOWNS in START): once it is loaded, give the towns out by
// player id, host first - the same on every machine. Also called when a command executes (it may come first).
static void AssignSavedTowns() {
    if (!g_started || towns::Count() < 2 || !g_townOf.empty()) return;
    if (ProbeSessionCount() == g_startSessions) return;  // the shared save isn't loaded yet (a game started earlier may still run)
    std::vector<int> ids;
    for (auto& [id, name] : g_names) ids.push_back(id);
    if (ids.empty()) ids.push_back(g_me);
    // returning players first get the town the save remembers for their name; the others take free towns in order,
    // towns nobody is remembered for first (an absent friend's town stays theirs). Same result on every machine:
    // the roster and the save are the same everywhere.
    std::vector<int> byTown(towns::Count(), -1);
    for (int id : ids)
        if (int t = SavedTownOf(g_names.count(id) ? g_names[id] : "", byTown, towns::Count()); t >= 0) { byTown[t] = id; g_townOf[id] = t; }
    for (int id : ids) {
        if (g_townOf.count(id)) continue;
        int t = UnclaimedTown(byTown, towns::Count());
        for (int k = 0; t < 0 && k < towns::Count(); k++) if (byTown[k] == -1) t = k;
        if (t < 0) break;
        byTown[t] = id;
        g_townOf[id] = t;
    }
    for (auto& [id, t] : g_townOf)
        Log("net: town %d -> %s%s", t, g_names.count(id) ? g_names[id].c_str() : "?",
            towns::TownPlayer(t).empty() ? "" : (towns::TownPlayer(t) == g_names[id] ? " (theirs in the save)" : " (was someone else's)"));
    PublishTownPlayers();
    towns::SetLocal(g_townOf.count(g_me) ? g_townOf[g_me] : 0);
    g_separateSession = true;
    if (g_mode == kHost) g_separate = true;  // later resyncs keep everyone's town
    Log("net: the save has %d towns: player towns assigned by id, mine is %d", towns::Count(), towns::Local());
}

// ---- status texts the players see (top bar)
static std::string PlayingStatus() {
    return (g_mode == kHost ? "hosting - " : "playing - ") + std::to_string(g_players) + " players";
}
static std::string ResyncText(const std::string& why) {
    const std::string joined = " joined", asked = " asked for a resync";
    auto ends = [&](const std::string& s) { return why.size() > s.size() && why.compare(why.size() - s.size(), s.size(), s) == 0; };
    if (why == "towns went out of sync") return "The games drifted apart - reloading the host's game to fix it...";
    if (ends(joined)) return why + " - reloading the game so they can play...";
    if (ends(" reconnected")) return why + " - reloading the game so they're back in sync...";
    if (ends(asked)) return why.substr(0, why.size() - asked.size()) + " asked for a resync - reloading the host's game...";
    if (why == "requested by the host") return "The host asked for a resync - reloading the host's game...";
    return "Reloading the host's game (" + why + ")...";
}
// The lockstep gate holds the game while a player's turns are late: after 2 s say whom we are waiting for.
static void WaitingNotice() {
    static uint64_t lastTick = UINT64_MAX, lastMove;
    static bool shown;
    uint64_t now = GetTickCount64(), tick = ProbeTick();
    bool held = g_sessionInGame && g_resyncAt < 0 && !g_netPaused && TicksAllowed(tick) <= 0;
    if (tick != lastTick || !held) {
        lastTick = tick;
        lastMove = now;
        if (shown) { shown = false; g_status = PlayingStatus(); }
        return;
    }
    if (now - lastMove < 2000) return;
    int slow = -1;
    int64_t lo = INT64_MAX;
    for (auto& [p, t] : g_turns) if (p != g_me && t < lo) { lo = t; slow = p; }
    for (auto& [id, name] : g_names) if (id != g_me && !g_turns.count(id)) slow = id;  // nothing from them at all yet
    if (slow < 0) return;
    std::string who = g_names.count(slow) ? g_names[slow] : "player " + std::to_string(slow + 1);
    g_status = "Waiting for " + who + "... (" + std::to_string((now - lastMove) / 1000) + " s)";
    if (!shown) Log("net: %s", g_status.c_str());
    shown = true;
}

void Frame() {
    static bool steamTried;
    if (!steamTried && gameload::InMenu()) {
        steamTried = true;
        if (steam::Init()) {
            // launched by Steam from an invite: "+connect_lobby <id>"
            const wchar_t* cl = wcsstr(GetCommandLineW(), L"+connect_lobby");
            if (cl) JoinSteamLobby(_wcstoui64(cl + 14, nullptr, 10));
        }
    }
    static bool optsLogged;
    if (!optsLogged && gameload::InMenu()) {
        optsLogged = true;
        auto o = gameload::GetNewGameOptions();
        auto join = [](const std::vector<std::string>& v) { std::string s; for (auto& x : v) s += "'" + x + "' "; return s; };
        Log("net: new-game options: starts %s| terrains %s| climates %s| sizes %s", join(o.starts).c_str(),
            join(o.terrains).c_str(), join(o.climates).c_str(), join(o.sizes).c_str());
    }
    steam::RunCallbacks();
    PollSteam();
    static ULONGLONG presenceAt;  // Steam rich presence: who you play with (friends list "View game info", grouping, Join Game)
    if (steamTried && GetTickCount64() - presenceAt > 1000) {
        presenceAt = GetTickCount64();
        int n = std::max<int>({(int)g_names.size(), g_players, 1}), friends = n - 1;
        std::string status, group, connect;
        if (g_active) {
            std::string with = " (" + std::to_string(n) + " players)";
            status = !g_sessionInGame ? "Multiplayer lobby" + (friends ? with : std::string())
                     : friends ? "Multiplayer" + with : "Multiplayer, waiting for players";
            if (steam::Lobby()) {
                group = std::to_string(steam::Lobby());
                connect = "+connect_lobby " + group;
            } else if (!g_room.empty()) {
                group = "room-" + g_room;
            }
        }
        steam::SetPresence(status, group, n, connect);
    }
    static ULONGLONG pingAt;  // guest: "still here" for the host (tells a reconnect from a namesake, HostReceive HELLO)
    if ((g_mode == kClientTcp || g_mode == kClientSteam) && g_me > 0 && GetTickCount64() - pingAt > 2000) {
        pingAt = GetTickCount64();
        SendLine("PING");
    }
    static int modSent = kModOk;  // guest: the companion mod state the host knows (HELLO carries the one at join time)
    if ((g_mode == kClientTcp || g_mode == kClientSteam) && g_me > 0 && CompanionModKnown() != modSent) {
        modSent = CompanionModKnown();
        SendLine("MOD " + std::to_string(modSent));
    }
    Hotkeys();
    UiActions();
    if (!g_leaveNote.empty()) {
        std::string note = g_leaveNote;
        g_leaveNote.clear();
        if (g_mode != kOff) DoLeave();
        g_status = note;
    }
    if (g_switchLobby) {
        uint64_t lobby = g_switchLobby;
        g_switchLobby = 0;
        if (g_mode != kOff) DoLeave();
        JoinSteamLobby(lobby);
    }
    if (g_mode == kHost) {
        std::deque<RelayIn> in;
        {
            std::lock_guard<std::mutex> lk(g_relayMx);
            in.swap(g_relayIn);
        }
        for (auto& r : in) {
            if (r.kind == kRelay && r.key == 0) {  // from the relay server itself
                if (r.line.compare(0, 9, "RELAY OK ") == 0) {
                    g_relayUp = true;
                    Log("net(host): relay room %s is open", g_room.c_str());
                    HostStatus();
                } else if (r.line == "RELAY LOST") {  // the relay thread is reconnecting; guests rejoin by themselves
                    g_relayUp = false;
                    for (auto it = g_peers.begin(); it != g_peers.end();) {
                        int id = it->first;
                        bool relayPeer = it->second.kind == kRelay;
                        ++it;
                        if (relayPeer) RemovePeer(id, "the relay connection was lost");
                    }
                    g_status = "lost the connection to the relay - reconnecting...";
                    Log("!! net(host): relay connection lost - reconnecting");
                } else if (r.line.compare(0, 10, "RELAY ERR ") == 0) {
                    g_relayUp = false;
                    g_status = r.line.substr(10);
                    Log("!! net(host): relay: %s", g_status.c_str());
                }
                continue;
            }
            if (r.closed) { int id = FindPeer(r.kind, r.key); if (id > 0) RemovePeer(id, "disconnected"); }
            else HostReceive(r.kind, r.key, r.line);
        }
        if (!g_hostStarted && g_autoStart && (int)g_peers.size() >= g_autoStart) HostStart();
        HostResyncFrame();
        static int testEpoch;  // a later test resync waits for the session after the previous one
        if (g_testResyncAt >= 0 && g_sessionInGame && g_epoch >= testEpoch && (int64_t)ProbeTick() >= g_testResyncAt) {
            g_testResyncAt = -1;
            if (!g_testResyncMore.empty()) { g_testResyncAt = g_testResyncMore.front(); g_testResyncMore.pop_front(); }
            testEpoch = g_epoch + 1;
            HostResync("test");
        }
        if (!g_hostStarted && steam::Lobby()) HostStatus();
    }
    if (!g_active) { UpdateTitle(); return; }
    // Banished pauses (and shows its pause screen) when its window loses focus; in lockstep that would freeze every
    // player whenever one alt-tabs. Keep the application "active" for the whole session.
    if (g_started) {
        using AppGet_t = uint8_t*(__fastcall*)();
        static AppGet_t appGet = (AppGet_t)RuntimeExport("?Get@Application@1@SAPEAV11@XZ");
        if (uint8_t* app = appGet ? appGet() : nullptr) app[0x11] = 1;
    }

    AssignSavedTowns();
    if (g_testDumpAt >= 0 && g_sessionInGame && (int64_t)ProbeTick() >= g_testDumpAt) { g_testDumpAt = -1; DumpDraws("test"); }
    if (g_testStallAt >= 0 && g_sessionInGame && (int64_t)ProbeTick() >= g_testStallAt) {
        g_testStallAt = -1;
        Log("net: test: freezing for 6 s at tick %llu", ProbeTick());
        Sleep(6000);
    }
    if (g_testDropAt >= 0 && g_mode == kClientTcp && g_sessionInGame && (int64_t)ProbeTick() >= g_testDropAt &&
        g_sock != INVALID_SOCKET) {
        g_testDropAt = -1;
        Log("net: test: dropping the connection at tick %llu", ProbeTick());
        shutdown(g_sock, SD_BOTH);
    }
    std::deque<std::string> lines;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        lines.swap(g_inbox);
    }
    for (auto& l : lines) {
        int p = 0, n = 0;
        unsigned long long a = 0, b = 0, c = 0;
        if (sscanf_s(l.c_str(), "WELCOME %d %d", &g_me, &g_players) == 2) {
            Log("net: I am player %d of %d", g_me, g_players);
        } else if (l.compare(0, 5, "START") == 0) {
            int epoch = 0;
            sscanf_s(l.c_str(), "START %d %d", &n, &epoch);
            if (n > 0) g_players = n;
            towns::SetPlayers(g_players);
            bool first = !g_started;
            g_started = true;
            g_epoch = epoch;
            g_turns.clear();
            g_myHash.clear();
            g_myHeap.clear();
            g_heapDumped = false;
            g_sentTurn = -1;
            g_resyncAt = -1;
            g_startSessions = ProbeSessionCount();  // the sim stays frozen until the reloaded session runs
            commands::ClearScheduled();
            Log("net: START with %d players, epoch %d", g_players, epoch);
            size_t nw = l.find(" NEW "), tw = l.find(" TOWNS ");
            if (tw != std::string::npos) {  // separate towns: NEW = a new map, otherwise the (resync) save has the towns
                g_loadNewArgs = nw != std::string::npos ? l.substr(nw + 5, tw - nw - 5) : "";
                g_separateSession = true;
                g_townOf.clear();
                int idx = 0;
                bool pick = l.find(" PICK") != std::string::npos;
                std::string ids = l.substr(tw + 7);
                if (size_t pk = ids.find(' '); pk != std::string::npos) ids.resize(pk);
                for (size_t s = 0; s < ids.size();) {
                    size_t e = ids.find(',', s);
                    if (e == std::string::npos) e = ids.size();
                    g_townOf[atoi(ids.c_str() + s)] = idx++;
                    s = e + 1;
                }
                towns::Configure(idx, g_townOf.count(g_me) ? g_townOf[g_me] : 0, pick);
                PublishTownPlayers();
                if (!g_loadNewArgs.empty()) g_saveName.clear();
                Log("net: separate towns: %s, %d towns, mine is %d",
                    g_loadNewArgs.empty() ? "loading the shared save" : ("new map " + g_loadNewArgs).c_str(), idx, towns::Local());
            } else if (nw != std::string::npos) {  // one shared town on a new map ("START n 0 NEW args")
                g_loadNewArgs = l.substr(nw + 5);
                if (size_t sp = g_loadNewArgs.find(' '); sp != std::string::npos) g_loadNewArgs.resize(sp);
                g_separateSession = false;
                g_townOf.clear();
                towns::Configure(1, 0, false);
                g_saveName.clear();
                Log("net: shared town on a new map %s", g_loadNewArgs.c_str());
            } else {
                g_loadNewArgs.clear();  // a save (first start or resync): never a leftover new map
            }
            if (epoch > 0) SendLine("SYNCED " + std::to_string(epoch));
            if (first) commands::FlushNetOutbox();
            if (!autotest::Active() || epoch > 0 || !g_loadNewArgs.empty()) g_loadPending = true;
            g_sessionInGame = false;
            g_status = epoch > 0 ? "reloading the shared game..." : "loading the shared save...";
        } else if (l.compare(0, 7, "STATUS ") == 0) {  // from our own connection thread
            g_status = l.substr(7);
        } else if (l.compare(0, 9, "RELAY OK ") == 0) {
            g_status = "in room " + l.substr(9) + " - waiting for the host";
        } else if (l.compare(0, 10, "RELAY ERR ") == 0) {
            Log("!! net: relay: %s", l.c_str() + 10);
            if (g_me > 0) g_status = "waiting for the host's game to come back (up to a minute)...";  // we were in it: a reconnect
            else if (g_status.find("reconnecting") == std::string::npos) g_status = l.substr(10);
        } else if (sscanf_s(l.c_str(), "SYNCED %d %d", &p, &n) == 2) {
            g_peerEpoch[p] = n;
        } else if (sscanf_s(l.c_str(), "RESYNC %llu", &a) == 1) {
            g_resyncAt = (int64_t)a;
            size_t sp = l.find(' ', 7);
            std::string why = sp == std::string::npos ? "" : l.substr(sp + 1);
            Log("net: resync at tick %llu (%s)", a, why.c_str());
            g_status = ResyncText(why);
            g_wasResync = true;
        } else if (l.compare(0, 6, "TRADE ") == 0) {
            // TRADE <from> OFFER <id> <to> <give> <nGive> <ask> <nAsk> | TRADE <from> ANSWER <id> <to> <0|1>
            char verb[16], give[512], ask[512];
            int from = 0, id = 0, to = 0, ng = 0, na = 0, yes = 0, period = 0;
            if (sscanf_s(l.c_str(), "TRADE %d %15s %d %d %511s %d %511s %d %d", &from, verb, (unsigned)sizeof verb, &id, &to, give,
                         (unsigned)sizeof give, &ng, ask, (unsigned)sizeof ask, &na, &period) >= 8 && !strcmp(verb, "OFFER")) {
                if (to == g_me) {
                    g_offers.push_back({id, from, to, give, ask, ng, na, period == 1 || period == 2 ? period : 0});
                    g_tradeNews = (g_names.count(from) ? g_names[from] : "a player") + " sent you a trade offer";
                }
            } else if (sscanf_s(l.c_str(), "TRADE %d %15s %d %d %d", &from, verb, (unsigned)sizeof verb, &id, &to, &yes) == 5 &&
                       !strcmp(verb, "ANSWER") && to == g_me) {
                g_tradeNews = (g_names.count(from) ? g_names[from] : "The other player") +
                              (yes ? " accepted your trade" : " declined your trade");
            }
        } else if (l.compare(0, 6, "NAMES ") == 0) {
            g_names.clear();
            size_t s = 6;
            while (s < l.size()) {
                size_t e = l.find(';', s);
                if (e == std::string::npos) e = l.size();
                std::string item = l.substr(s, e - s);
                size_t eq = item.find('=');
                if (eq != std::string::npos) g_names[atoi(item.c_str())] = item.substr(eq + 1);
                s = e + 1;
            }
        } else if (l.compare(0, 4, "MSG ") == 0) {
            g_status = l.substr(4);
            Log("net: %s", g_status.c_str());
        } else if (l.compare(0, 5, "NOTE ") == 0) {  // host only (Push): a 20 s notice over the status
            g_notice = l.substr(5);
            g_noticeUntil = GetTickCount() + 20000;
            Log("net: notice: %s", g_notice.c_str());
        } else if (l.compare(0, 11, "BADVERSION ") == 0) {  // BADVERSION <host's build id> <mine as the host saw it>
            std::string hostId = l.substr(11, l.find(' ', 11) - 11);
            Log("!! net: the host runs mod %s, I run %s - refused", hostId.c_str(), BuildId().c_str());
            DoLeave();
            g_status = "different mod version: the host has " + hostId + ", you have " + BuildId() +
                       " - you both need the same dinput8.dll";
        } else if (l == "FULL") {
            Log("!! net: the host's game has already started");
            g_status = "that game has already started";
        } else if (sscanf_s(l.c_str(), "LEFT %d", &p) == 1) {
            g_turns.erase(p);
            g_peerEpoch.erase(p);
            g_players--;
            Log("!! net: player %d left the game", p);
            if (g_mode == kHost) HostStatus();
            else g_status = "a player left";
        } else if (l.compare(0, 5, "FILE ") == 0) {
            char name[64];
            if (sscanf_s(l.c_str(), "FILE %63s %llu %llx", name, (unsigned)sizeof name, &a, &c) == 3) {
                g_fileName = g_recvName.empty() ? name : g_recvName;
                g_fileSize = (size_t)a;
                g_fileHash = c;
                g_fileBytes.clear();
                g_fileBytes.reserve(g_fileSize);
                g_status = "receiving the host's save...";
            }
        } else if (l.compare(0, 5, "DATA ") == 0) {
            B64Decode(l.substr(5), g_fileBytes);
        } else if (l == "FILEEND") {
            FileDone();
        } else if (sscanf_s(l.c_str(), "CTL %d speed %llu", &p, &a) == 2) {
            g_netSpeed = (int)a;
        } else if (sscanf_s(l.c_str(), "CTL %d pause %llu", &p, &a) == 2) {
            g_netPaused = (int)a;
            Log("net: player %d %s the game", p, a ? "paused" : "unpaused");
        } else if (sscanf_s(l.c_str(), "TURN %d %llu", &p, &a) == 2) {
            if (g_peerEpoch[p] == g_epoch) g_turns[p] = (int64_t)a;
        } else if (sscanf_s(l.c_str(), "CMD %d %llu %llu", &p, &a, &b) == 3) {
            if (g_peerEpoch[p] != g_epoch) continue;  // a command for the session that was just replaced
            // "CMD <player> <seq> <slot> <text>" -> a replay line "<slot> <text>", applied in relay order
            size_t s1 = l.find(' ', 4), s2 = l.find(' ', s1 + 1);
            commands::ScheduleLine(l.substr(s2 + 1).c_str(), -1 - p);  // the player's town is looked up when it runs
            g_cmdsIn++;
        } else if (sscanf_s(l.c_str(), "HEAP %d %llu %llx", &p, &a, &c) == 3) {
            if (p != g_me && g_peerEpoch[p] == g_epoch) {
                auto it = g_myHeap.find(a);
                if (it != g_myHeap.end() && it->second != c) HeapDrift(a, ("player " + std::to_string(p)).c_str());
            }
        } else if (sscanf_s(l.c_str(), "HASH %d %llu %llx", &p, &a, &c) == 3) {
            if (p != g_me && g_peerEpoch[p] == g_epoch) {
                auto it = g_myHash.find(a);
                if (it != g_myHash.end() && it->second != c) {
                    g_desyncs++;
                    Log("!! net: DESYNC at tick %llu: mine %016llx, player %d %016llx", a, it->second, p, c);
                    DumpDraws("DESYNC at tick " + std::to_string(a));
                    g_status = "Out of sync - the host reloads the game to fix it...";
                }
            }
        }
    }

    // no harness: load the shared save from the main menu once the host started the game
    if (g_loadPending && !g_loadNewArgs.empty()) {
        unsigned seed = 1;
        int size = 0, start = 0, terrain = 0, climate = 0;
        sscanf_s(g_loadNewArgs.c_str(), "%u,%d,%d,%d,%d", &seed, &size, &start, &terrain, &climate);
        if (gameload::RequestNewGame(seed, size, start, terrain, climate, false)) g_loadPending = false;
    } else if (g_loadPending && !g_saveName.empty() && gameload::Request(g_saveName)) g_loadPending = false;
    if (g_started && !g_sessionInGame && ProbeGameLoop() && ProbeSessionCount() != g_startSessions) {
        g_sessionInGame = true;
        g_sharedSession = ProbeSessionCount();
        PublishTownPlayers();  // loading replaced the remembered names with the save's: add who plays which town now
        if (g_wasResync) {  // shown for a few seconds, then the normal status
            g_status = "Back in sync";
            g_statusExpires = GetTickCount64() + 4000;
            g_wasResync = false;
        } else {
            g_status = PlayingStatus();
        }
    }
    if (g_statusExpires && GetTickCount64() > g_statusExpires) { g_statusExpires = 0; g_status = PlayingStatus(); }
    // A game the player loaded himself (the game's own Load menu) during a session: only this PC has it. The host's
    // load becomes everybody's game (saved at once and sent like a resync); a guest leaves the session.
    if (g_sessionInGame && ProbeGameLoop() && !gameload::InMenu() && ProbeSessionCount() != g_sharedSession &&
        g_resyncAt < 0 && !g_loadPending) {
        g_sharedSession = ProbeSessionCount();
        if (g_mode == kHost) {
            Log("net(host): the host loaded a saved game - sending it to everybody");
            HostResync("the host loaded a saved game", true);
        } else {
            Log("!! net: this player loaded another game - leaving the session");
            DoLeave();
            g_status = "you loaded another game, so you left the multiplayer game";
        }
    }
    WaitingNotice();
    if (g_sessionInGame && gameload::InMenu()) {
        ResetSession();
        g_status = "session over";
    }
    UpdateTitle();
}

void DebugStatus(uint64_t tick, bool uiPaused, int speed) {
    static int frames;
    if (!g_active || ++frames % 120) return;
    char buf[256];
    int n = sprintf_s(buf, "net: status tick=%llu started=%d me=%d players=%d uiPaused=%d speed=%d turns:", tick,
                      g_started, g_me, g_players, uiPaused, speed);
    for (auto& [p, t] : g_turns) n += sprintf_s(buf + n, sizeof buf - n, " p%d=%lld", p, t);
    Log("%s", buf);
}

void SyncSpeed(uint8_t* speedCtl) {
    if (!g_active || !g_started || !speedCtl) return;
    int* speed = (int*)(speedCtl + 0x34);
    uint8_t* paused = speedCtl + 0x30;
    if (g_netSpeed < 0) {  // first in-game frame: the host announces its speed, everyone else adopts it
        g_netSpeed = *speed;
        if (g_me == 0) {
            char line[32];
            sprintf_s(line, "CTL speed %d", *speed);
            SendLine(line);
        }
    }
    // A local change = the value differs from what we wrote last frame (speed buttons/keys, the pause toggle,
    // autopause). Our own overwrite with the network value is not a change - comparing against the network
    // value instead made host and relay echo ping-pong the pause flag every frame.
    if (g_wroteSpeed >= 0 && *speed != g_wroteSpeed && *speed != g_netSpeed) {
        char line[32];
        sprintf_s(line, "CTL speed %d", *speed);
        SendLine(line);
        Log("net: local speed change %d -> %d", g_wroteSpeed, *speed);
    }
    if (g_wrotePaused >= 0 && (int)*paused != g_wrotePaused && (int)*paused != g_netPaused) {
        char line[32];
        sprintf_s(line, "CTL pause %d", (int)*paused);
        SendLine(line);
    }
    *speed = g_wroteSpeed = g_netSpeed;  // the UI shows the shared state; local changes take effect when echoed back
    *paused = (uint8_t)g_netPaused;
    g_wrotePaused = g_netPaused;
}

int64_t TicksAllowed(uint64_t tick) {
    if (!g_active || !g_started) return INT64_MAX;  // before START the running game (if any) isn't a shared one
    if (g_netPaused || ProbeSessionCount() == g_startSessions) return 0;
    if ((int)g_turns.size() < g_players) return 0;
    int64_t lo = INT64_MAX;
    for (auto& [p, t] : g_turns) lo = t < lo ? t : lo;
    if (g_resyncAt >= 0 && g_resyncAt < lo) lo = g_resyncAt;  // stop exactly at the resync tick
    return lo - (int64_t)tick;
}

void AfterFrame(uint64_t tick) {
    if (!g_active || !g_started || ProbeSessionCount() == g_startSessions) return;  // old session: no promises
    // any command issued from now on gets slot >= tick + delay
    int64_t turn = std::max<int64_t>((int64_t)(tick + Delay()) - 1, g_sentTurn);  // never take a promise back
    if (turn != g_sentTurn) {
        char line[48];
        sprintf_s(line, "TURN %lld", turn);
        SendLine(line);
        g_sentTurn = turn;
    }
}

void OnTickHash(uint64_t tick, uint64_t hash, uint64_t heap) {
    if (!g_active) return;
    g_myHash[tick] = hash;
    if (g_myHash.size() > 64) g_myHash.erase(g_myHash.begin());
    g_myHeap[tick] = heap;
    if (g_myHeap.size() > 64) g_myHeap.erase(g_myHeap.begin());
    char line[64];
    sprintf_s(line, "HASH %llu %016llx", tick, hash);
    SendLine(line);
    sprintf_s(line, "HEAP %llu %016llx", tick, heap);
    SendLine(line);
    if (tick % 6000 == 0) Log("net: tick %llu, %llu remote commands so far, %llu desyncs", tick, g_cmdsIn, g_desyncs);
}

std::vector<TradeOffer> IncomingOffers() { return g_offers; }
int TownOfPlayer(int p) {
    AssignSavedTowns();
    if (g_testDumpAt >= 0 && g_sessionInGame && (int64_t)ProbeTick() >= g_testDumpAt) { g_testDumpAt = -1; DumpDraws("test"); }
    if (g_testStallAt >= 0 && g_sessionInGame && (int64_t)ProbeTick() >= g_testStallAt) {
        g_testStallAt = -1;
        Log("net: test: freezing for 6 s at tick %llu", ProbeTick());
        Sleep(6000);
    }
    if (g_testDropAt >= 0 && g_mode == kClientTcp && g_sessionInGame && (int64_t)ProbeTick() >= g_testDropAt &&
        g_sock != INVALID_SOCKET) {
        g_testDropAt = -1;
        Log("net: test: dropping the connection at tick %llu", ProbeTick());
        shutdown(g_sock, SD_BOTH);
    }
    return towns::Count() > 1 && g_townOf.count(p) ? g_townOf[p] : 0;
}
std::string TradeNews() { return g_tradeNews; }
void SetTradeNews(const std::string& s) { g_tradeNews = s; }

void SendOffer(int to, const std::string& give, int nGive, const std::string& ask, int nAsk, int period) {
    char line[1200];
    sprintf_s(line, "TRADE OFFER %d %d %s %d %s %d %d", ++g_offerSeq * 100 + g_me, to, give.empty() ? "-" : give.c_str(), nGive,
              ask.empty() ? "-" : ask.c_str(), nAsk, period);
    SendLine(line);
    g_tradeNews = "offer sent";
}

void AnswerOffer(int id, bool accept) {
    for (size_t i = 0; i < g_offers.size(); i++) {
        TradeOffer o = g_offers[i];
        if (o.id != id) continue;
        g_offers.erase(g_offers.begin() + i);
        if (accept && o.period) commands::RequestAgreement(TownOfPlayer(o.from), o.give, o.nGive, o.ask, o.nAsk, o.period);
        else if (accept) commands::RequestTrade(TownOfPlayer(o.from), o.give, o.nGive, o.ask, o.nAsk);
        char line[96];
        sprintf_s(line, "TRADE ANSWER %d %d %d", id, o.from, accept ? 1 : 0);
        SendLine(line);
        g_tradeNews = accept ? "trade accepted - goods move at the next tick" : "trade declined";
        return;
    }
}

UiInfo GetUiInfo() {
    UiInfo u{};
    u.active = g_active;
    u.hosting = g_mode == kHost;
    u.started = g_mode == kHost ? g_hostStarted : g_started;
    u.inGame = ProbeGameLoop() != nullptr && !gameload::InMenu();
    u.inMenu = gameload::InMenu();
    u.steamOk = steam::Init();
    u.lobby = steam::Lobby() != 0;
    u.resyncing = g_resyncAt >= 0 || (g_started && ProbeSessionCount() == g_startSessions) || g_waitReady;
    u.me = g_me;
    u.players = g_players;
    u.status = GetTickCount() < g_noticeUntil ? g_notice : g_status;
    if (!u.status.empty() && u.status[0] >= 'a' && u.status[0] <= 'z') u.status[0] -= 32;  // sentence case on screen
    u.version = BuildId();
    u.hostSave = g_hostSave;
    u.tick = ProbeTick();
    u.desyncs = g_desyncs;
    u.speed = g_netSpeed;
    u.paused = g_netPaused != 0;
    u.separate = g_mode == kHost ? g_separate : g_separateSession;
    u.town = towns::Local();
    u.towns = towns::Count();
    u.hostVia = g_hostVia;
    u.room = g_room;
    u.relayUp = g_relayUp;
    u.listenPort = g_hostPort;
    if (g_mode == kHost) {
        for (auto& [id, p] : g_peers) u.roster.push_back({id, p.name, p.ready, p.epoch == g_hostEpoch});
    } else {
        for (auto& [id, n] : g_names) u.roster.push_back({id, n, true, g_peerEpoch.count(id) ? g_peerEpoch[id] == g_epoch : g_epoch == 0});
    }
    return u;
}

std::vector<std::string> UiSaves() { return ListSaves(); }

// ---- connection settings: <game>\bmp_settings.txt (key=value)
static std::wstring SettingsPath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wcscpy_s(wcsrchr(path, L'\\') + 1, 64, L"bmp_settings.txt");
    return path;
}

static void LoadSettings() {
    FILE* f = nullptr;
    _wfopen_s(&f, SettingsPath().c_str(), L"r");
    if (!gamebuild::HasSteam()) g_method = kViaRelay;  // GOG: no Steam
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char key[64], val[400] = "";
        if (sscanf_s(line, " %63[^= ] = %399[^\r\n]", key, (unsigned)sizeof key, val, (unsigned)sizeof val) < 1) continue;
        std::string k = key, v = val;
        if (k == "connection") g_method = v == "direct" ? kViaDirect : v == "relay" ? kViaRelay : kViaSteam;
        else if (k == "port") g_hostPort = atoi(v.c_str()) > 0 ? atoi(v.c_str()) : 7777;
        else if (k == "relay") g_relayAddr = v;
        else if (k == "name") g_playerName = v;
        else if (k == "latency") g_latencyMs = std::clamp(atoi(v.c_str()), 30, 1000);
    }
    fclose(f);
    if (g_method == kViaSteam && !gamebuild::HasSteam()) g_method = kViaRelay;  // GOG: no Steam
}

Settings GetSettings() { return Settings{g_method, g_hostPort, g_relayAddr, g_playerName}; }

void SetSettings(const Settings& s) {
    g_method = s.method < kViaSteam || s.method > kViaRelay ? kViaSteam : s.method;
    g_hostPort = s.port > 0 && s.port < 65536 ? s.port : 7777;
    g_relayAddr = s.relay;
    g_playerName = s.name;
    FILE* f = nullptr;
    _wfopen_s(&f, SettingsPath().c_str(), L"w");
    if (!f) return;
    fprintf(f, "connection=%s\nport=%d\nrelay=%s\nname=%s\nlatency=%d\n", g_method == kViaDirect ? "direct" : g_method == kViaRelay ? "relay" : "steam",
            g_hostPort, g_relayAddr.c_str(), g_playerName.c_str(), g_latencyMs);
    fclose(f);
}

void UiDo(UiAction a, const std::string& arg) {
    std::lock_guard<std::mutex> lk(g_uiMx);
    g_uiActions.push_back({(int)a, arg});
}

}  // namespace net
