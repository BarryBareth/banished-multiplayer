// UI test scripts (see uiscript.h). One line per frame once the sim tick reaches its slot; " ; " joins steps that must
// run in the same frame. Steps:
//   list [VTRVA]                  log buildings: cell, size, window, owner, component vtables (only those with VTRVA)
//   known                         log the local town's known seeds / saplings / livestock
//   build PATH X Y [ROT]          place like the placement tool (command over the network like a click)
//   build PATH X Y X2 Y2          drag-placed area (fields, orchards) / fenced pasture
//   build PATH near X Y [W H]     at the nearest free spot (remembered as @Name for open)
//   cmd TEXT                      submit a raw command ("win 10 0 0 5 1 - 0" = 5 more builders)
//   open X Y | open @Name         open the building's window like the game does (exe+0x1073b0 recipe: DialogInstance
//                                 from the template's UIDescription, window vtable exe+0x1659b0, AddDialog)
//   close                         close the windows the script opened
//   windows                       log every open window's controllers
//   press CLASS EVT ID            controller CLASS of the opened building's window: vf9(ui, &elem, EVT, ID) - what a
//                                 button click delivers (EVT 0x12 click, 0x14 combo, 0x15 number/spinner/check)
//   pick CLASS PATH               select-resource controllers (CropUI / OrchardUI / PastureUI): vf11(ui, description)
//   spin|number|check CLASS OFF STRIDE ROW FIELD VALUE   element at *(*(ui+OFF) + ROW*STRIDE + FIELD) := VALUE
//   state                         log the opened building's simulation values (workers, limit, crop, tree, animal)
//   expect TEXT                   "UITEST PASS/FAIL": the last state line contains TEXT
//   cursor X Y | cursor off       presence: my cursor points at cell X,Y (sent like a real one) / back to the mouse
//   ping X Y                      presence: ping cell X,Y (like Alt+click)
//   cursor focus X Y              the camera flies to cell X,Y (like a ping's "go there" button)
// CLASS = controller class (WorkPlaceUI, TownHallUI, TradeUI, ...); "any" after it: any open window of that class.
#include "report.h"
#include "presence.h"
#include "uiscript.h"
#include "common.h"
#include "autotest.h"
#include "towns.h"
#include "commands.h"
#include "simheap.h"
#include "gameload.h"
#include "overlay.h"
#include "ingame.h"
#include "net.h"
#include <string>
#include <vector>
#include <cstring>

namespace uiscript {

struct Line { uint64_t tick; std::string text; };
static std::vector<Line> g_lines;
static size_t g_next;
static uintptr_t g_exe;
static void* g_entity;       // window target of the last "open"
static uint64_t g_session;

struct ClassVt { const char* name; uintptr_t vt; };
static const ClassVt kClasses[] = {
    {"WorkPlaceUI", 0x164450}, {"TownHallUI", 0x164100},   {"TradeUI", 0x1642a0},     {"CropUI", 0x165680},
    {"OrchardUI", 0x163360},   {"PastureUI", 0x165a20},    {"ResourceLimitUI", 0x163620}, {"ControlUI", 0x1614b0},
    {"UpgradeUI", 0x164320},   {"BuildUI", 0x165620},      {"ConsumeProduceUI", 0x161410}, {"ImmigrationUI", 0x1621c0},
    {"ProfessionUI", 0x163520}, {"StorageUI", 0x165d80},   {"DestroyUI", 0x161c30},   {"ResidenceUI", 0x165ae0},
    {"EntityUI", 0x161cc0},    {"CitizenUI", 0x161b30},    {"StorageCapacityUI", 0x163d90}, {"StatusIconUI", 0x163cd0},
    {"ToolUI", 0x163e90},      {"EventLogUI", 0x161d20},
};
static const uintptr_t kVtComponentEntity = 0x165e88;

static std::wstring g_scriptPath;  // the script file (screenshots go next to it, in shots\)
bool Load(const wchar_t* path) {
    g_scriptPath = path;
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"r");
    if (!f) { Log("!! uiscript: cannot open %ls", path); return false; }
    char buf[512];
    while (fgets(buf, sizeof buf, f)) {
        char* s = buf;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == '\r' || *s == '\n' || !*s) continue;
        char* end = nullptr;
        uint64_t t = strtoull(s, &end, 10);
        while (*end == ' ') end++;
        std::string text = end;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        g_lines.push_back({t, text});
    }
    fclose(f);
    g_exe = (uintptr_t)GetModuleHandleW(nullptr);
    Log("uiscript: %zu steps from %ls", g_lines.size(), path);
    return true;
}

struct Rect { int x0, y0, x1, y1; };
static std::vector<Rect> g_reserved;  // spots taken by earlier build steps
static std::vector<std::pair<std::string, Rect>> g_built;  // template short name -> where the script built it
void OnNewSession() { g_entity = nullptr; g_reserved.clear(); }

static bool Readable(const void* p, size_t n = 8) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    return (uintptr_t)p + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize && !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
}
static uintptr_t VtRva(const void* p) {
    if (!Readable(p)) return 0;
    uintptr_t vt = *(const uintptr_t*)p;
    return vt > g_exe && vt < g_exe + 0x300000 ? SteamRva(vt - g_exe) : 0;  // Steam RVA on every build
}
static bool IsEntity(const void* p) { return VtRva(p) == kVtComponentEntity; }
static uint8_t* GameLoop() { return (uint8_t*)ProbeGameLoop(); }

static bool EntityCell(uint8_t* e, int* x, int* y) {  // MapComponent (exe+0x4680): +0x28/+0x2a cell
    void* map = nullptr;
    ((void(__fastcall*)(void*, void**))(g_exe + Rva(0x4680)))(e, &map);
    if (!map) return false;
    *x = *(short*)((uint8_t*)map + 0x28);
    *y = *(short*)((uint8_t*)map + 0x2a);
    return true;
}

static uintptr_t ClassVtable(const std::string& name) {
    for (auto& c : kClasses) if (name == c.name) return c.vt;
    return 0;
}

// open windows: UserInterface (GameLoop+0x98) -> list node {+0 dialog, +0x10 next}; dialog controllers +0x78/+0x80
template <class F> static void ForEachController(F f) {
    uint8_t* gl = GameLoop();
    uint8_t* ui = gl ? *(uint8_t**)(gl + 0x98) : nullptr;
    if (!ui) return;
    for (uint8_t* node = *(uint8_t**)ui; node; node = *(uint8_t**)(node + 0x10)) {
        uint8_t* dlg = *(uint8_t**)node;
        if (!Readable(dlg, 0x88)) continue;
        int n = *(int*)(dlg + 0x80);
        for (int i = 0; i < n && i < 64; i++) {
            uint8_t* c = (*(uint8_t***)(dlg + 0x78))[i];
            if (c && f(dlg, c)) return;
        }
    }
}

