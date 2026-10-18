// See report.h.
#include "report.h"
#include "common.h"
#include "gamedlg.h"
#include "gameui.h"
#include "net.h"
#include "overlay.h"
#include "steam.h"
#include "zipwrite.h"
#include "autotest.h"
#include <shlobj.h>
#include <shellapi.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace report {

// Where Send goes. Overridable for tests / a moved server: <game>\bmp_settings.txt "bugurl=https://host/report".
static const wchar_t* kDefaultUrl = L"https://bugs.tupakkaalapsille.store/report";

static std::wstring GamePath(const wchar_t* name) {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    wcscpy_s(wcsrchr(p, L'\\') + 1, MAX_PATH - (wcsrchr(p, L'\\') + 1 - p), name);
    return p;
}
static std::wstring Suffixed(const wchar_t* stem, const wchar_t* ext) {
    return GamePath((std::wstring(stem) + SlotSuffix() + ext).c_str());
}
static std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}
static uint64_t Fnv(const std::string& s) {
    uint64_t h = 1469598103934665603ull;
    for (unsigned char c : s) h = (h ^ c) * 1099511628211ull;
    return h;
}
static std::string Hex(uint64_t v, int digits) {
    char b[24];
    sprintf_s(b, "%016llx", v);
    return std::string(b + 16 - digits);
}

// The last `max` bytes of a file (from a line start), even while another handle writes it.
static bool ReadTail(const std::wstring& path, size_t max, std::string& out) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    int64_t from = size.QuadPart > (int64_t)max ? size.QuadPart - (int64_t)max : 0;
    LARGE_INTEGER pos;
    pos.QuadPart = from;
    SetFilePointerEx(h, pos, nullptr, FILE_BEGIN);
    out.resize((size_t)(size.QuadPart - from));
    DWORD got = 0, total = 0;
    while (total < out.size() && ReadFile(h, out.data() + total, (DWORD)std::min<size_t>(out.size() - total, 1 << 20), &got, nullptr) && got)
        total += got;
    CloseHandle(h);
    out.resize(total);
    if (from > 0) {
        size_t nl = out.find('\n');
        out = "[... " + std::to_string(from) + " earlier bytes left out ...]\n" + (nl == std::string::npos ? out : out.substr(nl + 1));
    }
    return true;
}
static FILETIME WriteTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a);
    return a.ftLastWriteTime;
}
static double AgeHours(const FILETIME& ft) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    uint64_t a = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime, b = ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
    return a ? (double)(b - a) / 36e9 : 1e9;
}

// ---------------------------------------------------------------- scrubbing
struct Scrubber {
    std::vector<std::pair<std::string, std::string>> names;  // longest first
    std::string user;                                      // Windows user name

