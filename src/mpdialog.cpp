// The multiplayer window in the game's own UI (see mpdialog.h): the companion mod's Dialog/BmpMultiplayer.rsc as a
// gamedlg::Window. Every text is a "@0" parameter set from here.
#include "report.h"
#include "mpdialog.h"
#include "gamedlg.h"
#include "gameui.h"
#include "common.h"
#include "net.h"
#include "gameload.h"
#include "commands.h"
#include "overlay.h"
#include "autotest.h"
#include "update.h"
#include <string>
#include <vector>
#include <algorithm>

namespace mpdialog {

struct MpWindow;
static MpWindow& Win();

// ---- the window's state (kept while it is closed)
static int g_mode = 1;          // 0 share one town, 1 separate towns
static int g_source;            // separate towns: 0 new map, 1 continue a save
static int g_start, g_size = 1, g_terrain, g_climate, g_seed;
static bool g_pick = true;
static int g_save;
static std::vector<std::string> g_saves;
static DWORD g_savesAt;
static gameload::NewGameOptions g_opts;
static net::Settings g_set;
static bool g_setLoaded;
static int lastPort = -1;              // what the settings fields showed last frame (-1 / "\x01": not read yet)
static std::string lastRelay = "\x01", lastName = "\x01";

enum Id {
    kModeShare = 2000, kModeSeparate, kSourceNew, kSourceSave, kPick, kHost, kJoin, kConnSteam, kConnDirect, kConnRelay,
    kInvite, kCopy, kStartGame, kLeave, kClose, kStart, kSize, kTerrain, kClimate, kSave, kPaste, kReport,
};
struct Named { const char* name; int id; };
static const Named kIds[] = {
    {"bmpModeShare", kModeShare}, {"bmpModeSeparate", kModeSeparate}, {"bmpSourceNew", kSourceNew},
    {"bmpSourceSave", kSourceSave}, {"bmpPick", kPick}, {"bmpHost", kHost}, {"bmpJoin", kJoin},
    {"bmpConnSteam", kConnSteam}, {"bmpConnDirect", kConnDirect}, {"bmpConnRelay", kConnRelay}, {"bmpInvite", kInvite},
    {"bmpCopy", kCopy}, {"bmpStartGame", kStartGame}, {"bmpLeave", kLeave}, {"bmpClose", kClose}, {"bmpStart", kStart},
    {"bmpSize", kSize}, {"bmpTerrain", kTerrain}, {"bmpClimate", kClimate}, {"bmpSave", kSave}, {"bmpPaste", kPaste}, {"bmpReport", kReport},
};

static void* E(const char* name);
static void Text(const char* name, const std::string& s) { gameui::SetTextDeep(E(name), s.c_str()); }
static void Vis(const char* name, bool on) { gameui::Show(E(name), on); }

static void CopyToClipboard(const std::string& s) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, s.size() + 1);
    if (h) {
        memcpy(GlobalLock(h), s.c_str(), s.size() + 1);
        GlobalUnlock(h);
        SetClipboardData(CF_TEXT, h);
    }
    CloseClipboard();
}

