// Separate towns (see towns.h). Addresses: exe build 0x59b56a2d.
#include "towns.h"
#include "actionprobe.h"
#include "common.h"
#include "MinHook.h"
#include "commands.h"
#include "autotest.h"
#include "visualstub.h"
#include "simheap.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <unordered_map>
#include <intrin.h>
#include <set>
#include <vector>

namespace towns {

static const uintptr_t kRvaGameLoopGlobal = 0x1cbdd8;   // DAT_1401cbdd8: the running GameLoop
static const uintptr_t kRvaEntityTick = 0xc82b0;        // per-tick entity update (entity, dt) -> components vf8
static const uintptr_t kRvaBucketCall = 0xc9784;        // EntityManager::Update: "mov rax,[rcx]; call [rax+48h]", rbx = entity
static const uintptr_t kRvaTownCtor = 0x95f00;          // Town(mem) (0x1c0 bytes)
static const uintptr_t kRvaArrayResize = 0x18050;       // array<int>::Resize(array*, n)
static const uintptr_t kRvaStartAll = 0xa87d0;          // new game: pick start, place everything (mapgen, Vector3* out)
static const uintptr_t kRvaStartStructures = 0xa7490;   // starting buildings around a cell (mapgen, x, y)
static const uintptr_t kRvaStartFamilies = 0xa62a0;     // starting families near a position (mapgen, Vector3*)
static const uintptr_t kRvaSiteOk = 0xa7d50;            // every cell within r (r^2) is buildable land (mapgen, x, y, r, r2)
static const uintptr_t kRvaCellToWorld = 0x2080;        // (map, Vector3* out, x, y)
static const uintptr_t kRvaTownAddResource = 0x97330;   // Town+0x110 list add (unique)
static const uintptr_t kRvaTownAddTemplate = 0x96ef0;   // Town+0x120 list add (unique)
static const uintptr_t kRvaStartSupplies = 0xa73d0;     // starting food/fuel/tools/clothes into the current town's storage (mapgen)

static uintptr_t g_exe;
static int g_count = 1, g_local = 0, g_cur = 0;
// what Configure set (the session's setup); a loaded save may change g_count/g_local/g_pick for that game only -
// every load starts again from these (a two-town save once left the NEXT, one-town game with a phantom empty town)
static int g_cfgCount = 1, g_cfgLocal = 0;
static bool g_cfgPick;
static std::vector<void*> g_towns;                    // [0] = the GameLoop's own Town
static std::unordered_map<uintptr_t, uint8_t> g_owner; // entity object -> town (missing = 0)
static std::unordered_map<uintptr_t, uint8_t> g_deliverTo;  // trade delivery pile entity -> receiving town
static std::unordered_map<uintptr_t, uint8_t> g_courier;    // citizen entity carrying delivery goods -> receiving town
static std::unordered_map<uintptr_t, void*> g_courierDesc;    // courier -> the delivered good (not saved)
static std::unordered_map<uintptr_t, uint8_t> g_stuck;      // delivery piles with no walking route to the receiver (UI)
static std::vector<void*> g_tradedLog;                      // goods seen in deliveries (progress log; per session)
static int g_modMoves;  // > 0 while trade code moves goods itself (h_StorageRemove watch)
struct ModMove { ModMove() { g_modMoves++; } ~ModMove() { g_modMoves--; } };
static uint64_t g_steps;                                    // town steps this session (log cadence)
struct Site { int x, y; float pos[4]; bool placed; };
static std::vector<Site> g_sites;
static void* g_sessionLoop;
static int g_players = 1;                              // players in the session (START), for loading saves
static bool g_pick;                                    // extra towns are settled by their players

using MemAllocFn_t = void*(__fastcall*)(uint64_t size, int align);
static MemAllocFn_t p_MemAlloc;

// Logging inside the simulation: item names come from engine Strings (heap allocations) -> keep them off the sim heap.
struct NonSim {
    NonSim() { InterlockedIncrement(&g_visualStub); }
    ~NonSim() { InterlockedDecrement(&g_visualStub); }
};

static uint8_t* GameLoop() { return *(uint8_t**)(g_exe + kRvaGameLoopGlobal); }

int Count() { return g_count; }
static void FocusCamera(int x, int y);
int Local() { return g_local; }
void SetPlayers(int n) { g_players = n < 1 ? 1 : n > 8 ? 8 : n; }
static bool g_focusLocal;  // fly the camera to the local town at the next frame (outside the sim)
void SetLocal(int t) {  // may run inside the sim: no switching here - after the steps the local town is switched in
    if (t < 0 || t >= g_count || t == g_local) return;
    g_local = t;
    g_focusLocal = true;
    Log("towns: I am town %d", t);
}
int Current() { return g_cur; }

// A session ends (new START / a load begins) while its GameLoop still exists: that loop's shutdown (exe+0x9cc90) later
// kills every entity, and each kill must still run in its owner's town (a zone looked up in the wrong Town crashed).
// Keep the old towns + ownership aside until then; the loop gets its own Town back in its slot.
static std::vector<void*> g_retiredTowns;
static std::unordered_map<uintptr_t, uint8_t> g_retiredOwner;
static bool g_inShutdown;
static void Retire() {
    if (g_towns.size() < 2) return;
    if (uint8_t* gl = GameLoop()) *(void**)(gl + 0xf0) = g_towns[0];
    g_retiredTowns = g_towns;
    g_retiredOwner = std::move(g_owner);
    g_owner.clear();
}

void Configure(int count, int local, bool pickStarts) {
    Retire();
    g_pick = pickStarts && count > 1;
    g_count = count < 1 ? 1 : count;
    g_local = local >= 0 && local < g_count ? local : 0;
    g_cfgCount = g_count, g_cfgLocal = g_local, g_cfgPick = g_pick;
    g_cur = 0;
    g_towns.clear();
    g_owner.clear();
    g_sites.clear();
    Log("towns: configured %d town(s), local town %d%s", g_count, g_local, g_pick ? ", players pick their start" : "");
}

// The event log (GameLoop+0x138: +0x18 entries of 0x28 B {u32 colour, String text +8, type +0x10, popup +0x14, cell
// +0x18}, +0x20 count) is shared by every town. Entries added since the last check belong to the town whose context
// was active: checked at every town switch and every frame. Read-only (no effect on the simulation).
static std::vector<int8_t> g_evTown;
static void AttributeEvents() {
    uint8_t* gl = GameLoop();
    uint8_t* ev = gl ? *(uint8_t**)(gl + 0x138) : nullptr;
    if (!ev) return;
    int n = *(int*)(ev + 0x20);
    if (n < (int)g_evTown.size()) g_evTown.resize(n < 0 ? 0 : n);
    while ((int)g_evTown.size() < n) g_evTown.push_back((int8_t)g_cur);
}

void Switch(int t) {
    if (g_towns.size() < 2) return;
    if (t < 0 || t >= (int)g_towns.size()) t = 0;
    if (t != g_cur) AttributeEvents();
    g_cur = t;
    if (uint8_t* gl = GameLoop()) *(void**)(gl + 0xf0) = g_towns[t];
}

int OwnerOf(void* entity) {
    if (g_towns.size() < 2 || !entity) return 0;
    auto it = g_owner.find((uintptr_t)entity);
    return it == g_owner.end() ? 0 : it->second;
}
int OwnerOfComponent(void* comp) { return comp ? OwnerOf(*(void**)((uint8_t*)comp + 0x10)) : 0; }

static const uintptr_t kVtComponentEntity = 0x165e88;
int OwnerOfObject(void* obj) {
    if (!obj) return -1;
    auto isEntity = [](void* p) {
        MEMORY_BASIC_INFORMATION mbi;
        if (!p || !VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
        return *(uintptr_t*)p == g_exe + kVtComponentEntity;
    };
    if (isEntity(obj)) return OwnerOf(obj);
    void* e = *(void**)((uint8_t*)obj + 0x10);
    if (isEntity(e)) return OwnerOf(e);
    return -1;
}

void OnEntityAdded(void* entity) {
    if (g_towns.size() < 2 || !entity) return;
    if (g_cur) g_owner[(uintptr_t)entity] = (uint8_t)g_cur;
    else g_owner.erase((uintptr_t)entity);
}

// ---------------------------------------------------------------- per-entity context
using EntityTick_t = void(__fastcall*)(void* entity, const int64_t* dt);
static EntityTick_t o_EntityTick;
static void __fastcall h_EntityTick(void* entity, const int64_t* dt) {
    if (g_towns.size() < 2) { o_EntityTick(entity, dt); return; }
    Scope s(OwnerOf(entity));
    o_EntityTick(entity, dt);
}

// Reached from the patched bucket loop: rcx = component, rdx = dt, r8 = rbx = the component's entity.
static void __fastcall OwnedComponentUpdate(void* comp, const int64_t* dt, void* entity) {
    using Vf9 = void(__fastcall*)(void*, const int64_t*);
    Vf9 update = (*(Vf9**)comp)[9];
    if (g_towns.size() < 2) { update(comp, dt); return; }
    Scope s(OwnerOf(entity));
    update(comp, dt);
}

static void* AllocNear(uintptr_t target, size_t size) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uintptr_t gran = si.dwAllocationGranularity;
    for (uintptr_t a = (target & ~(gran - 1)) - gran; a > target - 0x70000000ull; a -= gran) {
        void* p = VirtualAlloc((void*)a, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) return p;
    }
    return nullptr;
}

static bool PatchBucketCall() {
    uint8_t* site = (uint8_t*)(g_exe + kRvaBucketCall);
    static const uint8_t expect[] = {0x48, 0x8b, 0x01, 0xff, 0x50, 0x48};  // mov rax,[rcx]; call [rax+48h]
    if (memcmp(site, expect, sizeof expect) != 0) { Log("!! towns: bucket call site has unexpected bytes"); return false; }
    uint8_t* thunk = (uint8_t*)AllocNear(g_exe, 64);
    if (!thunk) { Log("!! towns: no memory near the exe for the thunk"); return false; }
    uint8_t code[] = {0x49, 0x89, 0xd8,                     // mov r8, rbx (entity)
                      0x48, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0,   // mov rax, OwnedComponentUpdate
                      0xff, 0xe0};                          // jmp rax (returns to the patched call's caller)
    void* fn = (void*)&OwnedComponentUpdate;
    memcpy(code + 5, &fn, 8);
    memcpy(thunk, code, sizeof code);
    DWORD old;
    VirtualProtect(site, 6, PAGE_EXECUTE_READWRITE, &old);
    site[0] = 0xe8;                                         // call thunk
    int32_t rel = (int32_t)((intptr_t)thunk - (intptr_t)(site + 5));
    memcpy(site + 1, &rel, 4);
    site[5] = 0x90;
    VirtualProtect(site, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), site, 6);
    return true;
}

static void* FindComponent(uint8_t* entity, uintptr_t typeinfoRva);

// ---------------------------------------------------------------- probe
// Citizen component -> +0x278 -> +0x68 = its model (transform: position floats at +0x158/+0x15c/+0x160), as read by
// MoveTo / BuildRoad::vf4. Town+0xa0/+0xa8 = citizen components.
void PositionHashes(char* out, size_t n) {
    out[0] = 0;
    uint8_t* gl = GameLoop();
    if (!gl || n < 32) return;
    int ntowns = g_towns.empty() ? 1 : (int)g_towns.size();
    uint32_t all = 2166136261u;
    size_t k = (size_t)snprintf(out, n, " pos=00000000 p=");
    for (int t = 0; t < ntowns; t++) {
        uint8_t* town = (uint8_t*)(g_towns.empty() ? *(void**)(gl + 0xf0) : g_towns[t]);
        if (!town) continue;
        int count = *(int*)(town + 0xa8);
        void** list = *(void***)(town + 0xa0);
        for (int i = 0; i < count && list; i++) {
            uint8_t* cit = (uint8_t*)list[i];
            uint8_t* a = cit ? *(uint8_t**)(cit + 0x278) : nullptr;
            uint8_t* model = a ? *(uint8_t**)(a + 0x68) : nullptr;
            uint32_t h = 2166136261u, hz = 2166136261u;  // x/y and height (z) separately
            if (model) {
                for (int b = 0; b < 8; b++) h = (h ^ model[0x158 + b]) * 16777619u;
                for (int b = 8; b < 12; b++) hz = (hz ^ model[0x158 + b]) * 16777619u;
            }
            all = (all ^ h) * 16777619u;  // x/y only: height follows a per-frame refresh (differs even on one PC)
            if (k + 16 < n) k += (size_t)snprintf(out + k, n - k, "%s%d:%04x/%02x", k > 0 && out[k - 1] != '=' ? "," : "", t,
                                                   (h ^ (h >> 16)) & 0xffff, (hz ^ (hz >> 8) ^ (hz >> 16) ^ (hz >> 24)) & 0xff);
        }
    }
    char head[16];
    snprintf(head, sizeof head, "%08x", all);
    memcpy(out + 5, head, 8);  // fill in " pos=XXXXXXXX"
}

bool g_copyZ;  // debug: CopyPositions reports z (in the x slot)
int CopyPositions(float* out, int maxVillagers) {
    uint8_t* gl = GameLoop();
    if (!gl) return 0;
    int ntowns = g_towns.empty() ? 1 : (int)g_towns.size(), n = 0;
    for (int t = 0; t < ntowns; t++) {
        uint8_t* town = (uint8_t*)(g_towns.empty() ? *(void**)(gl + 0xf0) : g_towns[t]);
        if (!town) continue;
        void** list = *(void***)(town + 0xa0);
        for (int i = 0; i < *(int*)(town + 0xa8) && list && n < maxVillagers; i++, n++) {
            uint8_t* cit = (uint8_t*)list[i];
            uint8_t* a = cit ? *(uint8_t**)(cit + 0x278) : nullptr;
            uint8_t* model = a ? *(uint8_t**)(a + 0x68) : nullptr;
            out[n * 2] = model ? *(float*)(model + 0x158) : 0.f;
            out[n * 2 + 1] = model ? *(float*)(model + 0x15c) : 0.f;
            if (g_copyZ) out[n * 2] = model ? *(float*)(model + 0x160) : 0.f;  // heights instead of x (POSWATCH z)
        }
    }
    return n;
}

