// Lockstep command layer (see commands.h).
//
// A command is plain data: tool class type id + entity template path + tile args (+ rotation/offset/variant for
// placements). It never refers to the player's live tool: execution builds (and caches) a private tool the same
// way SelectTool (exe+0x108030) does, so a command can be replayed in another process.
#include "commands.h"
#include "towns.h"
#include "gamewin.h"
#include <cctype>
#include "common.h"
#include "MinHook.h"
#include "autotest.h"
#include "simheap.h"
#include "net.h"
#include <share.h>
#include <deque>
#include <map>
#include <string>
#include <cstring>

namespace commands {

// ---- exe build 0x59b56a2d ----
static const uintptr_t kRvaPlaceBuilding = 0x102b20;  // (tool, x0, y0, x1, y1, float3* offset, float rot)
static const uintptr_t kRvaRerollVariant = 0x102e30;  // (tool): pick next preview variant
struct DragCommit { const char* name; uintptr_t rva; bool prevalidate; };
static const DragCommit kDrag[] = {                   // drag-tool vtable slot 13: commit(tool, startTile, endTile)
    {"Removal", 0x108ab0, false}, {"DragPlacement", 0x100dd0, true}, {"Fenced", 0xe02d0, true},
    {"Road", 0x10a200, false},    {"Bridge", 0xff1a0, false},
};
static const int kDragCount = sizeof(kDrag) / sizeof(kDrag[0]);

enum Kind : int {
    kPlace = 0, kDragBase = 1,
    // window actions (target object = sim-heap offset, identical on every machine)
    kWorker = 10,   // a0 = delta, a1 = profession index, -1 (obj = profession target) or -2 (obj = workplace comp)
    kLimit = 11,    // a0 = resource index, a1 = new limit
    kCrop = 12,     // obj = field component, path = crop description; a0 = field offset (0: crop field +0x50,
                    // orchard +0x60, pasture +0x28 with a1 = 1)
    kCallObj = 13,  // rva(obj) or rva(obj, a0) when a1 = 1
    kUpgrade = 14,  // obj = upgrade component, path = upgrade template
    kVCall = 15,    // obj->vtable[rva](a0)
    kSettle = 16,   // separate towns: settle the player's town at cell a0, a1
    kTrade = 17,    // separate towns: town rva gives a0 of path[0], the player's town gives a1 of path[1] ("p0|p1")
    kSetInt = 18,   // *(int*)(obj + a0) = a1 (workshop recipe, pasture animal limit)
    kNomads = 19,   // town hall entity obj: accept (a0 = 7) / turn away (a0 = 8) the waiting nomads
    kMerchant = 20, // trading post entity obj: trade with the merchant; path "S|desc|n|...|B|desc|n|...", a0 = value
    kDeliverNow = 21, // separate towns: hand over the player's waiting trade deliveries directly
    kWorkerMax = 22,  // obj = workplace component: max workers += a0
    kTradePost = 23,  // obj = trading post trade component, rva = operation (see ExecuteTradePost)
    kDebugKill = 25,  // tests (UI scripts): obj = entity, destroyed at once like the settle step's trees
    kShareStorage = 24, // separate towns: obj = one of the player's storage buildings, shared with town a0 (a1 = 1) or not
    kAgree = 26,    // separate towns: standing trade like kTrade, repeated every season/year: path "p0|p1|period" (1 season, 2 year)
    kAgreeEnd = 27, // separate towns: end standing trade a0 (either partner)
    kShareWork = 28, // separate towns: obj = one of the player's workplace buildings, town a0 may staff it (a1 = 1) or not
};
static const int kFencedIndex = 2;  // kDrag[] index of the pasture tool

struct Command {
    int kind;              // kPlace, or kDragBase + drag index
    uint64_t slot;         // runs at the start of this sim tick
    uint32_t toolType;     // tool class type id (registration order: identical across machines)
    std::string desc;      // entity template path
    int a[4];              // place: x0 y0 x1 y1 / drag: start end
    float offset[3];
    float rotation;
    int variant;
    int rotIndex;          // shared RotationSetting (+0x18 -> +0x10) index 0..3, -1 if the tool has none
    int fence[2];          // pasture tool only: +0x5c / +0x60 (gate layout computed while dragging)
    uint64_t obj;          // window actions: target sim object (sim-heap offset)
    uint32_t objType;      // its vtable RVA in the exe (0 = plain data): checked before execution
    uint32_t rva;          // kCallObj: exe function RVA, kVCall: vtable slot
    std::string path;      // window actions: resource argument ('-' = none)
    int player = 0;        // whose command (separate towns: runs in that player's town)
};

// ---- engine API ----
using PlaceFn = uint64_t(__fastcall*)(void* tool, int x0, int y0, int x1, int y1, float* offset, float rot);
using DragFn = uint64_t(__fastcall*)(void* tool, int start, int end);
using DragValidateFn = int(__fastcall*)(void* tool, int start, int end, int* a, int* b, int* c, int* d);
using GetPathFn = void*(__fastcall*)(const void* external, char** outFilename);
using FindOrLoadFn = void*(__fastcall*)(void* factory, char** filename);
using SingletonFn = void*(__fastcall*)();
using CreateObjectFn = void*(__fastcall*)(void* factory, uint32_t typeId);
using FilenameCtorFn = void*(__fastcall*)(char** self, const char* s);
using StringDtorFn = void(__fastcall*)(char** self);
static PlaceFn o_Place;
static DragFn o_Drag[kDragCount];
static GetPathFn p_GetPath;
static FindOrLoadFn p_FindOrLoad;
static SingletonFn p_ExternalFactoryGet, p_FactoryGet;
static CreateObjectFn p_CreateObject;
static FilenameCtorFn p_FilenameCtor;
static StringDtorFn p_StringDtor;

static uintptr_t g_exe;
static std::deque<Command> g_queue;                // live commands, in issue order
static std::multimap<uint64_t, Command> g_replay;  // commands loaded from a file, by slot
static bool g_executing;
static FILE* g_log;
static const uint64_t kDelayTicks = 1;             // single machine: next tick. Multiplayer: latency budget.

bool Executing() { return g_executing; }
bool Pending() { return !g_queue.empty(); }

static uint32_t TypeOf(void* obj) { return ((uint32_t(__fastcall**)(void*))(*(void***)obj))[2](obj); }

static std::string PathOf(const void* external) {
    char* name = nullptr;
    p_GetPath(external, &name);
    std::string s = name ? name : "";
    p_StringDtor(&name);
    return s;
}

static void* Resolve(const std::string& path) {
    char* name = nullptr;
    p_FilenameCtor(&name, path.c_str());
    void* ext = p_FindOrLoad(p_ExternalFactoryGet(), &name);
    p_StringDtor(&name);
    return ext;
}

// Tool UI = GameLoop+0x120 (also holds the pause flag +0x30 and speed +0x34). Its tree at +0x10 maps a
// child-description type to {tool class (+0x08), shared ToolSetting (+0x10)}; children at +0x20/+0x28.
static uint8_t** FindSettingSlot(uint8_t* node, uint32_t toolType) {
    if (!node) return nullptr;
    if (*(uint32_t*)(node + 8) == toolType) return (uint8_t**)(node + 0x10);
    if (auto s = FindSettingSlot(*(uint8_t**)(node + 0x20), toolType)) return s;
    return FindSettingSlot(*(uint8_t**)(node + 0x28), toolType);
}

static std::map<std::pair<uint32_t, void*>, void*> g_tools;

void ClearScheduled() {
    g_replay.clear();
    g_queue.clear();
}

void OnNewSession() {
    g_tools.clear();  // tools belong to the old GameLoop's tool UI
    g_queue.clear();
}

static void* ReplayTool(uint32_t toolType, void* desc) {
    auto key = std::make_pair(toolType, desc);
    auto it = g_tools.find(key);
    if (it != g_tools.end()) return it->second;
    uint8_t* toolUI = *(uint8_t**)((uint8_t*)ProbeGameLoop() + 0x120);
    ProbeNonSimScope(true);  // the tool is UI state: engine heap, UI RNG (its init rolls a preview variant)
    void* tool = p_CreateObject(p_FactoryGet(), toolType);
    void* setting = nullptr;
    if (uint8_t** slot = FindSettingSlot(*(uint8_t**)(toolUI + 0x10), toolType)) {
        if (!*slot) {  // never selected locally: create its setting like SelectTool does (tool vtable +0x58)
            uint32_t settingType = 0;
            uint32_t* st = ((uint32_t*(__fastcall**)(void*, uint32_t*))(*(void***)tool))[11](tool, &settingType);
            if (st && *st) *slot = (uint8_t*)p_CreateObject(p_FactoryGet(), *st);
        }
        setting = *slot;
    }
    *(void**)((uint8_t*)tool + 0x10) = toolUI;
    *(void**)((uint8_t*)tool + 0x20) = desc;
    *(void**)((uint8_t*)tool + 0x18) = setting;
    ((void(__fastcall**)(void*))(*(void***)tool))[4](tool);  // init: child descriptions, preview mesh
    ProbeNonSimScope(false);
    g_tools[key] = tool;
    Log("commands: replay tool type %u for %p created (setting %p)", toolType, desc, setting);
    return tool;
}

void* ToolFor(uint32_t toolType, const std::string& path) {
    void* desc = Resolve(path);
    return desc ? ReplayTool(toolType, desc) : nullptr;
}

static bool ShouldQueue() {
    // player actions made on the main thread between ticks during a session get queued; our own execution and
    // anything already inside the simulation run directly
    return !g_executing && ProbeGameLoop() && ProbeIsMainThread() && !ProbeInSim();
}

static int RotationOf(void* tool) {
    uint8_t* setting = *(uint8_t**)((uint8_t*)tool + 0x18);
    return setting ? *(int*)(setting + 0x10) : -1;
}

static std::string Format(const Command& c);

static void Enqueue(Command c) {
    if (net::Active() && net::Started()) {  // before START a running game is the player's own (e.g. single-player)
        net::SendCommand(net::CommandSlot(ProbeTick()), Format(c));
        return;
    }
    c.slot = ProbeTick() + kDelayTicks;
    c.player = towns::Local();
    g_queue.push_back(std::move(c));
}

static uint64_t __fastcall h_Place(void* tool, int x0, int y0, int x1, int y1, float* offset, float rot) {
    if (!ShouldQueue()) return o_Place(tool, x0, y0, x1, y1, offset, rot);
    Command c{};
    c.kind = kPlace;
    c.toolType = TypeOf(tool);
    c.desc = PathOf(*(void**)((uint8_t*)tool + 0x20));
    c.a[0] = x0; c.a[1] = y0; c.a[2] = x1; c.a[3] = y1;
    if (offset) memcpy(c.offset, offset, sizeof c.offset);
    c.rotation = rot;
    c.variant = *(int*)((uint8_t*)tool + 0x64);
    c.rotIndex = RotationOf(tool);
    Enqueue(std::move(c));
    // vanilla re-rolls the live tool's preview variant after a successful placement (UI RNG outside ticks)
    ((void(__fastcall*)(void*))(g_exe + kRvaRerollVariant))(tool);
    return 1;  // the UI plays the success sound; the full validation runs again at execution
}

template <int I> static uint64_t __fastcall h_Drag(void* tool, int start, int end) {
    if (!ShouldQueue()) return o_Drag[I](tool, start, end);
    if (kDrag[I].prevalidate) {  // read-only size/blocking check (tool vtable slot 14): reject now, like vanilla
        int a, b, c2, d;
        auto validate = (*(DragValidateFn**)tool)[14];
        if (validate(tool, start, end, &a, &b, &c2, &d) != 0) return 0;
    }
    Command c{};
    c.kind = kDragBase + I;
    c.toolType = TypeOf(tool);
    c.desc = PathOf(*(void**)((uint8_t*)tool + 0x20));
    c.a[0] = start; c.a[1] = end;
    c.rotIndex = RotationOf(tool);
    if (I == kFencedIndex) {
        c.fence[0] = *(int*)((uint8_t*)tool + 0x5c);
        c.fence[1] = *(int*)((uint8_t*)tool + 0x60);
    }
    Enqueue(std::move(c));
    return 1;
}


// ======================= window actions =======================
static const uintptr_t kRvaGetWorkplaceComp = 0x2e10;   // (entity, out comp*) - comp+0x48 = workplace target
static const uintptr_t kRvaGetCropComp = 0x5380;        // (entity, out comp*)
static const uintptr_t kRvaGetBuildComp = 0x5780;       // (entity, out comp*) - demolish/upgrade component
static const uintptr_t kRvaFindWorker = 0x925d0;        // (mgr, out candidate*, target)
static const uintptr_t kRvaAssignWorker = 0x918a0;      // (mgr, out*, candidate*, target)
static const uintptr_t kRvaRemoveWorker = 0x96650;      // (mgr, target)
static const uintptr_t kRvaUpgrade = 0x123470;          // (upgrade comp)
struct WinHook { const char* name; uintptr_t rva; };
static const WinHook kCallObjHooks[] = {                // void f(obj) / void f(obj, int): queued as kCallObj
    {"Demolish", 0x1238d0}, {"DestroyBtn2", 0x123940}, {"DestroyBtn3", 0x1239a0}, {"TradeToggle", 0x5c960},
};
static const uintptr_t kRvaFieldButton = 0x120190;      // (field comp, int)

using UiEventFn = void(__fastcall*)(void* ui, void** elem, int evt, int id);
using ObjFn = void(__fastcall*)(void* obj);
using ObjIntFn = void(__fastcall*)(void* obj, int v);
using OutFn = void(__fastcall*)(void* entity, void** out);
using ProfFn = void(__fastcall*)(void* dialog, int idx);
using CropFn = void(__fastcall*)(void* ui, void* crop);
using NumGetFn = int(__fastcall*)(void* element);
static UiEventFn o_WorkPlaceUI, o_LimitUI, o_UpgradeUI, o_ControlUI;
static ProfFn o_Profession;
static CropFn o_CropUI;
static ObjFn o_CallObj[4];
static ObjIntFn o_FieldButton;
static NumGetFn p_NumberGet;
using SpinnerSet_t = void(__fastcall*)(void* spinner, int value);
static SpinnerSet_t p_SpinnerSet = [](void*, int) {};

template <class F> static F Exe(uintptr_t rva) { return (F)(g_exe + rva); }
static uint8_t* GameLoop() { return (uint8_t*)ProbeGameLoop(); }
static void* Mgr() { return *(void**)(GameLoop() + 0xf0); }

static char* TryGetPath(void* external) {  // no C++ objects here: __try needs a plain frame
    char* name = nullptr;
    __try { p_GetPath(external, &name); } __except (1) { name = nullptr; }
    return name;
}

static std::string SafePath(void* external) {
    if (!external) return "-";
    char* name = TryGetPath(external);
    std::string s = name ? name : "";
    if (name) p_StringDtor(&name);
    return s.empty() ? "?" : s;
}

static uint32_t VtableRva(void* p) {
    uintptr_t vt = *(uintptr_t*)p;
    return vt > g_exe && vt < g_exe + 0x300000 ? (uint32_t)(vt - g_exe) : 0;
}

static uint32_t g_lastObjType;  // type of the object most recently referenced by ObjRef
static bool ObjRef(void* p, uint64_t* out) {
    if (!p || !simheap::Contains(p)) return false;
    *out = simheap::Offset(p);
    g_lastObjType = VtableRva(p);
    return true;
}

static void EnqueueWin(Command c) {
    if (c.obj) c.objType = g_lastObjType;
    Enqueue(std::move(c));
}

static void __fastcall h_WorkPlaceUI(void* ui, void** elem, int evt, int id) {
    if (gamewin::WorkEvent(ui, evt, id)) return;  // the companion mod's Share toggle
    if (!ShouldQueue() || evt != 0x12 || (id != 0x1450 && id != 0x1451)) return o_WorkPlaceUI(ui, elem, evt, id);
    void* comp = nullptr;
    Exe<OutFn>(kRvaGetWorkplaceComp)(*(void**)((uint8_t*)ui + 0x28), &comp);
    Command c{};
    c.kind = kWorker;
    c.a[0] = id == 0x1450 ? 1 : -1;
    // The target (comp+0x48) is the workplace's profession description - data, not in the sim heap. Reference the
    // workplace component instead (also gives the owner check) and read +0x48 at execution.
    c.a[1] = -2;
    if (!comp || !ObjRef(comp, &c.obj)) {
        Log("!! commands: workplace component not in sim heap - worker change dropped");
        return;
    }
    EnqueueWin(std::move(c));
}

static void __fastcall h_Profession(void* dialog, int idx) {
    if (!ShouldQueue()) return o_Profession(dialog, idx);
    uint64_t* d = (uint64_t*)dialog;
    int want = p_NumberGet(*(void**)(d[8] + (uint64_t)idx * 8));
    int have = *(int*)(d[6] + (uint64_t)idx * 4);
    if (want == have) return;
    Command c{};
    c.kind = kWorker;
    c.a[0] = want - have;
    c.a[1] = idx;
    EnqueueWin(std::move(c));
}

// Separate towns: the town hall and resource-limit windows show and edit the CURRENT town (GameLoop+0xf0 = the viewer's
// own town outside the simulation) whatever building they were opened on - another player's town hall looked like
// the viewer's own and changed the viewer's professions/limits. Such a window (ui+0x28 = its entity) now updates in
// the owner's town (read-only view of that town) and its town-wide changes are dropped.
static int ForeignTown(void* ui) {
    if (towns::Count() < 2 || !ui) return -1;
    void* e = *(void**)((uint8_t*)ui + 0x28);
    int owner = e ? towns::OwnerOfObject(e) : -1;
    return owner >= 0 && owner != towns::Local() ? owner : -1;
}
using UiUpdate_t = void(__fastcall*)(void* ui);
static UiUpdate_t o_TownHallVf7, o_TownHallVf8, o_LimitVf7;
template <UiUpdate_t* Orig> static void __fastcall h_ForeignView(void* ui) {
    int t = ForeignTown(ui);
    if (t < 0) return (*Orig)(ui);
    towns::Scope s(t);
    (*Orig)(ui);
}

static void __fastcall h_LimitUI(void* ui, void** elem, int evt, int id) {
    if (ShouldQueue() && evt == 0x15 && id == 0x3039 && ForeignTown(ui) >= 0) {
        Log("commands: resource limit change on town %d's building ignored", ForeignTown(ui));
        return;
    }
    if (!ShouldQueue() || evt != 0x15 || id != 0x3039) return o_LimitUI(ui, elem, evt, id);
    // let the game compute the value, then take the write back and queue it
    uint8_t* config = *(uint8_t**)((uint8_t*)ui + 0x10);
    int index = *(int*)(config + 0x20);
    int* slot = (int*)(*(uint8_t**)((uint8_t*)Mgr() + 0x170) + (int64_t)index * 0x20 + 8);
    int before = *slot;
    o_LimitUI(ui, elem, evt, id);
    int after = *slot;
    if (after == before) return;
    *slot = before;
    Command c{};
    c.kind = kLimit;
    c.a[0] = index;
    c.a[1] = after;
    EnqueueWin(std::move(c));
}

static void __fastcall h_CropUI(void* ui, void* crop) {
    if (!ShouldQueue()) return o_CropUI(ui, crop);
    void* comp = nullptr;
    Exe<OutFn>(kRvaGetCropComp)(*(void**)((uint8_t*)ui + 0x28), &comp);
    Command c{};
    c.kind = kCrop;
    c.path = SafePath(crop);
    if (!ObjRef(comp, &c.obj) || c.path == "?") {
        Log("commands: crop target/description not referencable (path '%s'), applying directly", c.path.c_str());
        return o_CropUI(ui, crop);
    }
    EnqueueWin(std::move(c));
}

static void __fastcall h_UpgradeUI(void* ui, void** elem, int evt, int id) {
    if (!ShouldQueue() || evt != 0x12 || id != 33000 || *elem != *(void**)((uint8_t*)ui + 0x30))
        return o_UpgradeUI(ui, elem, evt, id);
    void* comp = nullptr;
    Exe<OutFn>(kRvaGetBuildComp)(*(void**)((uint8_t*)ui + 0x28), &comp);
    Command c{};
    c.kind = kUpgrade;
    c.path = SafePath(*(void**)(*(uint8_t**)((uint8_t*)ui + 0x10) + 0x20));
    if (!ObjRef(comp, &c.obj) || c.path == "?") return o_UpgradeUI(ui, elem, evt, id);
    EnqueueWin(std::move(c));
}

static void __fastcall h_ControlUI(void* ui, void** elem, int evt, int id) {
    if (!ShouldQueue() || evt != 0x12 || id != 50000 || *elem != *(void**)((uint8_t*)ui + 0x30))
        return o_ControlUI(ui, elem, evt, id);
    Command c{};
    c.kind = kVCall;
    c.rva = 14;  // vtable +0x70
    c.a[0] = *(int*)(*(uint8_t**)((uint8_t*)ui + 0x10) + 0x20);
    if (!ObjRef(*(void**)((uint8_t*)ui + 0x38), &c.obj)) return o_ControlUI(ui, elem, evt, id);
    EnqueueWin(std::move(c));
}

template <int I> static void __fastcall h_CallObj(void* obj) {
    Command c{};
    if (!ShouldQueue() || !ObjRef(obj, &c.obj)) return o_CallObj[I](obj);
    c.kind = kCallObj;
    c.rva = (uint32_t)kCallObjHooks[I].rva;
    EnqueueWin(std::move(c));
}

static void __fastcall h_FieldButton(void* obj, int v) {
    Command c{};
    if (!ShouldQueue() || !ObjRef(obj, &c.obj)) return o_FieldButton(obj, v);
    c.kind = kCallObj;
    c.rva = (uint32_t)kRvaFieldButton;
    c.a[0] = v;
    c.a[1] = 1;
    EnqueueWin(std::move(c));
}

// Construction window pause button (BuildUI vf9, button 6000): FUN_1401350d0(buildComp, paused) - was not captured
// (only the clicking machine paused: desync, and it worked on other players' buildings).
static const uintptr_t kRvaBuildPause = 0x1350d0;
using BuildUiEvt_t = void(__fastcall*)(void* ui, void* elem, int evt, int id);
static BuildUiEvt_t o_BuildUI;
static void __fastcall h_BuildUI(void* ui, void* elem, int evt, int id) {
    if (!ShouldQueue() || evt != 0x12 || id != 6000) return o_BuildUI(ui, elem, evt, id);
    void* entity = *(void**)((uint8_t*)ui + 0x28);
    void* comp = nullptr;
    if (entity) Exe<void*(__fastcall*)(void*, void**)>(0x2e10)(entity, &comp);  // entity -> its build component
    Command c{};
    if (!comp || !ObjRef(comp, &c.obj)) return o_BuildUI(ui, elem, evt, id);
    c.kind = kCallObj;
    c.rva = (uint32_t)kRvaBuildPause;
    c.a[0] = *(uint8_t*)((uint8_t*)comp + 0x50) == 0;  // toggle
    c.a[1] = 1;
    EnqueueWin(std::move(c));
}

// ---- more window actions that changed the simulation without being captured (found 2026-10-11)
static const uintptr_t kRvaTownHallUI = 0x81650, kRvaTownHallRefresh = 0x833c0;
static const uintptr_t kRvaConsumeProduceUI = 0x5fc90, kRvaEntityConsumeProduce = 0xb6f0;
static const uintptr_t kRvaPastureUI = 0xba2f0, kRvaEntityPasture = 0x4fe0;
static const uintptr_t kRvaImmigrationUI = 0x6bc60, kRvaEntityImmigration = 0x12020, kRvaEntityStateMachine = 0x3ea0;
using UiEvt_t = void(__fastcall*)(void* ui, void* elem, int evt, int id);
using EntityGet_t = void*(__fastcall*)(void* entity, void** out);
static UiEvt_t o_TownHallUI, o_ConsumeProduceUI, o_PastureUI, o_ImmigrationUI;

// Town hall profession numbers (event 0x15, ids 0x1966 + profession): the same as the professions window
static void __fastcall h_TownHallUI(void* ui, void* elem, int evt, int id) {
    uint8_t* gl = GameLoop();
    int profCount = gl ? *(int*)(*(uint8_t**)(gl + 0xb0) + 0x28) : 0;
    if (!ShouldQueue() || evt != 0x15 || id <= 0x1965 || id > profCount + 0x1966) return o_TownHallUI(ui, elem, evt, id);
    if (int t = ForeignTown(ui); t >= 0) {
        Log("commands: profession change in town %d's town hall ignored", t);
        towns::Scope s(t);
        Exe<void(__fastcall*)(void*)>(kRvaTownHallRefresh)(ui);  // back to that town's numbers
        return;
    }
    int idx = id - 0x1966;
    uint8_t* row = *(uint8_t**)((uint8_t*)ui + 0x40) + (int64_t)idx * 0x40;
    int want = p_NumberGet ? p_NumberGet(*(void**)(row + 0x10)) : *(int*)row;
    int delta = want - *(int*)row;
    if (delta) {
        Command c{};
        c.kind = kWorker;
        c.a[0] = delta;
        c.a[1] = idx;
        c.path = "-";
        EnqueueWin(std::move(c));
    }
    Exe<void(__fastcall*)(void*)>(kRvaTownHallRefresh)(ui);  // shows the current numbers until the command runs
}

static void SetIntCommand(void* obj, int offset, int value) {
    Command c{};
    if (!obj || !ObjRef(obj, &c.obj)) return;
    c.kind = kSetInt;
    c.a[0] = offset;
    c.a[1] = value;
    c.path = "-";
    EnqueueWin(std::move(c));
}

// Workshop: which item to make (event 0x14, id 22000) -> component+0x50 = combo index
static void __fastcall h_ConsumeProduceUI(void* ui, void* elem, int evt, int id) {
    if (!ShouldQueue() || evt != 0x14 || id != 22000) return o_ConsumeProduceUI(ui, elem, evt, id);
    void* comp = nullptr;
    if (void* e = *(void**)((uint8_t*)ui + 0x28)) Exe<EntityGet_t>(kRvaEntityConsumeProduce)(e, &comp);
    SetIntCommand(comp, 0x50, *(int*)(*(uint8_t**)((uint8_t*)ui + 0x30) + 0x118));
}

// Pasture: animal limit slider (event 0x17, id 0x1f5) -> component+0x40; other events are local
static void __fastcall h_PastureUI(void* ui, void* elem, int evt, int id) {
    if (!ShouldQueue() || evt != 0x17 || id != 0x1f5) return o_PastureUI(ui, elem, evt, id);
    void* comp = nullptr;
    if (void* e = *(void**)((uint8_t*)ui + 0x28)) Exe<EntityGet_t>(kRvaEntityPasture)(e, &comp);
    SetIntCommand(comp, 0x40, (int)*(float*)(*(uint8_t**)((uint8_t*)ui + 0x38) + 0x128));
}

// Town hall nomads: accept (id 39000 -> message 7) / turn away (id 0x9859 -> message 8)
static void __fastcall h_ImmigrationUI(void* ui, void* elem, int evt, int id) {
    if (!ShouldQueue() || evt != 0x12 || (id != 39000 && id != 0x9859)) return o_ImmigrationUI(ui, elem, evt, id);
    Command c{};
    if (!ObjRef(*(void**)((uint8_t*)ui + 0x28), &c.obj)) return o_ImmigrationUI(ui, elem, evt, id);
    c.kind = kNomads;
    c.a[0] = id == 39000 ? 7 : 8;
    c.path = "-";
    EnqueueWin(std::move(c));
}

static void ExecuteNomads(uint8_t* entity, int msg) {
    void* comp = nullptr;
    Exe<EntityGet_t>(kRvaEntityImmigration)(entity, &comp);
    if (!comp) return;
    void* sm = nullptr;
    Exe<EntityGet_t>(kRvaEntityStateMachine)(*(void**)((uint8_t*)comp + 0x10), &sm);
    if (!sm) return;
    uint8_t* s = (uint8_t*)sm;
    void* target = *(void**)(s + 0x48);
    using Msg_t = void(__fastcall*)(void*, void*, int, int);
    (*(Msg_t**)target)[2](target, s + 0x48, (int)*(char*)(s + 0x51), msg);
}

// Trading post "Trade" button (TradeUI vf9, event 0x12 id 20000 -> FUN_1400868f0(ui)). The window holds the
// sell list (ui+0x30, 0x20-byte rows: desc, ..., amount at +0x18; count ui+0x38) and the buy list (ui+0x40,
// 0x18-byte rows: desc, amount at +0x10; count ui+0x48); trade value ui+0x100 -> +0xd8. Captured as kMerchant and
// replayed by ExecuteMerchant (same steps as exe+0x868f0, minus the UI updates).
static const uintptr_t kRvaTradeUI = 0x873f0;
static UiEvt_t o_TradeUI;
static void __fastcall h_TradeUI(void* ui, void* elem, int evt, int id) {
    if (!ShouldQueue() || evt != 0x12 || id != 20000) return o_TradeUI(ui, elem, evt, id);
    uint8_t* u = (uint8_t*)ui;
    Command c{};
    if (!ObjRef(*(void**)(u + 0x28), &c.obj)) return o_TradeUI(ui, elem, evt, id);
    std::string path = "S";
    for (int i = 0; i < *(int*)(u + 0x38); i++) {
        uint8_t* row = *(uint8_t**)(u + 0x30) + i * 0x20;
        int n = *(int*)(row + 0x18);
        if (n) path += "|" + PathOf(*(void**)row) + "|" + std::to_string(n);
        *(int*)(row + 0x18) = 0;
        if (*(uint8_t**)(u + 0x60)) p_SpinnerSet(*(void**)(*(uint8_t**)(u + 0x60) + i * 0x20 + 0x18), 0);
    }
    path += "|B";
    for (int i = 0; i < *(int*)(u + 0x48); i++) {
        uint8_t* row = *(uint8_t**)(u + 0x40) + i * 0x18;
        int n = *(int*)(row + 0x10);
        if (n) path += "|" + PathOf(*(void**)row) + "|" + std::to_string(n);
        *(int*)(row + 0x10) = 0;
        if (*(uint8_t**)(u + 0x70)) p_SpinnerSet(*(void**)(*(uint8_t**)(u + 0x70) + i * 0x20 + 0x10), 0);
    }
    if (path == "S|B") return;
    c.kind = kMerchant;
    c.a[0] = *(int*)(*(uint8_t**)(u + 0x100) + 0xd8);
    c.path = path;
    EnqueueWin(std::move(c));
    for (uintptr_t refresh : {0x89b80, 0x89140, 0x885d0, 0x87df0}) Exe<void(__fastcall*)(void*)>(refresh)(ui);
}

// ---- more uncaptured window actions (UI audit 2026-10-11: every DialogController event handler checked)
// Workplace max-worker arrows (WorkPlaceUI vf9 ids 0x1452/0x1453 -> FUN_14008cae0(ui, +-1) -> exe+0x134fd0(comp,
// limit)). The limit is the work component's +0x30 (0 = not computed yet: exe+0x133a90 = the building's maximum).
static const uintptr_t kRvaWorkerMaxUI = 0x8cae0, kRvaWorkComp = 0x3050, kRvaWorkMax = 0x133a90,
                       kRvaSetWorkerMax = 0x134fd0;
using UiInt_t = void(__fastcall*)(void* ui, int v);
using UiInt2_t = void(__fastcall*)(void* ui, int a, int b);
using UiPtr_t = void(__fastcall*)(void* ui, void* p);
using Ui_t = void(__fastcall*)(void* ui);
static UiInt_t o_WorkerMaxUI;
static void __fastcall h_WorkerMaxUI(void* ui, int delta) {
    if (!ShouldQueue()) return o_WorkerMaxUI(ui, delta);
    void* comp = nullptr;
    Exe<OutFn>(kRvaGetWorkplaceComp)(*(void**)((uint8_t*)ui + 0x28), &comp);
    Command c{};
    if (!comp || !ObjRef(comp, &c.obj)) { Log("!! commands: worker limit target not in sim heap - dropped"); return; }
    c.kind = kWorkerMax;
    c.a[0] = delta;
    c.path = "-";
    EnqueueWin(std::move(c));
}

static void ExecuteWorkerMax(uint8_t* comp, int delta) {
    void* work = nullptr;
    Exe<EntityGet_t>(kRvaWorkComp)(*(void**)(comp + 0x10), &work);
    if (!work) return;
    int max = Exe<int(__fastcall*)(void*)>(kRvaWorkMax)(work);
    int* limit = (int*)((uint8_t*)work + 0x30);
    if (*limit == 0) *limit = max;
    int want = *limit + delta;
    if (want < 0 || want > max) return;  // the arrows are disabled at the ends; queued clicks may overshoot
    Exe<void(__fastcall*)(void*, int)>(kRvaSetWorkerMax)(comp, want);
}

// Orchard tree type (OrchardUI vf11 exe+0x7a850: comp(0x5440)+0x60) and pasture animal type (PastureUI vf11
// exe+0xba110: comp(0x4fe0)+0x28, then +0x40 = exe+0x12da40(comp) = default animal limit). Same shape as CropUI:
// queued as kCrop with a0 = field offset, a1 = 1 for the pasture variant.
static const uintptr_t kRvaOrchardUI = 0x7a850, kRvaEntityOrchard = 0x5440, kRvaPastureTypeUI = 0xba110,
                       kRvaPastureDefault = 0x12da40, kRvaPastureRefresh = 0xba470;
static UiPtr_t o_OrchardUI, o_PastureTypeUI;
static void QueueSelect(void* ui, void* desc, uintptr_t getComp, int offset, int pasture) {
    void* comp = nullptr;
    Exe<OutFn>(getComp)(*(void**)((uint8_t*)ui + 0x28), &comp);
    Command c{};
    c.kind = kCrop;
    c.a[0] = offset;
    c.a[1] = pasture;
    c.path = SafePath(desc);
    if (!comp || !ObjRef(comp, &c.obj) || c.path == "?") {
        Log("!! commands: selection target/description not referencable (path '%s') - dropped", c.path.c_str());
        return;
    }
    EnqueueWin(std::move(c));
}
static void __fastcall h_OrchardUI(void* ui, void* desc) {
    if (!ShouldQueue()) return o_OrchardUI(ui, desc);
    QueueSelect(ui, desc, kRvaEntityOrchard, 0x60, 0);
}
static void __fastcall h_PastureTypeUI(void* ui, void* desc) {
    if (!ShouldQueue()) return o_PastureTypeUI(ui, desc);
    QueueSelect(ui, desc, kRvaEntityPasture, 0x28, 1);
}

// Trading post window (TradeUI vf9 exe+0x873f0), everything besides the Trade button. All write the trade
// component (entity -> exe+0x15b50): queued as kTradePost with rva = operation:
//   1 id 0x4e21 (exe+0x86e20): message 10 to the post's state machine (send the merchant away)
//   2 evt 0x14 id 0x4e22 (exe+0x87090): comp+0x28 = combo value
//   3 evt 0x14 id 0x4e23 (exe+0x87350): (comp+0x30)[comp+0x2c].+8 = combo value
//   4 ids 8000+i (exe+0x872a0): order checkbox: exe+0x59df0(comp, desc, checked)
//   5 ids 5000+i / 6000+i (exe+0x871d0): swap order rows a0, a1 of comp+0x40 (12-byte rows)
//   6 evt 0x15 ids 7000+i (exe+0x87120): order row i amount (comp+0x40 row +8)
//   7 evt 0x15 ids 2000+i (exe+0x86f80): comp+0x60 tree {desc -> amount}
static const uintptr_t kRvaTradeComp = 0x15b50;
static Ui_t o_TradeSendAway, o_TradeCombo2, o_TradeCombo3;
static UiInt_t o_TradeOrderCheck, o_TradeOrderAmount, o_TradeKeep;
static UiInt2_t o_TradeSwap;
using SpinnerGet_t = int(__fastcall*)(void* spinner);
static SpinnerGet_t p_SpinnerGet;

static bool QueueTradeOp(void* ui, uint32_t op, int a0, int a1, const std::string& path) {
    void* comp = nullptr;
    Exe<OutFn>(kRvaTradeComp)(*(void**)((uint8_t*)ui + 0x28), &comp);
    Command c{};
    if (!comp || !ObjRef(comp, &c.obj)) { Log("!! commands: trading post not in sim heap - op %u dropped", op); return false; }
    c.kind = kTradePost;
    c.rva = op;
    c.a[0] = a0;
    c.a[1] = a1;
    c.path = path;
    EnqueueWin(std::move(c));
    return true;
}
static void __fastcall h_TradeSendAway(void* ui) {
    if (!ShouldQueue()) return o_TradeSendAway(ui);
    QueueTradeOp(ui, 1, 0, 0, "-");
}
static void __fastcall h_TradeCombo2(void* ui) {
    if (!ShouldQueue()) return o_TradeCombo2(ui);
    QueueTradeOp(ui, 2, *(int*)(*(uint8_t**)((uint8_t*)ui + 0x138) + 0x118), 0, "-");
}
static void __fastcall h_TradeCombo3(void* ui) {
    if (!ShouldQueue()) return o_TradeCombo3(ui);
    QueueTradeOp(ui, 3, *(int*)(*(uint8_t**)((uint8_t*)ui + 0xc0) + 0x118), 0, "-");
}
static void __fastcall h_TradeOrderCheck(void* ui, int i) {
    if (!ShouldQueue()) return o_TradeOrderCheck(ui, i);
    uint8_t* u = (uint8_t*)ui;
    void* desc = (*(void***)(u + 0xb0))[i];
    char checked = *(char*)((*(uint8_t***)(u + 0xa0))[i] + 0xd8);
    QueueTradeOp(ui, 4, checked ? 1 : 0, 0, PathOf(desc));
}
static void __fastcall h_TradeSwap(void* ui, int a, int b) {
    if (!ShouldQueue()) return o_TradeSwap(ui, a, b);
    QueueTradeOp(ui, 5, a, b, "-");
}
static void __fastcall h_TradeOrderAmount(void* ui, int i) {
    if (!ShouldQueue() || !p_SpinnerGet) return o_TradeOrderAmount(ui, i);
    void* spinner = *(void**)(*(uint8_t**)((uint8_t*)ui + 0x90) + (int64_t)i * 0x20 + 0x18);
    QueueTradeOp(ui, 6, i, p_SpinnerGet(spinner), "-");
}
static void __fastcall h_TradeKeep(void* ui, int i) {
    if (!ShouldQueue() || !p_SpinnerGet) return o_TradeKeep(ui, i);
    uint8_t* u = (uint8_t*)ui;
    void* spinner = *(void**)(*(uint8_t**)(u + 0x80) + (int64_t)i * 0x18 + 8);
    void* desc = *(void**)(*(uint8_t**)(u + 0x50) + (int64_t)i * 0x18);
    QueueTradeOp(ui, 7, p_SpinnerGet(spinner), 0, PathOf(desc));
}

static int ExecuteTradePost(uint8_t* comp, const Command& c) {
    switch (c.rva) {
    case 1: {
        void* sm = nullptr;
        Exe<EntityGet_t>(kRvaEntityStateMachine)(*(void**)(comp + 0x10), &sm);
        if (!sm) return 0;
        uint8_t* s = (uint8_t*)sm;
        void* target = *(void**)(s + 0x48);
        using Msg_t = void(__fastcall*)(void*, void*, int, int);
        (*(Msg_t**)target)[2](target, s + 0x48, (int)*(char*)(s + 0x51), 10);
        return 1;
    }
    case 2:
        *(int*)(comp + 0x28) = c.a[0];
        return 1;
    case 3:
        *(int*)(*(uint8_t**)(comp + 0x30) + (int64_t)*(int*)(comp + 0x2c) * 0x20 + 8) = c.a[0];
        return 1;
    case 4: {
        void* desc = Resolve(c.path);
        if (!desc) return 0;
        Exe<void(__fastcall*)(void*, void*, char)>(0x59df0)(comp, desc, (char)c.a[0]);
        return 1;
    }
    case 5: {
        if (c.a[0] < 0 || c.a[1] < 0) return 0;
        uint8_t* rows = *(uint8_t**)(comp + 0x40);
        uint8_t tmp[12];
        memcpy(tmp, rows + c.a[0] * 12, 12);
        memcpy(rows + c.a[0] * 12, rows + c.a[1] * 12, 12);
        memcpy(rows + c.a[1] * 12, tmp, 12);
        return 1;
    }
    case 6:
        if (c.a[0] < 0) return 0;
        *(int*)(*(uint8_t**)(comp + 0x40) + 8 + (int64_t)c.a[0] * 12) = c.a[1];
        return 1;
    case 7: {
        uint64_t key = (uint64_t)Resolve(c.path);
        if (!key) return 0;
        for (uint64_t* n = *(uint64_t**)(comp + 0x60); n;) {  // same walk as exe+0x86f80
            if (key < n[0]) n = (uint64_t*)n[3];
            else if (key > n[0]) n = (uint64_t*)n[4];
            else { *(int*)(n + 1) = c.a[0]; return 1; }
        }
        struct { uint64_t key; int value; } pair{key, c.a[0]};
        uint8_t out[16];
        Exe<void(__fastcall*)(void*, void*, void*)>(0xe1c0)(comp + 0x60, out, &pair);
        return 1;
    }
    }
    return 0;
}

static bool DescHas(void* desc, uintptr_t typeinfoRva) {  // description's sub-descriptions: +0x28 array, +0x30 count
    int want = *(int*)(g_exe + typeinfoRva), n = *(int*)((uint8_t*)desc + 0x30);
    for (int i = 0; i < n; i++) {
        void* d = (*(void***)((uint8_t*)desc + 0x28))[i];
        if (((int(__fastcall*)(void*))(*(void***)d)[2])(d) == want) return true;
    }
    return false;
}

static void ExecuteMerchant(uint8_t* entity, const std::string& path, int value) {
    void* merchant = nullptr;
    void* storage = nullptr;
    Exe<EntityGet_t>(0x15b50)(entity, &merchant);
    Exe<EntityGet_t>(0x6270)(entity, &storage);
    if (!merchant || !storage) { Log("!! commands: trading post without merchant/storage"); return; }
    Exe<void(__fastcall*)(void*)>(0x5adf0)(merchant);
    uint8_t* town = *(uint8_t**)(GameLoop() + 0xf0);
    std::vector<std::string> f;
    for (size_t s = 0; s <= path.size();) {
        size_t e = path.find('|', s);
        if (e == std::string::npos) e = path.size();
        f.push_back(path.substr(s, e - s));
        s = e + 1;
    }
    bool buying = false;
    for (size_t i = 0; i < f.size(); i++) {
        if (f[i] == "S") { buying = false; continue; }
        if (f[i] == "B") { buying = true; continue; }
        if (i + 1 >= f.size()) break;
        void* desc = Resolve(f[i]);
        int n = atoi(f[i + 1].c_str());
        i++;
        if (!desc || n <= 0) continue;
        if (!buying) {
            Exe<void(__fastcall*)(void*, void*, int)>(0x132620)(storage, desc, n);
            int idx = (int)Exe<uint64_t(__fastcall*)(void*, void*)>(0x8e460)(town, desc);
            if (idx >= 0) *(int*)(*(uint8_t**)(town + 0x170) + (int64_t)idx * 0x20 + 0xc) += n;
            continue;
        }
        Exe<void(__fastcall*)(void*, void*, int)>(0x5c400)(merchant, desc, n);
        if (DescHas(desc, 0x1cfcb8)) {                 // RawMaterialDescription: into the trading post
            Exe<void(__fastcall*)(void*, void*, int)>(0x1324d0)(storage, desc, n);
            Exe<void(__fastcall*)(void*, void*)>(0x97330)(town, desc);
        } else if (DescHas(desc, 0x1cfb68)) {          // NaturalResourceDescription: seeds/saplings unlocked
            Exe<void(__fastcall*)(void*, void*)>(0x96ef0)(town, desc);
        } else if (DescHas(desc, 0x1cf9a8)) {          // LivestockDescription: animals delivered
            Exe<void(__fastcall*)(void*, void*)>(0x96ef0)(town, desc);
            Exe<void(__fastcall*)(void*, void*, int)>(0x5b640)(merchant, desc, n);
        }
    }
    *(int*)(town + 0x1ac) += value;
}

static int ExecuteWindow(const Command& c) {
    uint8_t* obj = c.obj ? (uint8_t*)simheap::FromOffset(c.obj) : nullptr;
    if (obj) {
        size_t size = 0;
        bool live = simheap::Contains(obj) && simheap::BlockOf(obj, &size) == obj;
        if (!live || VtableRva(obj) != c.objType) {
            Log("!! commands: DESYNC - window command kind %d target +%llx is not the expected object (type %x, found %x)",
                c.kind, (unsigned long long)c.obj, c.objType, live ? VtableRva(obj) : 0);
            return -1;
        }
    }
    if (obj && towns::Count() > 1) {
        int owner = towns::OwnerOfObject(obj);
        if (owner >= 0 && owner != c.player) {
            Log("commands: player %d's window command on town %d's object ignored", c.player, owner);
            return -2;
        }
    }
    switch (c.kind) {
    case kWorker: {
        void* target = c.a[1] >= 0  ? (*(void***)(*(uint8_t**)(GameLoop() + 0xb0) + 0x20))[c.a[1]]
                       : c.a[1] == -2 ? (obj ? *(void**)(obj + 0x48) : nullptr)  // workplace component
                                      : obj;
        if (!target) return 0;
        for (int i = 0; i < c.a[0]; i++) {
            void* cand = nullptr;
            void* out = nullptr;
            Exe<void*(__fastcall*)(void*, void**, void*)>(kRvaFindWorker)(Mgr(), &cand, target);
            Exe<void*(__fastcall*)(void*, void**, void**, void*)>(kRvaAssignWorker)(Mgr(), &out, &cand, target);
        }
        for (int i = 0; i < -c.a[0]; i++) Exe<void(__fastcall*)(void*, void*)>(kRvaRemoveWorker)(Mgr(), target);
        return 1;
    }
    case kLimit:
        *(int*)(*(uint8_t**)((uint8_t*)Mgr() + 0x170) + (int64_t)c.a[0] * 0x20 + 8) = c.a[1];
        return 1;
    case kCrop: {
        void* crop = c.path == "-" ? nullptr : Resolve(c.path);
        int off = c.a[0] ? c.a[0] : 0x50;
        if (off != 0x50 && off != 0x60 && off != 0x28) return 0;
        bool start = *(void**)(obj + off) == nullptr && crop;
        if (c.a[1]) *(void**)(obj + off) = crop;  // pasture: the type is set before the state change
        if (start) ((void(__fastcall**)(void*, int))(*(void***)obj))[14](obj, 2);
        *(void**)(obj + off) = crop;
        if (c.a[1]) *(int*)(obj + 0x40) = Exe<int(__fastcall*)(void*)>(kRvaPastureDefault)(obj);
        return 1;
    }
    case kCallObj:
        if (c.a[1]) Exe<ObjIntFn>(c.rva)(obj, c.a[0]);
        else Exe<ObjFn>(c.rva)(obj);
        return 1;
    case kUpgrade:
        *(void**)(obj + 0x60) = Resolve(c.path);
        Exe<ObjFn>(kRvaUpgrade)(obj);
        return 1;
    case kVCall:
        ((void(__fastcall**)(void*, int))(*(void***)obj))[c.rva](obj, c.a[0]);
        return 1;
    case kSettle:
        return towns::Settle(c.player, c.a[0], c.a[1]) ? 1 : 0;
    case kSetInt:
        if (!obj || c.a[0] < 0 || c.a[0] > 0x400) return 0;
        *(int*)(obj + c.a[0]) = c.a[1];
        return 1;
    case kNomads:
        if (!obj) return 0;
        ExecuteNomads(obj, c.a[0]);
        return 1;
    case kMerchant:
        if (!obj) return 0;
        ExecuteMerchant(obj, c.path, c.a[0]);
        return 1;
    case kDeliverNow:
        towns::DeliverNow(c.player);
        return 1;
    case kDebugKill: {
        if (!obj) return 0;
        uint8_t* gl = GameLoop();
        uint8_t* mgr = *(uint8_t**)(gl + 0xe8);
        Exe<void(__fastcall*)(void*)>(0xc8370)(obj);  // EntityKill (runs in the owner's town: towns hook)
        void* ref = obj;
        Exe<void(__fastcall*)(void*, int, void**)>(0x3110)(mgr + 0xd0, *(int*)(mgr + 0xd8) + 1, &ref);  // dead list
        return 1;
    }
    case kShareStorage:
        if (!obj) return 0;
        towns::ShareStorage(c.player, obj, c.a[0], c.a[1] != 0);
        return 1;
    case kShareWork:
        if (!obj) return 0;
        towns::ShareWork(c.player, obj, c.a[0], c.a[1] != 0);
        return 1;
    case kWorkerMax:
        if (!obj) return 0;
        ExecuteWorkerMax(obj, c.a[0]);
        return 1;
    case kTradePost:
        if (!obj) return 0;
        return ExecuteTradePost(obj, c);
    case kAgree: {  // "give|ask|period"
        size_t b1 = c.path.find('|'), b2 = b1 == std::string::npos ? std::string::npos : c.path.find('|', b1 + 1);
        if (b2 == std::string::npos) return 0;
        std::string give = c.path.substr(0, b1), ask = c.path.substr(b1 + 1, b2 - b1 - 1);
        int period = atoi(c.path.c_str() + b2 + 1);
        void* gd = give == "-" ? nullptr : Resolve(give);
        void* ad = ask == "-" ? nullptr : Resolve(ask);
        return towns::AddAgreement((int)c.rva, gd, c.a[0], c.player, ad, c.a[1], period) ? 1 : 0;
    }
    case kAgreeEnd:
        return towns::EndAgreement(c.player, c.a[0]) ? 1 : 0;
    case kTrade: {
        size_t bar = c.path.find('|');
        std::string give = c.path.substr(0, bar), ask = bar == std::string::npos ? "-" : c.path.substr(bar + 1);
        void* gd = give == "-" ? nullptr : Resolve(give);
        void* ad = ask == "-" ? nullptr : Resolve(ask);
        const char* why = "";
        bool ok = towns::Trade((int)c.rva, gd, c.a[0], c.player, ad, c.a[1], &why);
        if (!ok) {
            Log("commands: trade between town %u and %d failed: %s", c.rva, c.player, why);
            // the offer window said "goods move at the next tick": tell both players it didn't happen
            if ((int)c.rva == towns::Local() || c.player == towns::Local()) net::SetTradeNews(std::string("trade failed: ") + why);
        }
        return ok ? 1 : 0;
    }
    }
    return 0;
}

template <int... Is> static void HookCallObjs(uintptr_t exe, std::integer_sequence<int, Is...>) {
    ((MH_CreateHook((void*)(exe + kCallObjHooks[Is].rva), (void*)h_CallObj<Is>, (void**)&o_CallObj[Is]) == MH_OK
          ? (void)MH_EnableHook((void*)(exe + kCallObjHooks[Is].rva))
          : Log("commands: hook %s failed", kCallObjHooks[Is].name)),
     ...);
}

static void HookAtRva(uintptr_t rva, void* detour, void** orig, const char* name) {
    if (MH_CreateHook((void*)(g_exe + rva), detour, orig) == MH_OK) MH_EnableHook((void*)(g_exe + rva));
    else Log("commands: hook %s failed", name);
}

static void InstallWindowHooks() {
    p_NumberGet = (NumGetFn)RuntimeExport("?GetValue@NumberEditElement@UI@@QEBAHXZ");
    HookAtRva(0x8ccb0, (void*)h_WorkPlaceUI, (void**)&o_WorkPlaceUI, "WorkPlaceUI");
    HookAtRva(0x7b890, (void*)h_Profession, (void**)&o_Profession, "Profession");
    HookAtRva(0x7ccb0, (void*)h_LimitUI, (void**)&o_LimitUI, "ResourceLimitUI");
    HookAtRva(0xb7df0, (void*)h_CropUI, (void**)&o_CropUI, "CropUI");
    HookAtRva(0x8c160, (void*)h_UpgradeUI, (void**)&o_UpgradeUI, "UpgradeUI");
    HookAtRva(0x60150, (void*)h_ControlUI, (void**)&o_ControlUI, "ControlUI");
    HookAtRva(0xb7370, (void*)h_BuildUI, (void**)&o_BuildUI, "BuildUI");
    HookAtRva(kRvaTownHallUI, (void*)h_TownHallUI, (void**)&o_TownHallUI, "TownHallUI");
    HookAtRva(kRvaConsumeProduceUI, (void*)h_ConsumeProduceUI, (void**)&o_ConsumeProduceUI, "ConsumeProduceUI");
    HookAtRva(kRvaPastureUI, (void*)h_PastureUI, (void**)&o_PastureUI, "PastureUI");
    HookAtRva(kRvaImmigrationUI, (void*)h_ImmigrationUI, (void**)&o_ImmigrationUI, "ImmigrationUI");
    HookAtRva(kRvaTradeUI, (void*)h_TradeUI, (void**)&o_TradeUI, "TradeUI");
    p_SpinnerSet = (SpinnerSet_t)RuntimeExport("?SetValue@SpinnerElement@UI@@QEAAXH@Z");
    HookAtRva(kRvaFieldButton, (void*)h_FieldButton, (void**)&o_FieldButton, "FieldButton");
    p_SpinnerGet = (SpinnerGet_t)RuntimeExport("?GetValue@SpinnerElement@UI@@QEBAHXZ");
    HookAtRva(0x82a20, (void*)h_ForeignView<&o_TownHallVf7>, (void**)&o_TownHallVf7, "TownHallVf7");
    HookAtRva(0x83a20, (void*)h_ForeignView<&o_TownHallVf8>, (void**)&o_TownHallVf8, "TownHallVf8");
    HookAtRva(0x7cda0, (void*)h_ForeignView<&o_LimitVf7>, (void**)&o_LimitVf7, "LimitVf7");
    HookAtRva(kRvaWorkerMaxUI, (void*)h_WorkerMaxUI, (void**)&o_WorkerMaxUI, "WorkerMaxUI");
    HookAtRva(kRvaOrchardUI, (void*)h_OrchardUI, (void**)&o_OrchardUI, "OrchardUI");
    HookAtRva(kRvaPastureTypeUI, (void*)h_PastureTypeUI, (void**)&o_PastureTypeUI, "PastureTypeUI");
    HookAtRva(0x86e20, (void*)h_TradeSendAway, (void**)&o_TradeSendAway, "TradeSendAway");
    HookAtRva(0x87090, (void*)h_TradeCombo2, (void**)&o_TradeCombo2, "TradeCombo2");
    HookAtRva(0x87350, (void*)h_TradeCombo3, (void**)&o_TradeCombo3, "TradeCombo3");
    HookAtRva(0x872a0, (void*)h_TradeOrderCheck, (void**)&o_TradeOrderCheck, "TradeOrderCheck");
    HookAtRva(0x871d0, (void*)h_TradeSwap, (void**)&o_TradeSwap, "TradeSwap");
    HookAtRva(0x87120, (void*)h_TradeOrderAmount, (void**)&o_TradeOrderAmount, "TradeOrderAmount");
    HookAtRva(0x86f80, (void*)h_TradeKeep, (void**)&o_TradeKeep, "TradeKeep");
    HookCallObjs(g_exe, std::make_integer_sequence<int, 4>{});
    Log("commands: window action hooks installed");
}

// One text format for the command log, replay files and the network: "<slot> <text>" where text is:
//   place <toolType> <template> x0 y0 x1 y1 offX offY offZ rot variant rotIndex
//   dragN <toolType> <template> start end rotIndex fence0 fence1
//   win <kind> <obj> <rva> a0 a1 <path> <objType>
static std::string Format(const Command& c) {
    char buf[1024];
    if (c.kind >= kWorker)
        sprintf_s(buf, "win %d %llx %x %d %d %s %x", c.kind, (unsigned long long)c.obj, c.rva, c.a[0], c.a[1],
                  c.path.empty() ? "-" : c.path.c_str(), c.objType);
    else if (c.kind == kPlace)
        sprintf_s(buf, "place %u %s %d %d %d %d %a %a %a %a %d %d", c.toolType, c.desc.c_str(), c.a[0], c.a[1], c.a[2],
                  c.a[3], c.offset[0], c.offset[1], c.offset[2], c.rotation, c.variant, c.rotIndex);
    else
        sprintf_s(buf, "drag%d %u %s %d %d %d %d %d", c.kind - kDragBase, c.toolType, c.desc.c_str(), c.a[0], c.a[1],
                  c.rotIndex, c.fence[0], c.fence[1]);
    return buf;
}

static bool Parse(const char* line, Command& c) {
    c = Command{};
    unsigned long long slot;
    {
        int wk, a0, a1;
        unsigned long long obj;
        unsigned rva, objType = 0;
        char path[512];
        if (sscanf_s(line, "%llu win %d %llx %x %d %d %511s %x", &slot, &wk, &obj, &rva, &a0, &a1, path,
                     (unsigned)sizeof path, &objType) >= 7) {
            c.slot = slot; c.kind = wk; c.obj = obj; c.rva = rva; c.a[0] = a0; c.a[1] = a1; c.path = path;
            c.objType = objType;
            return true;
        }
    }
    char kind[16], desc[512];
    if (sscanf_s(line, "%llu %15s %u %511s", &slot, kind, (unsigned)sizeof kind, &c.toolType, desc, (unsigned)sizeof desc) != 4)
        return false;
    c.slot = slot;
    c.desc = desc;
    const char* rest = strstr(line, desc) + strlen(desc);
    c.rotIndex = -1;
    if (strcmp(kind, "place") == 0) {
        c.kind = kPlace;
        return sscanf_s(rest, "%d %d %d %d %a %a %a %a %d %d", &c.a[0], &c.a[1], &c.a[2], &c.a[3], &c.offset[0],
                        &c.offset[1], &c.offset[2], &c.rotation, &c.variant, &c.rotIndex) >= 9;
    }
    if (strncmp(kind, "drag", 4) == 0) {
        c.kind = kDragBase + atoi(kind + 4);
        return sscanf_s(rest, "%d %d %d %d %d", &c.a[0], &c.a[1], &c.rotIndex, &c.fence[0], &c.fence[1]) >= 2;
    }
    return false;
}

static void LogCommand(const Command& c, uint64_t slot, int result) {
    if (!g_log) return;
    fprintf(g_log, "%llu %s -> %d\n", slot, Format(c).c_str(), result);
    fflush(g_log);
}

static void Execute(const Command& cmd, uint64_t slot) {
    Command c = cmd;
    if (c.player < 0) c.player = net::TownOfPlayer(-1 - c.player);  // network command: -1 - player id
    towns::Scope town(c.player);
    towns::TablesChangedBySim();
    if (c.kind >= kWorker) {
        g_executing = true;
        int r = ExecuteWindow(c);
        g_executing = false;
        LogCommand(c, slot, r);
        return;
    }
    void* desc = Resolve(c.desc);
    if (!desc) {
        Log("!! commands: cannot resolve template '%s'", c.desc.c_str());
        return;
    }
    void* tool = ReplayTool(c.toolType, desc);
    uint8_t* setting = *(uint8_t**)((uint8_t*)tool + 0x18);
    int savedRot = setting ? *(int*)(setting + 0x10) : 0;
    if (setting && c.rotIndex >= 0) *(int*)(setting + 0x10) = c.rotIndex;  // shared with the UI: restored below
    uint64_t r;
    g_executing = true;
    if (c.kind == kPlace) {
        *(int*)((uint8_t*)tool + 0x64) = c.variant;  // the variant the player saw when clicking
        float offset[3];
        memcpy(offset, c.offset, sizeof offset);
        r = o_Place(tool, c.a[0], c.a[1], c.a[2], c.a[3], offset, c.rotation);
    } else {
        *(int*)((uint8_t*)tool + 0x2c) = c.a[0];  // DragTool keeps the drag start/end on the tool too
        *(int*)((uint8_t*)tool + 0x30) = c.a[1];
        if (c.kind - kDragBase == kFencedIndex) {
            *(int*)((uint8_t*)tool + 0x5c) = c.fence[0];
            *(int*)((uint8_t*)tool + 0x60) = c.fence[1];
        }
        if (c.kind - kDragBase == 0) towns::ClaimNaturals(c.player, c.a[0], c.a[1]);  // removal marks: player's work
        r = o_Drag[c.kind - kDragBase](tool, c.a[0], c.a[1]);
    }
    g_executing = false;
    if (setting) *(int*)(setting + 0x10) = savedRot;
    LogCommand(c, slot, (int)(uint8_t)r);
}

void OnTickStart(uint64_t tick) {
    auto range = g_replay.equal_range(tick);
    for (auto it = range.first; it != range.second; ++it) Execute(it->second, tick);
    g_replay.erase(range.first, range.second);
    while (!g_queue.empty() && g_queue.front().slot <= tick) {
        Command c = std::move(g_queue.front());
        g_queue.pop_front();
        Execute(c, tick);
    }
}

std::string DescPath(const void* desc) { return desc ? PathOf(desc) : "-"; }
void* DescFromPath(const std::string& path) { return Resolve(path); }
std::string DescName(const void* desc) {
    std::string p = PathOf(desc);  // e.g. Template\RawMaterialLogs.rsc:resource
    size_t s = p.find_last_of("\\/"), e = p.find('.', s == std::string::npos ? 0 : s);
    std::string n = p.substr(s == std::string::npos ? 0 : s + 1, e == std::string::npos ? std::string::npos : e - (s + 1));
    if (n.compare(0, 11, "RawMaterial") == 0 && n.size() > 11) n = n.substr(11);
    std::string out;  // "IronTools" -> "Iron Tools"
    for (size_t i = 0; i < n.size(); i++) {
        if (i && isupper((unsigned char)n[i]) && islower((unsigned char)n[i - 1])) out += ' ';
        out += n[i];
    }
    return out;
}

// Overlay: an open game window of one of the local town's storage buildings (UserInterface GameLoop+0x98: list node
// {+0 dialog, +0x10 next}, dialog controllers +0x78/+0x80, controller +0x28 = entity) and its top-left corner in
// screen pixels (dialog +0x88/+0x8c in UI units x UserInterface+0xdc scale). Main thread.
void* OpenStorageWindow(float* x, float* y) {
    uint8_t* gl = GameLoop();
    uint8_t* ui = gl ? *(uint8_t**)(gl + 0x98) : nullptr;
    if (!ui || towns::Count() < 2) return nullptr;
    float scale = *(float*)(ui + 0xdc) > 0 ? 1.0f / *(float*)(ui + 0xdc) : 1.0f;  // UI units -> pixels (UI scale 0.9 = units x 1.11)
    void* found = nullptr;
    for (uint8_t* node = *(uint8_t**)ui; node; node = *(uint8_t**)(node + 0x10)) {
        uint8_t* dlg = *(uint8_t**)node;
        int n = dlg ? *(int*)(dlg + 0x80) : 0;
        bool modButton = false;  // the BMP data mod's "Share" checkbox (ControlUI on the StorageComponent) is there
        for (int i = 0; i < n && i < 64; i++) {
            uint8_t* c = (*(uint8_t***)(dlg + 0x78))[i];
            if (c && VtableRva(c) == 0x1614b0 && *(void**)(c + 0x38) && VtableRva(*(void**)(c + 0x38)) == 0x16ab50)
                modButton = true;
        }
        if (modButton) continue;
        for (int i = 0; i < n && i < 64; i++) {
            uint8_t* c = (*(uint8_t***)(dlg + 0x78))[i];
            void* e = c ? *(void**)(c + 0x28) : nullptr;
            if (!e || towns::OwnerOf(e) != towns::Local() || !towns::IsOwnStorage(e)) continue;
            found = e;  // the last one in the list = the newest window
            *x = *(int*)(dlg + 0x88) * scale;
            *y = *(int*)(dlg + 0x8c) * scale;
            break;
        }
    }
    return found;
}
// the newest open game window of one of the local town's workplaces (overlay: "let X work here" panel above it)
void* OpenWorkplaceWindow(float* x, float* y) {
    uint8_t* gl = GameLoop();
    uint8_t* ui = gl ? *(uint8_t**)(gl + 0x98) : nullptr;
    if (!ui || towns::Count() < 2) return nullptr;
    float scale = *(float*)(ui + 0xdc) > 0 ? 1.0f / *(float*)(ui + 0xdc) : 1.0f;  // UI units -> pixels (UI scale 0.9 = units x 1.11)
    void* found = nullptr;
    for (uint8_t* node = *(uint8_t**)ui; node; node = *(uint8_t**)(node + 0x10)) {
        uint8_t* dlg = *(uint8_t**)node;
        int n = dlg ? *(int*)(dlg + 0x80) : 0;
        for (int i = 0; i < n && i < 64; i++) {
            uint8_t* c = (*(uint8_t***)(dlg + 0x78))[i];
            void* e = c ? *(void**)(c + 0x28) : nullptr;
            if (!e || !towns::IsOwnWorkplace(e)) continue;
            found = e;
            *x = *(int*)(dlg + 0x88) * scale;
            *y = *(int*)(dlg + 0x8c) * scale;
            break;
        }
    }
    return found;
}
void RequestShareWork(void* building, int town, bool on) {
    Command c{};
    if (!ObjRef(building, &c.obj)) { Log("!! commands: workplace not in the sim heap"); return; }
    c.kind = kShareWork;
    c.a[0] = town;
    c.a[1] = on ? 1 : 0;
    c.path = "-";
    EnqueueWin(std::move(c));
}void RequestShareStorage(void* storage, int town, bool on) {
    Command c{};
    if (!ObjRef(storage, &c.obj)) { Log("!! commands: storage building not in the sim heap"); return; }
    c.kind = kShareStorage;
    c.a[0] = town;
    c.a[1] = on ? 1 : 0;
    c.path = "-";
    EnqueueWin(std::move(c));
}
void RequestTrade(int partnerTown, const std::string& givePath, int nGive, const std::string& askPath, int nAsk) {
    Command c{};
    c.kind = kTrade;
    c.rva = (uint32_t)partnerTown;
    c.a[0] = nGive;
    c.a[1] = nAsk;
    c.path = (givePath.empty() ? "-" : givePath) + "|" + (askPath.empty() ? "-" : askPath);
    Enqueue(std::move(c));
}

void RequestAgreement(int partnerTown, const std::string& givePath, int nGive, const std::string& askPath, int nAsk, int period) {
    Command c{};
    c.kind = kAgree;
    c.rva = (uint32_t)partnerTown;
    c.a[0] = nGive;
    c.a[1] = nAsk;
    c.path = (givePath.empty() ? "-" : givePath) + "|" + (askPath.empty() ? "-" : askPath) + "|" + std::to_string(period);
    Enqueue(std::move(c));
}
void RequestEndAgreement(int id) {
    Command c{};
    c.kind = kAgreeEnd;
    c.a[0] = id;
    c.path = "-";
    Enqueue(std::move(c));
}

void RequestDeliverNow() {
    Command c{};
    c.kind = kDeliverNow;
    c.path = "-";
    Enqueue(std::move(c));
}

void RequestSettle(int x, int y) {
    Command c{};
    c.kind = kSettle;
    c.a[0] = x;
    c.a[1] = y;
    c.path = "-";
    Enqueue(std::move(c));
}

void ExecuteAllNow(uint64_t tick) {
    // paused: nothing happens until the next tick starts, so this is equivalent to (and logged as) its start
    while (!g_queue.empty()) {
        Command c = std::move(g_queue.front());
        g_queue.pop_front();
        Execute(c, tick + 1);
    }
}

static std::deque<std::string> g_netOutbox;  // replay lines to send through the relay once it says START

void FlushNetOutbox() {
    while (!g_netOutbox.empty()) {
        const std::string& l = g_netOutbox.front();
        size_t sp = l.find(' ');
        net::SendCommand(strtoull(l.c_str(), nullptr, 10), l.substr(sp + 1));
        g_netOutbox.pop_front();
    }
}

// A player action given as command text ("place ...", "dragN ...", "win ..."): sent / queued exactly like one made
// with the mouse (test scripts).
bool Submit(const std::string& text) {
    Command c;
    std::string line = "0 " + text;
    if (!Parse(line.c_str(), c)) { Log("!! commands: Submit: unparsable '%s'", text.c_str()); return false; }
    Enqueue(std::move(c));
    return true;
}

bool ScheduleLine(const char* line, int player) {
    Command cmd;
    if (!Parse(line, cmd)) {
        Log("!! commands: unparsable command line: %s", line);
        return false;
    }
    cmd.player = player;
    g_replay.emplace(cmd.slot, std::move(cmd));  // same-slot commands keep arrival (= relay) order
    return true;
}

bool LoadReplay(const wchar_t* path) {
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"r");
    if (!f) return false;
    char line[1024];
    int n = 0;
    while (fgets(line, sizeof line, f)) {
        Command cmd;
        if (line[0] == '#' || !Parse(line, cmd)) continue;
        if (net::Active()) {
            char text[1100];
            sprintf_s(text, "%llu %s", (unsigned long long)cmd.slot, Format(cmd).c_str());
            g_netOutbox.push_back(text);
            n++;
            continue;
        }
        g_replay.emplace(cmd.slot, std::move(cmd));
        n++;
    }
    fclose(f);
    Log("commands: loaded %d replay commands from %ls", n, path);
    return true;
}