static std::string ClipboardText() {
    std::string s;
    if (!OpenClipboard(nullptr)) return s;
    if (HANDLE h = GetClipboardData(CF_TEXT)) {
        if (const char* p = (const char*)GlobalLock(h)) { s = p; GlobalUnlock(h); }
    }
    CloseClipboard();
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    s = a == std::string::npos ? "" : s.substr(a, b - a + 1);
    std::string out;  // the edits' _allowableCharacters (Dialog/BmpMultiplayer.rsc)
    for (char c : s)
        if (isalnum((unsigned char)c) || strchr(".:-_ ", c)) out += c;
    return out;
}
// The game's edit fields only take typed characters. Paste: the Paste button next to the join field, or Ctrl+V /
// Shift+Insert into the focused field - the join field when the game reports none focused (EditElement::IsActive did
// not work for the user: the first version only pasted into a focused field). The field allows 'v', so the game may
// type one for Ctrl+V: the paste goes onto the text of the frame before the key, and a stray 'v' after it is removed.
static const char* const kEdits[] = {"bmpJoinEdit", "bmpRelay", "bmpName"};
static std::string g_prevText[3], g_prevText2[3];  // last frame, the frame before
static int g_fixEdit = -1, g_fixFrames;
static std::string g_fixText;
static void PasteInto(int i, const std::string& base) {
    std::string clip = ClipboardText();
    std::string text = (base + clip).substr(0, 60);  // _maxLength
    Log("mpdialog: paste into %s: %d characters from the clipboard", kEdits[i], (int)clip.size());
    if (clip.empty()) return;
    gameui::EditSet(E(kEdits[i]), text.c_str());
    g_prevText[i] = text;
    g_fixEdit = i, g_fixText = text, g_fixFrames = 30;
}
static void PasteKeys() {
    if (g_fixEdit >= 0) {  // the game's own handling of the same key press may arrive a few frames later
        std::string now = gameui::EditGet(E(kEdits[g_fixEdit]));
        if (now == g_fixText + "v" || now == g_fixText + "V") gameui::EditSet(E(kEdits[g_fixEdit]), g_fixText.c_str());
        if (--g_fixFrames <= 0) g_fixEdit = -1;
    }
    static bool was;
    bool ctrl = GetAsyncKeyState(VK_CONTROL) & 0x8000, shift = GetAsyncKeyState(VK_SHIFT) & 0x8000;
    bool down = (ctrl && (GetAsyncKeyState('V') & 0x8000)) || (shift && (GetAsyncKeyState(VK_INSERT) & 0x8000));
    bool polled = down && !was;
    was = down;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    polled = polled && pid == GetCurrentProcessId();
    bool message = overlay::TakePasteKey();  // the game window's own WM_KEYDOWN / WM_CHAR
    static DWORD lastPaste;
    bool press = (polled || message) && GetTickCount() - lastPaste > 400;  // both report one key press: paste once
    if (polled || message)
        Log("mpdialog: Ctrl+V seen (%s%s)%s", polled ? "key state" : "", message ? (polled ? " + window message" : "window message") : "",
            press ? "" : " - same press, ignored");
    if (press) {
        lastPaste = GetTickCount();
        int target = -1;
        for (int i = 0; i < 3 && target < 0; i++)
            if (gameui::EditActive(E(kEdits[i]))) target = i;
        Log("mpdialog: Ctrl+V - focused field: %s", target < 0 ? "none (nothing pasted)" : kEdits[target]);
        if (target < 0) return;  // only into the field that has the keyboard (user: not when nothing is selected)
        std::string base = g_prevText[target];  // the text before the keys went down (no stray 'v')
        if (base == g_prevText2[target] + "v" || base == g_prevText2[target] + "V") base = g_prevText2[target];
        PasteInto(target, base);
        return;
    }
    if (!down)
        for (int i = 0; i < 3; i++) g_prevText2[i] = g_prevText[i], g_prevText[i] = gameui::EditGet(E(kEdits[i]));
}