    void AddName(std::string n) {
        while (!n.empty() && (n.back() == ' ' || n.back() == '.')) n.pop_back();
        if (n.size() < 3 || n.find_first_not_of("0123456789") == std::string::npos) return;
        for (auto& p : names) if (p.first == n) return;
        names.push_back({n, "player-" + Hex(Fnv("bmp-name:" + n), 4)});
    }
    // names the log mentions: "I am <id> (Name)", "player 1 joined (Name)", "Waiting for Name...", "town 1 was Name's"
    void NamesFromLog(const std::string& t) {
        auto grab = [&](const char* before, char endChar, const char* endStr) {
            size_t i = 0;
            while ((i = t.find(before, i)) != std::string::npos) {
                i += strlen(before);
                size_t e = endStr ? t.find(endStr, i) : t.find(endChar, i);
                size_t nl = t.find('\n', i);
                if (e != std::string::npos && e < nl && e - i < 64) AddName(t.substr(i, e - i));
            }
        };
        size_t i = 0;  // "I am 7656... (Name)"
        while ((i = t.find("I am ", i)) != std::string::npos) {
            i += 5;
            size_t open = t.find(" (", i), close = t.find(')', i), nl = t.find('\n', i);
            if (open != std::string::npos && close != std::string::npos && open < close && close < nl && close - open < 64)
                AddName(t.substr(open + 2, close - open - 2));
        }
        grab(" joined (", ')', nullptr);
        grab("Waiting for ", 0, "...");
        grab(" was ", 0, "'s");
        grab(" -> ", 0, " (theirs");
    }
    static bool Digit(char c) { return c >= '0' && c <= '9'; }
    static bool Alnum(char c) { return Digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

    std::string Run(const std::string& in) const {
        std::string s = in;
        if (user.size() >= 2)  // the Windows user first (a player name can be part of it): paths and plain mentions
            for (size_t i = 0; (i = s.find(user, i)) != std::string::npos;) { s.replace(i, user.size(), "<user>"); i += 6; }
        for (auto& [n, r] : names) {  // player names
            for (size_t i = 0; (i = s.find(n, i)) != std::string::npos;) { s.replace(i, n.size(), r); i += r.size(); }
        }
        std::string o;
        o.reserve(s.size());
        for (size_t i = 0; i < s.size();) {
            char c = s[i];
            bool start = i == 0 || !(Alnum(s[i - 1]) || s[i - 1] == '.');
            if (Digit(c) && start) {
                size_t j = i;
                while (j < s.size() && Digit(s[j])) j++;
                size_t run = j - i;
                if (run >= 15 && (j >= s.size() || !Alnum(s[j]))) {  // Steam ids (7656119...), lobby ids
                    std::string d = s.substr(i, run);
                    o += d.compare(0, 7, "7656119") == 0 ? "steam-" + Hex(Fnv("bmp-steam:" + d), 6) : "<id>";
                    i = j;
                    continue;
                }
                // IPv4 a.b.c.d (127.0.0.1 kept)
                size_t k = i;
                int groups = 0;
                while (groups < 4) {
                    size_t g = k;
                    while (k < s.size() && Digit(s[k]) && k - g < 3) k++;
                    if (k == g) break;
                    groups++;
                    if (groups < 4) { if (k < s.size() && s[k] == '.') k++; else break; }
                }
                if (groups == 4 && (k >= s.size() || !(Alnum(s[k]) || s[k] == '.'))) {
                    std::string ip = s.substr(i, k - i);
                    o += ip == "127.0.0.1" ? ip : "<ip>";
                    i = k;
                    continue;
                }
            }
            o += c;
            i++;
        }
        // room codes and join addresses: the word after these markers
        const char* markers[] = {"room ", "room-", "Room ", "RELAY HOST ", "RELAY JOIN ", "joining ", "connecting to ", "relay=", "relayhost="};
        for (const char* m : markers) {
            size_t ml = strlen(m);
            for (size_t i = 0; (i = o.find(m, i)) != std::string::npos;) {
                size_t a = i + ml, b = a;
                while (b < o.size() && (Alnum(o[b]) || o[b] == '.' || o[b] == ':' || o[b] == '-' || o[b] == '_')) b++;
                std::string word = o.substr(a, b - a);
                bool keep = word.empty() || word == "Steam" || word == "the" ||
                            word == "127.0.0.1" || word.compare(0, 10, "127.0.0.1:") == 0 || word == "<ip>" || word == "<id>";
                if (!keep) o.replace(a, b - a, "<hidden>");
                i = a + 1;
            }
        }
        return o;
    }
};

// ---------------------------------------------------------------- building
struct Job {
    bool crash = false, send = false;
    std::string description, manifestHead;
    Scrubber scrub;
};
static std::mutex g_mx;
static std::string g_status;
static std::atomic<bool> g_busy, g_sent;
static void SetStatus(const std::string& s) { std::lock_guard<std::mutex> l(g_mx); g_status = s; Log("report: %s", s.c_str()); }

static std::vector<uint8_t> Bytes(const std::string& s) { return std::vector<uint8_t>(s.begin(), s.end()); }

static std::vector<uint8_t> BuildZip(Job& job, std::string& listing) {
    std::vector<zipwrite::Entry> entries;
    auto addText = [&](const char* name, const std::wstring& path, size_t max) {
        std::string t;
        if (!ReadTail(path, max, t) || t.empty()) return;
        entries.push_back({name, Bytes(job.scrub.Run(t))});
        listing += std::string(name) + " (" + std::to_string(t.size() / 1024) + " KB)\n";
    };
    // names first, from every log that goes in (the scrubber must know them before any file is scrubbed)
    for (const wchar_t* stem : {L"bmp", L"bmp_prev"}) {
        std::string t;
        if (ReadTail(Suffixed(stem, L".log"), 3 << 20, t)) job.scrub.NamesFromLog(t);
    }
    addText("bmp.log", Suffixed(L"bmp", L".log"), 3 << 20);
    addText("bmp_prev.log", Suffixed(L"bmp_prev", L".log"), 3 << 20);
    addText("bmp_commands.log", Suffixed(L"bmp_commands", L".log"), 1 << 20);
    // the two newest desync / drift records of the last two days
    std::vector<std::pair<uint64_t, std::wstring>> desyncs;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(GamePath(L"bmp_desync_*.log").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            std::wstring n = fd.cFileName;
            bool mine = *SlotSuffix() ? n.find(SlotSuffix()) != std::wstring::npos : n.find(L"_p") == std::wstring::npos;
            if (mine && AgeHours(fd.ftLastWriteTime) < 48)
                desyncs.push_back({((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime, n});
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(desyncs.rbegin(), desyncs.rend());
    for (size_t i = 0; i < desyncs.size() && i < 2; i++)
        addText(Narrow(desyncs[i].second).c_str(), GamePath(desyncs[i].second.c_str()), 8 << 20);
    // crash dump: after a crash, or a recent one
    std::wstring dmp = GamePath(L"crash.dmp");
    if (GetFileAttributesW(dmp.c_str()) != INVALID_FILE_ATTRIBUTES && (job.crash || AgeHours(WriteTime(dmp)) < 24)) {
        std::string raw;
        if (ReadTail(dmp, 32 << 20, raw) && raw.compare(0, 4, "MDMP") == 0) {
            entries.push_back({"crash.dmp", Bytes(raw)});
            listing += "crash.dmp (" + std::to_string(raw.size() / 1024) + " KB, a snapshot of the game's memory at the crash)\n";
        }
    }
    std::string manifest = job.manifestHead + "files:\n" + listing + "\nwhat the player was doing:\n" + job.description + "\n";
    entries.insert(entries.begin(), {"report.txt", Bytes(manifest)});
    return zipwrite::Build(entries);
}

static std::wstring SaveToDesktop(const std::vector<uint8_t>& zip, std::wstring& name) {
    PWSTR desk = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk))) dir = desk;
    CoTaskMemFree(desk);
    if (dir.empty()) dir = GamePath(L"").substr(0, GamePath(L"").size() - 1);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t n[96];
    swprintf_s(n, L"BanishedMP-bug-%04d%02d%02d-%02d%02d%02d.zip", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    name = n;
    std::wstring path = dir + L"\\" + n;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"";
    DWORD w = 0;
    WriteFile(h, zip.data(), (DWORD)zip.size(), &w, nullptr);
    CloseHandle(h);
    return w == zip.size() ? path : L"";
}

static std::wstring ReportUrl() {
    FILE* f = nullptr;
    _wfopen_s(&f, GamePath(L"bmp_settings.txt").c_str(), L"r");
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f))
            if (strncmp(line, "bugurl=", 7) == 0) {
                std::string v = line + 7;
                while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) v.pop_back();
                fclose(f);
                return std::wstring(v.begin(), v.end());
            }
        fclose(f);
    }
    return kDefaultUrl;
}