template <int... Is> static void HookDrags(uintptr_t exe, std::integer_sequence<int, Is...>) {
    ((MH_CreateHook((void*)(exe + kDrag[Is].rva), (void*)h_Drag<Is>, (void**)&o_Drag[Is]) == MH_OK
          ? (void)MH_EnableHook((void*)(exe + kDrag[Is].rva))
          : Log("commands: hook drag %s failed", kDrag[Is].name)),
     ...);
}

void Install(uintptr_t exe, const wchar_t* logPath) {
    g_exe = exe;
    g_log = _wfsopen(logPath, L"w", _SH_DENYNO);
    if (g_log) fprintf(g_log, "# slot place toolType template x0 y0 x1 y1 offX offY offZ rot variant rotIndex -> ok | slot dragN toolType template start end rotIndex fence0 fence1 -> ok\n");
    p_GetPath = (GetPathFn)RuntimeExport("?GetPath@External@Object@@QEBA?AVFilename@System@@XZ");
    p_FindOrLoad = (FindOrLoadFn)RuntimeExport("?FindOrLoad@ExternalFactory@Object@@QEAAPEBVExternal@2@AEBVFilename@System@@@Z");
    p_ExternalFactoryGet = (SingletonFn)RuntimeExport("?Get@ExternalFactory@Object@@SAPEAV12@XZ");
    p_FactoryGet = (SingletonFn)RuntimeExport("?Get@Factory@Object@@SAPEAV12@XZ");
    p_CreateObject = (CreateObjectFn)RuntimeExport("?CreateObject@Factory@Object@@QEBAPEAVBase@2@I@Z");
    p_FilenameCtor = (FilenameCtorFn)RuntimeExport("??0Filename@System@@QEAA@PEBD@Z");
    p_StringDtor = (StringDtorFn)RuntimeExport("??1String@System@@QEAA@XZ");
    if (MH_CreateHook((void*)(exe + kRvaPlaceBuilding), (void*)h_Place, (void**)&o_Place) == MH_OK)
        MH_EnableHook((void*)(exe + kRvaPlaceBuilding));
    else
        Log("commands: hook PlaceBuilding failed");
    HookDrags(exe, std::make_integer_sequence<int, kDragCount>{});
    InstallWindowHooks();
    Log("commands: installed (placement + %d drag tools), delay %llu tick(s)", kDragCount, kDelayTicks);
}

}  // namespace commands