static uint8_t* FindController(const std::string& cls, bool any) {
    uintptr_t vt = ClassVtable(cls);
    if (!vt) { Log("!! uiscript: unknown controller class %s", cls.c_str()); return nullptr; }
    uint8_t* found = nullptr;
    ForEachController([&](uint8_t*, uint8_t* c) {
        if (VtRva(c) != vt) return false;
        if (!any && g_entity && *(void**)(c + 0x28) != g_entity) return false;
        found = c;
        return true;
    });
    if (!found) Log("!! uiscript: no open %s window%s", cls.c_str(), any ? "" : " for the opened building");
    return found;
}

static std::string ToHex(unsigned v) { char b[16]; sprintf_s(b, "%x", v); return b; }
static void List(unsigned only = 0) {  // only: just entities with a component of this vtable rva (list 15fd60 = markets)
    uint8_t* gl = GameLoop();
    uint8_t* mgr = gl ? *(uint8_t**)(gl + 0xe8) : nullptr;
    if (!mgr) return;
    int n = *(int*)(mgr + 0x10), shown = 0;
    for (int i = 0; i < n; i++) {
        uint8_t* e = (*(uint8_t***)(mgr + 0x08))[i];
        if (!e || (*(uint32_t*)(e + 0x40) & 0x400)) continue;
        int comps = *(int*)(e + 0x38);
        char line[600];
        int k = 0, x = -1, y = -1;
        bool building = false;
        for (int c = 0; c < comps && k < 500; c++) {
            uintptr_t vt = VtRva((*(void***)(e + 0x30))[c]);
            k += sprintf_s(line + k, sizeof line - k, " %x", (unsigned)vt);
            // WorkPlace / Field / Orchard / Pasture / Trade / TownHall / Storage etc. all carry a window; natural
            // resources and villagers are skipped below by count of components
        }
        if (comps >= 4) building = true;
        if (only && !strstr(line, (" " + ToHex(only)).c_str())) continue;
        if (!building || !EntityCell(e, &x, &y)) continue;
        void* mc = nullptr;
        ((void(__fastcall*)(void*, void**))(g_exe + Rva(0x4680)))(e, &mc);
        int uiType = *(int*)(g_exe + Rva(0x1cffc8)), hasUi = 0;
        for (int c = 0; c < comps; c++) {
            uint8_t* comp = (*(uint8_t***)(e + 0x30))[c];
            hasUi |= ((int(__fastcall*)(void*))(*(void***)comp)[2])(comp) == uiType;
        }
        Log("uiscript: entity %d at %d,%d size %dx%d ui %d town %d comps%s", i, x, y, mc ? *(short*)((uint8_t*)mc + 0x2c) : -1,
            mc ? *(short*)((uint8_t*)mc + 0x2e) : -1, hasUi, towns::OwnerOfObject(e), line);
        if (++shown >= 400) break;
    }
}

// The template's placement sub-description (template +0x28 array of descriptions, +0x30 count; type id via vf2).
static uint8_t* SubDesc(uint8_t* tpl, uintptr_t typeinfoRva) {
    int want = *(int*)(g_exe + Rva(typeinfoRva)), n = *(int*)(tpl + 0x30);
    for (int i = 0; i < n; i++) {
        uint8_t* d = (*(uint8_t***)(tpl + 0x28))[i];
        if (((int(__fastcall*)(void*))(*(void***)d)[2])(d) == want) return d;
    }
    return nullptr;
}
// The building covering cell x,y: the entity whose MapComponent footprint (+0x28 x, +0x2a y, +0x2c width,
// +0x2e height) contains it and that has a window (UIComponent); the smallest such footprint wins.
static void* EntityAt(int x, int y) {
    uint8_t* gl = GameLoop();
    uint8_t* mgr = gl ? *(uint8_t**)(gl + 0xe8) : nullptr;
    if (!mgr) return nullptr;
    int uiType = *(int*)(g_exe + Rva(0x1cffc8));
    void* best = nullptr;
    int bestArea = 1 << 30;
    for (int i = 0; i < *(int*)(mgr + 0x10); i++) {
        uint8_t* e = (*(uint8_t***)(mgr + 0x08))[i];
        if (!e || (*(uint32_t*)(e + 0x40) & 0x400)) continue;
        (void)uiType;
        if (!*(uint8_t**)(e + 0x28) || !SubDesc(*(uint8_t**)(e + 0x28), 0x1cff90)) continue;  // no window (UIDescription)
        void* map = nullptr;
        ((void(__fastcall*)(void*, void**))(g_exe + Rva(0x4680)))(e, &map);
        if (!map) continue;
        short x0 = *(short*)((uint8_t*)map + 0x28), y0 = *(short*)((uint8_t*)map + 0x2a);
        short w = *(short*)((uint8_t*)map + 0x2c), h = *(short*)((uint8_t*)map + 0x2e);
        if (w <= 0 || h <= 0 || x < x0 || y < y0 || x >= x0 + w || y >= y0 + h) continue;
        if (w * h < bestArea) { bestArea = w * h; best = e; }
    }
    return best;
}
static const uintptr_t kTiCreatePlaced = 0x1cf468, kTiCreateDrag = 0x1cf430, kTiCreateFenced = 0x1cc100;
static const uintptr_t kTiPlacementTool = 0x1cee10, kTiDragPlacementTool = 0x1ced68, kTiFencedTool = 0x1ce358;

// Nearest free rectangle (wx x wy cells plus a 1-cell margin) around cx,cy: no object on a cell (+0/+8), open land
// (+0x29 == 2), one terrain level (+0x26) - as the trade piles use. Rectangles picked earlier in the script are skipped
// (their commands may not have run yet).
static bool FindFree(int cx, int cy, int wx, int wy, int* ox0, int* oy0) {
    int* map = *(int**)(GameLoop() + 0xf8);
    int size = *map;
    uint8_t* cells = *(uint8_t**)((uint8_t*)map + 0x80);
    auto ok = [&](int x0, int y0) {
        if (x0 < 2 || y0 < 2 || x0 + wx + 2 >= size || y0 + wy + 2 >= size) return false;
        for (auto& r : g_reserved)
            if (x0 - 1 <= r.x1 && x0 + wx >= r.x0 && y0 - 1 <= r.y1 && y0 + wy >= r.y0) return false;
        int level = cells[((int64_t)y0 * size + x0) * 0x30 + 0x26];
        for (int y = y0 - 1; y <= y0 + wy; y++)
            for (int x = x0 - 1; x <= x0 + wx; x++) {
                uint8_t* c = cells + ((int64_t)y * size + x) * 0x30;
                if (*(void**)c || *(void**)(c + 8) || c[0x29] != 2 || c[0x26] != level) return false;
            }
        return true;
    };
    for (int r = 0; r < 80; r++)
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++) {
                if (dx != -r && dx != r && dy != -r && dy != r) continue;  // ring only
                int x0 = cx + dx - wx / 2, y0 = cy + dy - wy / 2;
                if (ok(x0, y0)) { *ox0 = x0; *oy0 = y0; g_reserved.push_back({x0, y0, x0 + wx - 1, y0 + wy - 1}); return true; }
            }
    return false;
}