// ---------------------------------------------------------------- town step
// Each town's resource table (Town+0x170: 0x20 B per resource kind, counts at +0/+4, limit at +8) decides which jobs
// idle villagers prefer (exe+0xfca20: "below a tenth of the limit" = urgent). A 2026-10-13 desync came from the two PCs
// disagreeing about it. At the end of every sim step: changed entries go into the desync record (tag R + town/kind), and
// a snapshot is kept; every frame (outside the sim) the tables are compared with it - a change there means something
// outside the simulation wrote a town's numbers (logged once per town/kind/field).
static const int kTableKinds = 12;
static int g_tableSnap[8][kTableKinds][3];
static bool g_tableSnapValid;
static void SnapResourceTables() {
    int n = (int)g_towns.size() < 8 ? (int)g_towns.size() : 8;
    for (int t = 0; t < n; t++) {
        int* tab = *(int**)((uint8_t*)g_towns[t] + 0x170);
        if (!tab) continue;
        for (int k = 0; k < kTableKinds; k++) {
            int* e = tab + k * 8;
            int* s = g_tableSnap[t][k];
            if (g_tableSnapValid && (s[0] != e[0] || s[1] != e[1] || s[2] != e[2]))
                ProbeNote(0x52000000u | (uint32_t)(t << 8) | (uint32_t)k, (uint32_t)e[0], e[1], e[2]);
            s[0] = e[0], s[1] = e[1], s[2] = e[2];
        }
    }
    g_tableSnapValid = true;
}
static void CheckResourceTables() {
    if (!g_tableSnapValid || g_towns.size() < 2) return;
    static std::set<int> reported;
    int n = (int)g_towns.size() < 8 ? (int)g_towns.size() : 8;
    for (int t = 0; t < n; t++) {
        int* tab = *(int**)((uint8_t*)g_towns[t] + 0x170);
        if (!tab) continue;
        for (int k = 0; k < kTableKinds; k++)
            for (int f = 0; f < 3; f++)
                if (tab[k * 8 + f] != g_tableSnap[t][k][f] && reported.insert(t * 100 + k * 3 + f).second)
                    Log("!! towns: town %d resource %d field %d changed OUTSIDE the simulation: %d -> %d", t, k, f,
                        g_tableSnap[t][k][f], tab[k * 8 + f]);
    }
}
// Professions: the 2026-10-14 repro split on a villager who was a laborer on one PC only. Each citizen's profession
// (citizen +0x1f0 = profession description; list GameLoop+0xb0 +0x20/+0x28, laborer = +0x38) as an index into that
// list, per town (Town+0xa0 citizens, +0xa8 count). Changes at the end of a step -> desync record tag P (value =
// citizen offset, a = old index, b = new index); changes found outside the simulation are logged.
static std::unordered_map<uint64_t, int> g_profSnap;  // citizen offset -> profession index
static bool g_profSnapValid;
static int ProfessionIndex(void* prof) {
    uint8_t* gl = GameLoop();
    uint8_t* list = gl ? *(uint8_t**)(gl + 0xb0) : nullptr;
    if (!list || !prof) return -1;
    void** a = *(void***)(list + 0x20);
    for (int i = 0; i < *(int*)(list + 0x28); i++)
        if (a[i] == prof) return i;
    return -2;
}
template <class F> static void ForEachCitizen(F&& f) {
    for (size_t t = 0; t < g_towns.size(); t++) {
        uint8_t* town = (uint8_t*)g_towns[t];
        uint8_t** cits = *(uint8_t***)(town + 0xa0);
        int n = *(int*)(town + 0xa8);
        for (int i = 0; i < n && cits; i++)
            if (cits[i] && simheap::Contains(cits[i])) f(cits[i]);
    }
}
static void SnapProfessions(bool inSim) {
    if (g_towns.size() < 2) return;
    int outside = 0;
    ForEachCitizen([&](uint8_t* c) {
        uint64_t off = simheap::Offset(c);
        int p = ProfessionIndex(*(void**)(c + 0x1f0));
        auto it = g_profSnap.find(off);
        if (it != g_profSnap.end() && it->second != p && g_profSnapValid) {
            if (inSim) ProbeNote(0x50000000u, (uint32_t)off, it->second, p);
            else if (outside++ < 5) Log("!! towns: citizen %llx profession %d -> %d changed OUTSIDE the simulation", off, it->second, p);
        }
        g_profSnap[off] = p;
    });
    if (inSim) g_profSnapValid = true;
}
void TablesChangedBySim() { g_tableSnapValid = false; g_profSnapValid = false; }  // a command ran between steps (e.g. while paused): not a UI write
static void ReconcileShares();  // shared storage (below)
static void PruneCouriers();    // trade couriers (below)
static void ReconcileWorkShares();  // shared workplaces (below)
static int HeldOf(void* entity, void* desc);  // (below)
static void RunAgreements();    // standing trades (below)
// A town's known goods (Town+0x110, exe+0x97330 adds one) are registered when a pile of a good is created - in the
// town that is current then. Goods that reach a town another way (trade piles are created in the GIVER's town and
// carried into the receiver's storages; shared storages; goods handed over directly) were unknown to it, and a market
// vendor's job search (MarketWork vf4 exe+0xf5450) walks the town's storages and looks every good up in a tree built
// from that list -> null node, crash at exe+0xf5dd7 (2026-10-14, three crashes "while adding workers to market").
// Each step: every good in a town's storages is registered with that town (no-op when known; deterministic order).
static std::vector<uint8_t*> StoragesOf(int t);  // (below)
static std::vector<uint8_t*> TradeSources(int t);  // storages + markets (below)
static void RegisterStoredGoods() {
    static const bool off = GetFileAttributesW(L"bmp_noregister.txt") != INVALID_FILE_ATTRIBUTES;  // test: old behaviour
    if (off) return;
    for (int t = 0; t < (int)g_towns.size(); t++)
        for (uint8_t* s : TradeSources(t)) {
            int n = *(int*)(s + 0x30);
            uint8_t* rows = *(uint8_t**)(s + 0x28);
            for (int i = 0; i < n && rows; i++) {
                void* desc = *(void**)(rows + i * 0x10);
                if (!desc) continue;
                int before = *(int*)((uint8_t*)g_towns[t] + 0x118);
                ((void(__fastcall*)(void*, void*))(g_exe + 0x97330))(g_towns[t], desc);
                if (*(int*)((uint8_t*)g_towns[t] + 0x118) != before) {
                    NonSim ns;
                    Log("towns: town %d now knows %s (it reached a storage without a pile of it being made there)", t,
                        commands::DescPath(desc).c_str());
                }
            }
        }
}
void StepTowns(void* origTown, const int64_t* dt, void (*step)(void*, const int64_t*)) {
    if (g_towns.size() < 2) { step(origTown, dt); return; }
    RegisterStoredGoods();
    ReconcileShares();
    ReconcileWorkShares();
    PruneCouriers();
    RunAgreements();
    for (int t = 0; t < (int)g_towns.size(); t++) {
        Scope s(t);
        step(g_towns[t], dt);
    }
    SnapResourceTables();
    SnapProfessions(true);
    if (++g_steps % 300 == 0 && (!g_deliverTo.empty() || !g_tradedLog.empty())) {
        NonSim ns;
        char buf[512];
        int k = sprintf_s(buf, "towns: step %llu deliveries:", g_steps);
        for (auto& d : Deliveries()) {
            k += sprintf_s(buf + k, sizeof buf - k, " [%d->%d %d %s in %d piles]", d.from, d.to, d.count,
                           commands::DescName(d.desc).c_str(), d.piles);
            if (std::find(g_tradedLog.begin(), g_tradedLog.end(), d.desc) == g_tradedLog.end()) g_tradedLog.push_back(d.desc);
        }
        for (void* desc : g_tradedLog)
            for (int t = 0; t < (int)g_towns.size(); t++)
                k += sprintf_s(buf + k, sizeof buf - k, " | town %d has %d %s", t, CountOf(t, desc), commands::DescName(desc).c_str());
        Log("%s", buf);
        // logging only: must not change anything the simulation reads (this cadence differs between machines)
        int carried = 0, couriers = 0;
        for (auto& [e, d] : g_courier) {
            int n = 0;
            if (uint8_t* inv = (uint8_t*)FindComponent((uint8_t*)e, 0x1cfe78))  // StorageComponent
                for (int i = 0; i < *(int*)(inv + 0x30); i++) n += *(int*)(*(uint8_t**)(inv + 0x28) + i * 0x10 + 8);
            carried += n;
            couriers += n > 0;
            if (n > 0 && g_steps % 3000 == 0) {
                uint8_t* inv = (uint8_t*)FindComponent((uint8_t*)e, 0x1cfe78);
                for (int i = 0; i < *(int*)(inv + 0x30); i++)
                    Log("towns:     courier %p (town %d -> %d): %d x %s", (void*)e, OwnerOf((void*)e), d,
                        *(int*)(*(uint8_t**)(inv + 0x28) + i * 0x10 + 8),
                        commands::DescName(*(void**)(*(uint8_t**)(inv + 0x28) + i * 0x10)).c_str());
            }
        }
        Log("towns:   %d villagers carrying %d trade goods", couriers, carried);
        if (g_deliverTo.empty() && !carried) g_tradedLog.clear();
    }
    if (g_steps % 1200 == 0) {  // progress per town: citizens (Town+0xa8) and entities (Town+0x08)
        char buf[256];
        int k = sprintf_s(buf, "towns: step %llu", g_steps);
        for (int t = 0; t < (int)g_towns.size(); t++)
            k += sprintf_s(buf + k, sizeof buf - k, " | town %d: %d citizens, %d entities", t,
                           *(int*)((uint8_t*)g_towns[t] + 0xa8), *(int*)((uint8_t*)g_towns[t] + 0x08));
        Log("%s", buf);
        NonSim ns;
        if (g_steps == 1200)
            for (auto& g : Goods(0)) Log("towns:   town 0 has %d x %s", g.count, commands::DescPath(g.desc).c_str());
        for (int t = 0; t < (int)g_towns.size(); t++) {  // resource table (Town+0x170, 0x20 per resource type)
            uint8_t* lim = *(uint8_t**)((uint8_t*)g_towns[t] + 0x170);
            k = sprintf_s(buf, "towns:   town %d res:", t);
            for (int i = 0; lim && i < 12; i++)
                k += sprintf_s(buf + k, sizeof buf - k, " [%d %d %d]", *(int*)(lim + i * 0x20), *(int*)(lim + i * 0x20 + 4),
                               *(int*)(lim + i * 0x20 + 12));
            Log("%s", buf);
        }
    }
}

// ---------------------------------------------------------------- new game: extra towns and their starts
// sizeGrid: a new game sizes the Town+0x140 grid here; a loaded town gets it from its post-load (exe+0x94a50 rebuilds
// the grid only while it is empty)
static void* CreateTown(bool sizeGrid = true) {
    uint8_t* gl = GameLoop();
    void* t = p_MemAlloc(0x1c0, 4);
    ((void*(__fastcall*)(void*))(g_exe + kRvaTownCtor))(t);
    int size = **(int**)(gl + 0xf8);
    if (sizeGrid) ((void(__fastcall*)(void*, int))(g_exe + kRvaArrayResize))((uint8_t*)t + 0x140, (size >> 3) * (size >> 3));
    return t;
}

static void EnsureTowns() {
    if (g_count < 2 || !g_towns.empty()) return;
    g_towns.push_back(*(void**)(GameLoop() + 0xf0));  // town 0 = the GameLoop's own
    for (int t = 1; t < g_count; t++) g_towns.push_back(CreateTown());
    g_sites.assign(g_count, Site{});
    Log("towns: created %d towns", g_count);
}

using SiteOk_t = bool(__fastcall*)(void* mapgen, int x, int y, int r, int r2);
static bool FindSite(void* mapgen, int t, int* outX, int* outY) {
    int size = **(int**)(GameLoop() + 0xf8);
    int r = *(int*)((uint8_t*)mapgen + 0x3c);
    int c = size >> 1;
    // integer directions only (bit-identical on every machine): east, west, south, north, then diagonals
    static const int dirs[8][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}, {1, 1}, {-1, -1}, {1, -1}, {-1, 1}};
    int minDist = r * 5 > 48 ? r * 5 : 48;
    for (int d = 0; d < 8; d++) {
        int di = (t - 1 + d) % 8;
        int tx = c + dirs[di][0] * size * 3 / 10, ty = c + dirs[di][1] * size * 3 / 10;
        for (int ring = 0; ring < size / 4; ring += 2) {
            for (int i = -ring; i <= ring; i += 2) {
                int cand[4][2] = {{tx + i, ty - ring}, {tx + i, ty + ring}, {tx - ring, ty + i}, {tx + ring, ty + i}};
                for (auto& p : cand) {
                    int x = p[0], y = p[1];
                    if (x < r + 2 || y < r + 2 || x >= size - r - 2 || y >= size - r - 2) continue;
                    bool farEnough = true;
                    for (int o = 0; o < t; o++) {
                        int dx = x - g_sites[o].x, dy = y - g_sites[o].y;
                        if (dx * dx + dy * dy < minDist * minDist) farEnough = false;
                    }
                    if (!farEnough) continue;
                    if (((SiteOk_t)(g_exe + kRvaSiteOk))(mapgen, x, y, r, r * r)) { *outX = x; *outY = y; return true; }
                }
            }
        }
    }
    return false;
}

using StartStructures_t = void(__fastcall*)(void* mapgen, int x, int y);
static StartStructures_t o_StartStructures;
static void __fastcall h_StartStructures(void* mapgen, int x, int y) {
    o_StartStructures(mapgen, x, y);
    if (g_count < 2) return;
    EnsureTowns();
    g_sites[0] = Site{x, y, {}, true};
    for (int t = 1; t < g_count && !g_pick; t++) {
        int tx, ty;
        if (!FindSite(mapgen, t, &tx, &ty)) { Log("!! towns: no start site for town %d", t); continue; }
        g_sites[t].x = tx;
        g_sites[t].y = ty;
        g_sites[t].placed = true;
        ((void(__fastcall*)(void*, float*, int, int))(g_exe + kRvaCellToWorld))(*(void**)(GameLoop() + 0xf8), g_sites[t].pos, tx, ty);
        Log("towns: town %d starts at cell %d,%d (town 0 at %d,%d)", t, tx, ty, x, y);
        Scope s(t);
        o_StartStructures(mapgen, tx, ty);
    }
}

using StartFamilies_t = void(__fastcall*)(void* mapgen, float* pos);
static StartFamilies_t o_StartFamilies;
static void __fastcall h_StartFamilies(void* mapgen, float* pos) {
    o_StartFamilies(mapgen, pos);
    if (g_towns.size() < 2) return;
    memcpy(g_sites[0].pos, pos, 12);
    for (int t = 1; t < (int)g_towns.size(); t++) {
        if (!g_sites[t].placed) continue;
        Scope s(t);
        o_StartFamilies(mapgen, g_sites[t].pos);
    }
}

// Killing an entity runs its components' shutdown (unregistering from *the current* town's lists). It is often
// triggered from another entity's update (a citizen finishing a clearing job, a tool): run it in the owner's town.
// (Crash seen: a zone removed while the other town was current -> lookup in the wrong Town+0x160 -> null.)
static bool InStorageList(void* town, void* e);  // shared storage (below)
static void StorageListRemove(int t, void* e);
static void ForgetShare(void* e);
using EntityKill_t = void(__fastcall*)(void* entity);
static EntityKill_t o_EntityKill;
static void __fastcall h_EntityKill(void* entity) {
    if (g_inShutdown && !g_retiredTowns.empty()) {  // an old session's entity, killed by its loop's shutdown
        auto it = g_retiredOwner.find((uintptr_t)entity);
        int t = it == g_retiredOwner.end() ? 0 : it->second;
        uint8_t* gl = GameLoop();
        void* prev = gl ? *(void**)(gl + 0xf0) : nullptr;
        if (gl && t < (int)g_retiredTowns.size()) *(void**)(gl + 0xf0) = g_retiredTowns[t];
        o_EntityKill(entity);
        if (gl) *(void**)(gl + 0xf0) = prev;
        return;
    }
    if (g_towns.size() < 2) { o_EntityKill(entity); return; }
    g_deliverTo.erase((uintptr_t)entity);
    g_courier.erase((uintptr_t)entity);
    g_courierDesc.erase((uintptr_t)entity);
    g_stuck.erase((uintptr_t)entity);
    int owner = OwnerOf(entity);
    for (int t = 0; t < (int)g_towns.size(); t++)  // a shared storage leaves the other towns' lists too
        if (t != owner && InStorageList(g_towns[t], entity)) StorageListRemove(t, entity);
    ForgetShare(entity);  // a later building at the same address must not inherit the sharing
    Scope s(owner);
    o_EntityKill(entity);
}

// ZoneComponent shutdown (vf7 exe+0x135550, recon/decomp/zoneshutdown.c): clears the zone's map cells, then erases the
// entity from the CURRENT town's zone list - Town+0x160, a tree keyed by the entity's description (*(entity+0x28)),
// node: key +0, left +0x20, right +0x28, array {data +8, count +0x10} - and the game dereferences the tree lookup
// unchecked: a zone not listed in the current town = null + 8 = crash (host 2026-10-15 00:23, tick ~848 of a new
// 2-town map, via EntityKill). Run it in the town whose list really holds this zone; if none does, clear the cells
// only (exe+0xa2830, same arguments as the game) and log, instead of crashing.
static const uintptr_t kRvaZoneShutdown = 0x135550;
using ZoneShutdown_t = void(__fastcall*)(uint8_t* comp);
static ZoneShutdown_t o_ZoneShutdown;
static bool ZoneListed(void* town, uint8_t* entity) {
    if (!town) return false;
    uintptr_t key = *(uintptr_t*)(entity + 0x28);
    uint8_t* node = *(uint8_t**)((uint8_t*)town + 0x160);
    while (node) {
        uintptr_t k = *(uintptr_t*)node;
        if (key < k) node = *(uint8_t**)(node + 0x20);
        else if (key > k) node = *(uint8_t**)(node + 0x28);
        else {
            void** data = *(void***)(node + 8);
            int n = *(int*)(node + 0x10);
            for (int i = 0; i < n; i++) if (data && data[i] == entity) return true;
            return false;
        }
    }
    return false;
}
static void __fastcall h_ZoneShutdown(uint8_t* comp) {
    uint8_t* entity = comp ? *(uint8_t**)(comp + 0x10) : nullptr;
    if (!entity || g_towns.size() < 2 || g_inShutdown) return o_ZoneShutdown(comp);
    int cur = Current();
    if (ZoneListed(g_towns[cur], entity)) return o_ZoneShutdown(comp);
    for (int t = 0; t < (int)g_towns.size(); t++) {
        if (t == cur || !ZoneListed(g_towns[t], entity)) continue;
        Log("!! towns: zone %s (owner %d) was removed in town %d's context but is listed in town %d - fixed",
            commands::DescPath(*(void**)(entity + 0x18)).c_str(), OwnerOf(entity), cur, t);
        Scope s(t);
        return o_ZoneShutdown(comp);
    }
    // listed nowhere: do the cell part only (the list erase would crash)
    uint8_t* map = nullptr;
    ((void(__fastcall*)(void*, uint8_t**))(g_exe + 0x4680))(entity, &map);  // the entity's MapComponent
    Log("!! towns: zone %s (owner %d, town %d) is in no town's zone list - cleared its cells only",
        commands::DescPath(*(void**)(entity + 0x18)).c_str(), OwnerOf(entity), cur);
    if (!map) return;
    void* desc = *(void**)(map + 0x18);
    void* none = nullptr;
    ((void(__fastcall*)(void*, void**, int, int, int, int, void*, uint32_t))(g_exe + 0xa2830))(
        *(void**)(GameLoop() + 0xf8), &none, *(short*)(map + 0x28), *(short*)(map + 0x2a), *(short*)(map + 0x2c),
        *(short*)(map + 0x2e), (uint8_t*)desc + 0x28, *(uint32_t*)(map + 0x30));
}

using LoopShutdown_t = void(__fastcall*)(uint8_t* loop);
static LoopShutdown_t o_LoopShutdown;
static void __fastcall h_LoopShutdown(uint8_t* loop) {
    if (!g_retiredTowns.empty()) *(void**)(loop + 0xf0) = g_retiredTowns[0];
    g_inShutdown = true;
    o_LoopShutdown(loop);
    g_inShutdown = false;
    if (!g_retiredTowns.empty()) *(void**)(loop + 0xf0) = g_retiredTowns[0];
    g_retiredTowns.clear();  // (towns 1+ of the old session leak: a few KB per session)
    g_retiredOwner.clear();
}

using StartSupplies_t = void(__fastcall*)(void* mapgen);
static StartSupplies_t o_StartSupplies;
static void __fastcall h_StartSupplies(void* mapgen) {
    o_StartSupplies(mapgen);
    uint8_t* t0 = g_towns.empty() ? nullptr : (uint8_t*)g_towns[0];
    for (int t = 1; t < (int)g_towns.size(); t++) {
        if (!g_sites[t].placed) continue;
        // the supplies are spread over the town's known resource kinds (Town+0x110), filled for town 0 just before
        for (int i = 0; i < *(int*)(t0 + 0x118); i++)
            ((void(__fastcall*)(void*, void*))(g_exe + kRvaTownAddResource))(g_towns[t], (*(void***)(t0 + 0x110))[i]);
        Scope s(t);
        o_StartSupplies(mapgen);
    }
}

using StartAll_t = void*(__fastcall*)(void* mapgen, float* out);
static StartAll_t o_StartAll;
static void* __fastcall h_StartAll(void* mapgen, float* out) {
    void* r = o_StartAll(mapgen, out);
    if (g_towns.size() < 2) return r;
    // the town-wide "known" lists (resources, seeds/livestock templates) the start filled for town 0
    uint8_t* t0 = (uint8_t*)g_towns[0];
    for (int t = 1; t < (int)g_towns.size(); t++) {
        for (int i = 0; i < *(int*)(t0 + 0x118); i++)
            ((void(__fastcall*)(void*, void*))(g_exe + kRvaTownAddResource))(g_towns[t], (*(void***)(t0 + 0x110))[i]);
        for (int i = 0; i < *(int*)(t0 + 0x128); i++)
            ((void(__fastcall*)(void*, void*))(g_exe + kRvaTownAddTemplate))(g_towns[t], (*(void***)(t0 + 0x120))[i]);
    }
    return r;
}