// fixed texts and the edits' starting values (once per window)
static void Init() {
    if (!g_setLoaded) { g_set = net::GetSettings(); g_setLoaded = true; }
    if (!g_seed) g_seed = (int)(GetTickCount() % 999999) + 1;
    for (auto& n : kIds) gameui::SetId(E(n.name), n.id);
    Text("bmpIntro", "Play Banished together with other players.");
    Text("bmpModeShare", "Share one town");
    Text("bmpModeSeparate", "Separate towns");
    Text("bmpSourceNew", "New map");
    Text("bmpSourceSave", "Continue a saved game");
    Text("bmpStartLabel", "Start");
    Text("bmpSizeLabel", "Map size");
    Text("bmpTerrainLabel", "Terrain");
    Text("bmpClimateLabel", "Climate");
    Text("bmpSeedLabel", "Map seed");
    Text("bmpPick", "Players choose where their town starts");
    Text("bmpHost", "Host game");
    Text("bmpJoinHeading", "Join a game");
    Text("bmpJoin", "Join");
    Text("bmpConnHeading", "Connect through");
    Text("bmpConnSteam", "Steam");
    Text("bmpConnDirect", "Direct (IP)");
    Text("bmpConnRelay", "Relay server");
    Text("bmpPortLabel", "Port when hosting");
    Text("bmpRelayLabel", "Relay address");
    Text("bmpNameLabel", "Your name");
    Text("bmpInvite", "Invite players");
    Text("bmpClose", "Close");
    gameui::NumberSet(E("bmpSeed"), g_seed);
    gameui::NumberSet(E("bmpPort"), g_set.port);
    gameui::EditSet(E("bmpRelay"), g_set.relay.c_str());
    gameui::EditSet(E("bmpName"), g_set.name.c_str());
    if (g_opts.starts.empty()) g_opts = gameload::GetNewGameOptions();
    struct C { const char* name; std::vector<std::string>* items; int* v; } combos[] = {
        {"bmpStart", &g_opts.starts, &g_start}, {"bmpSize", &g_opts.sizes, &g_size},
        {"bmpTerrain", &g_opts.terrains, &g_terrain}, {"bmpClimate", &g_opts.climates, &g_climate},
    };
    for (auto& c : combos) {
        if (*c.v >= (int)c.items->size()) *c.v = 0;
        gameui::ComboSet(E(c.name), *c.items);
        gameui::ComboSelect(E(c.name), *c.v);
    }
    lastPort = -1, lastRelay = lastName = "\x01";
    g_saves.clear();  // filled by the first Refresh
    g_savesAt = 0;
}

static std::string RosterText(const net::UiInfo& u) {
    std::string s;
    for (auto& p : u.roster) {
        if (!s.empty()) s += ",   ";  // text elements have no line breaks
        s += p.name;
        if (p.id == 0) s += "  (host)";
        if (p.id == u.me) s += "  (you)";
        if (u.started && !p.synced) s += "  loading...";
    }
    return s.empty() ? "(nobody yet)" : s;
}