// The game's placement check (exe+0xa2a50) with the arguments PlacementTool's commit (exe+0x102b20) passes: tool+0x28
// = placed description (+0x20 int, +0x30, +0x28, +0x50), tool+0x30 -> +0x28, rotation in degrees.
static bool PlacementValid(uint8_t* tool, int x0, int y0, int x1, int y1, int rotDeg) {
    using Valid_t = uint64_t(__fastcall*)(void*, int, int, int, int, uint32_t, void*, void*, uint32_t, uint32_t, uint32_t,
                                          uint8_t, void*, int, uint64_t);
    uint8_t* placed = *(uint8_t**)(tool + 0x28);
    uint8_t* other = *(uint8_t**)(tool + 0x30);
    if (!placed || !other) return false;
    uint32_t f = *(uint32_t*)(g_exe + Rva(0x178948));
    void* map = *(void**)(GameLoop() + 0xf8);
    return (uint8_t)((Valid_t)(g_exe + Rva(0xa2a50)))(map, x0, y0, x1 - x0 + 1, y1 - y0 + 1, *(uint32_t*)(placed + 0x20),
                                               placed + 0x30, placed + 0x28, 0, f, 0, 0, other + 0x28, rotDeg,
                                               *(uint64_t*)(placed + 0x50)) != 0;
}
// build PATH X Y [ROT]          placed building centred on cell X,Y like the placement tool (exe+0x102ed0): width
//                               +0x44 / height +0x48 of CreatePlacedDescription (swapped for rotation 1/3)
// build PATH X Y X2 Y2          drag-placed area (fields, orchards) or fenced pasture from cell X,Y to X2,Y2
// build PATH near X Y [W H]     the same at the nearest free spot around X,Y (W x H for areas, default 6 x 6)
static void Build(const std::string& text) {
    char path[256] = "", word[16] = "";
    int x = 0, y = 0, p3 = -1, p4 = -1;
    int got = sscanf_s(text.c_str(), "%*s %255s %d %d %d %d", path, (unsigned)sizeof path, &x, &y, &p3, &p4);
    bool nearby = sscanf_s(text.c_str(), "%*s %*s %15s", word, (unsigned)sizeof word) == 1 && (std::string(word) == "near" || std::string(word) == "valid");
    bool validated = std::string(word) == "valid";
    if (nearby) {
        p3 = p4 = -1;
        got = sscanf_s(text.c_str(), "%*s %*s %*s %d %d %d %d", &x, &y, &p3, &p4) + 1;
    }
    uint8_t* tpl = (uint8_t*)commands::DescFromPath(path);
    if (!tpl) { Log("!! uiscript: build: unknown template %s", path); return; }
    int* map = *(int**)(GameLoop() + 0xf8);
    int size = *map;
    char cmd[512];
    uint8_t* placed = SubDesc(tpl, kTiCreatePlaced);
    bool drag = !placed && SubDesc(tpl, kTiCreateDrag), fenced = !placed && !drag && SubDesc(tpl, kTiCreateFenced);
    if (!placed && !drag && !fenced) { Log("!! uiscript: build: %s has no placement description I know", path); return; }
    int x0, y0, x1, y1, rot = 0;
    if (placed) {
        rot = !nearby && got >= 4 ? p3 & 3 : 0;
        int w = *(int*)(placed + 0x44), h = *(int*)(placed + 0x48);
        int wx = (rot & 1) ? h : w, wy = (rot & 1) ? w : h;
        if (validated) {  // the game decides: spiral around X,Y, all four rotations
            uint8_t* tool = (uint8_t*)commands::ToolFor(*(uint32_t*)(g_exe + Rva(kTiPlacementTool)), path);
            bool found = false;
            for (int r = 0; r < 70 && !found && tool; r++)
                for (int dy = -r; dy <= r && !found; dy++)
                    for (int dx = -r; dx <= r && !found; dx++) {
                        if (dx != -r && dx != r && dy != -r && dy != r) continue;
                        for (int ro = 0; ro < 4 && !found; ro++) {
                            int ax = (ro & 1) ? h : w, ay = (ro & 1) ? w : h;
                            int bx = x + dx - ax / 2, by = y + dy - ay / 2;
                            bool clash = false;
                            for (auto& q : g_reserved)
                                clash |= bx - 1 <= q.x1 && bx + ax >= q.x0 && by - 1 <= q.y1 && by + ay >= q.y0;
                            if (clash || !PlacementValid(tool, bx, by, bx + ax - 1, by + ay - 1, ro * 90)) continue;
                            found = true;
                            rot = ro; wx = ax; wy = ay; x0 = bx; y0 = by;
                            g_reserved.push_back({bx, by, bx + ax - 1, by + ay - 1});
                        }
                    }
            if (!found) { Log("!! uiscript: build %s: the game accepts no spot near %d,%d", path, x, y); return; }
        } else if (nearby) {
            if (!FindFree(x, y, wx, wy, &x0, &y0)) { Log("!! uiscript: build %s: no free %dx%d spot near %d,%d", path, wx, wy, x, y); return; }
        } else {
            x0 = (wx & 1) ? x - (wx - 1) / 2 : x - wx / 2 + 1;
            y0 = (wy & 1) ? y - (wy - 1) / 2 : y - wy / 2 + 1;
        }
        x1 = x0 + wx - 1;
        y1 = y0 + wy - 1;
        // offset = midpoint of the corner cells' centres (the tool takes it from the terrain points there; buildable
        // ground is flat at height 0 - recorded player placements all carry z = 0)
        float half = (float)(size >> 1) - 0.5f;
        float ox = ((float)x0 + (float)x1) * 0.5f - half, oy = ((float)y0 + (float)y1) * 0.5f - half;
        sprintf_s(cmd, "place %u %s %d %d %d %d %a %a %a %a 0 %d", *(uint32_t*)(g_exe + Rva(kTiPlacementTool)), path, x0, y0,
                  x1, y1, ox, oy, 0.f, (float)rot * 90.f, rot);
    } else {
        if (nearby) {
            int wx = p3 > 0 ? p3 : 6, wy = p4 > 0 ? p4 : 6;
            // area limits of the drag/fence description (FencedTool vf14 exe+0xe0960): max +0x58/+0x5c, min +0x60/+0x64
            if (uint8_t* d = fenced ? SubDesc(tpl, kTiCreateFenced) : nullptr) {
                int maxx = *(int*)(d + 0x58), maxy = *(int*)(d + 0x5c), minx = *(int*)(d + 0x60), miny = *(int*)(d + 0x64);
                Log("uiscript: %s area limits %dx%d .. %dx%d", path, minx, miny, maxx, maxy);
                if (minx > 0 && minx < 64 && wx < minx) wx = minx;
                if (miny > 0 && miny < 64 && wy < miny) wy = miny;
                if (maxx > 0 && maxx < 256 && wx > maxx) wx = maxx;
                if (maxy > 0 && maxy < 256 && wy > maxy) wy = maxy;
            }
            if (!FindFree(x, y, wx, wy, &x0, &y0)) { Log("!! uiscript: build %s: no free %dx%d spot near %d,%d", path, wx, wy, x, y); return; }
            x1 = x0 + wx - 1;
            y1 = y0 + wy - 1;
        } else {
            if (got < 5) { Log("!! uiscript: build %s needs X Y X2 Y2 (area)", path); return; }
            x0 = x; y0 = y; x1 = p3; y1 = p4;
        }
        sprintf_s(cmd, "drag%d %u %s %d %d %s", fenced ? 2 : 1,
                  *(uint32_t*)(g_exe + Rva((fenced ? kTiFencedTool : kTiDragPlacementTool))), path, y0 * size + x0,
                  y1 * size + x1, fenced ? "0 0 0" : "-1 0 0");
    }
    Log("uiscript: build %s at %d,%d-%d,%d -> %s", path, x0, y0, x1, y1, cmd);
    std::string shortName = path;
    if (size_t sl = shortName.find_last_of("\\/"); sl != std::string::npos) shortName = shortName.substr(sl + 1);
    if (size_t dot = shortName.find('.'); dot != std::string::npos) shortName.resize(dot);
    g_built.push_back({shortName, Rect{x0, y0, x1, y1}});
    commands::Submit(cmd);
}
// Open the entity's window the way the game builds one (exe+0x1073b0, restoring pinned windows): the template's
// UIDescription (+0x28 = its Dialog) and the entity's UIComponent (+0x28 user data, +0x30 int) -> new DialogInstance
// (0xb8 bytes, window vtable exe+0x1659b0) -> UserInterface::AddDialog.
static std::vector<void*> g_opened;
// e = a building, or nullptr with `templ` = a template without an entity (toolbar utility windows: event log, ...)
static void* OpenWindow(uint8_t* e, uint8_t* templ = nullptr) {
    static void* memAlloc = RuntimeExport("?MemAlloc@MemoryAllocator@System@@SAPEAX_KH@Z");
    static void* ctor = RuntimeExport("??0DialogInstance@UI@@QEAA@PEAVUserInterface@1@PEBVDialog@1@V?$InternalPtr@VDialogInstance@UI@@@Object@@PEAVBase@5@PEAXH@Z");
    static void* addDialog = RuntimeExport("?AddDialog@UserInterface@UI@@QEAAXV?$InternalPtr@VDialogInstance@UI@@@Object@@@Z");
    static void* setUpperLeft = RuntimeExport("?SetUpperLeft@DialogInstance@UI@@QEAAXHH_N@Z");
    if (!memAlloc || !ctor || !addDialog) return nullptr;
    uint8_t* uiDesc = SubDesc(e ? *(uint8_t**)(e + 0x28) : templ, 0x1cff90);  // UIDescription
    if (!uiDesc) { Log("!! uiscript: this building has no window (no UIDescription)"); return nullptr; }
    uint8_t* uiComp = nullptr;
    int want = *(int*)(g_exe + Rva(0x1cffc8));  // UIComponent
    for (int i = 0; e && i < *(int*)(e + 0x38); i++) {
        uint8_t* c = (*(uint8_t***)(e + 0x30))[i];
        if (((int(__fastcall*)(void*))(*(void***)c)[2])(c) == want) { uiComp = c; break; }
    }
    void* ui = *(void**)(GameLoop() + 0x98);
    void* dlg = ((void*(__fastcall*)(size_t, int))memAlloc)(0xb8, 8);
    void* parent = nullptr;
    using Ctor_t = void*(__fastcall*)(void*, void*, void*, void**, void*, void*, int);
    ((Ctor_t)ctor)(dlg, ui, *(void**)(uiDesc + 0x28), &parent, e, uiComp ? *(void**)(uiComp + 0x28) : nullptr,
                   uiComp ? *(int*)(uiComp + 0x30) : 0);
    *(uintptr_t*)dlg = g_exe + Rva(0x1659b0);
    void* ref = dlg;
    ((void(__fastcall*)(void*, void**))addDialog)(ui, &ref);
    if (setUpperLeft) ((void(__fastcall*)(void*, int, int, bool))setUpperLeft)(dlg, 200, 120, false);
    g_opened.push_back(dlg);
    return dlg;
}
static void CloseOpened() {
    static void* closeFn = RuntimeExport("?CloseDialog@UserInterface@UI@@QEAAXV?$InternalPtr@VDialogInstance@UI@@@Object@@@Z");
    void* ui = *(void**)(GameLoop() + 0x98);
    for (void* d : g_opened) {
        bool still = false;  // only windows still in the list (the player / game may have closed them)
        for (uint8_t* node = *(uint8_t**)ui; node; node = *(uint8_t**)(node + 0x10)) still |= *(void**)node == d;
        void* ref = d;
        if (still && closeFn) ((void(__fastcall*)(void*, void**))closeFn)(ui, &ref);
    }
    g_opened.clear();
}