// ---------------------------------------------------------------- settling mid-game
// By the time a player picks, the map is covered in trees/rocks (scattered after the automatic start); the game's
// site check (all cells buildable, cell byte +0x29 == 2) fails on them. They are cleared like a harvest without the
// drop: EntityKill (exe+0xc8370) + the manager's removal list (+0xd0, processed at the end of its update).
static const uintptr_t kTiMapComponent = 0x1cf9e0;       // type id globals
static const uintptr_t kTiNaturalResource = 0x1cfb30;
static const uintptr_t kRvaEntityKill = 0xc8370;
static const uintptr_t kRvaArrayAppend = 0x3110;         // (array*, newCount, InternalBasePtr* value)

static void* FindComponent(uint8_t* entity, uintptr_t typeinfoRva) {
    int want = *(int*)(g_exe + typeinfoRva), n = *(int*)(entity + 0x38);
    for (int i = 0; i < n; i++) {
        void* comp = (*(void***)(entity + 0x30))[i];
        if (((int(__fastcall*)(void*))(*(void***)comp)[2])(comp) == want) return comp;
    }
    return nullptr;
}

// natural resources (trees, rocks, ...) whose map cell lies within r of (x, y)
static std::vector<std::pair<uint8_t*, std::pair<int, int>>> NaturalsNear(int x, int y, int r) {
    std::vector<std::pair<uint8_t*, std::pair<int, int>>> out;
    uint8_t* mgr = *(uint8_t**)(GameLoop() + 0xe8);
    int n = *(int*)(mgr + 0x10);
    for (int i = 0; i < n; i++) {
        uint8_t* e = (*(uint8_t***)(mgr + 0x08))[i];
        if (!e || (*(uint32_t*)(e + 0x40) & 0x400)) continue;  // dead
        uint8_t* map = (uint8_t*)FindComponent(e, kTiMapComponent);
        if (!map) continue;
        int cx = *(int16_t*)(map + 0x28), cy = *(int16_t*)(map + 0x2a);
        if ((cx - x) * (cx - x) + (cy - y) * (cy - y) > r * r) continue;
        if (FindComponent(e, kTiNaturalResource)) out.push_back({e, {cx, cy}});
    }
    return out;
}

// cells that are buildable now or only blocked by natural resources
static bool SiteClearable(int x, int y, int r) {
    uint8_t* gl = GameLoop();
    int* map = *(int**)(gl + 0xf8);
    int size = map[0];
    uint8_t* cells = *(uint8_t**)((uint8_t*)map + 0x80);  // map: int size, ..., +0x80 cells (0x30 B each, +0x29 type)
    auto naturals = NaturalsNear(x, y, r + 3);
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy > r * r) continue;
            int cx = x + dx, cy = y + dy;
            if (cells[((int64_t)cy * size + cx) * 0x30 + 0x29] == 2) continue;
            bool natural = false;
            for (auto& [e, c] : naturals)
                if (abs(c.first - cx) <= 1 && abs(c.second - cy) <= 1) { natural = true; break; }
            if (!natural) return false;
        }
    return true;
}

// ---------------------------------------------------------------- goods / trade
// StorageComponent: +0x28 array of {desc, int count} (0x10 each), +0x30 entries, +0x38 capacity;
// used space = exe+0x1323e0 (sum desc+0x48 * count), room check exe+0x132250, add exe+0x1324d0, remove exe+0x132620.
static const uintptr_t kTiStorage = 0x1cfe78;
static const uintptr_t kRvaStorageUsed = 0x1323e0;
static const uintptr_t kRvaStorageRoom = 0x132250;
static const uintptr_t kRvaStorageAdd = 0x1324d0;
static const uintptr_t kRvaStorageRemove = 0x132620;

static std::vector<uint8_t*> StoragesOf(int t) {
    std::vector<uint8_t*> out;
    if (t < 0 || t >= (int)(g_towns.size() < 2 ? 1 : g_towns.size())) return out;
    uint8_t* town = g_towns.size() < 2 ? *(uint8_t**)(GameLoop() + 0xf0) : (uint8_t*)g_towns[t];
    int n = *(int*)(town + 0x08);
    for (int i = 0; i < n; i++) {
        uint8_t* e = (*(uint8_t***)town)[i];
        if (!e || (*(uint32_t*)(e + 0x40) & 0x400)) continue;
        if (uint8_t* s = (uint8_t*)FindComponent(e, kTiStorage)) out.push_back(s);
    }
    return out;
}

// Markets are NOT in a town's storage list (Town+0x00) - their stock sits in the market's own StorageComponent (vendors
// carry it there from the storages). Trades count and take goods from the storages AND the town's markets (user:
// "can't trade things that are in the market"); deliveries still go into storages. Markets = entities with a
// MarketComponent (vtable exe+0x15fd60), in entity-manager order (deterministic), cached per sim step.
static bool IsMarket(uint8_t* e) {
    for (int c = 0; c < *(int*)(e + 0x38); c++)
        if ((uintptr_t)*(void**)(*(uint8_t***)(e + 0x30))[c] - g_exe == 0x15fd60) return true;
    return false;
}
static std::vector<uint8_t*> MarketsOf(int t) {
    static std::vector<std::vector<uint8_t*>> cache;
    static uint64_t cachedStep = ~0ull;
    static int cachedCount = -1;
    uint8_t* gl = GameLoop();
    uint8_t* mgr = gl ? *(uint8_t**)(gl + 0xe8) : nullptr;
    if (!mgr || t < 0) return {};
    int count = *(int*)(mgr + 0x10);
    if (cachedStep != g_steps || cachedCount != count || cache.size() != g_towns.size()) {
        cache.assign(g_towns.size() < 1 ? 1 : g_towns.size(), {});
        for (int i = 0; i < count; i++) {
            uint8_t* e = (*(uint8_t***)(mgr + 0x08))[i];
            if (!e || (*(uint32_t*)(e + 0x40) & 0x400) || !IsMarket(e)) continue;
            int owner = g_towns.size() < 2 ? 0 : OwnerOf(e);
            if (owner < 0 || owner >= (int)cache.size()) continue;
            if (uint8_t* s = (uint8_t*)FindComponent(e, kTiStorage)) cache[owner].push_back(s);
        }
        cachedStep = g_steps, cachedCount = count;
    }
    return t < (int)cache.size() ? cache[t] : std::vector<uint8_t*>{};
}
static std::vector<uint8_t*> TradeSources(int t) {  // storages first, then markets
    std::vector<uint8_t*> out = StoragesOf(t);
    for (uint8_t* m : MarketsOf(t)) out.push_back(m);
    return out;
}

// Debug (uiscript `storages`): each town's storage list (Town+0x00) and every market (component vtable exe+0x15fd60)
// with its own StorageComponent stock, and whether it is in its town's list.
void DebugStorages() {
    auto goods = [](uint8_t* s) {
        std::string out;
        int n = *(int*)(s + 0x30);
        for (int i = 0; i < n; i++) {
            uint8_t* row = *(uint8_t**)(s + 0x28) + i * 0x10;
            std::string p = commands::DescPath(*(void**)row);
            if (p.rfind("Template\\RawMaterial", 0) == 0) p = p.substr(20, p.find('.') - 20);
            out += " " + std::to_string(*(int*)(row + 8)) + " " + p;
        }
        return out;
    };
    auto isMarket = [&](uint8_t* e) {
        for (int c = 0; c < *(int*)(e + 0x38); c++)
            if ((uintptr_t)*(void**)(*(uint8_t***)(e + 0x30))[c] - g_exe == 0x15fd60) return true;
        return false;
    };
    for (int t = 0; t < (int)g_towns.size(); t++) {
        uint8_t* town = (uint8_t*)g_towns[t];
        int n = *(int*)(town + 0x08), markets = 0;
        for (int i = 0; i < n; i++) {
            uint8_t* e = (*(uint8_t***)town)[i];
            if (e && isMarket(e)) markets++;
        }
        Log("storages: town %d list has %d entities, %d of them markets", t, n, markets);
    }
    uint8_t* mgr = *(uint8_t**)(GameLoop() + 0xe8);
    for (int i = 0; i < *(int*)(mgr + 0x10); i++) {
        uint8_t* e = (*(uint8_t***)(mgr + 0x08))[i];
        if (!e || !isMarket(e)) continue;
        int t = OwnerOf(e);
        bool listed = false;
        uint8_t* town = (uint8_t*)g_towns[t];
        for (int k = 0; k < *(int*)(town + 0x08); k++) listed |= (*(uint8_t***)town)[k] == e;
        uint8_t* s = (uint8_t*)FindComponent(e, kTiStorage);
        Log("storages: market of town %d, in its list: %d, storage %s:%s", t, listed, s ? "yes" : "no", s ? goods(s).c_str() : "");
    }
}

std::vector<Good> Goods(int t) {
    std::vector<Good> out;
    for (uint8_t* s : TradeSources(t)) {
        int n = *(int*)(s + 0x30);
        for (int i = 0; i < n; i++) {
            void* desc = *(void**)(*(uint8_t**)(s + 0x28) + i * 0x10);
            int count = *(int*)(*(uint8_t**)(s + 0x28) + i * 0x10 + 8);
            bool merged = false;
            for (auto& g : out) if (g.desc == desc) { g.count += count; merged = true; break; }
            if (!merged && count > 0) out.push_back({desc, count});
        }
    }
    return out;
}

int CountOf(int t, void* desc) {
    for (auto& g : Goods(t)) if (g.desc == desc) return g.count;
    return 0;
}

static int RoomFor(int t, void* desc) {  // how many more of desc fit into t's storages
    int total = 0;
    for (uint8_t* s : StoragesOf(t)) {
        int lo = 0, hi = 100000;  // binary search the largest n the game's own check accepts
        while (lo < hi) {
            int mid = (lo + hi + 1) / 2;
            if (((bool(__fastcall*)(void*, void*, int))(g_exe + kRvaStorageRoom))(s, desc, mid)) lo = mid; else hi = mid - 1;
        }
        total += lo;
    }
    return total;
}

static void Move(int from, int to, void* desc, int n) {
    int left = n;
    for (uint8_t* s : TradeSources(from)) {
        if (left <= 0) break;
        int have = 0, m = *(int*)(s + 0x30);
        for (int i = 0; i < m; i++)
            if (*(void**)(*(uint8_t**)(s + 0x28) + i * 0x10) == desc) have = *(int*)(*(uint8_t**)(s + 0x28) + i * 0x10 + 8);
        int take = have < left ? have : left;
        if (take > 0) ((void(__fastcall*)(void*, void*, int))(g_exe + kRvaStorageRemove))(s, desc, take);
        left -= take;
    }
    left = n;
    for (uint8_t* s : StoragesOf(to)) {
        while (left > 0 && ((bool(__fastcall*)(void*, void*, int))(g_exe + kRvaStorageRoom))(s, desc, 1)) {
            int chunk = left;
            while (chunk > 1 && !((bool(__fastcall*)(void*, void*, int))(g_exe + kRvaStorageRoom))(s, desc, chunk)) chunk /= 2;
            ((void(__fastcall*)(void*, void*, int))(g_exe + kRvaStorageAdd))(s, desc, chunk);
            left -= chunk;
        }
    }
}

// ---------------------------------------------------------------- jobs + deliveries
// All jobs ("works") of every town sit in one global WorkManager (GameLoop+0x118); a citizen looking for work gets
// them per list from exe+0xfd030 (mgr, InternalBasePtr* citizen, tree of works sorted by priority): vf9 (refresh),
// then vf4 (try: 2 = assigned, 1 = nothing to do now -> stamped with mgr+8 for this round). Replaced below so a
// citizen only gets jobs whose entity (work+0x10) belongs to the citizen's town (before this, any town's laborers
// took any town's clearing/building/hauling jobs).
//
// ---------------------------------------------------------------- shared storage
// Town+0x00/+0x08/+0x0c = the town's storage entities (array of entity pointers, count, capacity): the town step
// recomputes its goods totals from them (exe+0x8f3a0) and every storage search walks them (exe+0x92a60).
// StorageLocation vf17 (exe+0x132740) UNREGISTERS a building (exe+0x2cc0 = erase from the current town's list).
// Sharing one building = it is also in the other town's list: that town's villagers store and take goods there and
// count them as theirs. g_shareMask[entity] = bit per town it is shared with (lockstep command kShareStorage, saved in
// the save block "BPSB"); ReconcileShares (every sim step) keeps the lists matching it - walking the towns' own lists
// (deterministic order), the map is only looked up.
static const uintptr_t kRvaStorageStop = 0x132740;
static std::unordered_map<uintptr_t, uint8_t> g_shareMask;

static void ForgetShare(void* e) { g_shareMask.erase((uintptr_t)e); }
static uint8_t ShareMaskOf(void* e) {
    auto it = g_shareMask.find((uintptr_t)e);
    return it == g_shareMask.end() ? 0 : it->second;
}

static bool InStorageList(void* town, void* e) {
    void** list = *(void***)town;
    for (int i = 0; i < *(int*)((uint8_t*)town + 8); i++)
        if (list[i] == e) return true;
    return false;
}
static void StorageListAdd(int t, void* e) {
    if (t < 0 || t >= (int)g_towns.size() || InStorageList(g_towns[t], e)) return;
    uint8_t* town = (uint8_t*)g_towns[t];
    void**& list = *(void***)town;
    int& n = *(int*)(town + 8);
    int& cap = *(int*)(town + 0xc);
    if (n >= cap) {  // grow like the game's arrays; the buffer must come from the sim heap (identical on every PC)
        int newCap = cap < 4 ? 8 : cap * 2;
        void** grown = (void**)simheap::Alloc((size_t)newCap * 8, 8);
        if (!grown) { Log("!! towns: shared storage: no sim-heap memory"); return; }
        for (int i = 0; i < n; i++) grown[i] = list[i];
        void** old = list;
        list = grown;
        cap = newCap;
        if (old) {
            static void* memFree = RuntimeExport("?MemFree@MemoryAllocator@System@@SAXPEAX@Z");  // routed by address
            if (memFree) ((void(__fastcall*)(void*))memFree)(old);
        }
    }
    list[n++] = e;
}
static void StorageListRemove(int t, void* e) {
    if (t < 0 || t >= (int)g_towns.size()) return;
    uint8_t* town = (uint8_t*)g_towns[t];
    void** list = *(void***)town;
    int& n = *(int*)(town + 8);
    for (int i = 0; i < n; i++)
        if (list[i] == e) {
            for (int j = i; j + 1 < n; j++) list[j] = list[j + 1];  // keep the order (storage search walks it)
            n--;
            return;
        }
}

using StorageStop_t = void(__fastcall*)(uint8_t* comp);
static StorageStop_t o_StorageStop;
static void __fastcall h_StorageStop(uint8_t* comp) {
    if (g_towns.size() > 1) {  // the building leaves every other town's list at the same moment
        void* e = *(void**)(comp + 0x10);
        void* cur = *(void**)(GameLoop() + 0xf0);
        for (int t = 0; t < (int)g_towns.size(); t++)
            if (g_towns[t] != cur) StorageListRemove(t, e);
        g_shareMask.erase((uintptr_t)e);
    }
    o_StorageStop(comp);
}

// Once per sim step: each town's list holds its own storages plus exactly the buildings shared with it.
static void ReconcileShares() {
    int n = (int)g_towns.size();
    for (int t = 0; t < n; t++) {
        uint8_t* town = (uint8_t*)g_towns[t];
        for (int i = *(int*)(town + 8) - 1; i >= 0; i--) {
            void* e = (*(void***)town)[i];
            if (OwnerOf(e) != t && !(ShareMaskOf(e) & (1 << t))) StorageListRemove(t, e);
        }
    }
    if (g_shareMask.empty()) return;
    for (int o = 0; o < n; o++) {
        uint8_t* town = (uint8_t*)g_towns[o];
        for (int i = 0; i < *(int*)(town + 8); i++) {
            void* e = (*(void***)town)[i];
            uint8_t mask = OwnerOf(e) == o ? ShareMaskOf(e) : 0;
            for (int t = 0; t < n && mask; t++)
                if (t != o && (mask & (1 << t))) StorageListAdd(t, e);
        }
    }
}

static bool OwnStorage(int owner, void* e) {  // e is one of owner's storage buildings
    return owner >= 0 && owner < (int)g_towns.size() && OwnerOf(e) == owner && InStorageList(g_towns[owner], e);
}

void ShareStorage(int owner, void* e, int target, bool on) {  // simulation (command)
    if (target < 0 || target >= (int)g_towns.size() || target == owner || target > 7 || !OwnStorage(owner, e)) {
        Log("!! towns: share storage: town %d can't share %p with town %d", owner, e, target);
        return;
    }
    uint8_t mask = ShareMaskOf(e);
    mask = on ? (uint8_t)(mask | (1 << target)) : (uint8_t)(mask & ~(1 << target));
    if (mask) g_shareMask[(uintptr_t)e] = mask; else g_shareMask.erase((uintptr_t)e);
    if (on) StorageListAdd(target, e); else StorageListRemove(target, e);
    Log("towns: town %d %s a storage with town %d", owner, on ? "shares" : "stops sharing", target);
}
bool IsOwnStorage(void* e) { return g_towns.size() > 1 && e && OwnStorage(OwnerOf(e), e); }
bool SharedWith(void* e, int town) { return town >= 0 && town < 8 && (ShareMaskOf(e) & (1 << town)); }

