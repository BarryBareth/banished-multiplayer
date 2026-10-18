// The companion mod's elements inside the game's own windows (see gamewin.h):
//  - event log (Template UtilityEventLog -> Dialog/EventLog.rsc, controller EventLogUI): "Mine" / "Everyone" checks and
//    the player's name in front of every message;
//  - workplaces (Dialog/Work.rsc:workPlace in every workplace's title bar, controller WorkPlaceUI): "Share" check =
//    other players' villagers may work here.
// The controllers find their elements by name in an init function and number them (SetElementId); clicks arrive in
// their event function with that number. We hook the same three functions and handle our own ids.
#include "gamewin.h"
#include "gameui.h"
#include "common.h"
#include "towns.h"
#include "commands.h"
#include "net.h"
#include "MinHook.h"
#include <string>
#include <vector>

namespace gamewin {

static uintptr_t g_exe;
using Init_t = void(__fastcall*)(uint8_t* ctl);
using Refresh_t = void(__fastcall*)(uint8_t* ctl);
using Event_t = void(__fastcall*)(uint8_t* ctl, void* elem, int evt, int id);
static const int kClick = 0x12;

static std::string TownName(int t) {  // player names by town, refreshed twice a second (the log redraws every frame)
    if (t < 0) return "?";
    if (t == towns::Local()) return "You";
    static std::vector<std::pair<int, std::string>> names;
    static DWORD at;
    if (GetTickCount() - at > 500) {
        at = GetTickCount();
        names.clear();
        for (auto& p : net::GetUiInfo().roster) names.push_back({net::TownOfPlayer(p.id), p.name});
    }
    for (auto& [town, name] : names)
        if (town == t) return name;
    return "Town " + std::to_string(t + 1);
}

// ---------------------------------------------------------------- event log
// EventLogUI: +0x18 dialog, +0x20 root element, +0x30 bottom / +0x38 previous / +0x40 next buttons, +0x48 rows scrolled
// back from the newest, +0x50 row text elements (count +0x58), +0x60 row "go to" buttons (ids 0x6c + row).
// Rows: the last row shows the newest message. Event log: GameLoop+0x138 (+0x18 entries 0x28 B, +0x20 count).
static Init_t o_LogInit;
static Refresh_t o_LogRefresh;
static Event_t o_LogEvent;
static const int kIdMine = 0x300, kIdAll = 0x301;
static bool g_logButtons;  // the mod's EventLog.rsc is loaded (else the overlay's Messages window is the fallback)

static uint8_t* EventList() {
    uint8_t* gl = *(uint8_t**)(g_exe + Rva(0x1cbdd8));
    return gl ? *(uint8_t**)(gl + 0x138) : nullptr;
}
static std::vector<int> Shown() {  // event indices the log shows (oldest first)
    std::vector<int> out;
    uint8_t* ev = EventList();
    if (!ev) return out;
    bool mine = towns::EventsMineOnly();
    for (int i = 0; i < *(int*)(ev + 0x20); i++) {
        int t = towns::EventTown(i);
        if (!mine || t < 0 || t == towns::Local()) out.push_back(i);
    }
    return out;
}
static void __fastcall h_LogInit(uint8_t* ctl) {
    o_LogInit(ctl);
    void* root = *(void**)(ctl + 0x20);
    void* mine = gameui::Find(root, "bmpLogMine");
    void* all = gameui::Find(root, "bmpLogAll");
    gameui::SetId(mine, kIdMine);
    gameui::SetId(all, kIdAll);
    if (mine && !g_logButtons) Log("gamewin: event log has the Mine / Everyone buttons");
    g_logButtons = g_logButtons || mine;
}
static void __fastcall h_LogRefresh(uint8_t* ctl) {
    void* root = *(void**)(ctl + 0x20);
    void* mineBtn = gameui::Find(root, "bmpFilter");
    uint8_t* ev = EventList();
    if (towns::Count() < 2 || !ev) {
        gameui::Show(mineBtn, false);
        return o_LogRefresh(ctl);
    }
    gameui::Show(mineBtn, true);
    bool mine = towns::EventsMineOnly();
    gameui::SetChecked(gameui::Find(root, "bmpLogMine"), mine);
    gameui::SetChecked(gameui::Find(root, "bmpLogAll"), !mine);
    std::vector<int> list = Shown();
    int rows = *(int*)(ctl + 0x58), n = (int)list.size();
    int maxScroll = n - rows > 0 ? n - rows : 0;
    int& scroll = *(int*)(ctl + 0x48);
    if (scroll > maxScroll) scroll = maxScroll;
    if (scroll < 0) scroll = 0;
    uint8_t* entries = *(uint8_t**)(ev + 0x18);
    for (int r = rows - 1; r >= 0; r--) {
        int k = n - 1 - scroll - (rows - 1 - r);
        void* text = (*(void***)(ctl + 0x50))[r];
        void* go = (*(void***)(ctl + 0x60))[r];
        if (k < 0) {
            gameui::SetParam(text, 0, "");
            gameui::Enable(go, false);
            continue;
        }
        uint8_t* e = entries + list[k] * 0x28;
        const char* msg = *(const char**)(e + 8);
        std::string line = "[" + TownName(towns::EventTown(list[k])) + "] " + (msg ? msg : "");
        gameui::SetParam(text, 0, line.c_str());
        if (text) (*(void(__fastcall**)(void*, uint32_t))(*(uint8_t**)text + 0x50))(text, *(uint32_t*)e);  // colour
        gameui::Enable(go, *(int*)(e + 0x18) >= 0);
    }
    gameui::Enable(*(void**)(ctl + 0x30), scroll > 0);
    gameui::Enable(*(void**)(ctl + 0x40), scroll > 0);
    gameui::Enable(*(void**)(ctl + 0x38), scroll < maxScroll);
}
static void __fastcall h_LogEvent(uint8_t* ctl, void* elem, int evt, int id) {
    if (evt == kClick && (id == kIdMine || id == kIdAll)) {
        towns::SetEventsMineOnly(id == kIdMine);
        return;
    }
    int rows = *(int*)(ctl + 0x58);
    if (evt == kClick && towns::Count() > 1 && id >= 0x6c && id < 0x6c + rows) {  // "go to": our row -> event mapping
        std::vector<int> list = Shown();
        int n = (int)list.size(), k = n - 1 - *(int*)(ctl + 0x48) - (rows - 1 - (id - 0x6c));
        uint8_t* ev = EventList();
        if (k >= 0 && ev) towns::FocusCell(*(int*)(*(uint8_t**)(ev + 0x18) + list[k] * 0x28 + 0x18));
        return;
    }
    o_LogEvent(ctl, elem, evt, id);
}

// ---------------------------------------------------------------- workplaces
// WorkPlaceUI: +0x20 root element, +0x28 entity (the building). Its own buttons are 0x1450/0x1451 (+/- worker).
static Init_t o_WorkInit;
static Refresh_t o_WorkRefresh;
static const int kIdShareWork = 0x1460;
static bool g_workButton;

static bool SharedWithAnyone(void* b) {
    for (int t = 0; t < towns::Count(); t++)
        if (t != towns::Local() && towns::WorkSharedWith(b, t)) return true;
    return false;
}
static void __fastcall h_WorkInit(uint8_t* ctl) {
    o_WorkInit(ctl);
    void* check = gameui::Find(*(void**)(ctl + 0x20), "bmpShareWork");
    gameui::SetId(check, kIdShareWork);
    if (check && !g_workButton) Log("gamewin: workplace windows have the Share button");
    g_workButton = g_workButton || check;
}
static void __fastcall h_WorkRefresh(uint8_t* ctl) {
    o_WorkRefresh(ctl);
    void* check = gameui::Find(*(void**)(ctl + 0x20), "bmpShareWork");
    if (!check) return;
    void* b = *(void**)(ctl + 0x28);
    bool mine = towns::IsOwnWorkplace(b);
    gameui::Show(check, mine);
    if (mine) gameui::SetChecked(check, SharedWithAnyone(b));
}
// WorkPlaceUI's event function (exe+0x8ccb0) is hooked by commands.cpp (+/- worker buttons -> network commands); it
// asks here first. Two hooks on one function don't work with MinHook (the second one failed and +/- went local-only).
// The toggle sits in the title bar, where a press also starts the window's drag (event 8) and the element then gets no
// click event (0x12) - in tests at least. A press (2) followed by a release (1) on it is the click; 0x12 is ignored so
// one click never toggles twice.
bool WorkEvent(void* ui, int evt, int id) {
    if (id != kIdShareWork) return false;
    static void* pressedOn;
    if (evt == 2) { pressedOn = ui; return true; }
    if (evt != 1 || pressedOn != ui) return true;
    pressedOn = nullptr;
    void* b = *(void**)((uint8_t*)ui + 0x28);
    if (towns::IsOwnWorkplace(b)) {
        bool on = !SharedWithAnyone(b);
        for (int t = 0; t < towns::Count(); t++)
            if (t != towns::Local()) commands::RequestShareWork(b, t, on);
    }
    return true;
}

// ---------------------------------------------------------------- window layout across session reloads
// The save carries the saver's window layout: ToolUI (GameLoop+0x120) +0x50 = tree template -> {x, y, open}
// (utility windows), +0x60 = tree entity -> {x, y, open} (building windows); nodes 0x38 B: +0 key, +8 x, +0xc y,
// +0x10 open, +0x18 parent, +0x20 left, +0x28 right. ToolUI read/write exe+0x107df0(toolUI, stream); after a load,
// exe+0x1073b0 opens every node marked open. A resync or join loads the host's save, so everybody got the host's
// windows. Now each player's own utility-window layout is snapshotted while playing and written over the loaded one;
// building windows from the save are never reopened (their buildings are someone else's selection).
using ToolUiSerialize_t = void(__fastcall*)(uint8_t* toolUi, uint8_t* stream);
static ToolUiSerialize_t o_ToolUiSerialize;
struct SavedWindow { void* key; std::string path; int x, y; bool open; };
static std::vector<SavedWindow> g_layout;   // this player's utility windows (main thread snapshot)
static bool g_layoutValid;                  // snapshot taken in the current multiplayer session
static volatile bool g_mp, g_mpHost;        // set on the main thread, read by the loader
static std::vector<void*> g_reopen;         // open in my layout but not in the loaded tree: open after the load
static SRWLOCK g_layoutLock = SRWLOCK_INIT;

template <class F> static void WalkTree(uint8_t* node, F&& f) {
    if (!node) return;
    WalkTree(*(uint8_t**)(node + 0x20), f);
    f(node);
    WalkTree(*(uint8_t**)(node + 0x28), f);
}
static uint8_t* ToolUi() {
    uint8_t* gl = *(uint8_t**)(g_exe + Rva(0x1cbdd8));
    return gl ? *(uint8_t**)(gl + 0x120) : nullptr;
}
static void __fastcall h_ToolUiSerialize(uint8_t* toolUi, uint8_t* stream) {
    o_ToolUiSerialize(toolUi, stream);
    if (*(int*)(stream + 8) != 0 || !g_mp) return;  // only loads during a multiplayer session
    AcquireSRWLockExclusive(&g_layoutLock);
    int kept = 0, closed = 0;
    if (g_layoutValid) {
        std::vector<bool> found(g_layout.size());
        WalkTree(*(uint8_t**)(toolUi + 0x50), [&](uint8_t* n) {
            void* key = *(void**)n;
            std::string path = commands::DescPath(key);
            bool open = false;
            for (size_t i = 0; i < g_layout.size(); i++) {
                if (g_layout[i].key != key && (path.empty() || g_layout[i].path != path)) continue;
                found[i] = true;
                *(int*)(n + 8) = g_layout[i].x, *(int*)(n + 0xc) = g_layout[i].y;
                open = g_layout[i].open;
            }
            *(bool*)(n + 0x10) = open;
            open ? kept++ : closed++;
        });
        g_reopen.clear();
        for (size_t i = 0; i < g_layout.size(); i++)
            if (!found[i] && g_layout[i].open) g_reopen.push_back(commands::DescFromPath(g_layout[i].path));
    } else if (!g_mpHost) {  // joining: start without the host's windows
        WalkTree(*(uint8_t**)(toolUi + 0x50), [&](uint8_t* n) { *(bool*)(n + 0x10) = false; closed++; });
    }
    if (g_layoutValid || !g_mpHost)
        WalkTree(*(uint8_t**)(toolUi + 0x60), [&](uint8_t* n) { if (*(bool*)(n + 0x10)) closed++; *(bool*)(n + 0x10) = false; });
    ReleaseSRWLockExclusive(&g_layoutLock);
    Log("gamewin: loaded game - kept my window layout (%d open, %d closed, %d to reopen)%s", kept, closed,
        (int)g_reopen.size(), g_layoutValid ? "" : " [no layout yet]");
}
void LayoutFrame() {
    bool mp = net::Active();
    g_mp = mp;
    if (!mp) { g_layoutValid = false; return; }
    net::UiInfo u = net::GetUiInfo();
    g_mpHost = u.hosting;
    uint8_t* tui = ToolUi();
    if (!tui || !u.inGame) return;
    if (!g_reopen.empty()) {
        AcquireSRWLockExclusive(&g_layoutLock);
        std::vector<void*> list;
        list.swap(g_reopen);
        ReleaseSRWLockExclusive(&g_layoutLock);
        for (void* tmpl : list) {
            void* dlg = nullptr;  // the toolbar's Utility action (owner = the template)
            if (tmpl) ((void(__fastcall*)(void*, void**, void*))(g_exe + Rva(0x106f50)))(tui, &dlg, tmpl);
        }
    }
    static int frame;
    if (++frame % 30) return;
    std::vector<SavedWindow> now;
    WalkTree(*(uint8_t**)(tui + 0x50), [&](uint8_t* n) {
        now.push_back({*(void**)n, commands::DescPath(*(void**)n), *(int*)(n + 8), *(int*)(n + 0xc), *(bool*)(n + 0x10)});
    });
    AcquireSRWLockExclusive(&g_layoutLock);
    g_layout.swap(now);
    g_layoutValid = true;
    ReleaseSRWLockExclusive(&g_layoutLock);
}

bool HasLogButtons() { return g_logButtons; }
bool HasWorkButton() { return g_workButton; }

void Install() {
    g_exe = (uintptr_t)GetModuleHandleW(nullptr);
    struct H { uintptr_t rva; void* fn; void** orig; const char* name; } hooks[] = {
        {0x67fa0, (void*)h_LogInit, (void**)&o_LogInit, "EventLogUI init"},
        {0x67da0, (void*)h_LogRefresh, (void**)&o_LogRefresh, "EventLogUI refresh"},
        {0x67f30, (void*)h_LogEvent, (void**)&o_LogEvent, "EventLogUI event"},
        {0x8ce10, (void*)h_WorkInit, (void**)&o_WorkInit, "WorkPlaceUI init"},
        {0x8c550, (void*)h_WorkRefresh, (void**)&o_WorkRefresh, "WorkPlaceUI refresh"},
        {0x107df0, (void*)h_ToolUiSerialize, (void**)&o_ToolUiSerialize, "ToolUI serialize"},
    };
    for (auto& h : hooks) {
        void* target = (void*)(g_exe + Rva(h.rva));
        MH_STATUS s = MH_CreateHook(target, h.fn, h.orig);
        if (s == MH_OK) s = MH_EnableHook(target);
        Log("gamewin: hook %s -> %s", h.name, MH_StatusToString(s));
    }
}

}  // namespace gamewin