// browse N: like a player clicking around - every N frames the last window closes and the next entity's opens
// (entity manager order, every entity with a window: buildings, citizens, ...). Tests the UI -> simulation leaks.
static int g_browseEvery, g_browseFrame, g_browseIdx, g_browseCount;
// camtour N: the camera flies somewhere else every N frames (a fixed walk over the map, local view only) - a real
// player looks at different places than the other PCs; tests camera -> simulation leaks.
static int g_camEvery, g_camFrame, g_camStep;
static void CamTour() {
    if (!g_camEvery || ++g_camFrame % g_camEvery) return;
    uint8_t* gl = GameLoop();
    int size = gl && *(int**)(gl + 0xf8) ? **(int**)(gl + 0xf8) : 0;
    if (size < 32) return;
    g_camStep++;
    int x = 16 + (g_camStep * 37) % (size - 32), y = 16 + (g_camStep * 61) % (size - 32);
    towns::FocusCell(y * size + x);
    if (g_camStep <= 5 || g_camStep % 200 == 0) Log("uiscript: camera tour #%d -> %d,%d", g_camStep, x, y);
}

static void Browse() {
    if (!g_browseEvery || ++g_browseFrame % g_browseEvery) return;
    uint8_t* gl = GameLoop();
    uint8_t* mgr = gl ? *(uint8_t**)(gl + 0xe8) : nullptr;
    if (!mgr || !*(void**)(gl + 0x98)) return;
    CloseOpened();
    if (g_browseCount % 8 == 7) {  // every 8th: one of the toolbar's info windows instead (limits, professions, ...)
        static const char* kUtil[] = {"Template/UtilityLimits.rsc", "Template/UtilityProfessions.rsc",
                                      "Template/UtilityEventLog.rsc", "Template/UtilityMap.rsc", "Template/UtilityStatusBar.rsc"};
        const char* path = kUtil[(g_browseCount / 8) % 5];
        void* desc = commands::DescFromPath(path);
        void* dlg = nullptr;
        if (desc && *(void**)(gl + 0x120))
            ((void(__fastcall*)(void*, void**, void*))(g_exe + Rva(0x106f50)))(*(void**)(gl + 0x120), &dlg, desc);
        if (dlg) g_opened.push_back(dlg);
        if (g_browseCount++ < 60) Log("uiscript: browse #%d %s -> window %p", g_browseCount, path, dlg);
        return;
    }
    int n = *(int*)(mgr + 0x10);
    for (int k = 0; k < n; k++) {
        int i = (g_browseIdx + k) % n;
        uint8_t* e = (*(uint8_t***)(mgr + 0x08))[i];
        if (!e || (*(uint32_t*)(e + 0x40) & 0x400) || !*(uint8_t**)(e + 0x28) || !SubDesc(*(uint8_t**)(e + 0x28), 0x1cff90)) continue;
        g_browseIdx = i + 1;
        void* dlg = OpenWindow(e);
        if (g_browseCount++ < 30 || g_browseCount % 500 == 0)
            Log("uiscript: browse #%d entity %p %s -> window %p", g_browseCount, e, commands::DescPath(*(void**)(e + 0x28)).c_str(), dlg);
        return;
    }
}