// ---------------------------------------------------------------- shared workplaces
// Each Town keeps +0x100 = tree profession -> array of its workplace buildings (node: +0 key, +8 array, +0x10 count,
// +0x20 left, +0x28 right). Hiring (exe+0x925d0) looks only there: a workplace with a free slot (WorkPlaceComponent
// +0x60 workers < WorkComponent +0x30 limit). exe+0x8ea70(town, &building, key) adds a building, exe+0x8ea20 removes it
// (registration: exe+0x1342c0 with key = WorkPlaceComponent +0x48 = the profession). Sharing one workplace with another
// town = it is also in that town's tree: that town's villagers of the profession get hired there (up to the limit the
// owner sets). Villagers: CitizenComponent +0x1f0 profession, +0x248 workplace building, exe+0x1173b0(cit, &building)
// sets it. g_workShare[building] = bit per town (command kShareWork, save block "BPWS").
static const uintptr_t kTiWorkPlace = 0x1d00e0;
static const uintptr_t kRvaWorkMapAdd = 0x8ea70, kRvaWorkMapRemove = 0x8ea20, kRvaSetWorkplace = 0x1173b0;
static std::unordered_map<uintptr_t, uint8_t> g_workShare;
static uint8_t WorkShareMaskOf(void* e) {
    auto it = g_workShare.find((uintptr_t)e);
    return it == g_workShare.end() ? 0 : it->second;
}
static void* WorkKey(void* e) {
    uint8_t* wp = e ? (uint8_t*)FindComponent((uint8_t*)e, kTiWorkPlace) : nullptr;
    return wp ? *(void**)(wp + 0x48) : nullptr;
}
static bool InWorkMap(int t, void* e, void* key) {
    uint8_t* node = *(uint8_t**)((uint8_t*)g_towns[t] + 0x100);
    while (node) {
        uint64_t k = *(uint64_t*)node;
        if ((uint64_t)key < k) node = *(uint8_t**)(node + 0x20);
        else if ((uint64_t)key > k) node = *(uint8_t**)(node + 0x28);
        else break;
    }
    if (!node) return false;
    void** arr = *(void***)(node + 8);
    for (int i = 0; i < *(int*)(node + 0x10); i++)
        if (arr[i] == e) return true;
    return false;
}
static void WorkMapAdd(int t, void* e) {
    void* key = WorkKey(e);
    if (!key || t < 0 || t >= (int)g_towns.size() || InWorkMap(t, e, key)) return;
    ((void(__fastcall*)(void*, void**, void*))(g_exe + kRvaWorkMapAdd))(g_towns[t], &e, key);
}
static void WorkMapRemove(int t, void* e) {
    void* key = WorkKey(e);
    if (!key || t < 0 || t >= (int)g_towns.size() || !InWorkMap(t, e, key)) return;
    ((void(__fastcall*)(void*, void**, void*))(g_exe + kRvaWorkMapRemove))(g_towns[t], &e, key);
}
// town t's villagers working at building e go back to being laborers (what the game does when a workplace goes away)
static void ReleaseWorkers(int t, void* e) {
    uint8_t* town = (uint8_t*)g_towns[t];
    void* laborer = *(void**)(*(uint8_t**)(GameLoop() + 0xb0) + 0x30);
    int released = 0;
    for (int i = 0; i < *(int*)(town + 0xa8); i++) {
        uint8_t* cit = (*(uint8_t***)(town + 0xa0))[i];
        if (!cit || *(void**)(cit + 0x248) != e) continue;
        void* none = nullptr;
        ((void(__fastcall*)(void*, void**))(g_exe + kRvaSetWorkplace))(cit, &none);
        *(void**)(cit + 0x1f0) = laborer;
        released++;
    }
    if (released) Log("towns: %d of town %d's villagers left a workplace that is no longer shared", released, t);
}
void ShareWork(int owner, void* e, int target, bool on) {  // simulation (command)
    if (target < 0 || target >= (int)g_towns.size() || target == owner || target > 7 || OwnerOf(e) != owner || !WorkKey(e)) {
        Log("!! towns: share workplace: town %d can't share %p with town %d", owner, e, target);
        return;
    }
    uint8_t mask = WorkShareMaskOf(e);
    mask = on ? (uint8_t)(mask | (1 << target)) : (uint8_t)(mask & ~(1 << target));
    if (mask) g_workShare[(uintptr_t)e] = mask; else g_workShare.erase((uintptr_t)e);
    if (on) WorkMapAdd(target, e);
    else { WorkMapRemove(target, e); ReleaseWorkers(target, e); }
    Log("towns: town %d %s a workplace with town %d", owner, on ? "shares" : "stops sharing", target);
}
bool WorkSharedWith(void* e, int town) { return town >= 0 && town < 8 && (WorkShareMaskOf(e) & (1 << town)); }
bool IsOwnWorkplace(void* e) {  // e may be anything a window controller points at: entity check first (vtable)
    if (g_towns.size() < 2 || !e) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(e, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || *(uintptr_t*)e != g_exe + kVtComponentEntity)
        return false;
    return OwnerOf(e) == g_local && WorkKey(e) != nullptr;
}
int StockAt(void* e) { return HeldOf(e, nullptr); }  // goods kept in a building (tests)
int WorkersOfTownAt(void* e, int t) {  // town t's villagers working at building e
    if (t < 0 || t >= (int)g_towns.size()) return 0;
    uint8_t* town = (uint8_t*)g_towns[t];
    int n = 0;
    for (int i = 0; i < *(int*)(town + 0xa8); i++) {
        uint8_t* cit = (*(uint8_t***)(town + 0xa0))[i];
        n += cit && *(void**)(cit + 0x248) == e;
    }
    return n;
}
int ForeignWorkersAt(void* e) {  // UI: villagers of other towns working there
    int n = 0;
    for (int t = 0; t < (int)g_towns.size(); t++) {
        if (t == OwnerOf(e)) continue;
        uint8_t* town = (uint8_t*)g_towns[t];
        for (int i = 0; i < *(int*)(town + 0xa8); i++) {
            uint8_t* cit = (*(uint8_t***)(town + 0xa0))[i];
            n += cit && *(void**)(cit + 0x248) == e;
        }
    }
    return n;
}
// every sim step: shared workplaces are in their towns' trees (a load rebuilds the trees from the owners only), and a
// building that stopped being a workplace (demolished) is forgotten. Sorted by sim-heap offset: the trees' arrays get
// the same order on every PC.
static void ReconcileWorkShares() {
    if (g_workShare.empty()) return;
    std::vector<std::pair<uint64_t, void*>> list;
    for (auto& [e, m] : g_workShare) list.push_back({simheap::Offset((void*)e), (void*)e});
    std::sort(list.begin(), list.end());
    for (auto& [off, e] : list) {
        if (!WorkKey(e) || (*(uint32_t*)((uint8_t*)e + 0x40) & 0x400)) {  // gone (entity flag bit 10 = dead)
            for (int t = 0; t < (int)g_towns.size(); t++)
                if (t != OwnerOf(e) && (WorkShareMaskOf(e) & (1 << t))) ReleaseWorkers(t, e);
            g_workShare.erase((uintptr_t)e);
            continue;
        }
        uint8_t mask = WorkShareMaskOf(e);
        for (int t = 0; t < (int)g_towns.size(); t++)
            if (t != OwnerOf(e) && (mask & (1 << t))) WorkMapAdd(t, e);
    }
}
// The monthly labor rebalance (exe+0xd9880, from the town step) of the current town builds an entry per workplace in
// its tree (Town+0x100) from the building's worker count (WorkPlaceComponent +0x60) and matches the town's own
// villagers against them - a shared workplace's count includes other towns' villagers, which it never finds (crash at
// tick 18000 = month 1, both on the owner's and the guest town's side). During the call each shared workplace in the
// current town's tree counts only that town's villagers.
using LaborBalance_t = void(__fastcall*)(void* p);
static LaborBalance_t o_LaborBalance;
static const uintptr_t kRvaLaborBalance = 0xd9880;
static void __fastcall h_LaborBalance(void* p) {
    if (g_workShare.empty()) return o_LaborBalance(p);
    std::vector<std::pair<uint8_t*, int>> hidden;
    std::vector<std::pair<uint64_t, void*>> list;
    for (auto& [e, m] : g_workShare) list.push_back({simheap::Offset((void*)e), (void*)e});
    std::sort(list.begin(), list.end());
    int cur = Current();
    for (auto& [off, e] : list) {
        if (!WorkKey(e) || (OwnerOf(e) != cur && !WorkSharedWith(e, cur))) continue;
        int foreign = 0;  // villagers of the other towns working there
        for (int t = 0; t < (int)g_towns.size(); t++)
            if (t != cur) foreign += WorkersOfTownAt(e, t);
        uint8_t* comp = (uint8_t*)FindComponent((uint8_t*)e, kTiWorkPlace);
        if (!foreign || !comp) continue;
        *(int*)(comp + 0x60) -= foreign;
        hidden.push_back({comp, foreign});
    }
    o_LaborBalance(p);
    for (auto& [comp, foreign] : hidden) *(int*)(comp + 0x60) += foreign;
}

// The rebalance's last step (exe+0xdb290..db2ae) gives the villagers left over (taken from a job, not picked by any
// workplace) their professions back from a tree "profession description -> count", always taking the LEFTMOST node =
// the lowest description ADDRESS. Descriptions live in the engine heap, whose layout differs per PC -> each PC paired
// villagers and professions differently (the 2026-10-14 desyncs, every game month; reproduced locally with
// bmp_heapjitter). Patched: that loop asks RebalancePickNode for the node instead = the profession that comes first in
// the game's profession list (GameLoop+0xb0 +0x20) - the same everywhere. Tree node: +0 key, +8 count, +0x10 parent,
// +0x18 left, +0x20 right; erase (exe+0x1d3c0) works by node, so the BST order stays intact.
static int ProfessionIndex(void* prof);
static void* RebalancePickNode(uint8_t* root) {
    uint8_t* best = nullptr;
    int bestIdx = 0x7fffffff;
    std::vector<uint8_t*> stack;
    if (root) stack.push_back(root);
    while (!stack.empty()) {
        uint8_t* n = stack.back();
        stack.pop_back();
        int i = ProfessionIndex(*(void**)n);
        if (i < 0) i = 0x7ffffff0;  // not in the list (never seen): after every listed one
        if (!best || i < bestIdx || (i == bestIdx && commands::DescPath(*(void**)n) < commands::DescPath(*(void**)best)))
            best = n, bestIdx = i;
        if (uint8_t* l = *(uint8_t**)(n + 0x18)) stack.push_back(l);
        if (uint8_t* r = *(uint8_t**)(n + 0x20)) stack.push_back(r);
    }
    return best;
}
static bool PatchRebalancePick() {
    if (GetFileAttributesW(L"bmp_norebalancefix.txt") != INVALID_FILE_ATTRIBUTES) return false;  // test: game's order
    uint8_t* at = (uint8_t*)(g_exe + 0xdb290);
    static const uint8_t expect[] = {0x48, 0x8B, 0x7C, 0x24, 0x40, 0x48, 0x85, 0xFF, 0x74, 0x14};  // mov rdi,[rsp+40h]; test; jz
    if (memcmp(at, expect, sizeof expect) != 0) return false;
    uint8_t* stub = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) return false;
    uint8_t* p = stub;
    auto emit = [&](std::initializer_list<uint8_t> b) { for (uint8_t x : b) *p++ = x; };
    auto emit64 = [&](uint64_t v) { memcpy(p, &v, 8); p += 8; };
    // push rax, rcx, rdx, r8, r9, r10, r11 (7 x 8 = 56 B: the tree root moves from [rsp+40h] to [rsp+78h])
    emit({0x50, 0x51, 0x52, 0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53});
    emit({0x48, 0x8B, 0x4C, 0x24, 0x78});                   // mov rcx, [rsp+78h]
    emit({0x48, 0x83, 0xEC, 0x28});                         // sub rsp, 28h  (56 + 40 = 96: stays 16-aligned)
    emit({0x48, 0xB8}); emit64((uint64_t)&RebalancePickNode);  // mov rax, imm64
    emit({0xFF, 0xD0});                                     // call rax
    emit({0x48, 0x83, 0xC4, 0x28});                         // add rsp, 28h
    emit({0x48, 0x89, 0xC7});                               // mov rdi, rax
    emit({0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58, 0x5A, 0x59, 0x58});  // pop r11..rax
    emit({0xFF, 0x25, 0, 0, 0, 0}); emit64(g_exe + 0xdb2ae);  // jmp [rip] -> back after the leftmost walk
    FlushInstructionCache(GetCurrentProcess(), stub, p - stub);
    DWORD old;
    if (!VirtualProtect(at, 30, PAGE_EXECUTE_READWRITE, &old)) return false;
    uint8_t jmp[30];
    memset(jmp, 0x90, sizeof jmp);
    jmp[0] = 0xFF, jmp[1] = 0x25, memset(jmp + 2, 0, 4);
    uint64_t dest = (uint64_t)stub;
    memcpy(jmp + 6, &dest, 8);
    memcpy(at, jmp, sizeof jmp);
    VirtualProtect(at, 30, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, 30);
    return true;
}

// the game unregisters a workplace from its owner's town (demolished, ...): it leaves the other towns' trees too
using WorkMapRemove_t = void(__fastcall*)(void* town, void** e, void* key);
static WorkMapRemove_t o_WorkMapRemove;
static void __fastcall h_WorkMapRemove(void* town, void** e, void* key) {
    o_WorkMapRemove(town, e, key);
    if (g_towns.size() < 2 || !e || !*e || !WorkShareMaskOf(*e)) return;
    int owner = OwnerOf(*e);
    if (owner < 0 || owner >= (int)g_towns.size() || g_towns[owner] != town) return;
    void* b = *e;
    uint8_t mask = WorkShareMaskOf(b);
    g_workShare.erase((uintptr_t)b);
    for (int t = 0; t < (int)g_towns.size(); t++)
        if (t != owner && (mask & (1 << t))) {
            if (InWorkMap(t, b, key)) o_WorkMapRemove(g_towns[t], &b, key);  // still listed there: take it out
            ReleaseWorkers(t, b);
        }
}
// a villager working at another town's shared workplace: the building's jobs are open to it, and what it carries to
// storage goes to the building's owner
static void* ForeignWorkplace(uint8_t* cit) {
    void* wp = cit ? *(void**)(cit + 0x248) : nullptr;
    return wp && OwnerOf(wp) != OwnerOf(*(void**)(cit + 0x10)) && WorkSharedWith(wp, OwnerOf(*(void**)(cit + 0x10))) ? wp : nullptr;
}
// ---------------------------------------------------------------- standing trades ("10 leather every season")
// A trade both players agreed to, repeated at the start of every season or year (game calendar, so the same tick on
// every PC). Each run is an ordinary Trade: all or nothing, the goods walk over as delivery piles. A run that can't be
// paid (not enough of a good) is skipped and remembered for the UI. Saved in the save block ("BPAG").
struct Agreement {
    int id, a, b;          // a gives nA of descA to b, b gives nB of descB to a
    void* descA;
    void* descB;
    int nA, nB, period;    // period 1 = season, 2 = year
    int64_t lastIdx;       // calendar index of the last run
    int lastOk;            // 1 delivered, 0 skipped (not enough), -1 not run yet
};
static std::vector<Agreement> g_agreements;
static int g_nextAgreement = 1;

// Game calendar: *(GameLoop+0x128) = {int year; Time (int64 microseconds) into the year}; a year is 3600 game seconds,
// 12 months (StatusBarUI::vf7 exe+0x7eda0), a season = 3 months.
static int64_t CalendarIndex(int period) {
    uint8_t* cal = GameLoop() ? *(uint8_t**)(GameLoop() + 0x128) : nullptr;
    if (!cal) return -1;
    int year = *(int*)cal;
    int64_t us = *(int64_t*)(cal + 8);
    int month = (int)(us * 12 / 3600000000ll);
    if (month < 0) month = 0;
    if (month > 11) month = 11;
    return period == 1 ? (int64_t)year * 4 + month / 3 : year;
}

static void RunAgreement(Agreement& g, const char* when) {
    const char* why = "";
    g.lastOk = Trade(g.a, g.descA, g.nA, g.b, g.descB, g.nB, &why) ? 1 : 0;
    Log("towns: standing trade %d (%s): town %d gives %d, town %d gives %d - %s%s", g.id, when, g.a, g.nA, g.b, g.nB,
        g.lastOk ? "delivered" : "skipped: ", g.lastOk ? "" : why);
}

bool AddAgreement(int a, void* descA, int nA, int b, void* descB, int nB, int period) {  // simulation (command)
    if (a == b || a < 0 || b < 0 || a >= (int)g_towns.size() || b >= (int)g_towns.size() || (period != 1 && period != 2) ||
        (!descA && !descB) || nA < 0 || nB < 0)
        return false;
    g_agreements.push_back(Agreement{g_nextAgreement++, a, b, descA, descB, nA, nB, period, CalendarIndex(period), -1});
    RunAgreement(g_agreements.back(), "first delivery");
    return true;
}
bool EndAgreement(int town, int id) {  // simulation (command): either partner
    for (size_t i = 0; i < g_agreements.size(); i++)
        if (g_agreements[i].id == id && (g_agreements[i].a == town || g_agreements[i].b == town)) {
            Log("towns: standing trade %d ended by town %d", id, town);
            g_agreements.erase(g_agreements.begin() + i);
            return true;
        }
    return false;
}
static void RunAgreements() {  // every sim step
    for (auto& g : g_agreements) {
        int64_t idx = CalendarIndex(g.period);
        if (idx < 0 || idx == g.lastIdx) continue;
        g.lastIdx = idx;
        RunAgreement(g, g.period == 1 ? "new season" : "new year");
    }
}
std::vector<AgreementInfo> Agreements() {
    std::vector<AgreementInfo> out;
    for (auto& g : g_agreements) out.push_back(AgreementInfo{g.id, g.a, g.b, g.descA, g.descB, g.nA, g.nB, g.period, g.lastOk});
    return out;
}

std::string StorageSummary(int t) {
    if (t < 0 || t >= (int)g_towns.size()) return "no such town";
    uint8_t* town = (uint8_t*)g_towns[t];
    int n = *(int*)(town + 8), own = 0;
    for (int i = 0; i < n; i++) own += OwnerOf((*(void***)town)[i]) == t;
    int total = 0;
    for (auto& g : Goods(t)) total += g.count;
    char buf[160];
    sprintf_s(buf, "%d storages (own %d, shared by others %d), goods total %d", n, own, n - own, total);
    return buf;
}