// every frame while open: read what the player changed, show the current state
static void Refresh() {
    net::UiInfo u = net::GetUiInfo();
    PasteKeys();
    // read back the inputs
    struct C { const char* name; int* v; } combos[] = {
        {"bmpStart", &g_start}, {"bmpSize", &g_size}, {"bmpTerrain", &g_terrain}, {"bmpClimate", &g_climate}, {"bmpSave", &g_save},
    };
    for (auto& c : combos) { int i = gameui::ComboGet(E(c.name)); if (i >= 0) *c.v = i; }
    g_seed = gameui::NumberGet(E("bmpSeed"));
    // settings edits: saved only when what the field shows changes (never from a field that failed to show the value)
    int port = gameui::NumberGet(E("bmpPort"));
    std::string relay = gameui::EditGet(E("bmpRelay")), name = gameui::EditGet(E("bmpName"));
    bool changed = false;
    if (lastPort >= 0 && port != lastPort && port >= 1 && port <= 65535) { g_set.port = port; changed = true; }
    if (lastRelay != "\x01" && relay != lastRelay) { g_set.relay = relay; changed = true; }
    if (lastName != "\x01" && name != lastName) { g_set.name = name; changed = true; }
    lastPort = port, lastRelay = relay, lastName = name;
    if (changed) net::SetSettings(g_set);

    bool setup = !u.active;
    static int lastState = -1;
    int state = setup ? 0 : u.hosting && !u.started ? 1 : !u.started ? 2 : 3;
    if (state != lastState) { Log("mpdialog: window state %d (active %d hosting %d started %d)", state, u.active, u.hosting, u.started); lastState = state; }
    Vis("bmpSetup", setup);
    Vis("bmpLobby", !setup);
    Vis("bmpClose", setup);
    update::Info upd = update::Get();
    Text("bmpVersion", "Mod version " + u.version + (upd.newer ? "  -  version " + upd.version + " is out" : ""));
    Text("bmpStatus", u.status);
    Vis("bmpStatus", !u.status.empty());
    if (setup) {
        if (GetTickCount() - g_savesAt > 2000 || !g_savesAt) {
            auto saves = net::UiSaves();
            g_savesAt = GetTickCount();
            if (saves != g_saves) {
                std::string cur = g_save < (int)g_saves.size() ? g_saves[g_save] : "";
                g_saves = saves;
                auto it = std::find(g_saves.begin(), g_saves.end(), cur);
                g_save = it == g_saves.end() ? 0 : (int)(it - g_saves.begin());
                std::vector<std::string> shown;  // the game's own file names, said the way a player knows them
                for (auto& n : g_saves)
                    shown.push_back(_stricmp(n.c_str(), "Auto") == 0 ? "Autosave"
                                    : _stricmp(n.c_str(), "Multiplayer") == 0 ? "Last multiplayer game" : n);
                gameui::ComboSet(E("bmpSave"), shown);
                gameui::ComboSelect(E("bmpSave"), g_save);
            }
        }
        if (g_set.method == 0 && !u.steamOk) {  // GOG, or a Steam emulator without lobbies/networking: Relay instead
            g_set.method = 2;
            net::SetSettings(g_set);
            Log("mpdialog: Steam isn't available here - connection switched to Relay");
        }
        bool canHost = g_set.method != 0 || u.steamOk;
        gameui::SetChecked(E("bmpModeShare"), g_mode == 0);
        gameui::SetChecked(E("bmpModeSeparate"), g_mode == 1);
        gameui::SetChecked(E("bmpSourceNew"), g_source == 0);
        gameui::SetChecked(E("bmpSourceSave"), g_source == 1);
        gameui::SetChecked(E("bmpPick"), g_pick);
        // Both modes have the same rows (new map settings or a save), so switching modes keeps the window's size
        bool newMap = g_source == 0;
        Text("bmpSourceNew", g_mode == 0 ? "New town" : "New map");
        Text("bmpSourceSave", g_mode == 0 ? "Load town" : "Continue a saved game");
        Vis("bmpNewTable", newMap);
        Vis("bmpSaveRow", !newMap && !g_saves.empty());
        gameui::Enable(E("bmpPick"), g_mode == 1);  // where friends' towns start: separate towns only
        Text("bmpSaveLabel", g_mode == 0 ? "Town to share" : "Saved game");
        std::string hint;
        if (g_mode == 0) hint = newMap ? "Everybody runs one new town together." : "Everybody runs the same saved town together.";
        else if (newMap) hint = "Each player builds their own town on one new map.";
        else hint = "The host keeps town 1, the other players get towns 2, 3... in the order they join (a one-town save: they settle new towns).";
        if (!newMap && g_saves.empty()) hint += g_mode == 0 ? " Save a game first - the host shares a saved town." : " No saves yet.";
        if (!canHost) hint += " Steam is not running - choose Direct or Relay below.";
        Text("bmpHint", hint);
        gameui::Enable(E("bmpHost"), canHost && (newMap || !g_saves.empty()));
        // join
        bool steam = g_set.method == 0;
        // every row stays (greyed out when it doesn't apply): the window keeps its size between connection methods
        // Edit fields are never disabled: a disabled edit reads back empty and the settings save (text changed) wiped
        // the relay address and name (2026-10-15). Only the buttons grey out.
        for (const char* e : {"bmpPaste", "bmpJoin"}) gameui::Enable(E(e), !steam);
        Text("bmpJoinHint", steam ? "Accept their Steam invite, or right-click them in your Steam friends list and choose Join Game."
                                  : g_set.method == 1 ? "The host's public IP address and port, e.g. 203.0.113.5:7777 (the host forwards that port on their router)."
                                  : "The room code the host sent you - they copy it with the copy button next to it once they host.");
        // connection
        gameui::SetChecked(E("bmpConnSteam"), g_set.method == 0);
        gameui::SetChecked(E("bmpConnDirect"), g_set.method == 1);
        gameui::SetChecked(E("bmpConnRelay"), g_set.method == 2);
        gameui::Enable(E("bmpConnSteam"), u.steamOk);  // GOG / Steam emulators: no Steam
        Text("bmpConnHint", !u.steamOk && g_set.method != 1
                                ? "Everybody uses the same relay server. (Steam isn't available in this copy of Banished: Relay or Direct.)"
                            : g_set.method == 0 ? (u.steamOk ? "Other players join through Steam invites." : "Steam is not running.")
                            : g_set.method == 1 ? "The host must forward this TCP port on their router to their PC."
                                                : "Everybody uses the same relay server.");
        return;
    }
    // lobby
    if (u.hosting && !u.started) {
        Text("bmpLobbyTitle", u.separate ? "Hosting separate towns" : "Hosting " + u.hostSave);
        std::string info;
        if (u.hostVia == 0) info = u.lobby ? "Invite players through Steam." : "Creating the Steam lobby...";
        else if (u.hostVia == 1) info = "Players choose Direct (IP) and join your public IP address with port " + std::to_string(u.listenPort) +
                                        ". Forward TCP port " + std::to_string(u.listenPort) + " on your router to this PC.";
        else info = u.relayUp ? "Room code: " + u.room + "  -  players choose Relay server (same address) and join with this code."
                              : "Connecting to the relay...";
        Text("bmpLobbyInfo", info);
        int friends = (int)u.roster.size() - 1;
        Text("bmpStartGame", friends > 0 ? "Start game (" + std::to_string(u.roster.size()) + " players)" : "Start game alone");
        Text("bmpLeave", "Cancel");
        Vis("bmpInvite", u.hostVia == 0);
        Vis("bmpCopy", u.hostVia == 2 && u.relayUp);
        Vis("bmpStartGame", true);
        Vis("bmpLeave", true);
    } else if (!u.started) {
        Text("bmpLobbyTitle", u.roster.empty() ? "Connecting..." : "Connected to the host");
        Text("bmpLobbyInfo", u.roster.empty() ? "" : "Waiting for the host to start...");
        Text("bmpLeave", "Leave");
        Vis("bmpInvite", false);
        Vis("bmpCopy", false);
        Vis("bmpStartGame", false);
        Vis("bmpLeave", true);
    } else {
        Text("bmpLobbyTitle", "Loading the game...");
        Text("bmpLobbyInfo", "");
    }
    Vis("bmpLobbyButtons", !u.started);
    Text("bmpRoster", RosterText(u));
}