// state: simulation values of the opened building (whatever applies): workplace workers (comp 0x2e10: +0x60 count,
// +0x48 profession) and limit (work comp 0x3050 +0x30), crop (0x5380 +0x50), orchard tree (0x5440 +0x60), pasture
// animal (0x4fe0 +0x28) and animal limit (+0x40).
static std::string g_lastState;  // the last state line (for expect)
static std::string StateOf(uint8_t* e) {
    auto get = [&](uintptr_t rva) {
        void* c = nullptr;
        ((void(__fastcall*)(void*, void**))(g_exe + Rva(rva)))(e, &c);
        return (uint8_t*)c;
    };
    char buf[512];
    int k = sprintf_s(buf, "uiscript: state of %p (town %d):", e, towns::OwnerOfObject(e));
    if (uint8_t* wp = get(0x2e10)) k += sprintf_s(buf + k, sizeof buf - k, " workers %d", *(int*)(wp + 0x60));
    if (uint8_t* w = get(0x3050)) k += sprintf_s(buf + k, sizeof buf - k, " limit %d", *(int*)(w + 0x30));
    if (uint8_t* c = get(0x5380)) k += sprintf_s(buf + k, sizeof buf - k, " crop %s", commands::DescPath(*(void**)(c + 0x50)).c_str());
    if (uint8_t* o = get(0x5440)) k += sprintf_s(buf + k, sizeof buf - k, " tree %s", commands::DescPath(*(void**)(o + 0x60)).c_str());
    if (uint8_t* p = get(0x4fe0))
        k += sprintf_s(buf + k, sizeof buf - k, " animal %s limit %d", commands::DescPath(*(void**)(p + 0x28)).c_str(), *(int*)(p + 0x40));
    if (uint8_t* t = get(0x15b50)) {  // trading post: +0x28 setting, +0x40 order rows (12 B, amount +8), count at +0x48?
        k += sprintf_s(buf + k, sizeof buf - k, " trade28 %d trade2c %d orders", *(int*)(t + 0x28), *(int*)(t + 0x2c));
        uint8_t* rows = *(uint8_t**)(t + 0x40);
        for (int i = 0; i < 6 && rows; i++) k += sprintf_s(buf + k, sizeof buf - k, " %d", *(int*)(rows + i * 12 + 8));
    }
    if (get(0x2e10)) k += sprintf_s(buf + k, sizeof buf - k, " | by town: %d %d | stock %d", towns::WorkersOfTownAt(e, 0), towns::WorkersOfTownAt(e, 1), towns::StockAt(e));
    return buf;
}
static void State() {
    if (!g_entity) { Log("!! uiscript: state: nothing opened"); return; }
    g_lastState = StateOf((uint8_t*)g_entity);
    Log("%s", g_lastState.c_str());
}
static void Run(const std::string& text) {
    char verb[32] = "", a[128] = "", b[256] = "";
    sscanf_s(text.c_str(), "%31s %127s %255s", verb, (unsigned)sizeof verb, a, (unsigned)sizeof a, b, (unsigned)sizeof b);
    std::string v = verb;
    uint8_t* gl = GameLoop();
    if (v == "storages") { towns::DebugStorages(); return; }
    if (v == "report") {  // report open | report save TEXT | report send TEXT (the bug report window's buttons)
        std::string rest = text.size() > strlen(verb) + strlen(a) + 2 ? text.substr(strlen(verb) + strlen(a) + 2) : "";
        if (std::string(a) == "open") report::Open(false);
        else report::Test(std::string(a) == "send", rest);
        return;
    }
    if (v == "list") { List((unsigned)strtoul(a, nullptr, 16)); return; }
    if (v == "build") { Build(text); return; }
    if (v == "loadgame") {  // loadgame SAVE: load a save from inside the running game, like the game's Load menu
        Log("uiscript: loadgame %s -> %s", a, gameload::Request(a) ? "loading" : "refused");
        return;
    }
    if (v == "select") {  // select PATH | select none: pick a tool like the toolbar (exe+0x108030(toolUI, template))
        void* desc = std::string(a) == "none" ? nullptr : commands::DescFromPath(a);
        if (gl && *(void**)(gl + 0x120) && (desc || std::string(a) == "none")) {
            ((void(__fastcall*)(void*, void*))(g_exe + Rva(0x108030)))(*(void**)(gl + 0x120), desc);
            Log("uiscript: selected tool %s", a);
        } else {
            Log("uiscript: select %s: not found", a);
        }
        return;
    }
    if (v == "mouse") {  // mouse X Y down|up|move | mouse off: the game's mouse at client pixel X,Y, left button held or not
        int x = 0, y = 0;
        char state[16] = "move";
        if (std::string(a) == "off") { autotest::ScriptMouse(-1, -1, false); return; }
        sscanf_s(text.c_str(), "%*s %d %d %15s", &x, &y, state, (unsigned)sizeof state);
        static bool down;
        if (!strcmp(state, "down")) down = true;
        if (!strcmp(state, "up")) down = false;
        autotest::ScriptMouse(x, y, down);
        return;
    }
    if (v == "screenshot") {  // screenshot NAME: tools\screenshot.ps1 of this window -> <script dir>\shots\NAME.png
        std::wstring dir = g_scriptPath.substr(0, g_scriptPath.find_last_of(L"\\/"));
        std::wstring root = dir.substr(0, dir.find_last_of(L"\\/"));
        root = root.substr(0, root.find_last_of(L"\\/"));  // <project>\tests\uitests -> <project>
        CreateDirectoryW((dir + L"\\shots").c_str(), nullptr);
        std::wstring name(a, a + strlen(a));
        overlay::RequestScreenshot((dir + L"\\shots\\" + name + L".png").c_str());  // saved at the next Present
        Log("uiscript: screenshot %s requested", a);
        return;
    }
    if (v == "state") { State(); return; }
    if (v == "cursor" || v == "ping") {  // cursor X Y | cursor off | ping X Y: presence.cpp as if the mouse were at cell X,Y
        int x = -1, y = -1;
        sscanf_s(text.c_str(), "%*s %d %d", &x, &y);
        if (v == "ping") presence::TestPing(x, y);
        else if (v == "cursor" && std::string(a) == "probe") {  // cursor probe SX SY: the mouse-ray path, logged
            float sx = 0, sy = 0;
            sscanf_s(text.c_str(), "%*s %*s %f %f", &sx, &sy);
            presence::TestProbe(sx, sy);
        } else if (v == "cursor" && std::string(a) == "focus") {  // cursor focus X Y: the camera flies to cell X,Y
            sscanf_s(text.c_str(), "%*s %*s %d %d", &x, &y);
            towns::FocusCell(y * towns::MapSize() + x);
        }
        else presence::TestCursor(std::string(a) == "off" ? -1 : x, y);
        Log("uiscript: %s %d %d", v.c_str(), x, y);
        return;
    }
    if (v == "towns") {
        for (int t = 0; t < towns::Count(); t++) Log("uiscript: town %d: %s", t, towns::StorageSummary(t).c_str());
        return;
    }
    if (v == "events") {  // events [N]: the newest N event-log entries with their towns
        int n = 10;
        sscanf_s(text.c_str(), "%*s %d", &n);
        for (auto& e : towns::Events(n)) Log("uiscript: event %d town %d cell %d: %s", e.index, e.town, e.cell, e.text);
        return;
    }
    if (v == "cells") {  // cells X Y R: histogram of cell types (+0x29) and levels (+0x26) within R, nearest of each type
        int cx = 0, cy = 0, r = 30;
        sscanf_s(text.c_str(), "%*s %d %d %d", &cx, &cy, &r);
        int* map = *(int**)(gl + 0xf8);
        int size = *map;
        uint8_t* cells = *(uint8_t**)((uint8_t*)map + 0x80);
        int count[256] = {0}, nx[256], ny[256], nd[256];
        for (int i = 0; i < 256; i++) nd[i] = 1 << 30;
        for (int y = cy - r; y <= cy + r; y++)
            for (int x = cx - r; x <= cx + r; x++) {
                if (x < 0 || y < 0 || x >= size || y >= size) continue;
                int t = cells[((int64_t)y * size + x) * 0x30 + 0x29];
                count[t]++;
                int d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
                if (d < nd[t]) { nd[t] = d; nx[t] = x; ny[t] = y; }
            }
        for (int t = 0; t < 256; t++)
            if (count[t]) Log("uiscript: cells type %d: %d (nearest %d,%d)", t, count[t], nx[t], ny[t]);
        return;
    }
    if (v == "expect") {  // expect TEXT: the last state line contains TEXT
        std::string want = text.substr(text.find(' ') + 1);
        bool ok = g_lastState.find(want) != std::string::npos;
        Log("UITEST %s: expected '%s' in [%s]", ok ? "PASS" : "FAIL", want.c_str(), g_lastState.c_str());
        return;
    }
    if (v == "known") {  // the local town's known templates (seeds, saplings, livestock): Town+0x120 array, +0x128 count
        uint8_t* town = *(uint8_t**)(gl + 0xf0);
        for (int i = 0; i < *(int*)(town + 0x128); i++)
            Log("uiscript: known %d %s", i, commands::DescPath((*(void***)(town + 0x120))[i]).c_str());
        return;
    }
    if (v == "windows") {
        int n = 0;
        ForEachController([&](uint8_t* dlg, uint8_t* c) {
            uintptr_t vt = VtRva(c);
            const char* name = "?";
            for (auto& k : kClasses) if (k.vt == vt) name = k.name;
            void* e = *(void**)(c + 0x28);
            Log("uiscript: window %p controller %s (vt %x) entity %p%s", dlg, name, (unsigned)vt, e,
                e && e == g_entity ? " <- opened" : "");
            n++;
            return false;
        });
        Log("uiscript: %d controllers in open windows", n);
        uint8_t* gl = GameLoop();
        uint8_t* uiObj = gl ? *(uint8_t**)(gl + 0x98) : nullptr;
        Log("uiscript: UserInterface %p (vt %x) head %p", uiObj, (unsigned)VtRva(uiObj), uiObj ? *(void**)uiObj : nullptr);
        int k = 0;
        for (uint8_t* node = uiObj ? *(uint8_t**)uiObj : nullptr; node && k < 40; node = *(uint8_t**)(node + 0x10), k++) {
            uint8_t* dlg = *(uint8_t**)node;
            Log("uiscript:   node %p dialog %p vt %x readable %d count %d", node, dlg, (unsigned)VtRva(dlg),
                Readable(dlg, 0x88), Readable(dlg, 0x88) ? *(int*)(dlg + 0x80) : -1);
        }
        return;
    }
    if (v == "cmd") {
        std::string raw = text.substr(text.find(' ') + 1);
        Log("uiscript: cmd %s", raw.c_str());
        commands::Submit(raw);
        return;
    }
    if (v == "open") {
        int x = 0, y = 0;
        if (a[0] == '@') {  // a building this script placed: the centre of its rectangle
            bool found = false;
            for (auto& [name, r] : g_built)
                if (name == a + 1) { x = (r.x0 + r.x1) / 2; y = (r.y0 + r.y1) / 2; found = true; }
            if (!found) { Log("!! uiscript: open %s: not built by this script", a); return; }
        } else {
            sscanf_s(text.c_str(), "%*s %d %d", &x, &y);
        }
        void* e = EntityAt(x, y);
        if (!e) { Log("!! uiscript: open %d,%d: no building there", x, y); return; }
        g_entity = e;
        void* dlg = OpenWindow((uint8_t*)e);
        Log("uiscript: open %d,%d -> entity %p (town %d) window %p", x, y, e, towns::OwnerOfObject(e), dlg);
        return;
    }
    if (v == "leave") { Log("uiscript: leave"); net::UiDo(net::kUiLeave); return; }  // the bar's Leave (confirmed)
    if (v == "tradewin") { ingame::ToggleTrade(); Log("uiscript: trade window toggled"); return; }
    if (v == "offer") {  // offer PLAYER NGIVE GIVEPATH NASK ASKPATH PERIOD: a trade offer like the trade window's button
        int to = 0, ng = 0, na = 0, period = 0;
        char give[160] = "", ask[160] = "";
        sscanf_s(text.c_str(), "%*s %d %d %159s %d %159s %d", &to, &ng, give, (unsigned)sizeof give, &na, ask, (unsigned)sizeof ask, &period);
        net::SendOffer(to, give, ng, ask, na, period);
        Log("uiscript: offer to player %d: %d %s for %d %s (period %d)", to, ng, give, na, ask, period);
        return;
    }
    if (v == "openui") {  // openui PATH: a template's window without a building (Template\UtilityEventLog.rsc, ...)
        void* desc = commands::DescFromPath(a);
        void* dlg = nullptr;  // the toolbar's Utility action: exe+0x106f50(toolUI, &window, template) (owner = template)
        if (desc && gl && *(void**)(gl + 0x120))
            ((void(__fastcall*)(void*, void**, void*))(g_exe + Rva(0x106f50)))(*(void**)(gl + 0x120), &dlg, desc);
        if (dlg) g_opened.push_back(dlg);
        Log("uiscript: openui %s -> window %p", a, dlg);
        return;
    }
    if (v == "sharework") {  // sharework @Name TOWN 0|1: the shared-workplace checkbox's request for that building
        int x = 0, y = 0, town = 1, on = 1;
        for (auto& [name, r] : g_built) if (name == a + 1) { x = (r.x0 + r.x1) / 2; y = (r.y0 + r.y1) / 2; }
        sscanf_s(text.c_str(), "%*s %*s %d %d", &town, &on);
        void* e = EntityAt(x, y);
        if (!e) { Log("!! uiscript: sharework %s: no building there", a); return; }
        Log("uiscript: sharework %d,%d (own workplace %d) with town %d: %d", x, y, towns::IsOwnWorkplace(e), town, on);
        commands::RequestShareWork(e, town, on != 0);
        return;
    }
    if (v == "share") {  // share X Y TOWN 0|1 | share @Name TOWN 0|1: the overlay checkbox's request for that building
        int x = 0, y = 0, town = 1, on = 1;
        if (a[0] == '@') {
            for (auto& [name, r] : g_built) if (name == a + 1) { x = (r.x0 + r.x1) / 2; y = (r.y0 + r.y1) / 2; }
            sscanf_s(text.c_str(), "%*s %*s %d %d", &town, &on);
        } else {
            sscanf_s(text.c_str(), "%*s %d %d %d %d", &x, &y, &town, &on);
        }
        void* e = EntityAt(x, y);
        if (!e) { Log("!! uiscript: share %d,%d: no building there", x, y); return; }
        Log("uiscript: share %d,%d (own storage %d) with town %d: %d", x, y, towns::IsOwnStorage(e), town, on);
        commands::RequestShareStorage(e, town, on != 0);
        return;
    }
    if (v == "kill") {  // kill X Y | kill @Name: test command destroying the building at once (owner only)
        int x = 0, y = 0;
        if (a[0] == '@') {
            for (auto& [name, r] : g_built) if (name == a + 1) { x = (r.x0 + r.x1) / 2; y = (r.y0 + r.y1) / 2; }
        } else {
            sscanf_s(text.c_str(), "%*s %d %d", &x, &y);
        }
        void* e = EntityAt(x, y);
        if (!e || !simheap::Contains(e)) { Log("!! uiscript: kill %d,%d: no building there", x, y); return; }
        char cmd[128];
        sprintf_s(cmd, "win 25 %llx 0 0 0 - %x", (unsigned long long)simheap::Offset(e), (unsigned)VtRva(e));
        Log("uiscript: kill %d,%d -> %s", x, y, cmd);
        commands::Submit(cmd);
        return;
    }
    if (v == "close") {
        Log("uiscript: closed %zu windows", g_opened.size());
        CloseOpened();
        return;
    }
    if (v == "camtour") {  // camtour N | camtour 0: every N frames the camera flies to another part of the map
        g_camEvery = atoi(a);
        Log("uiscript: camera tour every %d frames", g_camEvery);
        return;
    }
    if (v == "browse") {  // browse N | browse 0: every N frames close the last and open the next window (any entity)
        g_browseEvery = atoi(a);
        Log("uiscript: browsing windows every %d frames", g_browseEvery);
        return;
    }    bool any = strstr(text.c_str(), " any ") != nullptr || (text.size() > 4 && text.compare(text.size() - 4, 4, " any") == 0);
    uint8_t* ui = FindController(a, any);
    if (!ui) return;
    void** vt = *(void***)ui;
    if (v == "press") {
        int evt = 0, id = 0;
        sscanf_s(text.c_str(), any ? "%*s %*s %*s %i %i" : "%*s %*s %i %i", &evt, &id);
        void* elem = *(void**)(ui + 0x30);
        Log("uiscript: press %s evt %#x id %d (entity %p)", a, evt, id, *(void**)(ui + 0x28));
        ((void(__fastcall*)(void*, void**, int, int))vt[9])(ui, &elem, evt, id);
        return;
    }
    if (v == "pick") {
        void* desc = std::string(b) == "-" ? nullptr : commands::DescFromPath(b);
        if (!desc && std::string(b) != "-") { Log("!! uiscript: pick: unknown description %s", b); return; }
        Log("uiscript: pick %s %s", a, b);
        ((void(__fastcall*)(void*, void*))vt[11])(ui, desc);
        return;
    }
    if (v == "rows") {  // rows CLASS OFF STRIDE N: the first int of N rows at *(ui+OFF) (numbers a window shows)
        int off = 0, stride = 0, n = 0;
        sscanf_s(text.c_str(), "%*s %*s %i %i %i", &off, &stride, &n);
        uint8_t* base = *(uint8_t**)(ui + off);
        char buf[512];
        int k = sprintf_s(buf, "uiscript: rows %s:", a);
        for (int i = 0; i < n && base && k < 480; i++) k += sprintf_s(buf + k, sizeof buf - k, " %d", *(int*)(base + (int64_t)i * stride));
        Log("%s", buf);
        return;
    }
    if (v == "setf") {  // setf CLASS OFF FIELD VALUE: float at *(ui+OFF) + FIELD (sliders)
        int off = 0, field = 0;
        float value = 0;
        sscanf_s(text.c_str(), "%*s %*s %i %i %f", &off, &field, &value);
        uint8_t* elem = *(uint8_t**)(ui + off);
        if (!Readable(elem, field + 4)) { Log("!! uiscript: setf: no element at ui+%#x", off); return; }
        *(float*)(elem + field) = value;
        Log("uiscript: setf %s ui+%#x +%#x = %g", a, off, field, value);
        return;
    }
    if (v == "combo") {  // combo CLASS OFF VALUE: ComboElement *(ui+OFF) -> SetCurrentItem(VALUE)
        int off = 0, value = 0;
        sscanf_s(text.c_str(), "%*s %*s %i %i", &off, &value);
        static void* setItem = RuntimeExport("?SetCurrentItem@ComboElement@UI@@QEAAXH@Z");
        void* elem = *(void**)(ui + off);
        if (!setItem || !Readable(elem, 0x120)) { Log("!! uiscript: combo: no element at ui+%#x", off); return; }
        ((void(__fastcall*)(void*, int))setItem)(elem, value);
        Log("uiscript: combo %s ui+%#x = %d (reads %d)", a, off, value, *(int*)((uint8_t*)elem + 0x118));
        return;
    }
    if (v == "spin" || v == "number" || v == "check") {
        int off = 0, stride = 0, row = 0, field = 0, value = 0;
        char valueText[32] = "";
        sscanf_s(text.c_str(), "%*s %*s %i %i %i %i %31s", &off, &stride, &row, &field, valueText, (unsigned)sizeof valueText);
        value = atoi(valueText);
        uint8_t* base = *(uint8_t**)(ui + off);
        if (valueText[0] == '+' && base) value += *(int*)(base + (int64_t)row * stride);  // relative to the row's value
        uint8_t* elem = base ? *(uint8_t**)(base + (int64_t)row * stride + field) : nullptr;
        if (!Readable(elem, 0xe0)) { Log("!! uiscript: %s: no element at ui+%#x[%d]+%#x", verb, off, row, field); return; }
        if (v == "check") {
            elem[0xd8] = (uint8_t)(value != 0);
        } else {
            static void* setSpin = RuntimeExport("?SetValue@SpinnerElement@UI@@QEAAXH@Z");
            static void* setNum = RuntimeExport("?SetValue@NumberEditElement@UI@@QEAAXH@Z");
            void* fn = v == "spin" ? setSpin : setNum;
            if (fn) ((void(__fastcall*)(void*, int))fn)(elem, value);
        }
        static void* getNum = RuntimeExport("?GetValue@NumberEditElement@UI@@QEBAHXZ");
        int back = v == "number" && getNum ? ((int(__fastcall*)(void*))getNum)(elem) : -1;
        Log("uiscript: %s %s ui+%#x[%d]+%#x = %d (reads back %d, row starts with %d)", verb, a, off, row, field, value, back,
            *(int*)(base + (int64_t)row * stride));
        return;
    }
    Log("!! uiscript: unknown step '%s'", text.c_str());
}