// Trade deliveries: the giving town's goods leave its storage as piles on the ground next to that storage (the game's
// own loose-goods entities, like a cancelled construction site's materials: RawMaterialComponent + a Pickup job).
// The piles belong to the giver, so the giver's laborers pick them up; their Pickup job is tried with the *receiving*
// town current, so the game's "nearest storage with room" search (exe+0x92a60 on GameLoop+0xf0) picks a storage of
// the receiver and the villager walks the goods there. Pickup batches other piles within 5 cells (Pickup vf11 = taken):
// while a job is tried, piles of another town or with another destination look taken.
static const uintptr_t kRvaTryWorks = 0xfd030;
static const uintptr_t kRvaPickupTaken = 0x2c960;      // Pickup vf11: *(work+0x50) != 0 (a citizen is on it)
static const uintptr_t kVtPickup = 0x167c30;
static const uintptr_t kRvaEntityCreate = 0xc8bf0;     // (mem 0x48, entity description) -> entity
static const uintptr_t kRvaPilePlace = 0x12ec30;       // RawMaterialComponent: (comp, cell, yaw degrees)
static const uintptr_t kRvaTerrainPoint = 0xa48b0;     // (map, float out[4], float in[4], 0, 0) -> out
static const uintptr_t kRvaAddEntity = 0xc9d70;        // (mgr, InternalBasePtr* entity, bucket class = desc+0x20)
static const uintptr_t kTiRawMaterialComp = 0x1cfc80;
static const uintptr_t kTiRawMaterialDesc = 0x1cfcb8;
static int g_tryOwner = -1, g_tryDest = -1;                 // the citizen's town / pile destination while trying a job

static int DeliveryDest(void* entity) {
    auto it = entity ? g_deliverTo.find((uintptr_t)entity) : g_deliverTo.end();
    return it == g_deliverTo.end() ? -1 : it->second;
}

static void* RawDesc(void* desc);

// Can a villager walk from `cell` to town t (its start site)? exe+0xa55c0(map, walk mask 0xe1f, a, b): same walkable
// region. Towns on opposite sides of a river (no bridge) or a cliff are not connected.
static bool Reachable(int cell, int t) {
    if (t < 0 || t >= (int)g_sites.size() || !g_sites[t].placed) return false;
    int* map = *(int**)(GameLoop() + 0xf8);
    return ((bool(__fastcall*)(void*, int, int, int))(g_exe + 0xa55c0))(map, 0xe1f, cell, g_sites[t].y * *map + g_sites[t].x);
}

