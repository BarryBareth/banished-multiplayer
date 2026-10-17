#include "update.h"
#include "common.h"
#include "repo_gen.h"
#include <winhttp.h>
#include <mutex>
#include <thread>
#include <vector>

namespace update {

// the project's newest GitHub release (repo.txt -> src/repo_gen.h, written by build.ps1)
static const wchar_t* kDefaultUrl = L"https://api.github.com/repos/" BMP_GITHUB_REPO_W L"/releases/latest";
static std::mutex g_mx;
static Info g_info;

int Compare(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        long x = 0, y = 0;
        while (i < a.size() && a[i] != '.') x = x * 10 + (isdigit((unsigned char)a[i]) ? a[i] - '0' : 0), i++;
        while (j < b.size() && b[j] != '.') y = y * 10 + (isdigit((unsigned char)b[j]) ? b[j] - '0' : 0), j++;
        if (x != y) return x < y ? -1 : 1;
        i++, j++;
    }
    return 0;
}

// "key": "value" from a small flat JSON object (our own file; \" and \\ unescaped)
static std::string Field(const std::string& json, const char* key) {
    size_t k = json.find("\"" + std::string(key) + "\"");
    if (k == std::string::npos) return "";
    size_t q = json.find('"', json.find(':', k) + 1);
    if (q == std::string::npos) return "";
    std::string out;
    for (size_t i = q + 1; i < json.size() && json[i] != '"'; i++) {
        if (json[i] == '\\' && i + 1 < json.size()) i++;
        out += json[i];
    }
    return out;
}

static std::wstring Setting(const char* key) {  // bmp_settings.txt "key=value" ("" = not set)
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wcscpy_s(wcsrchr(path, L'\\') + 1, 64, L"bmp_settings.txt");
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"r");
    std::wstring v;
    if (f) {
        char line[512];
        size_t n = strlen(key);
        while (fgets(line, sizeof line, f))
            if (strncmp(line, key, n) == 0 && line[n] == '=') {
                std::string s = line + n + 1;
                while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
                v.assign(s.begin(), s.end());
            }
        fclose(f);
    }
    return v;
}

static bool Fetch(const std::wstring& url, std::string& body, DWORD& code) {
    URL_COMPONENTS u{sizeof u};
    wchar_t host[256] = L"", path[1024] = L"";
    u.lpszHostName = host; u.dwHostNameLength = 256;
    u.lpszUrlPath = path; u.dwUrlPathLength = 1024;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &u)) return false;
    HINTERNET s = WinHttpOpen(L"BanishedMP-update", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return false;
    WinHttpSetTimeouts(s, 10000, 10000, 15000, 15000);
    HINTERNET c = WinHttpConnect(s, host, u.nPort, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         u.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0) : nullptr;
    bool ok = false;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD len = sizeof code;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &len, nullptr);
        DWORD avail = 0;
        while (WinHttpQueryDataAvailable(r, &avail) && avail && body.size() < 16384) {
            std::string chunk(avail, '\0');
            DWORD got = 0;
            if (!WinHttpReadData(r, chunk.data(), avail, &got)) break;
            body.append(chunk.data(), got);
        }
        ok = true;
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}

void Start(const std::string& myVersion) {
    static bool started;
    if (started) return;
    started = true;
    if (g_slot > 0) return;  // local test instances: slot 0 asks for everyone
    if (Setting("updatecheck") == L"0") { Log("update: check switched off (updatecheck=0)"); return; }
    std::wstring url = Setting("updateurl");
    if (url.empty()) url = kDefaultUrl;
    std::thread([url, myVersion] {
        std::string body;
        DWORD code = 0;
        if (!Fetch(url, body, code)) { Log("update: couldn't reach %ls (error %lu)", url.c_str(), GetLastError()); return; }
        if (code != 200) { Log("update: %ls answered %lu", url.c_str(), code); return; }
        Info i;
        if (body.find("\"tag_name\"") != std::string::npos) {  // GitHub's release JSON: tag "v0.9.1", page, title
            i.version = Field(body, "tag_name");
            if (!i.version.empty() && (i.version[0] == 'v' || i.version[0] == 'V')) i.version.erase(0, 1);
            i.url = Field(body, "html_url");
            i.notes = Field(body, "name");
            if (i.notes == "v" + i.version || i.notes == i.version) i.notes.clear();  // a title that's just the tag
        } else {  // our own latest.json (tests, or a server of your own via updateurl=)
            i.version = Field(body, "version");
            i.url = Field(body, "url");
            i.notes = Field(body, "notes");
        }
        if (i.url.compare(0, 8, "https://") != 0) i.url.clear();  // only ever open a web page
        i.newer = !i.version.empty() && Compare(i.version, myVersion) > 0;
        Log("update: latest release %s, mine %s%s", i.version.c_str(), myVersion.c_str(), i.newer ? " - NEWER one is out" : "");
        std::lock_guard<std::mutex> lk(g_mx);
        g_info = i;
    }).detach();
}

Info Get() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_info;
}

}  // namespace update