// One script line may hold several steps separated by " ; " - they run in the same frame (e.g. set a number and press,
// before the window's per-frame refresh resets the number).
static void RunLine(const std::string& text) {
    size_t start = 0;
    while (true) {
        size_t sep = text.find(" ; ", start);
        Run(text.substr(start, sep == std::string::npos ? std::string::npos : sep - start));
        if (sep == std::string::npos) break;
        start = sep + 3;
    }
}

void OnFrame() {
    if (!GameLoop()) return;
    if (ProbeSessionCount() != g_session) { g_session = ProbeSessionCount(); g_entity = nullptr; g_opened.clear(); }
    if (!gameload::InMenu()) { Browse(); CamTour(); }
    if (g_next >= g_lines.size()) return;
    if (g_lines[g_next].text == "nextsession") {  // wait for a reload (resync): later ticks count in the new session
        static uint64_t waitFrom;
        if (!waitFrom) waitFrom = ProbeSessionCount();
        if (ProbeSessionCount() == waitFrom) return;
        waitFrom = 0;
        Log("uiscript: new session");
        g_next++;
        return;
    }
    if (ProbeTick() < g_lines[g_next].tick) return;
    if (g_lines[g_next].text.compare(0, 8, "waitfor ") == 0) {
        // waitfor @Name MAXTICKS TEXT: hold the script until that building's state contains TEXT (e.g. a finished
        // workplace); later steps move by the wait, so their spacing stays. Build times vary run to run (command latency).
        char name[128] = "";
        unsigned max = 0;
        int used = 0;
        sscanf_s(g_lines[g_next].text.c_str(), "waitfor @%127s %u %n", name, (unsigned)sizeof name, &max, &used);
        std::string want = used ? g_lines[g_next].text.substr(used) : "";
        std::string st;
        for (auto& [n, r] : g_built)
            if (n == name)
                if (void* e = EntityAt((r.x0 + r.x1) / 2, (r.y0 + r.y1) / 2)) st = StateOf((uint8_t*)e);
        uint64_t waited = ProbeTick() - g_lines[g_next].tick;
        bool met = !want.empty() && st.find(want) != std::string::npos;
        if (!met && waited < max) return;
        if (met) Log("uiscript: waitfor @%s '%s' met after %llu ticks [%s]", name, want.c_str(), waited, st.c_str());
        else Log("UITEST FAIL: waitfor @%s '%s' not met in %u ticks [%s]", name, want.c_str(), max, st.c_str());
        for (size_t i = g_next + 1; i < g_lines.size(); i++) g_lines[i].tick += waited;
        g_next++;
        return;
    }
    RunLine(g_lines[g_next].text);
    g_next++;
}

}  // namespace uiscript