// Roads are not entities: the road tool marks map cells (exe+0xa1e30(map, cell, type, ...)), which registers a
// BuildRoad work (+0x58 = cell) with no entity (+0x10 = null) - every town's builders took every road. The town that
// placed a cell is remembered here (sorted map: deterministic; saved in the save block). Unknown cells: anyone.
static const uintptr_t kRvaRoadMark = 0xa1e30, kVtBuildRoad = 0x167430;
static std::map<int, uint8_t> g_roadOwner;
static int g_roadLogs;  // per session: the first road jobs taken are logged
using RoadMark_t = void(__fastcall*)(void*, int, uint8_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
static RoadMark_t o_RoadMark;
static void __fastcall h_RoadMark(void* map, int cell, uint8_t type, uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                                  uint64_t e) {
    o_RoadMark(map, cell, type, a, b, c, d, e);
    if (g_towns.size() > 1 && ProbeInSim()) g_roadOwner[cell] = (uint8_t)g_cur;
}

// BuildRoad::vf4 (exe+0xe9660, (work, &citizenComp, ...) -> 2 assigned / 1 nothing now): road jobs reach builders by
// another path than the job loop below, so the town check sits on the work itself.
static const uintptr_t kRvaBuildRoadTry = 0xe9660;
using WorkTry_t = int(__fastcall*)(uint8_t* work, void** citizen, void* a, void* b);
static WorkTry_t o_BuildRoadTry;
static int __fastcall h_BuildRoadTry(uint8_t* work, void** citizen, void* a, void* b) {
    if (g_towns.size() > 1 && citizen && *citizen) {
        auto it = g_roadOwner.find(*(int*)(work + 0x58));
        int me = OwnerOf(*(void**)((uint8_t*)*citizen + 0x10));
        if (it != g_roadOwner.end() && it->second != me) return 1;
    }
    int r = o_BuildRoadTry(work, citizen, a, b);
    if (r == 2 && g_towns.size() > 1 && g_roadLogs < 20) {
        g_roadLogs++;
        auto it = g_roadOwner.find(*(int*)(work + 0x58));
        Log("towns: town %d villager took road cell %d (placed by town %d)", OwnerOf(*(void**)((uint8_t*)*citizen + 0x10)),
            *(int*)(work + 0x58), it == g_roadOwner.end() ? -1 : it->second);
    }
    return r;
}

static int WorkOwner(uint8_t* work) {  // -1 = anyone may take it
    void* ent = *(void**)(work + 0x10);
    if (ent) return OwnerOf(ent);
    if (*(uintptr_t*)work == g_exe + kVtBuildRoad) {
        auto it = g_roadOwner.find(*(int*)(work + 0x58));
        if (it != g_roadOwner.end()) return it->second;
    }
    return -1;
}

// Idle job picker exe+0xfca20(mgr, citizen): Rand(n) over the town's "low" resource categories (+1 for laborers). All
// four desyncs of the 2026-10-14 friend session split there with n off by one between PCs. Desync record before every
// call: tag I + current town index (GameLoop+0xf0) << 16 + low-category mask, value = hash of that town's whole table,
// a = the citizen (offset), b = laborer flag.
using IdlePick_t = char(__fastcall*)(void* mgr, void** citizen);
static IdlePick_t o_IdlePick;
static char __fastcall h_IdlePick(void* mgr, void** citizen) {
    uint8_t* gl = GameLoop();
    uint8_t* town = gl ? *(uint8_t**)(gl + 0xf0) : nullptr;
    int* tab = town ? *(int**)(town + 0x170) : nullptr;
    uint8_t* cit = citizen ? (uint8_t*)*citizen : nullptr;
    if (tab && cit) {
        int ti = 0xff;
        for (int t = 0; t < (int)g_towns.size(); t++)
            if (g_towns[t] == town) ti = t;
        static const int kinds[] = {5, 3, 4, 0, 2, 1, 10, 7, 6};  // fca20's order
        uint32_t mask = 0;
        for (int i = 0; i < 9; i++)
            if (tab[kinds[i] * 8] <= tab[kinds[i] * 8 + 2] / 10) mask |= 1u << i;
        uint64_t h = 1469598103934665603ull;
        for (int i = 0; i < kTableKinds * 8; i++) h = (h ^ (uint32_t)tab[i]) * 1099511628211ull;
        uint8_t* labor = *(uint8_t**)(gl + 0xb0);
        bool laborer = labor && *(void**)(cit + 0x1f0) == *(void**)(labor + 0x38);
        ProbeNote(0x49000000u | ((uint32_t)ti << 16) | mask, (uint32_t)(h ^ (h >> 32)),
                  (int32_t)(simheap::Contains(cit) ? simheap::Offset(cit) : -1), laborer);
    }
    return o_IdlePick(mgr, citizen);
}

using TryWorks_t = bool(__fastcall*)(uint8_t* mgr, void** citizen, uint8_t** tree);
static TryWorks_t o_TryWorks;
static bool g_oldJobs;  // debug: bmp_oldjobs.txt in the game folder = original job loop + direct trades
static bool __fastcall h_TryWorks(uint8_t* mgr, void** citizen, uint8_t** tree) {
    if (g_towns.size() < 2 || g_oldJobs) return o_TryWorks(mgr, citizen, tree);
    uint8_t* cit = (uint8_t*)*citizen;
    int me = OwnerOf(*(void**)(cit + 0x10));
    int stamp = *(int*)(mgr + 8);
    uint8_t* node = *tree;
    if (!node) return false;
    while (*(uint8_t**)(node + 0x18)) node = *(uint8_t**)(node + 0x18);
    while (node) {
        uint8_t* work = *(uint8_t**)node;
        void* ent = *(void**)(work + 0x10);
        int dest = DeliveryDest(ent);
        bool skip = false;
        if (dest >= 0) {  // a delivery nobody can walk: leave the pile (otherwise the courier drops it again on the spot)
            skip = !Reachable(*(int*)(work + 0x28), dest);
            if (skip) g_stuck[(uintptr_t)ent] = 1; else g_stuck.erase((uintptr_t)ent);
        }
        int owner = WorkOwner(work);
        bool hired = owner != me && ent && ent == ForeignWorkplace(cit);  // its job at a shared workplace of another town
        if (!skip && *(int*)(work + 0x2c) != stamp && (owner < 0 || owner == me || hired)) {
            void** vt = *(void***)work;
            ((void(__fastcall*)(void*))vt[9])(work);
            int prevOwner = g_tryOwner, prevDest = g_tryDest;
            g_tryOwner = me;
            g_tryDest = dest;
            void* arg = cit;
            int r;
            {
                Scope s(dest >= 0 ? dest : g_cur);
                r = ((int(__fastcall*)(void*, void**, void*))vt[4])(work, &arg, mgr + 0x130);
            }
            g_tryOwner = prevOwner;
            g_tryDest = prevDest;
            // desync record: "job offer" - tag J + work type (vtable rva), value = work's entity (sim-heap offset),
            // a = the citizen's entity (offset), b = result (2 taken, 1 not now, 0 no)
            ProbeNote(0x4a000000u | (uint32_t)(((uintptr_t)vt - g_exe) & 0xffffff),
                      simheap::Contains(ent) ? simheap::Offset(ent) : 0xffffffffu,
                      (int32_t)(simheap::Contains(*(void**)(cit + 0x10)) ? simheap::Offset(*(void**)(cit + 0x10)) : -1), r);
            if (r == 2) return true;
            if (r == 1) *(int*)(work + 0x2c) = stamp;
        }
        if (uint8_t* right = *(uint8_t**)(node + 0x20)) {  // in-order successor
            node = right;
            while (*(uint8_t**)(node + 0x18)) node = *(uint8_t**)(node + 0x18);
        } else {
            uint8_t* p = *(uint8_t**)(node + 0x10);
            while (p && *(uint8_t**)(p + 0x20) == node) { node = p; p = *(uint8_t**)(p + 0x10); }
            node = p;
        }
    }
    return false;
}

// Storage search exe+0x92a60(town list, &out, cell, desc, n, needRoom, float* dropPoint, InternalBasePtr* exclude):
// nearest storage of the town that takes desc. Its drop-point helper (exe+0x132aa0) only accepts a stockpile whose
// edge cell shares the map cell's +0x26 id with `cell` (roads aside) - fine next door, but a pile next to another
// town's storage never matches. For a delivery that finds nothing, search again from the receiving town's centre.
static const uintptr_t kRvaFindStorage = 0x92a60;
using FindStorage_t = void**(__fastcall*)(void* town, void** out, int cell, void* desc, int n, char needRoom, void* point, void** exclude);
static FindStorage_t o_FindStorage;
static void** __fastcall h_FindStorage(void* town, void** out, int cell, void* desc, int n, char needRoom, void* point, void** exclude) {
    void** r = o_FindStorage(town, out, cell, desc, n, needRoom, point, exclude);
    int d = g_tryDest;
    if (*out || d < 0 || d >= (int)g_sites.size() || town != g_towns[d] || !g_sites[d].placed) return r;
    int size = **(int**)(GameLoop() + 0xf8);
    return o_FindStorage(town, out, g_sites[d].y * size + g_sites[d].x, desc, n, needRoom, point, exclude);
}

// "Store what you carry" (exe+0x11b330, citizen component): picks a storage of the current town. Runs in the citizen's
// own town - or, for a courier with delivery goods, in the receiving town (with the FindStorage fallback above).
static const uintptr_t kRvaStoreInventory = 0x11b330;
using StoreInventory_t = void(__fastcall*)(uint8_t* citizen);
static StoreInventory_t o_StoreInventory;
static void __fastcall h_StoreInventory(uint8_t* cit) {
    if (g_towns.size() < 2) { o_StoreInventory(cit); return; }
    void* ent = *(void**)(cit + 0x10);
    auto it = g_courier.find((uintptr_t)ent);
    if (it != g_courier.end()) {
        uint8_t* inv = (uint8_t*)FindComponent((uint8_t*)ent, kTiStorage);
        if (!inv || ((int(__fastcall*)(void*))(g_exe + kRvaStorageUsed))(inv) == 0) g_courier.erase(it);  // delivered
    }
    it = g_courier.find((uintptr_t)ent);
    int dest = it == g_courier.end() ? -1 : it->second;
    int prevDest = g_tryDest;
    g_tryDest = dest;
    {
        void* hiredAt = dest < 0 ? ForeignWorkplace(cit) : nullptr;  // works at another town's shared workplace
        Scope s(dest >= 0 ? dest : hiredAt ? OwnerOf(hiredAt) : OwnerOf(ent));
        o_StoreInventory(cit);
    }
    g_tryDest = prevDest;
}

// Inventory = the citizen's StorageComponent: +0x28 entries {desc, ?, count at +8} (0x10 B), +0x30 entry count.
static int HeldOf(void* entity, void* desc) {
    uint8_t* inv = (uint8_t*)FindComponent((uint8_t*)entity, kTiStorage);
    if (!inv) return 0;
    int n = 0;
    for (int i = 0; i < *(int*)(inv + 0x30); i++) {
        uint8_t* slot = *(uint8_t**)(inv + 0x28) + i * 0x10;
        if (!desc || *(void**)slot == desc) n += *(int*)(slot + 8);
    }
    return n;
}

// A courier carries one delivery: the good it picked up (g_courierDesc). Its errand lasts while it still holds some of
// that good (checked every sim step) - not "until its hands are empty": villagers keep working with a few trade goods in
// hand, refill with their own produce and never empty out, so they stayed couriers and kept carrying their normal work
// goods to the other town's storages and markets. Couriers loaded from a save have no recorded good: hands-empty rule.
// Each courier is judged by its own state, so the map's order doesn't matter (deterministic).
static void PruneCouriers() {
    for (auto it = g_courier.begin(); it != g_courier.end();) {
        auto d = g_courierDesc.find(it->first);
        if (HeldOf((void*)it->first, d == g_courierDesc.end() ? nullptr : d->second) == 0) {
            if (d != g_courierDesc.end()) g_courierDesc.erase(d);
            it = g_courier.erase(it);
        } else {
            ++it;
        }
    }
}
// PickupRawMaterial action vf4 (exe+0xceda0): pile (+0x28) goes into the citizen's (+0x20) inventory and is killed.
// Picking up a delivery pile makes the citizen a courier for the pile's receiving town (the good that arrived in its
// hands is the delivery).
static const uintptr_t kRvaPickupAction = 0xceda0;
using PickupAction_t = uint64_t(__fastcall*)(uint8_t* action);
static PickupAction_t o_PickupAction;
static uint64_t __fastcall h_PickupAction(uint8_t* action) {
    int dest = g_towns.size() > 1 ? DeliveryDest(*(void**)(action + 0x28)) : -1;
    if (dest < 0) return o_PickupAction(action);
    void* ent = *(void**)(*(uint8_t**)(action + 0x20) + 0x10);
    std::vector<std::pair<void*, int>> before;
    if (uint8_t* inv = (uint8_t*)FindComponent((uint8_t*)ent, kTiStorage))
        for (int i = 0; i < *(int*)(inv + 0x30); i++) {
            uint8_t* slot = *(uint8_t**)(inv + 0x28) + i * 0x10;
            before.push_back({*(void**)slot, *(int*)(slot + 8)});
        }
    uint64_t r = o_PickupAction(action);
    g_courier[(uintptr_t)ent] = (uint8_t)dest;
    if (uint8_t* inv = (uint8_t*)FindComponent((uint8_t*)ent, kTiStorage))
        for (int i = 0; i < *(int*)(inv + 0x30); i++) {
            uint8_t* slot = *(uint8_t**)(inv + 0x28) + i * 0x10;
            int had = 0;
            for (auto& [d, n] : before) if (d == *(void**)slot) had = n;
            if (*(int*)(slot + 8) > had) g_courierDesc[(uintptr_t)ent] = *(void**)slot;
        }
    return r;
}
// Drop-off point of a stockpile (exe+0x132aa0(storageLocation, cell, float* point) -> distance or -1): the edge cell
// nearest `cell` on the same terrain level (map cell +0x26; town 0 may sit on level 3 and town 1 on level 2). For a
// delivery to a town on another level nothing matches -> the courier dropped the goods on the spot. Retry from the
// receiving town's centre (its level); the walk there is the pathfinder's job (ramps).
static const uintptr_t kRvaDropPoint = 0x132aa0;
using DropPoint_t = int(__fastcall*)(void* loc, int cell, float* point);
static DropPoint_t o_DropPoint;
static int __fastcall h_DropPoint(void* loc, int cell, float* point) {
    int r = o_DropPoint(loc, cell, point);
    int d = g_tryDest;
    if (r >= 0 || d < 0 || d >= (int)g_sites.size() || !g_sites[d].placed) return r;
    int size = **(int**)(GameLoop() + 0xf8);
    int r2 = o_DropPoint(loc, g_sites[d].y * size + g_sites[d].x, point);
    return r2;
}

using PickupTaken_t = bool(__fastcall*)(uint8_t* work);
static PickupTaken_t o_PickupTaken;
static bool __fastcall h_PickupTaken(uint8_t* work) {
    if (o_PickupTaken(work)) return true;
    if (g_tryOwner < 0 || *(uintptr_t*)work != g_exe + kVtPickup) return false;
    void* ent = *(void**)(work + 0x10);
    if (!ent) return false;
    return OwnerOf(ent) != g_tryOwner || DeliveryDest(ent) != g_tryDest;
}

static void* RawDesc(void* desc) {  // the RawMaterialDescription part of a resource description
    int want = *(int*)(g_exe + kTiRawMaterialDesc), n = *(int*)((uint8_t*)desc + 0x30);
    for (int i = 0; i < n; i++) {
        void* d = (*(void***)((uint8_t*)desc + 0x28))[i];
        if (((int(__fastcall*)(void*))(*(void***)d)[2])(d) == want) return d;
    }
    return nullptr;
}

// Piles of n x desc (owned by `from`, for `to`) on free open cells around a storage entity; returns how many placed.
static int SpawnDeliveryPiles(int from, int to, void* desc, int n, uint8_t* storageEntity) {
    uint8_t* gl = GameLoop();
    int* map = *(int**)(gl + 0xf8);
    int size = *map;
    uint8_t* cells = *(uint8_t**)((uint8_t*)map + 0x80);
    uint8_t* raw = (uint8_t*)RawDesc(desc);
    uint8_t* mc = storageEntity ? (uint8_t*)FindComponent(storageEntity, kTiMapComponent) : nullptr;
    if (!raw || !mc || !p_MemAlloc) return 0;
    int per = 100 / (*(int*)(raw + 0x48) > 0 ? *(int*)(raw + 0x48) : 1);
    if (per < 1) per = 1;
    int x0 = *(int16_t*)(mc + 0x28), y0 = *(int16_t*)(mc + 0x2a);
    int x1 = x0 + *(int16_t*)(mc + 0x2c) - 1, y1 = y0 + *(int16_t*)(mc + 0x2e) - 1;
    float half = (float)(size >> 1) - 0.5f;
    Scope s(from);
    int placed = 0;
    for (int ring = 1; ring <= 12 && placed < n; ring++) {
        for (int y = y0 - ring; y <= y1 + ring && placed < n; y++) {
            for (int x = x0 - ring; x <= x1 + ring && placed < n; x++) {
                if (x != x0 - ring && x != x1 + ring && y != y0 - ring && y != y1 + ring) continue;  // ring border only
                if (x < 0 || y < 0 || x >= size || y >= size) continue;
                int idx = y * size + x;
                uint8_t* c = cells + (int64_t)idx * 0x30;
                if (*(void**)c || *(void**)(c + 8) || c[0x29] != 2) continue;
                float v[4] = {(float)x - half, (float)y - half, 0, 0}, t1[4], t2[4];
                using Terrain_t = float*(__fastcall*)(void*, float*, float*, char, void*);
                ((Terrain_t)(g_exe + kRvaTerrainPoint))(map, t1, v, 0, nullptr);
                float* h = ((Terrain_t)(g_exe + kRvaTerrainPoint))(map, t2, t1, 0, nullptr);
                if (!(h[2] >= 0.f && h[2] < *(float*)(g_exe + 0x1787fc))) continue;
                int k = n - placed < per ? n - placed : per;
                // the entity must live in the sim heap like every sim object (its address orders entity sets and
                // pointer-keyed trees); the engine's MemAlloc only routes calls from the exe there, not from us
                void* mem = simheap::Alloc(0x48, 8);
                if (!mem) mem = p_MemAlloc(0x48, 8);
                uint8_t* e = mem ? ((uint8_t*(__fastcall*)(void*, void*))(g_exe + kRvaEntityCreate))(mem, desc) : nullptr;
                uint8_t* comp = e ? (uint8_t*)FindComponent(e, kTiRawMaterialComp) : nullptr;
                if (!comp) { Log("!! towns: delivery pile creation failed"); return placed; }
                ((void(__fastcall*)(void*, int, float))(g_exe + kRvaPilePlace))(comp, idx, 0.f);
                *(int*)(comp + 0x28) = k;
                *(uint64_t*)(comp + 0x30) = 0;
                void* ref = e;
                ((void(__fastcall*)(void*, void*, int))(g_exe + kRvaAddEntity))(*(void**)(gl + 0xe8), &ref, *(int*)((uint8_t*)desc + 0x20));
                g_deliverTo[(uintptr_t)e] = (uint8_t)to;
                placed += k;
            }
        }
    }
    return placed;
}

// Goods leave `from`'s storages as delivery piles next to each storage; what can't be placed goes over directly.
static void Deliver(int from, int to, void* desc, int n) {
    int left = n, piles = 0;
    for (int pass = 0; pass < 2 && left > 0; pass++) {  // pass 0: unreserved stock only, pass 1: anything
        for (uint8_t* s : TradeSources(from)) {
            if (left <= 0) break;
            int have = 0, m = *(int*)(s + 0x30);
            for (int i = 0; i < m; i++) {
                uint8_t* row = *(uint8_t**)(s + 0x28) + i * 0x10;
                if (*(void**)row == desc) have = *(int*)(row + 8) - (pass == 0 ? *(int*)(row + 0xc) : 0);
            }
            int take = have < left ? have : left;
            if (take <= 0) continue;
            ((void(__fastcall*)(void*, void*, int))(g_exe + kRvaStorageRemove))(s, desc, take);
            int placed = SpawnDeliveryPiles(from, to, desc, take, *(uint8_t**)(s + 0x10));
            if (placed < take) ((void(__fastcall*)(void*, void*, int))(g_exe + kRvaStorageAdd))(s, desc, take - placed);
            left -= placed;
            piles += placed;
        }
    }
    if (left > 0) {  // no free ground for piles: straight into the receiver's storages, as much as fits (the rest stays)
        int fit = RoomFor(to, desc);
        if (fit > left) fit = left;
        Log("towns: delivery %d -> %d: no room for piles, %d moved directly, %d stay with the giver", from, to, fit, left - fit);
        if (fit > 0) Move(from, to, desc, fit);
    }
    NonSim ns;
    Log("towns: delivery %d -> %d: %d x %s on the ground for pickup", from, to, piles, commands::DescPath(desc).c_str());
}

// Command: hand over all of town `from`'s waiting delivery piles directly (no walking route). Piles in sim-heap order.
void DeliverNow(int from) {
    ModMove mm;
    std::vector<uint8_t*> piles;
    for (auto& [e, to] : g_deliverTo)
        if (OwnerOf((void*)e) == from && !(*(uint32_t*)((uint8_t*)e + 0x40) & 0x400)) piles.push_back((uint8_t*)e);
    std::sort(piles.begin(), piles.end(), [](uint8_t* a, uint8_t* b) { return simheap::Offset(a) < simheap::Offset(b); });
    uint8_t* mgr = *(uint8_t**)(GameLoop() + 0xe8);
    int moved = 0;
    for (uint8_t* e : piles) {
        uint8_t* comp = (uint8_t*)FindComponent(e, kTiRawMaterialComp);
        void* desc = *(void**)(e + 0x28);
        int to = g_deliverTo[(uintptr_t)e], left = comp ? *(int*)(comp + 0x28) : 0;
        for (uint8_t* s : StoragesOf(to)) {
            while (left > 0 && ((bool(__fastcall*)(void*, void*, int))(g_exe + kRvaStorageRoom))(s, desc, 1)) {
                int chunk = left;
                while (chunk > 1 && !((bool(__fastcall*)(void*, void*, int))(g_exe + kRvaStorageRoom))(s, desc, chunk)) chunk /= 2;
                ((void(__fastcall*)(void*, void*, int))(g_exe + kRvaStorageAdd))(s, desc, chunk);
                left -= chunk;
                moved += chunk;
            }
        }
        if (left > 0) { *(int*)(comp + 0x28) = left; continue; }  // no room: the rest keeps waiting
        ((void(__fastcall*)(void*))(g_exe + kRvaEntityKill))(e);
        void* ref = e;
        ((void(__fastcall*)(void*, int, void*))(g_exe + kRvaArrayAppend))(mgr + 0xd0, *(int*)(mgr + 0xd8) + 1, &ref);
    }
    Log("towns: town %d handed over %d waiting delivery goods directly", from, moved);
}

void OtherTownFootprints(std::vector<Footprint>& out) {
    out.clear();
    uint8_t* gl = GameLoop();
    if (g_towns.size() < 2 || !gl || !*(uint8_t**)(gl + 0xe8) || !*(int**)(gl + 0xf8)) return;
    NonSim ns;  // the terrain query may allocate: never in the sim heap
    static const bool self = GetFileAttributesW(L"bmp_tintself.txt") != INVALID_FILE_ATTRIBUTES;  // debug: own town too
    uint8_t* mgr = *(uint8_t**)(gl + 0xe8);
    int* map = *(int**)(gl + 0xf8);
    float half = (float)(*map / 2);
    for (int i = 0; i < *(int*)(mgr + 0x10); i++) {
        uint8_t* e = (*(uint8_t***)(mgr + 0x08))[i];
        if (!e || (*(uint32_t*)(e + 0x40) & 0x400)) continue;
        int town = OwnerOf(e);
        if (town == g_local && !self) continue;
        uint8_t* mc = (uint8_t*)FindComponent(e, kTiMapComponent);
        if (!mc) continue;
        int w = *(int16_t*)(mc + 0x2c), h = *(int16_t*)(mc + 0x2e);
        if (w * h < 2 || FindComponent(e, kTiNaturalResource)) continue;
        Footprint f;
        f.town = town;
        f.x0 = *(int16_t*)(mc + 0x28) - half;
        f.y0 = *(int16_t*)(mc + 0x2a) - half;
        f.x1 = f.x0 + w;
        f.y1 = f.y0 + h;
        if (uint8_t* model = (uint8_t*)FindComponent(e, 0x1cfa50)) {  // ModelComponent: transform +0x148 (quat, position)
            f.z = *(float*)(model + 0x160);
        } else {
            float in[4] = {(f.x0 + f.x1) * 0.5f, (f.y0 + f.y1) * 0.5f, 0, 0}, o[4] = {};
            using Terrain_t = float*(__fastcall*)(void*, float*, float*, char, void*);
            float* t = ((Terrain_t)(g_exe + 0xa48b0))(map, o, in, 0, nullptr);
            f.z = t ? t[2] : 0.f;
        }
        out.push_back(f);
    }
}

std::vector<Delivery> Deliveries() {
    std::vector<Delivery> out;
    for (auto& [e, to] : g_deliverTo) {
        uint8_t* ent = (uint8_t*)e;
        if (*(uint32_t*)(ent + 0x40) & 0x400) continue;
        uint8_t* comp = (uint8_t*)FindComponent(ent, kTiRawMaterialComp);
        void* desc = *(void**)(ent + 0x28);
        if (!comp) continue;
        int from = OwnerOf(ent);
        bool merged = false;
        for (auto& d : out)
            if (d.from == from && d.to == to && d.desc == desc) {
                d.count += *(int*)(comp + 0x28);
                d.piles++;
                d.stuck = d.stuck || g_stuck.count(e);
                merged = true;
                break;
            }
        if (!merged) out.push_back({from, (int)to, desc, *(int*)(comp + 0x28), 1, g_stuck.count(e) > 0});
    }
    std::sort(out.begin(), out.end(), [](const Delivery& a, const Delivery& b) {  // map order = heap addresses
        return a.from != b.from ? a.from < b.from : a.to != b.to ? a.to < b.to : a.count < b.count;
    });
    return out;
}

// Watch (user 2026-10-14: "make sure the vendors aren't stealing my friend's goods"): every removal from a storage
// (exe+0x132620(storage, desc, n)) while another town is current is logged - except the mod's own trade moves and
// storages shared with that town. Never seen = no town takes from another town's storages.
using StorageRemove_t = void(__fastcall*)(void* storage, void* desc, int n);
static StorageRemove_t o_StorageRemove;
static void __fastcall h_StorageRemove(void* storage, void* desc, int n) {
    if (g_towns.size() > 1 && !g_modMoves && storage) {
        void* e = *(void**)((uint8_t*)storage + 0x10);
        int owner = e ? OwnerOf(e) : -1, cur = Current();
        if (owner >= 0 && owner != cur && !SharedWith(e, cur)) {
            ProbeNote(0x54000000u | (uint32_t)(cur << 8) | (uint32_t)owner, (uint32_t)n, 0, 0);
            static int logged;
            if (logged++ < 20) {
                NonSim ns;
                Log("!! towns: town %d took %d x %s from a storage of town %d (caller exe+%llx)", cur, n,
                    commands::DescPath(desc).c_str(), owner, (unsigned long long)((uintptr_t)_ReturnAddress() - g_exe));
            }
        }
    }
    o_StorageRemove(storage, desc, n);
}

bool Trade(int a, void* descA, int nA, int b, void* descB, int nB, const char** why) {
    ModMove mm;
    static const char* r;
    if (!why) why = &r;
    if (a == b || nA < 0 || nB < 0 || (nA == 0 && nB == 0)) { *why = "nothing to trade"; return false; }
    if (nA > 0 && (!descA || CountOf(a, descA) < nA)) { *why = "the giving town doesn't have enough"; return false; }
    if (nB > 0 && (!descB || CountOf(b, descB) < nB)) { *why = "the other town doesn't have enough"; return false; }
    // moved directly (old mode) the goods need room now; delivered as piles they wait on the ground until the receiving
    // town's villagers can store them (2026-10-14: a full storage made trades fail silently after "accepted")
    if (g_oldJobs && nA > 0 && RoomFor(b, descA) < nA) { *why = "no storage room in the receiving town"; return false; }
    if (g_oldJobs && nB > 0 && RoomFor(a, descB) < nB) { *why = "no storage room in the receiving town"; return false; }
    if (g_oldJobs) {
        if (nA > 0) Move(a, b, descA, nA);
        if (nB > 0) Move(b, a, descB, nB);
    } else {
        if (nA > 0) Deliver(a, b, descA, nA);
        if (nB > 0) Deliver(b, a, descB, nB);
    }
    *why = "";
    Log("towns: trade accepted: town %d gives %d, town %d gives %d (carried by their villagers)", a, nA, b, nB);
    return true;
}

// A removal drag (clear trees/stone/iron/...) by town t: the natural resources it covers become t's, so their
// updates, the jobs they create and their removal all happen in t's town (map objects start out as town 0's).
void ClaimNaturals(int t, int a, int b) {
    if (g_towns.size() < 2) return;
    int size = **(int**)(GameLoop() + 0xf8);
    int x0 = a % size, y0 = a / size, x1 = b % size, y1 = b / size;
    if (x0 > x1) std::swap(x0, x1);
    if (y0 > y1) std::swap(y0, y1);
    int cx = (x0 + x1) / 2, cy = (y0 + y1) / 2, r = (x1 - x0 > y1 - y0 ? x1 - x0 : y1 - y0) + 2;
    int n = 0;
    for (auto& [e, c] : NaturalsNear(cx, cy, r)) {
        if (c.first < x0 - 1 || c.first > x1 + 1 || c.second < y0 - 1 || c.second > y1 + 1) continue;
        if (t) g_owner[(uintptr_t)e] = (uint8_t)t;
        else g_owner.erase((uintptr_t)e);
        n++;
    }
    if (n) Log("towns: town %d claimed %d natural resources (%d,%d)-(%d,%d)", t, n, x0, y0, x1, y1);
}

bool PickMode() { return g_pick && g_towns.size() > 1; }
bool Settled(int t) { return t >= 0 && t < (int)g_sites.size() && g_sites[t].placed; }

// Camera focus (tool UI = GameLoop+0x120): +0xa4 = flying to the middle of cells +0xa8/+0xac (index y*size+x).
// A new game focuses the local town's start - for a town that isn't settled yet that is cell 0 (map corner).
static void FocusCamera(int x, int y) {
    uint8_t* gl = GameLoop();
    uint8_t* ui = gl ? *(uint8_t**)(gl + 0x120) : nullptr;
    if (!ui) return;
    int idx = y * **(int**)(gl + 0xf8) + x;
    *(int*)(ui + 0xa8) = idx;
    *(int*)(ui + 0xac) = idx;
    *(uint8_t*)(ui + 0xa4) = 1;
}

void FocusCell(int cell) {  // UI: camera to a map cell index (y * size + x)
    uint8_t* gl = GameLoop();
    int size = gl && *(int**)(gl + 0xf8) ? **(int**)(gl + 0xf8) : 0;
    if (size > 0 && cell >= 0 && cell < size * size) FocusCamera(cell % size, cell / size);
}
int EventTown(int i) { return i >= 0 && i < (int)g_evTown.size() ? g_evTown[i] : -1; }
static bool g_eventsMineOnly;
void SetEventsMineOnly(bool on) { g_eventsMineOnly = on; }
bool EventsMineOnly() { return g_eventsMineOnly; }
// EventNotifyDialog vf7 (exe+0x68590, UI): pops up the entries after dialog+0xc4 whose +0x14 > 0. "Only my messages":
// other towns' new entries get +0x14 = 0 for the call (restored after it).
using NotifyUpdate_t = void(__fastcall*)(uint8_t* dlg, void* dt);
static NotifyUpdate_t o_NotifyUpdate;
static void __fastcall h_NotifyUpdate(uint8_t* dlg, void* dt) {
    uint8_t* gl = GameLoop();
    uint8_t* ev = gl ? *(uint8_t**)(gl + 0x138) : nullptr;
    if (!g_eventsMineOnly || !ev || g_towns.size() < 2) return o_NotifyUpdate(dlg, dt);
    AttributeEvents();
    std::vector<std::pair<int*, int>> muted;
    int n = *(int*)(ev + 0x20);
    for (int i = *(int*)(dlg + 0xc4) + 1; i >= 0 && i < n; i++) {
        int t = EventTown(i);
        int* popup = (int*)(*(uint8_t**)(ev + 0x18) + i * 0x28 + 0x14);
        if (t >= 0 && t != g_local && *popup > 0) { muted.push_back({popup, *popup}); *popup = 0; }
    }
    o_NotifyUpdate(dlg, dt);
    for (auto& [p, v] : muted) *p = v;
}
std::vector<EventInfo> Events(int max) {
    std::vector<EventInfo> out;
    uint8_t* gl = GameLoop();
    uint8_t* ev = gl ? *(uint8_t**)(gl + 0x138) : nullptr;
    if (!ev || g_towns.size() < 2) return out;
    int n = *(int*)(ev + 0x20);
    for (int i = n - 1; i >= 0 && (int)out.size() < max; i--) {
        uint8_t* e = *(uint8_t**)(ev + 0x18) + i * 0x28;
        const char* text = *(const char**)(e + 8);
        out.push_back({i, EventTown(i), *(int*)(e + 0x18), text ? text : ""});
    }
    return out;
}

void OnFrame() {
    if (g_towns.size() > 1) AttributeEvents();  // outside the simulation: UI events are the local town's
    CheckResourceTables();
    SnapProfessions(false);
    if (g_focusLocal && g_sessionLoop && g_sessionLoop == GameLoop()) {
        g_focusLocal = false;
        if (g_local < (int)g_sites.size() && g_sites[g_local].placed) FocusCamera(g_sites[g_local].x, g_sites[g_local].y);
    }
    if (!PickMode() || Settled(g_local)) return;
    uint8_t* gl = GameLoop();
    uint8_t* ui = gl ? *(uint8_t**)(gl + 0x120) : nullptr;
    if (!ui) return;
    // the game focuses an unsettled town's start = cell 0 (map corner): look at the middle of the map instead
    static void* redirected;
    if ((*(uint8_t*)(ui + 0xa4) && *(int*)(ui + 0xa8) == 0) || redirected != gl) {
        redirected = gl;
        int half = **(int**)(gl + 0xf8) / 2;
        FocusCamera(half, half);
    }
}

bool CameraCell(int* x, int* y) {
    uint8_t* gl = GameLoop();
    if (!gl || !*(uint8_t**)(gl + 0xe0) || !*(int**)(gl + 0xf8)) return false;
    float* target = (float*)(*(uint8_t**)(gl + 0xe0) + 0x1c);  // world plane: cell + 0.5 - size/2
    int half = **(int**)(gl + 0xf8) / 2;
    *x = (int)floorf(target[0] + half);
    *y = (int)floorf(target[1] + half);
    return true;
}

bool CanSettle(int t, int x, int y, const char** why) {
    static const char* reason;
    if (!why) why = &reason;
    uint8_t* gl = GameLoop();
    void* mapgen = gl ? *(void**)(gl + 0x40) : nullptr;
    if (!mapgen || g_towns.size() < 2) { *why = "not in a separate-towns game"; return false; }
    if (Settled(t)) { *why = "your town is already settled"; return false; }
    int size = **(int**)(gl + 0xf8), r = *(int*)((uint8_t*)mapgen + 0x3c);
    if (x < r + 2 || y < r + 2 || x >= size - r - 2 || y >= size - r - 2) { *why = "too close to the map edge"; return false; }
    int minDist = r * 5 > 48 ? r * 5 : 48;
    for (int o = 0; o < (int)g_sites.size(); o++) {
        if (o == t || !g_sites[o].placed) continue;
        int dx = x - g_sites[o].x, dy = y - g_sites[o].y;
        if (dx * dx + dy * dy < minDist * minDist) { *why = "too close to another town"; return false; }
    }
    if (!SiteClearable(x, y, r)) { *why = "not enough flat land here (water, hills or buildings)"; return false; }
    *why = "";
    return true;
}

bool Settle(int t, int x, int y) {
    const char* why = "";
    if (!CanSettle(t, x, y, &why)) { Log("towns: town %d can't settle at %d,%d: %s", t, x, y, why); return false; }
    uint8_t* gl = GameLoop();
    void* mapgen = *(void**)(gl + 0x40);
    int r = *(int*)((uint8_t*)mapgen + 0x3c);
    uint8_t* mgr = *(uint8_t**)(gl + 0xe8);
    int cleared = 0;
    for (auto& [e, c] : NaturalsNear(x, y, r + 2)) {  // clear the site (trees, rocks)
        ((void(__fastcall*)(void*))(g_exe + kRvaEntityKill))(e);
        void* ref = e;
        ((void(__fastcall*)(void*, int, void*))(g_exe + kRvaArrayAppend))(mgr + 0xd0, *(int*)(mgr + 0xd8) + 1, &ref);
        cleared++;
    }
    if (!((SiteOk_t)(g_exe + kRvaSiteOk))(mapgen, x, y, r, r * r))
        Log("!! towns: town %d site %d,%d still not fully open after clearing %d objects - placing anyway", t, x, y, cleared);
    Scope s(t);
    g_sites[t].x = x;
    g_sites[t].y = y;
    g_sites[t].placed = true;
    ((void(__fastcall*)(void*, float*, int, int))(g_exe + kRvaCellToWorld))(*(void**)(gl + 0xf8), g_sites[t].pos, x, y);
    uint8_t* t0 = (uint8_t*)g_towns[0];
    o_StartStructures(mapgen, x, y);
    o_StartFamilies(mapgen, g_sites[t].pos);
    for (int i = 0; i < *(int*)(t0 + 0x118); i++)
        ((void(__fastcall*)(void*, void*))(g_exe + kRvaTownAddResource))(g_towns[t], (*(void***)(t0 + 0x110))[i]);
    o_StartSupplies(mapgen);
    for (int i = 0; i < *(int*)(t0 + 0x128); i++)
        ((void(__fastcall*)(void*, void*))(g_exe + kRvaTownAddTemplate))(g_towns[t], (*(void***)(t0 + 0x120))[i]);
    Log("towns: town %d settled at cell %d,%d (cleared %d trees/rocks)", t, x, y, cleared);
    if (t == g_local) FocusCamera(x, y);  // UI-only: fly to the new town
    return true;
}

static std::vector<std::string> g_townPlayer;  // town index -> the player who had it ("" = nobody known; BPNM)
void OnLoadStart() {
    g_townPlayer.clear();
    g_tableSnapValid = false;
    g_profSnapValid = false;
    g_profSnap.clear();
    Retire();  // the old GameLoop gets its own town back (it frees whatever sits in +0xf0)
    g_towns.clear();
    g_owner.clear();
    g_sites.clear();
    g_deliverTo.clear();
    g_courier.clear();
    g_courierDesc.clear();
    g_stuck.clear();
    g_tradedLog.clear();
    g_roadOwner.clear();
    g_roadLogs = 0;
    g_shareMask.clear();
    g_agreements.clear();
    g_nextAgreement = 1;
    g_workShare.clear();
    g_evTown.clear();
    g_steps = 0;
    g_cur = 0;
    g_count = g_cfgCount, g_local = g_cfgLocal, g_pick = g_cfgPick;
    g_sessionLoop = nullptr;
}

void OnSessionStart() {
    uint8_t* gl = GameLoop();
    if (!gl || gl == g_sessionLoop) return;
    g_sessionLoop = gl;
    if (g_towns.size() < 2) return;
    Log("towns: session with %d towns, I am town %d", (int)g_towns.size(), g_local);
    {
        float* ct = (float*)(*(uint8_t**)(gl + 0xe0) + 0x1c);
        Log("towns: camera target %.1f %.1f %.1f; site0 %.1f %.1f %.1f; site1 %.1f %.1f %.1f", ct[0], ct[1], ct[2],
            g_sites[0].pos[0], g_sites[0].pos[1], g_sites[0].pos[2], g_sites.size() > 1 ? g_sites[1].pos[0] : 0.f,
            g_sites.size() > 1 ? g_sites[1].pos[1] : 0.f, g_sites.size() > 1 ? g_sites[1].pos[2] : 0.f);
    }
    if (g_local > 0 && g_local < (int)g_sites.size() && g_sites[g_local].placed) {
        uint8_t* cam = *(uint8_t**)(gl + 0xe0);  // camera controller: +0x1c target position
        memcpy(cam + 0x1c, g_sites[g_local].pos, 12);
    }
    g_focusLocal = true;  // start at your own town (also after loading a save)
    Switch(g_local);
}

// ---------------------------------------------------------------- saves
// The GameLoop's serializer (exe+0x9af60, same code for save and load; Stream+8: 1 = writing, 0 = reading) writes the
// entity manager, then ITS Town (exe+0x94c30 on GameLoop+0xf0), the map, ... and ends with the entity manager's
// post-load step exe+0xc9c00(mgr, stream) while object ids still resolve. Separate towns:
//  - Town serialize: always town 0 in that slot (outside the sim GameLoop+0xf0 holds the local player's town).
//  - c9c00: an extra block at the very end of the stream (an old save simply has no bytes left there):
//      magic, version, town count, pick, sites; towns 1.. via the game's own Town serializer;
//      per entity of the manager's all-entities array (saved, fixed order): owner, delivery target, courier target.
//    Loading builds the towns from it, then runs every entity's component post-load (vf11) in its owner's town.
static const uintptr_t kRvaTownSerialize = 0x94c30;
static const uintptr_t kRvaTownPostLoad = 0x94a50;
static const uintptr_t kRvaEntityPostLoad = 0xc9c00;
static const int kSaveMagic = 0x544d5042;  // "BPMT"
static const int kRoadMagic = 0x44525042;  // "BPRD": road cell owners (added 2026-10-11)
static const int kShareMagic = 0x42535042;  // "BPSB": shared storage buildings + town masks (added 2026-10-12)
static const int kAgreeMagic = 0x47415042;  // "BPAG": standing trades (added 2026-10-13)
static const int kWorkMagic = 0x53575042;   // "BPWS": shared workplaces (added 2026-10-13)
static const int kEventMagic = 0x56455042;  // "BPEV": event log towns (added 2026-10-14)
static const int kNameMagic = 0x4d4e5042;   // "BPNM": player name per town (added 2026-10-14) - hosting the save again
                                            // gives each returning player their own town, whatever order they join in
void SetTownPlayer(int t, const std::string& name) {
    if (t < 0 || t > 64 || name.empty()) return;
    if ((int)g_townPlayer.size() <= t) g_townPlayer.resize(t + 1);
    g_townPlayer[t] = name;
}
std::string TownPlayer(int t) { return t >= 0 && t < (int)g_townPlayer.size() ? g_townPlayer[t] : ""; }
using TownSerialize_t = void(__fastcall*)(void* town, void* stream);
using TownPostLoad_t = void(__fastcall*)(void* town);
using EntityPostLoad_t = void(__fastcall*)(uint8_t* mgr, uint8_t* stream);
static TownSerialize_t o_TownSerialize;
static EntityPostLoad_t o_EntityPostLoad;

using SerCtor_t = void*(__fastcall*)(void* ser, void* stream, int version);
using SerDtor_t = void(__fastcall*)(void* ser);
using SerInt_t = void(__fastcall*)(void* ser, int* v, const char* name);
using SerFloat_t = void(__fastcall*)(void* ser, float* v, const char* name);
using SerInts_t = void(__fastcall*)(void* ser, int n, int* v, const char* name);
using GetMemStream_t = void*(__fastcall*)(void* compressed);
using StreamPos_t = unsigned(__fastcall*)(const void* memoryStream);
static SerCtor_t p_SerCtor;
static SerDtor_t p_SerDtor;
static SerInt_t p_SerInt;
static SerFloat_t p_SerFloat;
static SerInts_t p_SerInts;
static GetMemStream_t p_GetMemStream;
static StreamPos_t p_StreamPos, p_StreamSize;
using PtrSerialize_t = void(__fastcall*)(void* internalBasePtr, void* serializer);
static PtrSerialize_t p_PtrSerialize;  // Object::InternalBasePtr::Serialize: object <-> the save's id
static void* p_CompressedVtbl;

struct Ser {  // IO::Serializer on the game's stream
    alignas(16) uint8_t buf[64] = {};
    Ser(void* stream, int version) { p_SerCtor(buf, stream, version); }
    ~Ser() { p_SerDtor(buf); }
    void Int(int& v) { p_SerInt(buf, &v, nullptr); }
    void Float(float& v) { p_SerFloat(buf, &v, nullptr); }
    void Ints(std::vector<int>& v) { if (!v.empty()) p_SerInts(buf, (int)v.size(), v.data(), nullptr); }
};

static bool Writing(const uint8_t* stream) { return *(const int*)(stream + 8) == 1; }

// bytes left to read in the stream itself: IO::Stream virtuals 5 GetCurrentPos / 6 GetSize. (Not the inner MemoryStream
// of a CompressedMemoryStream: that only holds what has been decompressed so far - a block starting at a new compressed
// chunk looked like "nothing left" and the extra towns were silently dropped.)
static int64_t BytesLeft(void* stream) {
    using U_t = unsigned(__fastcall*)(void*);
    void** vt = *(void***)stream;
    return (int64_t)((U_t)vt[6])(stream) - (int64_t)((U_t)vt[5])(stream);
}

// Optional trailing sections (roads, shared storage) are recognised by their tag. Older saves end before them, but
// GetSize counts the last compressed window's padding (0xbb bytes), so "bytes left" isn't proof a section is there:
// reading on went past the real data and the game never finished loading (2-town saves from 2026-10-11 hung).
// CompressedStream keeps its read position at +0x1c (GetCurrentPos): read the tag, step back if it doesn't match.
static bool TagFollows(Ser& s, void* stream, int tag) {
    static void* getPos = RuntimeExport("?GetCurrentPos@CompressedStream@IO@@UEBAIXZ");
    if (BytesLeft(stream) < 8 || !getPos || (*(void***)stream)[5] != getPos) return false;
    uint32_t pos = *(uint32_t*)((uint8_t*)stream + 0x1c);
    int v = 0;
    s.Int(v);
    if (v == tag) return true;
    *(uint32_t*)((uint8_t*)stream + 0x1c) = pos;
    return false;
}

static void __fastcall h_TownSerialize(void* town, void* stream) {
    if (g_towns.size() > 1 && Writing((uint8_t*)stream)) town = g_towns[0];
    o_TownSerialize(town, stream);
}

static void SaveBlock(uint8_t* mgr, uint8_t* stream) {
    Ser s(stream, 1);
    int magic = kSaveMagic, count = (int)g_towns.size(), pick = g_pick;
    s.Int(magic);
    s.Int(count);
    s.Int(pick);
    for (auto& site : g_sites) {
        int placed = site.placed;
        s.Int(site.x);
        s.Int(site.y);
        s.Int(placed);
        for (int i = 0; i < 3; i++) s.Float(site.pos[i]);
    }
    for (int t = 1; t < count; t++) o_TownSerialize(g_towns[t], stream);
    // entities with a town other than 0 or delivery state, as object references (the save's own object ids)
    std::vector<uint8_t*> list;
    int n = *(int*)(mgr + 0x10);
    for (int i = 0; i < n; i++) {
        uint8_t* e = (*(uint8_t***)(mgr + 0x08))[i];
        if (!e || (*(uint32_t*)(e + 0x40) & 0x400)) continue;
        if (OwnerOf(e) || g_deliverTo.count((uintptr_t)e) || g_courier.count((uintptr_t)e)) list.push_back(e);
    }
    int m = (int)list.size();
    s.Int(m);
    for (uint8_t* e : list) {
        void* ref = e;
        p_PtrSerialize(&ref, s.buf);
        auto d = g_deliverTo.find((uintptr_t)e);
        auto k = g_courier.find((uintptr_t)e);
        int owner = OwnerOf(e), deliver = d == g_deliverTo.end() ? -1 : d->second, courier = k == g_courier.end() ? -1 : k->second;
        s.Int(owner);
        s.Int(deliver);
        s.Int(courier);
    }
    // road cells in progress (older saves end before this section)
    // only cells whose BuildRoad is still pending (map+0xe8: tree cell -> work id; node key +0, left +0x10, right +0x18)
    std::vector<std::pair<int, int>> pending;
    uint8_t* map = *(uint8_t**)(GameLoop() + 0xf8);
    for (auto& [cell, town] : g_roadOwner) {
        for (uint8_t* nd = *(uint8_t**)(map + 0xe8); nd;) {
            int key = *(int*)nd;
            if (cell == key) { pending.push_back({cell, town}); break; }
            nd = *(uint8_t**)(nd + (cell < key ? 0x10 : 0x18));
        }
    }
    int roadMagic = kRoadMagic, roads = (int)pending.size();
    s.Int(roadMagic);
    s.Int(roads);
    for (auto& [cell, town] : pending) {
        int c = cell, t = town;
        s.Int(c);
        s.Int(t);
    }
    // shared storage buildings (owner's list order) with their share masks, as object references
    std::vector<std::pair<void*, int>> shared;
    for (int t = 0; t < count; t++) {
        uint8_t* town = (uint8_t*)g_towns[t];
        for (int i = 0; i < *(int*)(town + 8); i++) {
            void* e = (*(void***)town)[i];
            if (OwnerOf(e) == t && ShareMaskOf(e)) shared.push_back({e, ShareMaskOf(e)});
        }
    }
    int shareMagic = kShareMagic, shares = (int)shared.size();
    s.Int(shareMagic);
    s.Int(shares);
    for (auto& [e, mask] : shared) {
        void* ref = e;
        int m2 = mask;
        p_PtrSerialize(&ref, s.buf);
        s.Int(m2);
    }
    // standing trades: "BPAG", count, next id, then per trade: id a b nA nB period lastIdx(lo, hi) lastOk + both goods' paths
    int agreeMagic = kAgreeMagic, agreements = (int)g_agreements.size();
    s.Int(agreeMagic);
    s.Int(agreements);
    s.Int(g_nextAgreement);
    for (auto& g : g_agreements) {
        int lo = (int)(g.lastIdx & 0xffffffff), hi = (int)(g.lastIdx >> 32);
        for (int* v : {&g.id, &g.a, &g.b, &g.nA, &g.nB, &g.period, &lo, &hi, &g.lastOk}) s.Int(*v);
        for (void* d : {g.descA, g.descB}) {
            std::string p = d ? commands::DescPath(d) : "-";
            std::vector<int> chars(p.begin(), p.end());
            int len = (int)chars.size();
            s.Int(len);
            s.Ints(chars);
        }
    }
    // shared workplaces: "BPWS", count, then (building reference, town mask) in sim-heap order
    std::vector<std::pair<uint64_t, void*>> works;
    for (auto& [e, mask] : g_workShare) works.push_back({simheap::Offset((void*)e), (void*)e});
    std::sort(works.begin(), works.end());
    int workMagic = kWorkMagic, workShares = (int)works.size();
    s.Int(workMagic);
    s.Int(workShares);
    for (auto& [off, e] : works) {
        void* ref = e;
        int mask = WorkShareMaskOf(e);
        p_PtrSerialize(&ref, s.buf);
        s.Int(mask);
    }
    // event log towns: "BPEV", count, one town per event (oldest first)
    AttributeEvents();
    int evMagic = kEventMagic, events = (int)g_evTown.size();
    std::vector<int> evTowns(g_evTown.begin(), g_evTown.end());
    s.Int(evMagic);
    s.Int(events);
    if (events) s.Ints(evTowns);
    // player names: "BPNM", count, per town: length + one int per byte
    int nmMagic = kNameMagic, names = (int)g_townPlayer.size();
    s.Int(nmMagic);
    s.Int(names);
    for (auto& name : g_townPlayer) {
        std::vector<int> bytes(name.begin(), name.end());
        int len = (int)bytes.size();
        s.Int(len);
        if (len) s.Ints(bytes);
    }
    Log("towns: saved %d towns, %d of %d entities with town data, %d road cells, %d storage shares, %d standing trades, "
        "%d shared workplaces, %d events", count, m, n, roads, shares, agreements, workShares, events);
}

static bool LoadBlock(uint8_t* mgr, uint8_t* stream) {
    int64_t left = BytesLeft(stream);
    Log("towns: save stream: %lld bytes after the game data", left);
    if (left < 8) {
        if (left < 0) Log("!! towns: unknown save stream type - extra towns not loaded");
        return false;  // an ordinary one-town save
    }
    Ser s(stream, 1);
    int magic = 0, count = 0, pick = 0;
    s.Int(magic);
    if (magic != kSaveMagic) { Log("!! towns: unexpected data after the save (%08x) - ignored", magic); return false; }
    s.Int(count);
    s.Int(pick);
    if (count < 2 || count > 8) { Log("!! towns: bad town count %d in save", count); return false; }
    int saved = count;
    if (g_count != count) Log("towns: the save has %d towns (session configured %d)", count, g_count);
    if (g_count > count) count = g_count;  // players who joined since: a new town each, settled by its player
    if (g_players > count) count = g_players;  // a multi-town save hosted for more players than it has towns
    g_count = count;
    if (g_local >= count) g_local = 0;
    g_pick = pick != 0 || count > saved;
    g_towns.clear();
    g_towns.push_back(*(void**)(GameLoop() + 0xf0));
    g_sites.assign(count, Site{});
    for (int t = 0; t < saved; t++) {
        Site& site = g_sites[t];
        int placed = 0;
        s.Int(site.x);
        s.Int(site.y);
        s.Int(placed);
        for (int i = 0; i < 3; i++) s.Float(site.pos[i]);
        site.placed = placed != 0;
    }
    for (int t = 1; t < count; t++) {
        g_towns.push_back(CreateTown(false));
        if (t < saved) o_TownSerialize(g_towns[t], stream);
    }
    int m = 0;
    s.Int(m);
    g_owner.clear();
    g_deliverTo.clear();
    g_courier.clear();
    g_courierDesc.clear();
    g_stuck.clear();
    int owned = 0, lost = 0;
    for (int i = 0; i < m; i++) {
        void* ref = nullptr;
        p_PtrSerialize(&ref, s.buf);
        int owner = 0, deliver = -1, courier = -1;
        s.Int(owner);
        s.Int(deliver);
        s.Int(courier);
        uintptr_t e = (uintptr_t)ref;
        if (!e) { lost++; continue; }
        if (owner > 0 && owner < count) { g_owner[e] = (uint8_t)owner; owned++; }
        if (deliver >= 0 && deliver < count) g_deliverTo[e] = (uint8_t)deliver;
        if (courier >= 0 && courier < count) g_courier[e] = (uint8_t)courier;
    }
    g_roadOwner.clear();
    if (TagFollows(s, stream, kRoadMagic)) {
        int roads = 0;
        s.Int(roads);
        if (roads >= 0 && roads < 10000000) {
            for (int i = 0; i < roads; i++) {
                int c = 0, t = 0;
                s.Int(c);
                s.Int(t);
                if (t >= 0 && t < count) g_roadOwner[c] = (uint8_t)t;
            }
        }
    }
    Log("towns: %d road cells with an owner", (int)g_roadOwner.size());
    g_shareMask.clear();
    if (TagFollows(s, stream, kShareMagic)) {
        int shares = 0;
        s.Int(shares);
        if (shares >= 0 && shares <= 100000) {
            for (int i = 0; i < shares; i++) {
                void* ref = nullptr;
                int mask = 0;
                p_PtrSerialize(&ref, s.buf);
                s.Int(mask);
                if (ref && mask > 0 && mask < 256) g_shareMask[(uintptr_t)ref] = (uint8_t)mask;
            }
            Log("towns: %d shared storage buildings", shares);
        }
    }
    g_agreements.clear();
    g_nextAgreement = 1;
    if (TagFollows(s, stream, kAgreeMagic)) {
        int agreements = 0;
        s.Int(agreements);
        s.Int(g_nextAgreement);
        for (int i = 0; i < agreements && i < 1000; i++) {
            Agreement g{};
            int lo = 0, hi = 0;
            for (int* v : {&g.id, &g.a, &g.b, &g.nA, &g.nB, &g.period, &lo, &hi, &g.lastOk}) s.Int(*v);
            g.lastIdx = ((int64_t)hi << 32) | (uint32_t)lo;
            void* descs[2] = {};
            for (int k = 0; k < 2; k++) {
                int len = 0;
                s.Int(len);
                if (len < 0 || len > 512) len = 0;
                std::vector<int> chars(len);
                s.Ints(chars);
                std::string p(chars.begin(), chars.end());
                descs[k] = p == "-" ? nullptr : commands::DescFromPath(p);
            }
            g.descA = descs[0];
            g.descB = descs[1];
            if (g.a >= 0 && g.a < count && g.b >= 0 && g.b < count) g_agreements.push_back(g);
        }
        Log("towns: %d standing trades", (int)g_agreements.size());
    }
    g_workShare.clear();
    if (TagFollows(s, stream, kWorkMagic)) {
        int works = 0;
        s.Int(works);
        for (int i = 0; i < works && i < 100000; i++) {
            void* ref = nullptr;
            int mask = 0;
            p_PtrSerialize(&ref, s.buf);
            s.Int(mask);
            if (ref && mask > 0 && mask < 256) g_workShare[(uintptr_t)ref] = (uint8_t)mask;
        }
        Log("towns: %d shared workplaces", (int)g_workShare.size());
    }
    g_evTown.clear();
    if (TagFollows(s, stream, kEventMagic)) {
        int events = 0;
        s.Int(events);
        if (events > 0 && events < 10000000) {
            std::vector<int> evTowns(events);
            s.Ints(evTowns);
            for (int t : evTowns) g_evTown.push_back((int8_t)t);
        }
        Log("towns: %d events with towns", (int)g_evTown.size());
    }
    g_townPlayer.clear();
    if (TagFollows(s, stream, kNameMagic)) {
        int names = 0;
        s.Int(names);
        for (int t = 0; t < names && t < 64; t++) {
            int len = 0;
            s.Int(len);
            if (len < 0 || len > 256) break;
            std::vector<int> bytes(len);
            if (len) s.Ints(bytes);
            g_townPlayer.push_back(std::string(bytes.begin(), bytes.end()));
            Log("towns: town %d was %s's", t, g_townPlayer.back().empty() ? "(nobody known)" : g_townPlayer.back().c_str());
        }
    }
    int n = *(int*)(mgr + 0x10);
    if (lost) Log("!! towns: %d saved entity references did not resolve", lost);
    Log("towns: loaded %d towns (I am town %d), %d of %d entities belong to towns 1+, %d delivery piles", count, g_local,
        owned, n, (int)g_deliverTo.size());
    return true;
}

// = exe+0xc9c00 (load: every component's vf11, entity -> manager, re-list flagged entities), each entity in its town
static void __fastcall h_EntityPostLoad(uint8_t* mgr, uint8_t* stream) {
    if (Writing(stream)) {
        if (g_towns.size() > 1) SaveBlock(mgr, stream);
        return o_EntityPostLoad(mgr, stream);
    }
    if (*(int*)(stream + 8) != 0) return o_EntityPostLoad(mgr, stream);
    if (!LoadBlock(mgr, stream)) {
        if (g_count > 1) {  // a one-town save in a separate-towns session (someone joined): new towns to settle
            EnsureTowns();
            g_pick = true;
            uint8_t* t0 = (uint8_t*)g_towns[0];
            for (int i = 0; i < *(int*)(t0 + 0x08) && !g_sites[0].placed; i++) {  // town 0's site: its first storage
                uint8_t* mc = (uint8_t*)FindComponent((*(uint8_t***)t0)[i], kTiMapComponent);
                if (!mc) continue;
                g_sites[0] = Site{*(int16_t*)(mc + 0x28), *(int16_t*)(mc + 0x2a), {}, true};
                ((void(__fastcall*)(void*, float*, int, int))(g_exe + kRvaCellToWorld))(*(void**)(GameLoop() + 0xf8), g_sites[0].pos,
                                                                                          g_sites[0].x, g_sites[0].y);
            }
            Log("towns: one-town save in a %d-town session: town 0 at %d,%d, the others settle", g_count, g_sites[0].x, g_sites[0].y);
        }
        return o_EntityPostLoad(mgr, stream);
    }
    int n = *(int*)(mgr + 0x10);
    for (int i = 0; i < n; i++) {
        uint8_t* e = (*(uint8_t***)(mgr + 0x08))[i];
        {
            Scope sc(OwnerOf(e));
            for (int j = 0; j < *(int*)(e + 0x38); j++) {
                void* comp = (*(void***)(e + 0x30))[j];
                ((void(__fastcall*)(void*))(*(void***)comp)[11])(comp);
            }
        }
        *(uint8_t**)(e + 0x10) = mgr;
        void* ref = e;
        uint32_t flags = *(uint32_t*)(e + 0x40);
        if (flags & 4) ((void(__fastcall*)(void*, int, void*))(g_exe + kRvaArrayAppend))(mgr + 0x88, *(int*)(mgr + 0x90) + 1, &ref);
        if (flags & 0x10) ((void(__fastcall*)(void*, int, void*))(g_exe + kRvaArrayAppend))(mgr + 0x98, *(int*)(mgr + 0xa0) + 1, &ref);
    }
    for (int t = 1; t < (int)g_towns.size(); t++) {  // town 0's runs right after this, from the GameLoop
        Scope sc(t);
        ((TownPostLoad_t)(g_exe + kRvaTownPostLoad))(g_towns[t]);
    }
}

// The "Share" checkbox of the BMP data mod (mod\Template\Storage*.rsc): a ControlUI on the StorageComponent with
// _controlId 77. ControlUI asks the component's vf13(id) every frame (bit 0 = enabled, bit 1 = checked) and a click
// becomes the networked command kVCall 14 -> vf14(id) inside the simulation on every PC. StorageComponent uses the
// shared do-nothing defaults for both, so its own vtable slots 13/14 are pointed here (other ids -> the originals).
static const int kShareControlId = 77;
static const uintptr_t kVtStorageComponent = 0x16ab50;
using ControlGet_t = uint32_t(__fastcall*)(uint8_t* comp, int id);
using ControlSet_t = void(__fastcall*)(uint8_t* comp, int id);
static ControlGet_t o_StorageControlGet;
static ControlSet_t o_StorageControlSet;
static uint32_t __fastcall StorageControlGet(uint8_t* comp, int id) {
    if (id != kShareControlId) return o_StorageControlGet(comp, id);
    void* e = *(void**)(comp + 0x10);
    if (g_towns.size() < 2 || OwnerOf(e) != g_local || !IsOwnStorage(e)) return 0;  // greyed out
    return 1u | (ShareMaskOf(e) ? 2u : 0u);
}
static void __fastcall StorageControlSet(uint8_t* comp, int id) {  // simulation (command)
    if (id != kShareControlId) return o_StorageControlSet(comp, id);
    void* e = *(void**)(comp + 0x10);
    int owner = OwnerOf(e);
    bool on = ShareMaskOf(e) == 0;  // toggle: shared with every other town / with none
    for (int t = 0; t < (int)g_towns.size(); t++)
        if (t != owner) ShareStorage(owner, e, t, on);
}
static void PatchStorageControl() {
    uintptr_t* vt = (uintptr_t*)(g_exe + kVtStorageComponent);
    DWORD old = 0;
    if (!VirtualProtect(vt + 13, 16, PAGE_READWRITE, &old)) { Log("!! towns: storage share control: no vtable access"); return; }
    o_StorageControlGet = (ControlGet_t)vt[13];
    o_StorageControlSet = (ControlSet_t)vt[14];
    vt[13] = (uintptr_t)&StorageControlGet;
    vt[14] = (uintptr_t)&StorageControlSet;
    VirtualProtect(vt + 13, 16, old, &old);
    Log("towns: storage share control (mod checkbox id %d) installed", kShareControlId);
}

void Install() {
    g_exe = (uintptr_t)GetModuleHandleW(nullptr);
    PatchStorageControl();
    p_MemAlloc = (MemAllocFn_t)RuntimeExport("?MemAlloc@MemoryAllocator@System@@SAPEAX_KH@Z");
    g_oldJobs = GetFileAttributesW(L"bmp_oldjobs.txt") != INVALID_FILE_ATTRIBUTES;
    if (g_oldJobs) Log("towns: DEBUG bmp_oldjobs.txt - original job loop, trades move directly");
    struct H { uintptr_t rva; void* fn; void** orig; const char* name; } hooks[] = {
        {kRvaEntityTick, (void*)h_EntityTick, (void**)&o_EntityTick, "EntityTick"},
        {kRvaStartStructures, (void*)h_StartStructures, (void**)&o_StartStructures, "StartStructures"},
        {kRvaStartFamilies, (void*)h_StartFamilies, (void**)&o_StartFamilies, "StartFamilies"},
        {kRvaStartAll, (void*)h_StartAll, (void**)&o_StartAll, "StartAll"},
        {kRvaStartSupplies, (void*)h_StartSupplies, (void**)&o_StartSupplies, "StartSupplies"},
        {0xc8370, (void*)h_EntityKill, (void**)&o_EntityKill, "EntityKill"},
        {kRvaZoneShutdown, (void*)h_ZoneShutdown, (void**)&o_ZoneShutdown, "ZoneShutdown"},
        {0x9cc90, (void*)h_LoopShutdown, (void**)&o_LoopShutdown, "GameLoopShutdown"},
        {kRvaTryWorks, (void*)h_TryWorks, (void**)&o_TryWorks, "TryWorks"},
        {0xfca20, (void*)h_IdlePick, (void**)&o_IdlePick, "IdlePick"},
        {kRvaRoadMark, (void*)h_RoadMark, (void**)&o_RoadMark, "RoadMark"},
        {kRvaStorageStop, (void*)h_StorageStop, (void**)&o_StorageStop, "StorageStop"},
        {kRvaWorkMapRemove, (void*)h_WorkMapRemove, (void**)&o_WorkMapRemove, "WorkMapRemove"},
        {kRvaStorageRemove, (void*)h_StorageRemove, (void**)&o_StorageRemove, "StorageRemove (watch)"},
        {kRvaLaborBalance, (void*)h_LaborBalance, (void**)&o_LaborBalance, "LaborBalance"},
        {kRvaBuildRoadTry, (void*)h_BuildRoadTry, (void**)&o_BuildRoadTry, "BuildRoadTry"},
        {kRvaPickupTaken, (void*)h_PickupTaken, (void**)&o_PickupTaken, "PickupTaken"},
        {kRvaFindStorage, (void*)h_FindStorage, (void**)&o_FindStorage, "FindStorage"},
        {kRvaStoreInventory, (void*)h_StoreInventory, (void**)&o_StoreInventory, "StoreInventory"},
        {kRvaPickupAction, (void*)h_PickupAction, (void**)&o_PickupAction, "PickupAction"},
        {kRvaDropPoint, (void*)h_DropPoint, (void**)&o_DropPoint, "DropPoint"},
        {kRvaTownSerialize, (void*)h_TownSerialize, (void**)&o_TownSerialize, "TownSerialize"},
        {kRvaEntityPostLoad, (void*)h_EntityPostLoad, (void**)&o_EntityPostLoad, "EntityPostLoad"},
        {0x68590, (void*)h_NotifyUpdate, (void**)&o_NotifyUpdate, "NotifyUpdate"},
    };
    for (auto& h : hooks) {
        void* target = (void*)(g_exe + h.rva);
        MH_STATUS s = MH_CreateHook(target, h.fn, h.orig);
        if (s == MH_OK) s = MH_EnableHook(target);
        Log("towns: hook %s -> %s", h.name, MH_StatusToString(s));
    }
    Log("towns: bucket update call patched: %s", PatchBucketCall() ? "yes" : "NO");
    Log("towns: rebalance profession order patched: %s", PatchRebalancePick() ? "yes" : "NO");
    p_SerCtor = (SerCtor_t)RuntimeExport("??0Serializer@IO@@QEAA@AEAVStream@1@H@Z");
    p_SerDtor = (SerDtor_t)RuntimeExport("??1Serializer@IO@@QEAA@XZ");
    p_SerInt = (SerInt_t)RuntimeExport("?Serialize@Serializer@IO@@QEAAXAEAHPEBD@Z");
    p_SerFloat = (SerFloat_t)RuntimeExport("?Serialize@Serializer@IO@@QEAAXAEAMPEBD@Z");
    p_SerInts = (SerInts_t)RuntimeExport("?Serialize@Serializer@IO@@QEAAXHPEAHPEBD@Z");
    p_GetMemStream = (GetMemStream_t)RuntimeExport("?GetMemoryStream@CompressedMemoryStream@IO@@QEAAAEAVMemoryStream@2@XZ");
    p_StreamPos = (StreamPos_t)RuntimeExport("?GetCurrentPos@MemoryStream@IO@@UEBAIXZ");
    p_StreamSize = (StreamPos_t)RuntimeExport("?GetSize@MemoryStream@IO@@UEBAIXZ");
    p_CompressedVtbl = RuntimeExport("??_7CompressedMemoryStream@IO@@6B@");
    p_PtrSerialize = (PtrSerialize_t)RuntimeExport("?Serialize@InternalBasePtr@Object@@QEAAXAEAVSerializer@IO@@@Z");
    Log("towns: save support %s", p_SerCtor && p_SerInts && p_StreamSize && p_CompressedVtbl && p_PtrSerialize ? "ready" : "MISSING EXPORTS");
}

}  // namespace towns