static void OnClick(int id) {
    switch (id) {
    case kModeShare: g_mode = 0; break;
    case kModeSeparate: g_mode = 1; break;
    case kSourceNew: g_source = 0; break;
    case kSourceSave: g_source = 1; break;
    case kPick: g_pick = !g_pick; break;
    case kConnSteam: if (net::GetUiInfo().steamOk) { g_set.method = 0; net::SetSettings(g_set); } break;
    case kConnDirect: g_set.method = 1; net::SetSettings(g_set); break;
    case kConnRelay: g_set.method = 2; net::SetSettings(g_set); break;
    case kHost:
        if (g_source == 0) {  // a new map: separate towns ("N|") or one shared town ("M|")
            char args[96];
            if (g_mode == 1) sprintf_s(args, "N|%d,%d,%d,%d,%d|%d", g_seed, g_size, g_start, g_terrain, g_climate, g_pick ? 1 : 0);
            else sprintf_s(args, "M|%d,%d,%d,%d,%d", g_seed, g_size, g_start, g_terrain, g_climate);
            net::UiDo(net::kUiHost, args);
        } else if (g_save < (int)g_saves.size()) {
            net::UiDo(net::kUiHost, "S|" + g_saves[g_save]);
        }
        break;
    case kJoin: {
        std::string where = gameui::EditGet(E("bmpJoinEdit"));
        if (!where.empty()) net::UiDo(net::kUiJoin, std::string(g_set.method == 1 ? "D|" : "R|") + where);
        break;
    }
    case kInvite: net::UiDo(net::kUiInvite); break;
    case kCopy: CopyToClipboard(net::GetUiInfo().room); break;
    case kPaste: PasteInto(0, ""); break;  // replaces what the join field had
    case kStartGame: net::UiDo(net::kUiStart); break;
    case kLeave: net::UiDo(net::kUiLeave); break;
    case kClose: Close(); break;
    case kReport: report::Open(false); break;
    }
}

// ---- the window
struct MpWindow : gamedlg::Window {
    const char* Resource() const override { return "Dialog/BmpMultiplayer.rsc"; }
    void Init() override { mpdialog::Init(); }
    void Refresh() override { mpdialog::Refresh(); }
    void OnEvent(int id, int evt) override { if (evt == 0x12 || evt == 0x15) OnClick(id); }
};
static MpWindow& Win() { static MpWindow w; return w; }
static void* E(const char* name) { return Win().E(name); }