// POST the zip; true + the server's report id on 200.
static bool Upload(const std::vector<uint8_t>& zip, const std::string& build, std::string& result) {
    std::wstring url = ReportUrl();
    URL_COMPONENTS u{sizeof u};
    wchar_t host[256] = L"", path[1024] = L"";
    u.lpszHostName = host; u.dwHostNameLength = 256;
    u.lpszUrlPath = path; u.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u)) { result = "bad report address"; return false; }
    HINTERNET s = WinHttpOpen(L"BanishedMP-bugreport", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) { result = "no internet access (WinHTTP)"; return false; }
    WinHttpSetTimeouts(s, 10000, 10000, 60000, 60000);
    HINTERNET c = WinHttpConnect(s, host, u.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"POST", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool ok = false;
    if (r) {
        std::wstring headers = L"Content-Type: application/zip\r\nX-BMP-Build: " + std::wstring(build.begin(), build.end());
        if (WinHttpSendRequest(r, headers.c_str(), (DWORD)-1, (void*)zip.data(), (DWORD)zip.size(), (DWORD)zip.size(), 0) &&
            WinHttpReceiveResponse(r, nullptr)) {
            DWORD code = 0, len = sizeof code;
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &len, nullptr);
            std::string body;
            DWORD avail = 0;
            while (WinHttpQueryDataAvailable(r, &avail) && avail && body.size() < 4096) {
                std::string chunk(avail, '\0');
                DWORD got = 0;
                if (!WinHttpReadData(r, chunk.data(), avail, &got)) break;
                body.append(chunk.data(), got);
            }
            ok = code == 200;
            if (ok) {  // {"id": "..."}
                size_t a = body.find("\"id\"");
                size_t q1 = a == std::string::npos ? a : body.find('"', a + 4);
                size_t q2 = q1 == std::string::npos ? q1 : body.find('"', q1 + 1);
                result = q2 == std::string::npos ? "" : body.substr(q1 + 1, q2 - q1 - 1);
            } else {
                result = "the server answered " + std::to_string(code);
            }
        } else {
            result = "couldn't reach the report server (error " + std::to_string(GetLastError()) + ")";
        }
    } else {
        result = "couldn't reach the report server";
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

static void Run(Job job, std::string build) {
    std::string listing;
    std::vector<uint8_t> zip = BuildZip(job, listing);
    Log("report: built %zu KB (%s)", zip.size() / 1024, job.send ? "send" : "save");
    if (job.send) {
        SetStatus("Sending the report (" + std::to_string(zip.size() / 1024) + " KB)...");
        std::string result;
        if (Upload(zip, build, result)) {
            g_sent = true;
            SetStatus("Sent - thank you!" + (result.empty() ? std::string() : " Report number: " + result));
            g_busy = false;
            return;
        }
        std::wstring name;
        std::wstring path = SaveToDesktop(zip, name);
        SetStatus("Couldn't send it (" + result + ")." + (path.empty() ? std::string(" Saving failed too.")
                                                                         : " Saved to your Desktop instead: " + Narrow(name)));
        g_busy = false;
        return;
    }
    std::wstring name;
    std::wstring path = SaveToDesktop(zip, name);
    if (path.empty()) {
        SetStatus("Couldn't save the report to the Desktop.");
    } else {
        SetStatus("Saved to your Desktop: " + Narrow(name) + " - attach it to your bug report.");
        std::wstring args = L"/select,\"" + path + L"\"";
        if (!autotest::Active()) ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
    }
    g_busy = false;
}

static Job MakeJob(bool crash, bool send, const std::string& description) {
    Job j;
    j.crash = crash;
    j.send = send;
    j.description = description.empty() ? "(nothing written)" : description;
    net::UiInfo u = net::GetUiInfo();
    // the scrubber's names: everyone this PC knows of
    for (auto& p : u.roster) j.scrub.AddName(p.name);
    j.scrub.AddName(net::GetSettings().name);
    if (steam::Init()) j.scrub.AddName(steam::PersonaName());
    char user[256] = "";
    DWORD ul = sizeof user;
    if (GetUserNameA(user, &ul)) j.scrub.user = user;
    // the same session id on every player's report (match them up), from what all of them share
    std::string sessionKey = steam::Lobby() ? std::to_string(steam::Lobby()) : !u.room.empty() ? u.room : std::string();
    if (sessionKey.empty()) {
        std::vector<std::string> names;
        for (auto& p : u.roster) names.push_back(p.name);
        std::sort(names.begin(), names.end());
        for (auto& n : names) sessionKey += n + "|";
    }
    OSVERSIONINFOEXW os{sizeof os};
    using RtlGetVersion_t = LONG(WINAPI*)(OSVERSIONINFOEXW*);
    if (auto rgv = (RtlGetVersion_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) rgv(&os);
    SYSTEMTIME t;
    GetSystemTime(&t);
    char head[1024];
    sprintf_s(head,
              "Banished Multiplayer bug report\n"
              "kind: %s\ntime (UTC): %04d-%02d-%02d %02d:%02d:%02d\nmod build: %s\ngame build: %s\nsession: %s\n"
              "multiplayer: %s, %d players, %d towns, my town %d, %s\nconnection: %s\nwindows: %lu.%lu.%lu\ngraphics: %s\n\n",
              crash ? "crash" : "bug", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, u.version.c_str(), gamebuild::Name(),
              sessionKey.empty() ? "-" : Hex(Fnv("bmp-session:" + sessionKey), 8).c_str(),
              u.active ? (u.hosting ? "hosting" : "joined") : "not in a session", u.players, u.towns, u.town,
              u.inGame ? "in game" : u.inMenu ? "in the menu" : "loading", steam::Lobby() ? "Steam" : !u.room.empty() ? "relay" : u.active ? "direct" : "-",
              os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber, overlay::RenderInfo().c_str());
    j.manifestHead = head;
    return j;
}

// ---------------------------------------------------------------- the window
struct Window : gamedlg::Window {
    enum { kSave = 3600, kSend, kClose };
    bool crash = false;
    const char* Resource() const override { return "Dialog/BmpReport.rsc"; }
    void Init() override {
        Id("bmpReportSave", kSave);
        Id("bmpReportSend", kSend);
        Id("bmpReportClose", kClose);
        Text("bmpReportTitle", crash ? "The game crashed" : "Report a bug");
        Text("bmpReportIntro", crash ? "Sorry - Banished closed unexpectedly last time. A report helps us fix it. What were you doing when it happened?"
                                     : "What went wrong, and what were you doing at the time?");
        Text("bmpReportWhat",
             std::string("The report holds your mod logs, recent desync records") + (crash ? ", the crash dump" : "") +
                 " and your game and mod version. Player names, Steam ids, your Windows user name, IP addresses and room "
                 "codes are removed. Save puts it on your Desktop; Send uploads it to the mod's developers.");
        Text("bmpReportSave", "Save to Desktop");
        Text("bmpReportSend", "Send");
        Text("bmpReportClose", "Close");
    }
    void Refresh() override {
        std::string s;
        { std::lock_guard<std::mutex> l(g_mx); s = g_status; }
        Text("bmpReportStatus", s);
        Enable("bmpReportSave", !g_busy);
        Enable("bmpReportSend", !g_busy && !g_sent);
    }
    void OnEvent(int id, int evt) override;
};
static Window g_win;
static bool g_wanted;

static void Start(bool crash, bool send, const std::string& text) {
    if (g_busy) return;
    g_busy = true;
    SetStatus(send ? "Preparing the report..." : "Saving the report...");
    Job job = MakeJob(crash, send, text);
    std::thread(Run, std::move(job), net::GetUiInfo().version).detach();
}

void Window::OnEvent(int id, int evt) {
    if (evt != 0x12) return;
    if (id == kClose) { g_wanted = false; Close(); return; }
    if (id == kSave || id == kSend) Start(crash, id == kSend, gameui::EditGet(E("bmpReportEdit")));
}

void Test(bool send, const std::string& text) {  // uiscript "report save|send TEXT": the buttons' path, text typed for them
    if (g_win.IsOpen()) gameui::EditSet(g_win.E("bmpReportEdit"), text.c_str());
    Start(g_win.crash, send, text);
}
bool Busy() { return g_busy; }

void Open(bool crash) {
    if (g_win.IsOpen() && g_win.crash == crash) return;
    if (g_win.IsOpen()) g_win.Close();
    g_win.crash = crash;
    g_wanted = true;
    g_sent = false;
    { std::lock_guard<std::mutex> l(g_mx); g_status.clear(); }
    Log("report: window requested (%s)", crash ? "after a crash" : "bug");
}

// After a crash the game wrote crash.dmp: offer a report once per dump, at the main menu.
static void CrashPrompt() {
    static bool checked;
    if (checked) return;
    checked = true;
    std::wstring dmp = GamePath(L"crash.dmp");
    if (GetFileAttributesW(dmp.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    FILETIME ft = WriteTime(dmp);
    char stamp[32];
    sprintf_s(stamp, "%08lx%08lx", ft.dwHighDateTime, ft.dwLowDateTime);
    std::wstring seenPath = GamePath(L"bmp_crashseen.txt");
    char seen[64] = "";
    FILE* f = nullptr;
    _wfopen_s(&f, seenPath.c_str(), L"r");
    if (f) { fgets(seen, sizeof seen, f); fclose(f); }
    if (strncmp(seen, stamp, 16) == 0 || AgeHours(ft) > 72) return;
    _wfopen_s(&f, seenPath.c_str(), L"w");
    if (f) { fputs(stamp, f); fclose(f); }
    Log("report: crash.dmp from %.1f h ago not reported yet - offering a report", AgeHours(ft));
    Open(true);
}

void Frame(void* ui, bool inMenu) {
    if (inMenu && ui && *SlotSuffix() == 0) CrashPrompt();  // not for test instances (slot 1+)
    if (!g_wanted || !ui) return;
    if (!g_win.IsOpen() && !g_win.Open(ui)) g_wanted = false;  // no companion mod: nothing to show
}

}  // namespace report