bool IsOpen() { return Win().IsOpen(); }

// <game>\bmp_menutest.txt (line 1: screenshot dir, optional line 2 "start"): open the window at the main menu, click
// through its states with screenshots, host a new map over Direct (local port) -> lobby; then cancel and quit, or with
// "start" press Start game and screenshot the running game. The connection setting is restored. Called every frame
// (ui = the main menu's UserInterface, nullptr outside the menu).
void MenuTest(void* ui, void (*shot)(const wchar_t*)) {
    static int step = -1, method, clickX = -1, clickY = -1;
    static bool start;
    static DWORD at;
    static std::wstring dir;
    if (step == -1) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring f = path;
        f = f.substr(0, f.find_last_of(L"\\/")) + L"\\bmp_menutest.txt";
        FILE* fp = nullptr;
        _wfopen_s(&fp, f.c_str(), L"r");
        if (!fp) { step = 999; return; }
        wchar_t buf[MAX_PATH] = {}, buf2[32] = {}, buf3[32] = {};
        fgetws(buf, MAX_PATH, fp);
        fgetws(buf2, 32, fp);
        fgetws(buf3, 32, fp);
        swscanf_s(buf3, L"%d %d", &clickX, &clickY);
        fclose(fp);
        dir = buf;
        while (!dir.empty() && (dir.back() == L'\n' || dir.back() == L'\r')) dir.pop_back();
        start = wcsncmp(buf2, L"start", 5) == 0;
        step = wcsncmp(buf2, L"paste", 5) == 0 ? 100 : wcsncmp(buf2, L"click", 5) == 0 ? 500 : 0;
        at = GetTickCount();
        Log("mpdialog: menu test -> %ls%s", dir.c_str(), start ? " (start a game)" : "");
    }
    if (step >= 999 || GetTickCount() - at < 1500) return;
    if ((step < 200 || step >= 500) && !ui) return;  // the window steps need the main menu
    at = GetTickCount();
    auto snap = [&](const wchar_t* name) { shot((dir + L"\\" + name + L".png").c_str()); };
    auto quit = [&]() {
        g_set.method = method;  // the test switched the connection to Direct
        net::SetSettings(g_set);
        Log("mpdialog: menu test done");
        TerminateProcess(GetCurrentProcess(), 0);
    };
    if (step >= 500) {  // "click" + line 3 "X Y": a scripted (engine-level) mouse click at client pixel X,Y, screenshots
        switch (step++) {
        case 500: Open(ui); method = g_set.method; break;
        case 501: snap(L"click_before"); break;
        case 502: autotest::ScriptMouse(clickX, clickY, false); break;
        case 503: autotest::ScriptMouse(clickX, clickY, true); break;
        case 504: autotest::ScriptMouse(clickX, clickY, false); break;
        case 505: autotest::ScriptMouse(-1, -1, false); snap(L"click_after"); break;
        case 506: quit(); break;
        }
        return;
    }
    if (step >= 100 && step < 200) {  // "paste": the Paste button and Ctrl+V into the join field
        static std::wstring saved;  // the user's clipboard, restored afterwards
        auto check = [&](const char* what) {
            std::string got = gameui::EditGet(E("bmpJoinEdit"));
            Log("mpdialog: menu test %s %s: join field '%s'", got == "TEST-ROOM42" ? "PASS" : "FAIL", what, got.c_str());
        };
        switch (step++) {
        case 100:
            Open(ui); method = g_set.method; OnClick(kConnRelay);
            if (OpenClipboard(nullptr)) {
                if (HANDLE h = GetClipboardData(CF_UNICODETEXT))
                    if (const wchar_t* p = (const wchar_t*)GlobalLock(h)) { saved = p; GlobalUnlock(h); }
                CloseClipboard();
            }
            CopyToClipboard(" TEST-ROOM42\r\n");
            gameui::EditSet(E("bmpJoinEdit"), "");
            break;
        case 101: OnClick(kPaste); break;
        case 102: check("Paste button"); snap(L"menu_paste_button"); gameui::EditSet(E("bmpJoinEdit"), ""); break;
        case 103: {  // a real Ctrl+V key press, only while this game window is in front
            DWORD pid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &pid);
            if (pid != GetCurrentProcessId()) { Log("mpdialog: menu test SKIP Ctrl+V: the game window is not in front"); break; }
            INPUT in[4] = {};
            for (auto& i : in) i.type = INPUT_KEYBOARD;
            in[0].ki.wVk = VK_CONTROL; in[1].ki.wVk = 'V';
            in[2].ki.wVk = 'V'; in[2].ki.dwFlags = KEYEVENTF_KEYUP;
            in[3].ki.wVk = VK_CONTROL; in[3].ki.dwFlags = KEYEVENTF_KEYUP;
            SendInput(2, in, sizeof(INPUT));
            Sleep(120);
            SendInput(2, in + 2, sizeof(INPUT));
            break;
        }
        case 104: check("Ctrl+V"); snap(L"menu_paste_ctrlv"); gameui::EditSet(E("bmpJoinEdit"), ""); break;
        // the field focused by a (scripted) mouse click, then what Ctrl+V does: SetText used to be ignored there
        case 105: autotest::ScriptMouse(900, 665, false); break;  // hover first, like a real mouse
        case 106: autotest::ScriptMouse(900, 665, true); break;
        case 107: autotest::ScriptMouse(900, 665, false); break;
        case 108:
            Log("mpdialog: menu test: join field focused: %d", gameui::EditActive(E("bmpJoinEdit")));
            // the scripted click doesn't focus menu fields: set EditElement's active flag (+0xe0) like a click would
            if (!gameui::EditActive(E("bmpJoinEdit"))) ((uint8_t*)E("bmpJoinEdit"))[0xe0] = 1;
            PasteInto(0, gameui::EditGet(E("bmpJoinEdit")));
            ((uint8_t*)E("bmpJoinEdit"))[0xe0] = 0;
            break;
        case 109:
            check("paste into the focused field");
            snap(L"menu_paste_focused");
            autotest::ScriptMouse(-1, -1, false);
            break;
        case 110:
            if (!saved.empty() && OpenClipboard(nullptr)) {
                EmptyClipboard();
                HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (saved.size() + 1) * sizeof(wchar_t));
                if (h) { memcpy(GlobalLock(h), saved.c_str(), (saved.size() + 1) * sizeof(wchar_t)); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
                CloseClipboard();
            }
            gameui::EditSet(E("bmpJoinEdit"), "");
            quit();
            break;
        }
        return;
    }
    if (step >= 200) {  // "start": wait for the started game (about 90 s at most), then a screenshot of it
        step++;
        if (step > 260 && step < 300) { Log("!! mpdialog: menu test: the game did not start"); quit(); }
        if (net::GetUiInfo().inGame && step < 300) step = 300;
        if (step == 220) snap(L"menu_started_wait");
        if (step == 304) snap(L"menu_started_ingame");
        if (step == 306) quit();
        return;
    }
    switch (step++) {
    case 0: Open(ui); method = g_set.method; OnClick(kConnSteam); break;  // first shots: Steam (join row greyed out)
    case 1: snap(L"menu_separate_new"); break;
    case 2: OnClick(kSourceSave); break;
    case 3: snap(L"menu_separate_save"); break;
    case 4: OnClick(kModeShare); break;
    case 5: snap(L"menu_share"); break;
    case 6: OnClick(kConnDirect); break;
    case 7: snap(L"menu_direct"); break;
    case 8: OnClick(kConnRelay); break;
    case 9: snap(L"menu_relay"); break;
    case 10: OnClick(kConnDirect); OnClick(kModeSeparate); OnClick(kSourceNew); OnClick(kHost); break;  // -> lobby
    case 11: break;
    case 12: snap(L"menu_lobby"); break;
    case 13:
        if (start) { OnClick(kStartGame); step = 200; }
        else OnClick(kLeave);
        break;
    case 14: quit(); break;
    }
}

bool Open(void* ui) {
    if (Win().IsOpen()) return true;
    if (!Win().Open(ui)) return false;
    Log("mpdialog: multiplayer window opened");
    return true;
}
void Close() { Win().Close(); }

}  // namespace mpdialog
