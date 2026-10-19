// Phase 3a: determinism probe.
//  - Sim ticks: GameLoop::Update (exe) runs `speed` fixed-dt sim steps per 60 Hz frame; each step starts
//    with World::Update (exe+0xab1e0). We hook that to number ticks exactly.
//  - Every System::Random draw is folded (caller RVA + output) into a per-tick hash, written to
//    bmp_ticks.log. Two runs from the same save with no input must produce identical tick logs.
//  - Draws outside a sim tick (UI/render) or off the main thread are counted separately: if those touch
//    sim RNG, camera/UI would leak into the simulation.
//  - Random() default ctor seeds from time(): log every call site.
//  NOTE: MainLoop::AddPostUpdate is unusable - Frame() reads the callback userdata from the wrong
//  array (this+0x80 instead of this+0x90) and crashes.
#include "common.h"
#include "MinHook.h"
#include "simheap.h"
#include "autotest.h"
#include "actionprobe.h"
#include "simwatch.h"
#include "commands.h"
#include "visualstub.h"
#include "net.h"
#include "gameload.h"
#include "overlay.h"
#include "towns.h"
#include "steam.h"
#include "gamewin.h"
#include "tint.h"
#include "uiscript.h"
#include "modversion.h"
#include <intrin.h>
#include <share.h>
#include <unordered_map>
#include <mutex>
#include <map>
#include <vector>
#include <algorithm>
#include <thread>

// exe build this was reverse-engineered against (PE TimeDateStamp, 2017-09-10)
static const DWORD kExeTimestamp = 0x59b56a2d;
static const uintptr_t kRvaGameLoopUpdate = 0x9b850;  // in-game UpdateLoop::Update (vtable 0x164880 slot 3)
static const uintptr_t kRvaWorldUpdate = 0xab1e0;     // first call of each sim step
static const uintptr_t kRvaStepEnd = 0x91780;         // last call of each sim step
static const uintptr_t kRvaAddEntity = 0xc9d70;       // EntityManager::AddEntity(mgr, entity*, bucketClass)
static const uintptr_t kRvaRemoveEntity = 0xc8db0;    // EntityManager::RemoveEntity(mgr, entity*)
static const uintptr_t kRvaPathFlush = 0xabb60;        // Pathfinder: wait until its job queue is drained
static const uintptr_t kRvaTreeErase = 0x1b040;        // generic ordered-set erase(set, node)
static const uintptr_t kRvaPerFrameSetSync = 0xc8f80;  // EntityManager: apply pending inserts to per-frame set +0x78
static const uintptr_t kRvaEntityFlagsChanged = 0xc8940;  // ComponentEntity: recompute +0x40 flags, queue set changes
static const uintptr_t kRvaWorldSingleton = 0x1cbdd8;  // global World* (scene +0x60, camera +0xe0, sim Random* +0xd8)
static const uint32_t kSessionSeed = 0x5EED0001;       // multiplayer: host-provided

static uintptr_t g_exeBase, g_exeEnd, g_rtBase, g_rtEnd;
static DWORD g_mainThread;
static FILE* g_ticks;
static FILE* g_posDump;  // bmp_posdump.txt: exact villager x/y per tick
static FILE* g_stateDump;  // bmp_statedump.txt: villager objects at some ticks of the first session
static unsigned long long g_stateFrom, g_stateTo, g_stateStep, g_stateLast;
static uint8_t* g_watchAddr;  // bmp_watch.txt: hardware write watch on one villager field (WatchTick)
static unsigned long long g_watchFrom = ~0ull, g_watchTo;
static int g_watchTown, g_watchIndex, g_watchOff;
static void WatchTick();
static bool g_loadAllocLog;  // bmp_loadallocs.txt: draws log gets "L gen caller size offset" for loader-thread allocations
static FILE* g_draws;                       // every in-tick draw for the first kDrawLogTicks ticks
// sim-heap allocation/free logging (size, offset, stack) for ticks in [g_allocLogFrom, g_allocLogTo]
static uint64_t g_allocLogFrom = 0, g_allocLogTo = 2000;
void ProbeSetAllocLog(uint64_t from, uint64_t to) { g_allocLogFrom = from; g_allocLogTo = to; }
static bool AllocLogTick();
static const uint64_t kDrawLogTicks = ~0ull;  // log every in-tick draw (with call stack)

// Object::Internal's ctor draws a random handle tag from InternalFactory's RNG (factory + 0x20).
// UI dialogs are Internal objects too, so this stream is polluted by menu use; track it separately.
using InternalFactoryGet_t = uint8_t*(__fastcall*)();
static InternalFactoryGet_t p_InternalFactoryGet;
static void* FactoryRng() {
    static void* rng;
    if (!rng && p_InternalFactoryGet) {
        uint8_t* f = p_InternalFactoryGet();
        if (f) rng = f + 0x20;
    }
    return rng;
}

static void ModuleRange(HMODULE m, uintptr_t& lo, uintptr_t& hi) {
    auto dos = (IMAGE_DOS_HEADER*)m;
    auto nt = (IMAGE_NT_HEADERS*)((uint8_t*)m + dos->e_lfanew);
    lo = (uintptr_t)m;
    hi = lo + nt->OptionalHeader.SizeOfImage;
}

// stable (ASLR-independent) id for a code address: 'E'/'R' + rva, 0 if elsewhere (e.g. hook trampolines)
static uint32_t CodeId(const void* p) {
    uintptr_t a = (uintptr_t)p;
    if (a >= g_exeBase && a < g_exeEnd) return 0x10000000u | uint32_t(a - g_exeBase);
    if (a >= g_rtBase && a < g_rtEnd) return 0x20000000u | uint32_t(a - g_rtBase);
    return 0;
}

static void Describe(const void* addr, char* out, size_t n) {
    uint32_t id = CodeId(addr);
    if (id >> 28 == 1) snprintf(out, n, "exe+%#x", id & 0xfffffff);
    else if (id >> 28 == 2) snprintf(out, n, "rt+%#x", id & 0xfffffff);
    else snprintf(out, n, "%p", addr);
}

// ---------- tick + RNG state ----------
static constexpr uint64_t kFnvBasis = 0xcbf29ce484222325ull, kFnvPrime = 0x100000001b3ull;
static inline uint64_t Fnv(uint64_t h, uint64_t v) {
    for (int i = 0; i < 8; i++) { h ^= (v >> (i * 8)) & 0xff; h *= kFnvPrime; }
    return h;
}

struct Session {
    void* loop = nullptr;
    bool inTick = false;
    uint64_t tick = 0;
    uint64_t tickCalls = 0, tickHash = kFnvBasis, chain = kFnvBasis;       // all draws
    uint64_t simCalls = 0, simHash = kFnvBasis, simChain = kFnvBasis;     // excluding factory handle tags
    uint64_t heapChain = kFnvBasis;  // in-sim allocations/reusable frees (sim-heap offsets): drifts before draws do
    uint64_t freeRun = 0;            // the reusable frees since the last in-sim allocation, summed (order-free, below)
    uint64_t outsideCalls = 0, offThreadCalls = 0;
    std::unordered_map<uint32_t, uint64_t> outsideCallers;  // caller id -> count (draws outside ticks)
    uint32_t heightHash = 2166136261u, pickHash = 2166136261u;  // sim terrain-height queries this tick (exe+0xa48b0)
    uint32_t heightCalls = 0, pickCalls = 0;
    std::map<uint32_t, uint32_t> heightCallers;  // caller id -> calls this tick
    uint32_t rootHash = 2166136261u, rootCalls = 0, rootRenderBranch = 0;  // RootAnimationOffset in the sim
} g_s;

// Render-driven work nested inside a tick (Graphics::MeshInstance::Update for on-screen meshes fires animation
// events such as AnimAudioEvent, which draws from the sim RNG) is visual, not simulation.
static bool AllocLogTick() { return g_s.tick >= g_allocLogFrom && g_s.tick <= g_allocLogTo; }
static thread_local int t_visual;  // per thread: also covers the loader thread
static inline bool InSim() { return g_s.inTick && t_visual == 0 && g_visualStub == 0; }
// Where sim-heap allocations/free-list reuse are allowed: main thread inside a sim tick, or the loader thread
// building a map/save before the session starts - never inside visual work.
static thread_local int t_loading;  // inside Engine::Loader::Run (the one thread that builds the map/save)
// Resource loading (External::Load, UpdateLoop::LoadResources): resources are cached for the whole process, so
// whether a load allocates depends on what the process loaded before -> never in the sim heap.
static thread_local int t_resource;
static inline bool SimHeapContext(bool mainThread) {
    if (t_resource) return false;
    return mainThread ? InSim() : (t_loading && !g_s.loop && t_visual == 0 && g_visualStub == 0);
}

// First few game/engine frames of the current call stack (skipping our DLL, hook trampolines and
// System::Random::*) = which system is doing something.
// the engine's System::Random functions (skipped in recorded stacks): around Random::Serialize, which sits at rt+0xca560
// in the Steam build's range rt+0xca500..0xca900 (the GOG build has them lower)
static bool InRandomCode(uint32_t f) {
    static uint32_t lo = [] {
        uintptr_t s = (uintptr_t)RuntimeExport("?Serialize@Random@System@@QEAAXAEAVSerializer@IO@@@Z");
        return s && s > g_rtBase ? 0x20000000u | uint32_t(s - g_rtBase - 0x60) : 0x200ca500u;
    }();
    return f >= lo && f < lo + 0x400;
}

static void StackStr(char* out, size_t n, int depth = 4) {
    void* frames[24];
    USHORT count = RtlCaptureStackBackTrace(2, 24, frames, nullptr);
    int kept = 0, len = 0;
    out[0] = 0;
    for (USHORT i = 0; i < count && kept < depth; i++) {
        uint32_t f = CodeId(frames[i]);
        if (!f || InRandomCode(f)) continue;
        len += snprintf(out + len, n - len, " %08x", f);
        kept++;
    }
}

// EntityManager (GameLoop+0xe8): +0x10 entity count, buckets at +0x18+i*0x10 (ptr, count at +8),
// round-robin cursors at +0xbc+i*4.
static void EmState(char* out, size_t n) {
    uint8_t* loop = (uint8_t*)g_s.loop;
    uint8_t* em = loop ? *(uint8_t**)(loop + 0xe8) : nullptr;
    if (!em) { snprintf(out, n, "em=-"); return; }
    int* c = (int*)(em + 0xbc);
    snprintf(out, n, "em=%d b=%d,%d,%d,%d,%d c=%d,%d,%d,%d,%d", *(int*)(em + 0x10), *(int*)(em + 0x20),
             *(int*)(em + 0x30), *(int*)(em + 0x40), *(int*)(em + 0x50), *(int*)(em + 0x60), c[0], c[1], c[2], c[3], c[4]);
}

// Reuse is lowest-address-first, so only WHICH blocks were freed between two allocations matters, not their order (the
// game frees pointer-keyed trees in tree order - e.g. the market vendor's goods tree, exe+0xf5450 - which differs
// between PCs while the outcome is the same). Frees are summed and folded into the chain at the next allocation.
static uint64_t Mix64(uint64_t x) {
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull; x ^= x >> 27; x *= 0x94d049bb133111ebull; return x ^ (x >> 31);
}
static std::map<uint32_t, std::string> g_pendingFreeLogs;  // SimFree: caller -> line, logged once the session goes on
static std::unordered_map<uint32_t, bool> g_loggedFrees;
static void FoldFrees() {
    if (g_s.freeRun) g_s.heapChain = Fnv(g_s.heapChain, g_s.freeRun), g_s.freeRun = 0;
}

static void FlushTick() {
    if (!g_s.inTick) return;
    FoldFrees();
    for (auto& [caller, l] : g_pendingFreeLogs) { Log("%s", l.c_str()); g_loggedFrees[caller] = true; }
    g_pendingFreeLogs.clear();
    g_s.chain = Fnv(g_s.chain, g_s.tickHash);
    g_s.simChain = Fnv(g_s.simChain, g_s.simHash);
    if (g_s.tick % 600 == 0) net::OnTickHash(g_s.tick, g_s.simChain, g_s.heapChain);
    if (g_s.tick == 1 || g_s.tick == 1000) {  // floating-point state of the sim thread (must match between PCs/builds)
        unsigned x87 = 0;
        _controlfp_s(&x87, 0, 0);
        Log("fp: tick %llu mxcsr %#x x87 control %#x", g_s.tick, _mm_getcsr(), x87);
    }
    if (g_watchFrom != ~0ull) WatchTick();
    if (g_stateDump) {
        if (g_s.tick < g_stateLast) { fclose(g_stateDump); g_stateDump = nullptr; }  // first session only
        else if (g_s.tick >= g_stateFrom && g_s.tick <= g_stateTo && (g_s.tick - g_stateFrom) % g_stateStep == 0) {
            towns::DumpCitizens(g_stateDump, g_s.tick);
            fflush(g_stateDump);
        }
        g_stateLast = g_s.tick;
    }
    char em[128];
    EmState(em, sizeof em);
    static char pos[8192];
    if (g_ticks) towns::PositionHashes(pos, sizeof pos);
    if (g_posDump) {
        static float xy[4096];
        int n = towns::CopyPositions(xy, 2048);
        fprintf(g_posDump, "%llu", g_s.tick);
        uint8_t* world = *(uint8_t**)((uint8_t*)g_s.loop + 0xf8);
        int size = world ? *(int*)world : 0;
        uint8_t* cells = world ? *(uint8_t**)(world + 0x80) : nullptr;
        for (int i = 0; i < n; i++) {  // + the type byte (+0x29) and level (+0x26) of the cell under the villager
            int cx = (int)(xy[i * 2] + (float)(size >> 1)), cy = (int)(xy[i * 2 + 1] + (float)(size >> 1));
            int type = -1, level = -1;
            if (cells && cx >= 0 && cy >= 0 && cx < size && cy < size) {
                uint8_t* c = cells + ((int64_t)cy * size + cx) * 0x30;
                type = c[0x29];
                level = c[0x26];
            }
            fprintf(g_posDump, " %a,%a,%d,%d", xy[i * 2], xy[i * 2 + 1], type, level);
        }
        fprintf(g_posDump, "\n");
    }
    if (g_ticks)
        fprintf(g_ticks, "%llu %llu %016llx %016llx sim %llu %016llx %016llx out=%llu off=%llu %s%s\n", g_s.tick,
                g_s.tickCalls, g_s.tickHash, g_s.chain, g_s.simCalls, g_s.simHash, g_s.simChain,
                g_s.outsideCalls, g_s.offThreadCalls, em, pos);
    if (g_ticks) {
        fprintf(g_ticks, "#h %llu %u %08x %u %08x", g_s.tick, g_s.heightCalls, g_s.heightHash, g_s.pickCalls, g_s.pickHash);
        for (auto& [c, n] : g_s.heightCallers) fprintf(g_ticks, " %x:%u", c, n);
        fprintf(g_ticks, " root %u %08x render %u", g_s.rootCalls, g_s.rootHash, g_s.rootRenderBranch);
        fprintf(g_ticks, "\n");
    }
    g_s.heightCallers.clear();
    g_s.rootHash = 2166136261u;
    g_s.rootCalls = g_s.rootRenderBranch = 0;
    g_s.heightHash = g_s.pickHash = 2166136261u;
    g_s.heightCalls = g_s.pickCalls = 0;
    g_s.inTick = false;
}

// Every in-sim draw of the last ~kRingSize draws (tick, caller, value, the call's limits, 3 stack frames), kept in
// memory and written to <game>\bmp_desync_<time>.log only when a desync is detected (ProbeDumpRecentDraws): the
// per-draw file log is test-only, but a real two-PC desync needs exactly this to find the call that differed.
struct RingDraw { uint32_t tick, caller, value; int32_t a, b; uint32_t stack[3]; };
static const size_t kRingSize = 1 << 21;  // draws + job offers + actions: well over 1000 ticks (64 MB)
static RingDraw* g_ring;
static size_t g_ringPos;
static void RingAdd(uint32_t caller, uint64_t value, int32_t a, int32_t b) {
    if (!g_ring) g_ring = (RingDraw*)VirtualAlloc(nullptr, kRingSize * sizeof(RingDraw), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!g_ring) return;
    RingDraw& d = g_ring[g_ringPos++ & (kRingSize - 1)];
    d = RingDraw{(uint32_t)g_s.tick, caller, (uint32_t)value, a, b, {}};
    void* frames[8];
    USHORT n = RtlCaptureStackBackTrace(3, 8, frames, nullptr);
    for (USHORT i = 0, k = 0; i < n && k < 3; i++)
        if (uint32_t f = CodeId(frames[i]); f && !InRandomCode(f)) d.stack[k++] = f;
}
// Extra in-sim events for the desync record (shown with their tag in the caller column): job offers, action starts.
void ProbeNote(uint32_t tag, uint64_t value, int32_t a, int32_t b) {
    if (InSim() && GetCurrentThreadId() == g_mainThread) RingAdd(tag, value, a, b);
}
void ProbeDumpRecentDraws(const char* why) {
    if (!g_ring || !g_ringPos) return;
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf_s(wcsrchr(path, L'\\') + 1, 80, L"bmp_desync_%04d%02d%02d_%02d%02d%02d%s.log", st.wYear, st.wMonth, st.wDay,
               st.wHour, st.wMinute, st.wSecond, SlotSuffix());
    FILE* f = _wfsopen(path, L"w", _SH_DENYNO);
    if (!f) return;
    fprintf(f, "# %s - last in-sim draws: tick caller value a b | stack (session %p)\n", why, g_s.loop);
    size_t n = g_ringPos < kRingSize ? g_ringPos : kRingSize;
    uint32_t last = g_ring[(g_ringPos - 1) & (kRingSize - 1)].tick, from = last > 1500 ? last - 1500 : 0;  // the last 1500 ticks
    for (size_t i = g_ringPos - n; i < g_ringPos; i++) {
        const RingDraw& d = g_ring[i & (kRingSize - 1)];
        if (d.tick < from) continue;
        fprintf(f, "%u %08x %08x %d %d | %08x %08x %08x\n", d.tick, d.caller, d.value, d.a, d.b, d.stack[0], d.stack[1], d.stack[2]);
    }
    fclose(f);
    Log("wrote the last %zu draws to %ls (%s)", n, path, why);
}

static void RngNote(void* self, void* ret, uint64_t value, int32_t a = 0, int32_t b = 0) {
    if (GetCurrentThreadId() != g_mainThread) { InterlockedIncrement64((LONG64*)&g_s.offThreadCalls); return; }
    uint32_t id = CodeId(ret);
    if (InSim()) {
        bool factory = self == FactoryRng();
        if (!factory) RingAdd(id, value, a, b);
        g_s.tickCalls++;
        g_s.tickHash = Fnv(g_s.tickHash, value);  // values only: caller ids differ between the Steam and GOG builds
        if (!factory) {
            g_s.simCalls++;
            g_s.simHash = Fnv(g_s.simHash, value);  // no caller id: it's a build-specific address (Steam/GOG players)
        }
        if (g_draws && g_s.tick <= kDrawLogTicks) {
            char stack[96];
            StackStr(stack, sizeof stack);
            fprintf(g_draws, "%llu %c %08x %08llx |%s\n", g_s.tick, factory ? 'F' : 'S', id, value, stack);
        }
    } else {
        g_s.outsideCalls++;
        g_s.outsideCallers[id]++;
    }
}

// Outside sim ticks (UI, tools, rendering) nothing may consume the sim RNG: e.g. EditTool picks a random
// preview variant from it when the player clicks the toolbar. Such draws go to a separate UI RNG.
alignas(16) static uint8_t g_uiRng[0x9d0];
static bool g_uiRngReady;
static uint64_t g_redirected;
static void* SimRng() {
    uint8_t* loop = *(uint8_t**)(g_exeBase + Rva(kRvaWorldSingleton));
    return loop ? *(void**)(loop + 0xd8) : nullptr;
}
// Same for the object handle-tag RNG (InternalFactory+0x20): UI objects created between ticks must not shift
// the tags later handed to sim objects. (Draws during a tick stay on it, so sim-object tags match everywhere.)
alignas(16) static uint8_t g_uiTagRng[0x9d0];
static void* RedirectOutsideTick(void* self) {
    if (!InSim() && g_uiRngReady && g_s.loop && GetCurrentThreadId() == g_mainThread) {
        if (self == SimRng()) { g_redirected++; return g_uiRng; }
        if (self == FactoryRng()) return g_uiTagRng;
    }
    return self;
}

#define RNG_HOOK(name, ret, mangled, params, args, bits, la, lb)                    \
    using name##_t = ret(__fastcall*) params;                                       \
    static name##_t o_##name;                                                       \
    static ret __fastcall h_##name params {                                         \
        self = RedirectOutsideTick(self);                                           \
        ret r = o_##name args;                                                      \
        RngNote(self, _ReturnAddress(), bits, la, lb);                              \
        return r;                                                                   \
    }                                                                               \
    static const char* m_##name = mangled;

static inline uint64_t FBits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

RNG_HOOK(RandU,        uint32_t, "?Rand@Random@System@@QEAAIXZ",        (void* self),                 (self),       r, 0, 0)
RNG_HOOK(RandMax,      uint32_t, "?Rand@Random@System@@QEAAII@Z",       (void* self, uint32_t m),     (self, m),    r, (int32_t)m, 0)
RNG_HOOK(RandRange,    int,      "?Rand@Random@System@@QEAAHHH@Z",      (void* self, int a, int b),   (self, a, b), (uint32_t)r, a, b)
RNG_HOOK(RandFloat,    float,    "?RandFloat@Random@System@@QEAAMXZ",   (void* self),                 (self),       FBits(r), 0, 0)
RNG_HOOK(RandFloatTol, float,    "?RandFloatTol@Random@System@@QEAAMXZ",(void* self),                 (self),       FBits(r), 0, 0)
RNG_HOOK(RandTol,      int,      "?RandTol@Random@System@@QEAAHH@Z",    (void* self, int t),          (self, t),    (uint32_t)r, t, 0)

using RandInit_t = void(__fastcall*)(void* self, uint32_t seed);
static RandInit_t o_RandInit;
static void __fastcall h_RandInit(void* self, uint32_t seed) {
    char who[64];
    Describe(_ReturnAddress(), who, sizeof who);
    Log("Random::Initialize this=%p seed=%u from %s (tick %llu)", self, seed, who, g_s.tick);
    o_RandInit(self, seed);
}

using RandTimeCtor_t = void*(__fastcall*)(void* self);
static RandTimeCtor_t o_RandTimeCtor;
// Random() seeds from time(). Replace with a deterministic seed per (call site, nth construction there),
// so every client gets identical generators regardless of wall clock. The counts restart at every load
// (h_LoaderStartup): a player who played another game first had them ahead -> different seeds from tick 1.
static std::unordered_map<uint32_t, uint32_t> g_ctorCounts;
static std::mutex g_ctorMutex;  // main + loader thread
static void* __fastcall h_RandTimeCtor(void* self) {
    void* r = o_RandTimeCtor(self);
    uint32_t site = CodeId(_ReturnAddress());
    // seeds must agree between the Steam and GOG builds: exe call sites by their Steam address (tools/gog_table.py
    // puts every Random() call site in the table)
    if (site >> 28 == 1) site = 0x10000000u | (uint32_t)SteamRva(site & 0xfffffff);
    else if (site >> 28 == 2 && gamebuild::Get() == gamebuild::kGog) Log("!! Random() constructed by the engine DLL (%08x) - its seed differs between builds", site);
    uint32_t n;
    { std::lock_guard<std::mutex> lock(g_ctorMutex); n = g_ctorCounts[site]++; }
    uint32_t seed = (uint32_t)Fnv(Fnv(kFnvBasis, site), n);
    o_RandInit(self, seed);
    char who[64];
    Describe(_ReturnAddress(), who, sizeof who);
    Log("Random() ctor this=%p from %s -> deterministic seed %08x (tick %llu, thread %lu)", self, who, seed, g_s.tick,
        GetCurrentThreadId());
    return r;
}

using RandSeedCtor_t = void*(__fastcall*)(void* self, uint32_t seed);
static RandSeedCtor_t o_RandSeedCtor;
static void* __fastcall h_RandSeedCtor(void* self, uint32_t seed) {
    char who[64];
    Describe(_ReturnAddress(), who, sizeof who);
    Log("Random(seed=%08x) ctor this=%p from %s (tick %llu, thread %lu)", seed, self, who, g_s.tick, GetCurrentThreadId());
    return o_RandSeedCtor(self, seed);
}

using RandSerialize_t = void(__fastcall*)(void* self, void* serializer);
static RandSerialize_t o_RandSerialize;
static void __fastcall h_RandSerialize(void* self, void* serializer) {
    char who[64];
    Describe(_ReturnAddress(), who, sizeof who);
    o_RandSerialize(self, serializer);
    uint64_t h = kFnvBasis;
    for (int i = 0; i < 625; i++) h = Fnv(h, ((uint32_t*)self)[i]);
    Log("Random::Serialize this=%p from %s -> index=%u statehash=%016llx (thread %lu)", self, who, *(uint32_t*)self, h,
        GetCurrentThreadId());
}

// Audio::Scene::Update(Time, Time, Random&, const Vector3& listener, float) is passed the *sim* RNG
// (GameLoop+0xd8) and draws from it inline for sounds near the camera -> camera position desyncs the
// simulation. Give audio its own generator.
using AudioSceneUpdate_t = bool(__fastcall*)(void* self, int64_t t0, int64_t t1, void* rng, const void* listener, float f);
static AudioSceneUpdate_t o_AudioSceneUpdate;
alignas(16) static uint8_t g_audioRng[0x9d0];
static bool __fastcall h_AudioSceneUpdate(void* self, int64_t t0, int64_t t1, void* rng, const void* listener, float f) {
    static bool init;
    if (!init && o_RandSeedCtor) {
        o_RandSeedCtor(g_audioRng, 0xA0D10);
        init = true;
        Log("audio RNG separated (game passed %p)", rng);
    }
    return o_AudioSceneUpdate(self, t0, t1, init ? g_audioRng : rng, listener, f);
}

// ---------- deterministic sim heap ----------
// Entity sets are ordered by raw pointer, so sim objects must not share the engine heap with UI/camera
// allocations. Route: game-exe caller AND (inside a sim tick on the main thread, OR on a non-main
// thread = loader building the map/save). Frees are routed by address.
using MemAlloc_t = void*(__fastcall*)(uint64_t size, int align);
using MemFree_t = void(__fastcall*)(void* p);
using AllocatorFree_t = void(__fastcall*)(void* self, void* p);
static MemAlloc_t o_MemAlloc;
static MemFree_t o_MemFree;
static AllocatorFree_t o_AllocatorFree;
static std::unordered_map<uint32_t, uint64_t> g_strayExeAllocs;  // exe allocs kept on the engine heap during a session
static SRWLOCK g_strayLock = SRWLOCK_INIT;

// Test (<game>\bmp_heapjitter<suffix>.txt): two instances on one PC lay out the engine heap identically, two PCs don't -
// anything ordered by an engine-heap pointer can differ between real players but never in local tests. With the flag,
// engine-heap allocations get random extra blocks around them (some freed again later), so this instance's objects end
// up at different addresses AND in a different address order.
static bool g_heapJitter;
// Test (<game>\bmp_heapnudge<suffix>.txt, content = tick): one extra in-sim allocation at that tick on this instance only
// = a sim-heap drift without any gameplay difference (the net HEAP check must report it and dump the ring).
static uint64_t g_heapNudgeAt;
static SRWLOCK g_jitterLock = SRWLOCK_INIT;
static void HeapJitter() {
    static uint32_t s = GetTickCount() | 1;
    static void* held[256];
    AcquireSRWLockExclusive(&g_jitterLock);
    s ^= s << 13, s ^= s >> 17, s ^= s << 5;
    if ((s & 3) == 0) {
        void*& slot = held[(s >> 8) & 255];
        if (slot) o_MemFree(slot);
        slot = o_MemAlloc(16 + ((s >> 16) & 0x3ff), 8);
    }
    ReleaseSRWLockExclusive(&g_jitterLock);
}

static void* __fastcall h_MemAlloc(uint64_t size, int align) {
    uint32_t caller = CodeId(_ReturnAddress());
    if (g_heapJitter && !SimHeapContext(GetCurrentThreadId() == g_mainThread)) HeapJitter();
    if ((caller >> 28) == 1) {
        bool mainThread = GetCurrentThreadId() == g_mainThread;
        if (SimHeapContext(mainThread)) {
            // main thread inside a sim tick, or the loader thread building a map/save before the session
            if (void* p = simheap::Alloc(size, align)) {
                if (mainThread && g_s.loop) {  // heap drift check + desync record: tag M + size, value = offset, a = caller
                    FoldFrees();
                    g_s.heapChain = Fnv(Fnv(g_s.heapChain, size), simheap::Offset(p));
                    RingAdd(0x4d000000u | (uint32_t)(size & 0xffffff), simheap::Offset(p), (int32_t)caller, 0);
                }
                if (g_draws && mainThread && g_s.loop && AllocLogTick()) {
                    char stack[96];
                    StackStr(stack, sizeof stack);
                    fprintf(g_draws, "%llu M %08llx %08llx |%s\n", g_s.tick, size, simheap::Offset(p), stack);
                } else if (g_draws && g_loadAllocLog && !mainThread) {  // bmp_loadallocs.txt: loader-time allocations
                    char stack[160];
                    StackStr(stack, sizeof stack, 10);
                    fprintf(g_draws, "L %llu %08x %08llx %08llx |%s\n", simheap::GetStats().generation, caller, size,
                            simheap::Offset(p), stack);
                }
                return p;
            }
        } else if (g_s.loop) {
            AcquireSRWLockExclusive(&g_strayLock);
            g_strayExeAllocs[caller | (mainThread ? 0 : 0x80000000u)]++;  // high bit = background thread
            ReleaseSRWLockExclusive(&g_strayLock);
        }
    }
    return o_MemAlloc(size, align);
}
// Frees outside a sim tick (UI/camera code releasing loader-time objects) must not feed the sim heap's free
// lists, or the next sim allocation lands elsewhere: quarantine those blocks instead of reusing them.
static void SimFree(void* p, uint32_t caller) {
    bool main = GetCurrentThreadId() == g_mainThread;
    bool reuse = SimHeapContext(main);
    if (reuse && main && g_s.loop) {  // a reusable free decides where a later allocation lands: tag X, value = offset
        g_s.freeRun += Mix64(simheap::Offset(p) | 0x8000000000000000ull);
        RingAdd(0x58000000u, simheap::Offset(p), (int32_t)caller, 0);
    }
    // UI code freeing simulation memory during play = the UI changed sim state on this PC only (how the production
    // window's statistics rollover was found). Logged per calling code, with the stack the first time - at the next
    // sim tick, so a session being torn down (resync, leaving: everything is freed) doesn't count.
    if (!reuse && main && g_s.loop && g_s.tick > 20) {
        if (!g_loggedFrees.count(caller) && !g_pendingFreeLogs.count(caller) && g_pendingFreeLogs.size() < 20) {
            char stack[160], line[256];
            StackStr(stack, sizeof stack, 8);
            snprintf(line, sizeof line, "!! sim memory freed outside the simulation at tick %llu by %08x |%s", g_s.tick, caller, stack);
            g_pendingFreeLogs[caller] = line;
        }
    }
    if (g_draws && main && g_s.loop && AllocLogTick()) {
        char stack[96];
        StackStr(stack, sizeof stack);
        fprintf(g_draws, "%llu %c %08x %08llx |%s\n", g_s.tick, reuse ? 'X' : 'x', 0, simheap::Offset(p), stack);
    }
    simheap::Free(p, reuse);
}
static void __fastcall h_MemFree(void* p) {
    if (simheap::Contains(p)) return SimFree(p, CodeId(_ReturnAddress()));
    o_MemFree(p);
}
static void __fastcall h_AllocatorFree(void* self, void* p) {
    if (simheap::Contains(p)) return SimFree(p, CodeId(_ReturnAddress()));
    o_AllocatorFree(self, p);
}

static void ReportHeap() {
    auto st = simheap::GetStats();
    Log("simheap: allocs=%llu frees=%llu live=%llu (%llu KB) committed=%llu MB (old generations given back %llu MB) "
        "generation %llu fallbacks=%llu quarantined=%llu KB", st.allocs, st.frees, st.live, st.liveBytes / 1024,
        st.committed >> 20, st.released >> 20, st.generation, st.fallbacks, st.quarantined / 1024);
    std::vector<std::pair<uint32_t, uint64_t>> top(g_strayExeAllocs.begin(), g_strayExeAllocs.end());
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (size_t i = 0; i < top.size() && i < 8; i++)
        Log("   exe alloc outside tick (main heap) exe+%#x%s: %llu", top[i].first & 0xfffffff,
            (top[i].first & 0x80000000u) ? " [background thread]" : "", top[i].second);
}

// ---------- game loop / sim tick ----------
static uint64_t g_sessionCount;      // game sessions started in this process
// The main menu's animated village runs World::Update too; only steps of the in-game GameLoop are sim ticks.
static bool g_inGameLoopUpdate;
using PausesGame_t = bool(__fastcall*)(void* ui);
static PausesGame_t p_PausesGame;
static bool g_watch;                 // sim-heap write watch armed (bmp_simwatch.txt present)
static int g_stepsExpected, g_stepsDone;
// Lockstep speed cap: GameLoop::Update reads the step count (speedCtl+0x34) once before its step loop; the cap must
// be gone before any UI code of that frame runs - the speed panel resets an unknown speed (e.g. a cap of 3 or 0)
// to 1x, which then looked like a player's choice and was broadcast (10x flip-flopping).
static uint8_t* g_capSpeedCtl;
static int g_capValue, g_capDesired;
static void UndoSpeedCap() {
    if (!g_capSpeedCtl) return;
    int* speed = (int*)(g_capSpeedCtl + 0x34);
    if (*speed == g_capValue) *speed = g_capDesired;  // a sim event (autoslow) may have set its own value: keep that
    g_capSpeedCtl = nullptr;
}
static int g_posEndN = -1;  // POSWATCH: villagers in g_posEnd (end of the last tick), -1 = none
// AchievementManager::Update is the first call after GameLoop::Update's step loop (runs even with 0 steps).
using AchievementUpdate_t = void(__fastcall*)(void* self, const int64_t* dt);
static AchievementUpdate_t o_AchievementUpdate;
static void __fastcall h_AchievementUpdate(void* self, const int64_t* dt) {
    towns::Switch(towns::Local());  // steps are over: UI/tools of this frame see the local player's town
    towns::OnFrame();
    gamewin::LayoutFrame();
    uiscript::OnFrame();  // UI test scripts: after the steps, before this frame's UI update
    UndoSpeedCap();
    o_AchievementUpdate(self, dt);
}
using GameLoopUpdate_t = uint64_t(__fastcall*)(void* self, const int64_t* dt);
static GameLoopUpdate_t o_GameLoopUpdate;
static uint64_t __fastcall h_GameLoopUpdate(void* self, const int64_t* dt) {
    if (self != g_s.loop) {
        Log("game session start: GameLoop=%p (prev %p, prev ticks %llu)", self, g_s.loop, g_s.tick);
        g_sessionCount++;
        towns::OnSessionStart();
        g_s = Session{};
        g_pendingFreeLogs.clear();
        g_s.loop = self;
        g_posEndN = -1;  // POSWATCH: positions of the previous session don't compare
        if (uint8_t* terrain = *(uint8_t**)((uint8_t*)self + 0x110)) {
            uintptr_t* vt = *(uintptr_t**)terrain;
            Log("terrain object %p vtable exe+%llx slot0 exe+%llx slot1 exe+%llx slot2 exe+%llx", terrain,
                (unsigned long long)((uintptr_t)vt - g_exeBase), (unsigned long long)(vt[0] - g_exeBase),
                (unsigned long long)(vt[1] - g_exeBase), (unsigned long long)(vt[2] - g_exeBase));
        }
        if (g_ticks) fprintf(g_ticks, "# session %p\n", self);
        if (g_draws) fprintf(g_draws, "# session %p\n", self);
        // The handle-tag RNG may have been time-seeded before our hooks existed: reseed at a point
        // that is identical on every client. (MT layout: uint32 index, uint32 state[624].)
        if (void* rng = FactoryRng()) {
            o_RandInit(rng, 0xB4A15ED);
            *(uint32_t*)rng = 0;
            Log("reseeded InternalFactory RNG %p", rng);
        }
        // Pathfinder (*(GameLoop+0xf8)+0x68) is a System::Thread solving path requests from a queue; MoveTo polls
        // the result slots each tick, so walk start times depend on thread timing. Its byte +0x5c switches every
        // request to synchronous solving on the caller's thread: drain the queue, then turn that on.
        {
            uint8_t* gl = (uint8_t*)self;
            uint8_t* world = *(uint8_t**)(gl + 0xf8);
            uint8_t* pf = world ? *(uint8_t**)(world + 0x68) : nullptr;
            if (pf) {
                ((void(__fastcall*)(void*))(g_exeBase + Rva(kRvaPathFlush)))(pf);
                pf[0x5c] = 1;
                Log("pathfinder %p switched to synchronous solving", pf);
            } else {
                Log("!! pathfinder not found (world=%p)", world);
            }
        }
        // The world's sim RNG: log its state (does the save restore it?) and reseed deterministically.
        uint8_t* world = *(uint8_t**)(g_exeBase + Rva(kRvaWorldSingleton));
        uint8_t* rng = world ? *(uint8_t**)(world + 0xd8) : nullptr;
        if (rng) {
            uint64_t h = kFnvBasis;
            for (int i = 0; i < 625; i++) h = Fnv(h, ((uint32_t*)rng)[i]);
            Log("world sim RNG %p before reseed: index=%u statehash=%016llx", rng, *(uint32_t*)rng, h);
            o_RandInit(rng, kSessionSeed);
            *(uint32_t*)rng = 0;
        } else {
            Log("!! world sim RNG not found (world=%p)", world);
        }
    }
    if (g_watch) {
        uint8_t* speedCtl = *(uint8_t**)((uint8_t*)self + 0x120);
        g_stepsExpected = (speedCtl && !speedCtl[0x30]) ? *(int*)(speedCtl + 0x34) : 0;
        g_stepsDone = 0;
    }
    if (commands::Pending()) {
        uint8_t* speedCtl = *(uint8_t**)((uint8_t*)self + 0x120);
        void* ui = *(void**)((uint8_t*)self + 0x98);
        bool noSteps = !speedCtl || speedCtl[0x30] || *(int*)(speedCtl + 0x34) <= 0 || (ui && p_PausesGame && p_PausesGame(ui));
        if (noSteps) {
            // no simulation time passes while paused, so applying the agreed command order right now is
            // deterministic; run them in sim scope (sim heap, sim RNG) like a tick would
            bool saved = g_s.inTick;
            g_s.inTick = true;
            commands::ExecuteAllNow(g_s.tick);
            g_s.inTick = saved;
        }
    }
    // Lockstep gate: run at most as many sim ticks as every player has promised commands for (the step count
    // is read from speedCtl+0x34 at the start of GameLoop::Update; the player's chosen speed is restored after).
    uint8_t* speedCtl = *(uint8_t**)((uint8_t*)self + 0x120);
    net::SyncSpeed(speedCtl);
    int desiredSpeed = speedCtl ? *(int*)(speedCtl + 0x34) : 0;
    g_capSpeedCtl = nullptr;
    if (net::Active() && speedCtl) {
        void* ui = *(void**)((uint8_t*)self + 0x98);
        net::DebugStatus(g_s.tick, ui && p_PausesGame && p_PausesGame(ui), desiredSpeed);
        int64_t allowed = net::TicksAllowed(g_s.tick);
        bool uiPausing = ui && p_PausesGame && p_PausesGame(ui);  // no steps run then anyway
        if (allowed < desiredSpeed && !uiPausing) {
            g_capValue = allowed > 0 ? (int)allowed : 0;
            g_capDesired = desiredSpeed;
            g_capSpeedCtl = speedCtl;
            *(int*)(speedCtl + 0x34) = g_capValue;
        }
    }
    g_inGameLoopUpdate = true;
    uint64_t r = o_GameLoopUpdate(self, dt);
    g_inGameLoopUpdate = false;
    UndoSpeedCap();  // normally already done right after the steps (AchievementManager::Update hook)
    net::AfterFrame(g_s.tick);
    FlushTick();
    if (g_watch) simwatch::Protect();  // close the last tick of this frame before UI/render RNG of the next frame
    return r;
}

// EntityManager::AddEntity(manager, InternalBasePtr* entity, int bucketClass). Buckets are time-sliced
// round-robin update lists (budgets 50/15/8/5/2 per tick) with swap-remove, so any transient non-sim entity
// (placement-preview ghosts made by tools outside ticks) permanently shifts which sim entity updates when.
// Class 6 = "no bucket": give it to entities the main thread adds outside a sim tick.
using AddEntity_t = void(__fastcall*)(void* mgr, void* entity, int bucketClass);
static AddEntity_t o_AddEntity;
static uint64_t g_demotedEntities;
static int g_debugAddEntity = -1;  // log this many AddEntity calls with deep stacks (bmp_debug_addentity.txt)
static void __fastcall h_AddEntity(void* mgr, void* entity, int bucketClass) {
    bool main = GetCurrentThreadId() == g_mainThread;
    if (g_debugAddEntity > 0) {
        g_debugAddEntity--;
        void* fr[32];
        USHORT n = CaptureStackBackTrace(1, 32, fr, nullptr);
        char buf[600];
        int k = sprintf_s(buf, "addentity e=%p class=%d thread=%lu loop=%p:", entity, bucketClass, GetCurrentThreadId(), g_s.loop);
        for (USHORT i = 0; i < n && k < 560; i++) {
            uintptr_t a = (uintptr_t)fr[i];
            if (a >= g_exeBase && a < g_exeEnd) k += sprintf_s(buf + k, sizeof buf - k, " e%llx", (unsigned long long)(a - g_exeBase));
            else if (a >= g_rtBase && a < g_rtEnd) k += sprintf_s(buf + k, sizeof buf - k, " r%llx", (unsigned long long)(a - g_rtBase));
        }
        Log("%s", buf);
    }
    if (bucketClass != 6 && !InSim() && g_s.loop && main) g_demotedEntities++;  // count only (player commands)
    if (g_draws && g_s.loop && main) {
        char stack[96];
        StackStr(stack, sizeof stack);
        fprintf(g_draws, "%llu %c %08x %08x |%s\n", g_s.tick, InSim() ? 'E' : 'e', bucketClass,
                *(int*)((uint8_t*)mgr + 0x10), stack);
    }
    o_AddEntity(mgr, entity, bucketClass);
    towns::OnEntityAdded(*(void**)entity);
}

// EntityManager::RemoveEntity(mgr, InternalBasePtr* entity) - swap-removes from its bucket.
using RemoveEntity_t = void(__fastcall*)(void* mgr, void** entity);
static RemoveEntity_t o_RemoveEntity;
static void __fastcall h_RemoveEntity(void* mgr, void** entity) {
    if (g_draws && g_s.loop && GetCurrentThreadId() == g_mainThread) {
        uint8_t* e = (uint8_t*)*entity;
        char stack[96];
        StackStr(stack, sizeof stack);
        fprintf(g_draws, "%llu %c %08x %08x |%s\n", g_s.tick, InSim() ? 'R' : 'r', e ? (int)(int8_t)e[0x1f] : -1,
                e ? (*(uint32_t*)(e + 0x1c) & 0xffffff) : 0, stack);
    }
    towns::Scope town(towns::OwnerOf(*entity));  // components unregister from their own town
    o_RemoveEntity(mgr, entity);
}

// EntityManager's per-frame set (+0x78, updated once per rendered frame by exe+0xc98a0) is visual; its
// pending inserts are processed by exe+0xc8f80, also from inside the sim tick. Membership follows what is
// on screen, so its tree-node allocations must not land in the sim heap.
// Debug (bmp_poswatch.txt): villager x/y must only change in simulation code. Snapshot them around every visual
// section inside a tick and between ticks; log the first sections that moved someone (frame-dependent movement).
static bool g_posWatch;
static int g_posHits;
static float g_posEnd[4096];
static void PosCompare(const float* before, int nb, const char* where, const void* ret) {
    if (g_posHits >= 40) return;
    float now[4096];
    int n = towns::CopyPositions(now, 2048);
    if (n != nb) return;
    for (int i = 0; i < n * 2; i++) {
        if (memcmp(&now[i], &before[i], 4) == 0) continue;
        char who[96];
        Describe(ret, who, sizeof who);
        Log("POSWATCH tick %llu: villager #%d %s %.6f -> %.6f in %s (caller %s)", g_s.tick, i / 2, i % 2 ? "y" : "x",
            before[i], now[i], where, who);
        g_posHits++;
        return;
    }
}
struct PosGuard {
    float before[4096];
    int n = -1;
    const char* where;
    const void* ret;
    PosGuard(const char* w, const void* r) : where(w), ret(r) {
        if (g_posWatch && g_s.inTick && g_posHits < 40) n = towns::CopyPositions(before, 2048);
    }
    ~PosGuard() { if (n >= 0) PosCompare(before, n, where, ret); }
};
using PerFrameSetSync_t = void(__fastcall*)(void* mgr);
static PerFrameSetSync_t o_PerFrameSetSync;
static void __fastcall h_PerFrameSetSync(void* mgr) {
    PosGuard pg("PerFrameSetSync", _ReturnAddress());
    t_visual++;
    o_PerFrameSetSync(mgr);
    t_visual--;
}

// ComponentEntity flag recompute (entity): OR of its components' update flags; a change of bit 4 (per-tick set)
// queues the entity on manager+0x88 (simulation), a change of bit 0x10 (per-frame set) on manager+0x98. Bit 0x10
// is cleared by the per-frame (visual) update - when depends on the frame rate - and set again by e.g.
// GrowthComponent inside the tick, so a change of only that bit is visual bookkeeping (its list growth allocated
// in the sim heap at frame-rate-dependent ticks).
using EntityFlagsChanged_t = void(__fastcall*)(void* entity);
static EntityFlagsChanged_t o_EntityFlagsChanged;
static void __fastcall h_EntityFlagsChanged(void* entity) {
    uint8_t* e = (uint8_t*)entity;
    uint32_t before = *(uint32_t*)(e + 0x40), after = 0;
    int n = *(int*)(e + 0x38);
    for (int i = 0; i < n; i++) after |= *(uint16_t*)(*(uint8_t**)(*(uint8_t**)(e + 0x30) + i * 8) + 0x20);
    if (((before ^ after) & 4) == 0) {
        PosGuard pg("EntityFlagsChanged(visual)", _ReturnAddress());
        t_visual++;
        o_EntityFlagsChanged(entity);
        t_visual--;
    } else {
        o_EntityFlagsChanged(entity);
    }
}

// Engine::Loader::Run runs on the loader thread: LoadResources + the next loop's Load (GameLoop builds the world
// from the save here). Other background threads (the pathfinder worker!) also allocate during loading, and their
// interleaving is timing-dependent, so only this thread may feed the sim heap before the session starts.
using LoaderRun_t = void(__fastcall*)(void* self);
static LoaderRun_t o_LoaderRun;
static void __fastcall h_LoaderRun(void* self) {
    t_loading++;
    o_LoaderRun(self);
    t_loading--;
}

using ExternalLoad_t = void*(__fastcall*)(const void* filename, void* existing);
static ExternalLoad_t o_ExternalLoad;
static void* __fastcall h_ExternalLoad(const void* filename, void* existing) {
    t_resource++;
    void* r = o_ExternalLoad(filename, existing);
    t_resource--;
    return r;
}
using LoadResources_t = void(__fastcall*)(void* loop);
static LoadResources_t o_LoadResources;
static void __fastcall h_LoadResources(void* loop) {
    t_resource++;
    o_LoadResources(loop);
    t_resource--;
}

// LoaderLoop::Startup (main thread) kicks off every load - new game, save, return to menu. End the current
// session here: a fresh sim-heap generation and fresh session state make the next game independent of anything
// this process did before (multiplayer no longer needs a freshly started Banished; resync = everyone reloads).
// Map walk-connectivity labels (cell +0x26) are rebuilt lazily by exe+0xa1950 when map+0xa2 is set, by whoever asks
// first (exe+0xa55c0 "can walk from A to B" calls it). A rebuild from outside the simulation (UI/tools/rendering on
// one PC only) would take in map changes earlier than the other PCs do, if some change doesn't set the flag - so
// only the simulation rebuilds; outside it the flag stays set and the old labels are used meanwhile.
using MapConnect_t = void(__fastcall*)(uint8_t* map);
static MapConnect_t o_MapConnect;
static std::unordered_map<uint32_t, uint32_t> g_connectSkips;  // caller -> count (outside-sim rebuild requests)
static void __fastcall h_MapConnect(uint8_t* map) {
    // (skipping such a rebuild crashed the game: after a load other code reads the labels assuming the first query
    // rebuilt them - now only logged)
    if (map[0xa2] && !InSim() && g_s.loop && g_connectSkips[CodeId(_ReturnAddress())]++ == 0)
        Log("map connectivity: rebuilt outside the simulation, from %08x", CodeId(_ReturnAddress()));
    o_MapConnect(map);
}

// A building's production statistics (TrackerComponent: +0x28/+0x30/+0x34 this year's {good, count} array, +0x38/
// +0x40/+0x44 last year's, +0x48 the year they belong to) roll over lazily: whoever touches them first in a new year
// copies this year's list to last year's (MemAlloc) and frees the old one. The simulation does it when it records
// production (exe+0x59930), the window's production panel (ProductionUI, exe+0x7af20) when it shows them - so an open
// window did it outside the simulation on one PC only: different allocations from then on, the sim heaps drifted apart
// (the "open windows" leak; nettest uichaos.txt on Friends.sav drifted at tick 1200). Now the window shows the rolled-
// over view (this year empty, last year = the current list) for that frame without changing anything; the real
// rollover stays with the simulation.
using ProductionRows_t = void(__fastcall*)(uint8_t* ctl, int which);
static ProductionRows_t o_ProductionRows;
static uint8_t* TrackerOf(uint8_t* entity) {
    static const uintptr_t kTiTracker = 0x1ccb10;  // TrackerComponent::typeinfo
    if (!entity || !*(uint8_t***)(entity + 0x30)) return nullptr;
    int want = *(int*)(g_exeBase + Rva(kTiTracker));
    for (int i = 0; i < *(int*)(entity + 0x38); i++) {
        uint8_t* c = (*(uint8_t***)(entity + 0x30))[i];
        if (c && ((int(__fastcall*)(void*))(*(void***)c)[2])(c) == want) return c;
    }
    return nullptr;
}
static void __fastcall h_ProductionRows(uint8_t* ctl, int which) {
    uint8_t* gl = *(uint8_t**)(g_exeBase + Rva(kRvaWorldSingleton));
    uint8_t* stats = gl && *(int**)(gl + 0x128) ? TrackerOf(*(uint8_t**)(ctl + 0x28)) : nullptr;
    int year = stats ? **(int**)(gl + 0x128) : 0;
    if (!stats || *(int*)(stats + 0x48) == year) return o_ProductionRows(ctl, which);
    static int logged;
    if (logged++ < 3) Log("production window: statistics of a new year shown without rolling them over (sim does that)");
    uint8_t saved[0x24];
    memcpy(saved, stats + 0x28, sizeof saved);
    *(uint64_t*)(stats + 0x38) = *(uint64_t*)(stats + 0x28);  // last year := this year's list (count + capacity too)
    *(uint64_t*)(stats + 0x40) = *(uint64_t*)(stats + 0x30);
    *(uint64_t*)(stats + 0x28) = 0;                            // this year: empty
    *(uint64_t*)(stats + 0x30) = 0;
    *(int*)(stats + 0x48) = year;
    o_ProductionRows(ctl, which);
    memcpy(stats + 0x28, saved, sizeof saved);
}

using LoaderStartup_t = void(__fastcall*)(void* self);
static LoaderStartup_t o_LoaderStartup;
// The companion data mod (WinData\bmpShare.pkm: the "Share" checkbox in storage windows) is switched on whenever a
// load starts. IO::Modification: +0x00 dirty flag, +0x08 array of 0x80-byte mods (+0x50 enabled, +0x58 file name
// String), +0x10 count.
static int g_modListed = -1;  // -1 = not looked yet, 0 = not installed (or switched off by the test), 1 = installed
static void EnsureCompanionMod() {
    static void* (*getMods)() = (void* (*)())RuntimeExport("?Get@Modification@IO@@SAPEAV12@XZ");
    static void(__fastcall * enable)(void*, int, bool) =
        (void(__fastcall*)(void*, int, bool))RuntimeExport("?Enable@Modification@IO@@QEAAXH_N@Z");
    uint8_t* mods = getMods ? (uint8_t*)getMods() : nullptr;
    if (!mods || !enable) return;
    // test: <game>\bmp_nomod<suffix>.txt (e.g. bmp_nomod_p1.txt) = behave like a player without the mod (switched off)
    static int noMod = -1;
    if (noMod < 0) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        swprintf_s(wcsrchr(path, L'\\') + 1, 64, L"bmp_nomod%s.txt", SlotSuffix());
        noMod = GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
    }
    int n = *(int*)(mods + 0x10);
    for (int i = 0; i < n; i++) {
        uint8_t* m = *(uint8_t**)(mods + 8) + (int64_t)i * 0x80;
        const char* name = *(const char**)(m + 0x58);
        bool on = m[0x50] != 0;
        if (name && strstr(name, "bmpShare")) {
            if (noMod) {
                if (on) enable(mods, i, false);
                Log("companion mod %s: switched OFF (test: bmp_nomod)", name);
                g_modListed = 0;
                return;
            }
            if (!on) {
                enable(mods, i, true);
                // remember it: the main menu (its Multiplayer button comes from the mod) is built from the mods that
                // were on when the game started - a save without the mod switches it off again, so store it each time
                static void(__fastcall * saveList)(void*) = (void(__fastcall*)(void*))RuntimeExport("?SaveList@Modification@IO@@QEAAXXZ");
                if (saveList) saveList(mods);
            }
            Log("companion mod %s: %s", name, on ? "enabled" : "now enabled");
            g_modListed = 1;
            return;
        }
    }
    Log("companion mod bmpShare.pkm not installed (%d mods) - storage windows use the overlay share panel", n);
    g_modListed = 0;
}

// The mod's marker BMP/Version_<hash>.rsc (mod/make_mod_sources.ps1) names the build of the mod; this DLL was built
// against BMP_MOD_VERSION. Looked up once (a found marker stays found; a missing one is looked for again at most
// every 5 s, in case the mod's files were not mounted yet).
static volatile int g_modKnown = kModOk;
int CompanionModKnown() { return g_modListed == 0 ? kModMissing : g_modKnown; }  // "missing" is known from the start
int CompanionMod() {
    static int state = -1;
    static DWORD lastTry;
    if (g_modListed < 0) return g_modKnown = kModOk;
    if (g_modListed == 0) return g_modKnown = kModMissing;
    if (state == kModOk) return g_modKnown = state;
    if (state < 0 || GetTickCount() - lastTry > 5000) {
        lastTry = GetTickCount();
        bool found = commands::DescFromPath("BMP/Version_" BMP_MOD_VERSION ".rsc:bmpVersion") != nullptr;
        int now = found ? kModOk : kModOther;
        if (now != state) Log("companion mod: %s (this DLL expects mod build " BMP_MOD_VERSION ")",
                              found ? "the expected build" : "ANOTHER build - BMP/Version_" BMP_MOD_VERSION ".rsc not found");
        state = now;
    }
    return g_modKnown = state;
}

static void __fastcall h_LoaderStartup(void* self) {
    EnsureCompanionMod();
    if (g_s.loop) Log("session end: GameLoop=%p after %llu ticks", g_s.loop, g_s.tick);
    FlushTick();
    g_s = Session{};
    g_pendingFreeLogs.clear();
    simheap::NewGeneration();
    { std::lock_guard<std::mutex> lock(g_ctorMutex); g_ctorCounts.clear(); }
    commands::OnNewSession();
    Log("loader startup: new sim-heap generation %llu", simheap::GetStats().generation);
    towns::OnLoadStart();
    o_LoaderStartup(self);
}

// Generic tree erase (set, node). RemoveEntity also erases from the per-frame set inside the tick; whether the
// entity was in it depends on the camera, so that erase counts as visual too.
using TreeErase_t = void*(__fastcall*)(void* set, void* node);
static TreeErase_t o_TreeErase;
static void* __fastcall h_TreeErase(void* set, void* node) {
    uint8_t* loop = *(uint8_t**)(g_exeBase + Rva(kRvaWorldSingleton));
    uint8_t* em = loop ? *(uint8_t**)(loop + 0xe8) : nullptr;
    if (!em || set != em + 0x78) return o_TreeErase(set, node);
    PosGuard pg("TreeErase(per-frame set)", _ReturnAddress());
    t_visual++;
    void* r = o_TreeErase(set, node);
    t_visual--;
    return r;
}

// Probe: every terrain-height query the simulation makes (exe+0xa48b0: terrain object, or the map's picking scene for
// cells with floors) is hashed per tick - cross-instance differences show frame-dependent height sources.
using TerrainPoint_t = float*(__fastcall*)(void* map, float* out, float* in, char noPick, void* normal);
static TerrainPoint_t o_TerrainPoint;
static float* __fastcall h_TerrainPoint(void* map, float* out, float* in, char noPick, void* normal) {
    float* r = o_TerrainPoint(map, out, in, noPick, normal);
    if (InSim()) {
        for (int i = 0; i < 12; i++) g_s.heightHash = (g_s.heightHash ^ ((uint8_t*)out)[i]) * 16777619u;
        g_s.heightCalls++;
        g_s.heightCallers[CodeId(_ReturnAddress())]++;
    }
    return r;
}
using PickIntersect_t = int(__fastcall*)(const void* scene, const float* origin, const float* dir, const void* maxDist,
                                         float* outPos, float* outNormal);
static PickIntersect_t o_PickIntersect;
static int __fastcall h_PickIntersect(const void* scene, const float* origin, const float* dir, const void* maxDist,
                                      float* outPos, float* outNormal) {
    int r = o_PickIntersect(scene, origin, dir, maxDist, outPos, outNormal);
    if (InSim()) {
        g_s.pickHash = (g_s.pickHash ^ (uint32_t)r) * 16777619u;
        for (int i = 0; i < 12; i++) g_s.pickHash = (g_s.pickHash ^ ((uint8_t*)outPos)[i]) * 16777619u;
        g_s.pickCalls++;
    }
    return r;
}

// Probe: MeshInstance::RootAnimationOffset (character movement per update = root motion x speed) in the sim. Branch:
// mesh+0xd8 == 0 -> the render-computed offset mesh+0x100, else the last queued clip's root velocity.
using RootOffset_t = float*(__fastcall*)(const uint8_t* mesh, float* out);
static RootOffset_t o_RootOffset;
static float* __fastcall h_RootOffset(const uint8_t* mesh, float* out) {
    float* r = o_RootOffset(mesh, out);
    if (InSim()) {
        for (int i = 0; i < 12; i++) g_s.rootHash = (g_s.rootHash ^ ((uint8_t*)out)[i]) * 16777619u;
        g_s.rootCalls++;
        if (mesh[0xd8] == 0) g_s.rootRenderBranch++;
    }
    return r;
}

// THE cross-PC desync (2026-10-12): GameLoop::Update's step loop sets MeshInstance::_rootOnlyUpdates = (step < steps-1)
// and calls AnimationManager::Update(dt, rootOnly) - only the LAST sim step of each rendered frame is a full update.
// CharacterComponent::vf8 snaps characters to the ground (exe+0xa48b0) and AnimationManager advances poses/blends only
// on full updates, so which ticks get them depended on frame rate, game speed and the lockstep gate -> villagers'
// height and walk speed changed at different ticks on different PCs -> walks/work ending a tick apart. Every sim step
// is a full update now (a little more CPU at high speeds).
using AnimMgrUpdate_t = void(__fastcall*)(void* mgr, const void* dt, bool rootOnly);
static AnimMgrUpdate_t o_AnimMgrUpdate;
static void (*p_EnableRootOnly)(bool);
static void __fastcall h_AnimMgrUpdate(void* mgr, const void* dt, bool rootOnly) {
    if (rootOnly && g_inGameLoopUpdate) {
        rootOnly = false;
        if (p_EnableRootOnly) p_EnableRootOnly(false);
    }
    o_AnimMgrUpdate(mgr, dt, rootOnly);
}

using MeshInstanceUpdate_t = void(__fastcall*)(void* self, const int64_t* dt);
static MeshInstanceUpdate_t o_MeshInstanceUpdate;
static void __fastcall h_MeshInstanceUpdate(void* self, const int64_t* dt) {
    PosGuard pg("MeshInstance::Update", _ReturnAddress());
    t_visual++;
    o_MeshInstanceUpdate(self, dt);
    t_visual--;
}

// Last call of each sim step (single caller: GameLoop::Update's step loop). After it returns the step is
// over; anything until the next World::Update is UI/tools/render.
using StepEnd_t = void(__fastcall*)(void* self, const int64_t* dt);
static StepEnd_t o_StepEnd;
static void __fastcall h_StepEnd(void* self, const int64_t* dt) {
    towns::StepTowns(self, dt, o_StepEnd);  // the Town step, once per town in its own context
    // Terrain heights are frame-dependent otherwise: exe+0xa48b0 (every height the sim reads - walking villagers'
    // z, slopes) ray-tests the terrain object GameLoop+0x110 (exe+0xb6460 -> afef0), whose dirty patches are rebuilt
    // by its slot 1 (exe+0xb4210 = jmp exe+0xb0220) once per RENDERED frame. A terrain change (a building flattening
    // the ground, a demolition) therefore reached the sim at different ticks on different PCs -> villager x/y drift
    // -> walks/work ending a tick apart (the cross-PC desyncs). Rebuild at the end of every tick: visual scope (its
    // allocations and vertex-buffer upload are not simulation state), main thread.
    if (uint8_t* gl = (uint8_t*)g_s.loop) {
        if (uint8_t* terrain = *(uint8_t**)(gl + 0x110)) {
            if (*(int*)(terrain + 0xf0) != 0) {  // dirty patches pending
                t_visual++;
                ((void(__fastcall*)(void*))(g_exeBase + Rva(0xb0220)))(terrain);
                t_visual--;
            }
        }
    }
    FlushTick();
    if (g_posWatch) g_posEndN = towns::CopyPositions(g_posEnd, 2048);
    if (g_watch && ++g_stepsDone >= g_stepsExpected) simwatch::Protect();  // UI/tools run after the last step
    autotest::OnSimTick(g_s.tick);
}

// Engine main loop: drives the autotest harness once per frame on the main thread.
using MainLoopUpdate_t = bool(__fastcall*)(void* self, const int64_t* time);
static MainLoopUpdate_t o_MainLoopUpdate;
// Steam's UserStatsReceived can arrive while the manager's achievement list (+0x70) is not loaded - e.g. right after
// EnsureCompanionMod switched the mod on at startup (resources reload) - and the game dereferences it (crash seen on
// the second instance of a nettest). Hold the callback until the list exists.
using StatsReceived_t = void(__fastcall*)(void* self, void* cb);
static StatsReceived_t o_StatsReceived;
static void* g_statsMgr;
static uint8_t g_statsHeld[24];  // UserStatsReceived_t {u64 game id; int result; u64 steam id}
static bool g_statsPending;
static void __fastcall h_StatsReceived(void* self, void* cb) {
    if (*(void**)((uint8_t*)self + 0x70)) return o_StatsReceived(self, cb);
    g_statsMgr = self;
    memcpy(g_statsHeld, cb, sizeof g_statsHeld);
    if (!g_statsPending) Log("steam stats arrived before the achievement list was loaded - held");
    g_statsPending = true;
}
static void RetryHeldStats() {
    if (!g_statsPending || !*(void**)((uint8_t*)g_statsMgr + 0x70)) return;
    g_statsPending = false;
    Log("steam stats: delivering the held callback");
    o_StatsReceived(g_statsMgr, g_statsHeld);
}

static bool __fastcall h_MainLoopUpdate(void* self, const int64_t* time) {
    steam::SetReady();  // the game has started Steam by now: the mod may talk to it (main thread only)
    RetryHeldStats();
    gameload::Frame();
    net::Frame();
    autotest::Frame();
    bool r = o_MainLoopUpdate(self, time);
    overlay::GameFrame();  // end of the frame (where Present used to drive it): bar, settle, trade, offer windows
    return r;
}

static void ActionSink(char kind, int type, uint64_t actionId, uint64_t result) {
    if (g_draws && InSim()) fprintf(g_draws, "%llu %c %08x %08llx | %llx\n", g_s.tick, kind, type, result, actionId);
    // desync record: tag A/Z + action type, value = action (sim-heap offset), a = result
    if (InSim() && GetCurrentThreadId() == g_mainThread) RingAdd((kind == 'a' ? 0x41000000u : 0x5a000000u) | (uint32_t)type, actionId, (int32_t)result, 0);
}

void* ProbeGameLoop() { return g_s.loop; }
uint64_t ProbeSessionCount() { return g_sessionCount; }
bool ProbeInSim() { return InSim(); }
bool ProbeUiPaused() {
    uint8_t* loop = (uint8_t*)g_s.loop;
    void* ui = loop ? *(void**)(loop + 0x98) : nullptr;
    return ui && p_PausesGame && p_PausesGame(ui);
}
bool ProbeIsMainThread() { return GetCurrentThreadId() == g_mainThread; }
void ProbeNonSimScope(bool enter) { t_visual += enter ? 1 : -1; }
uint64_t ProbeTick() { return g_s.tick; }
void ProbeFlush() {
    if (g_watch) simwatch::Report();
    if (g_ticks) fflush(g_ticks);
    if (g_draws) fflush(g_draws);
    ReportHeap();
}

using WorldUpdate_t = void(__fastcall*)(void* world, const int64_t* dt);
static WorldUpdate_t o_WorldUpdate;
static void __fastcall h_WorldUpdate(void* world, const int64_t* dt) {
    if (!g_inGameLoopUpdate) return o_WorldUpdate(world, dt);  // menu scene: not part of the simulation
    towns::Switch(0);  // world-level sim code runs in town 0's context; entities switch to their own
    if (g_watch) simwatch::Unprotect();
    FlushTick();
    if (g_posWatch && g_posEndN >= 0 && g_s.loop) PosCompare(g_posEnd, g_posEndN, "between ticks (UI/render/tools)", nullptr);
    g_s.inTick = true;
    g_s.tick++;
    g_s.tickCalls = 0;
    g_s.tickHash = kFnvBasis;
    g_s.simCalls = 0;
    g_s.simHash = kFnvBasis;
    commands::OnTickStart(g_s.tick);
    if (g_heapNudgeAt && g_s.tick == g_heapNudgeAt) {
        void* p = simheap::Alloc(64, 8);  // kept forever: everything allocated later lands 0x50 further on
        if (p) {
            FoldFrees();
            g_s.heapChain = Fnv(Fnv(g_s.heapChain, 64), simheap::Offset(p));
            RingAdd(0x4d000040u, simheap::Offset(p), 0x0bad0bad, 0);
        }
        Log("test: sim-heap nudge done at tick %llu", g_s.tick);
    }
    if (g_s.tick % 600 == 0) {
        Log("tick %llu: chain=%016llx outside-tick draws=%llu off-thread draws=%llu sim-rng draws redirected to UI rng=%llu",
            g_s.tick, g_s.chain, g_s.outsideCalls, g_s.offThreadCalls, g_redirected);
        Log("   entities added outside ticks (demoted to no-bucket): %llu", g_demotedEntities);
        std::vector<std::pair<uint32_t, uint64_t>> top(g_s.outsideCallers.begin(), g_s.outsideCallers.end());
        std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second > b.second; });
        for (size_t i = 0; i < top.size() && i < 10; i++)
            Log("   outside-tick caller %s+%#x: %llu", (top[i].first >> 28) == 1 ? "exe" : (top[i].first >> 28) == 2 ? "rt" : "?",
                top[i].first & 0xfffffff, top[i].second);
        ReportHeap();
        if (g_watch && g_s.tick % 3000 == 0) simwatch::Report();
        if (g_ticks) fflush(g_ticks);
        if (g_draws) fflush(g_draws);
    }
    o_WorldUpdate(world, dt);
}

// ---------- install ----------
static void HookAt(const char* label, void* target, void* detour, void** orig) {
    if (!target) return;
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s == MH_OK) s = MH_EnableHook(target);
    Log("hook %s -> %s", label, MH_StatusToString(s));
}
static void Hook(const char* mangled, void* detour, void** orig) { HookAt(mangled, RuntimeExport(mangled), detour, orig); }

static bool GameFileExists(const wchar_t* name) {
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    wcscpy_s(wcsrchr(p, L'\\') + 1, 64, name);
    return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES;
}
static std::string ReadGameFile(const wchar_t* name) {  // first line of a small switch file in the game folder
    wchar_t p[MAX_PATH];
    GetModuleFileNameW(nullptr, p, MAX_PATH);
    wcscpy_s(wcsrchr(p, L'\\') + 1, 64, name);
    FILE* f = _wfsopen(p, L"r", _SH_DENYNO);
    if (!f) return "";
    char buf[128] = "";
    fgets(buf, sizeof buf, f);
    fclose(f);
    return buf;
}

// Probe logs pile up in the game folder (one set per launch): keep the newest kKeepLogs of each kind and nothing older
// than 7 days. Files another running instance still has open can't be deleted and are skipped.
static void CleanOldLogs(const wchar_t* anyPathInGameDir) {
    const int kKeepLogs = 10;
    std::wstring dir(anyPathInGameDir, wcsrchr(anyPathInGameDir, L'\\') + 1);
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const uint64_t week = 7ull * 24 * 3600 * 10000000;
    uint64_t nowT = ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
    int removed = 0;
    for (const wchar_t* kind : {L"bmp_ticks_*.log", L"bmp_draws_*.log", L"bmp_pos___*.log", L"bmp_desync_*.log"}) {
        std::vector<std::pair<uint64_t, std::wstring>> files;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + kind).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do files.push_back({((uint64_t)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime, fd.cFileName});
        while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.first > b.first; });  // newest first
        for (size_t i = 0; i < files.size(); i++)
            if ((i >= kKeepLogs || nowT - files[i].first > week) && DeleteFileW((dir + files[i].second).c_str())) removed++;
    }
    if (removed) Log("removed %d old probe logs from the game folder", removed);
}

// Crash report: the first access violation (any thread) is logged with an unwound stack as module offsets, then the
// game's own handling continues (its crash.dmp is empty).
static uintptr_t g_selfBase, g_selfEnd;
static void CrashFrame(char* buf, size_t n, uintptr_t a) {
    if (a >= g_exeBase && a < g_exeEnd) snprintf(buf, n, " exe+%llx", (unsigned long long)(a - g_exeBase));
    else if (a >= g_rtBase && a < g_rtEnd) snprintf(buf, n, " rt+%llx", (unsigned long long)(a - g_rtBase));
    else if (a >= g_selfBase && a < g_selfEnd) snprintf(buf, n, " dll+%llx", (unsigned long long)(a - g_selfBase));
    else snprintf(buf, n, " %llx", (unsigned long long)a);
}
// ---- debug write watch (<game>\bmp_watch.txt "TOWN INDEX OFFSET FROMTICK TOTICK"): a hardware write breakpoint on one
// villager's CitizenComponent field; every write is logged with the tick, the new value and the code that wrote it
static void SetWatch(uint8_t* addr) {
    struct Arg { DWORD tid; uint8_t* addr; } arg{g_mainThread, addr};
    HANDLE h = CreateThread(nullptr, 0, [](void* p) -> DWORD {
        auto a = (Arg*)p;
        HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE, a->tid);
        if (!t) return 0;
        SuspendThread(t);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        GetThreadContext(t, &c);
        c.Dr0 = (DWORD64)a->addr;
        c.Dr7 = a->addr ? (1ull | (1ull << 16) | (3ull << 18)) : 0;  // L0, break on write, 4 bytes
        SetThreadContext(t, &c);
        ResumeThread(t);
        CloseHandle(t);
        return 0;
    }, &arg, 0, nullptr);
    if (h) { WaitForSingleObject(h, 2000); CloseHandle(h); }
}
static LONG CALLBACK WatchHandler(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP || !(ep->ContextRecord->Dr6 & 1)) return EXCEPTION_CONTINUE_SEARCH;
    ep->ContextRecord->Dr6 = 0;
    char line[512];
    int len = snprintf(line, sizeof line, "watch: tick %llu%s town %d villager %d +%#x = %08x (%g) by", g_s.tick,
                       g_s.inTick ? "" : " (outside a tick)", g_watchTown, g_watchIndex, g_watchOff, *(uint32_t*)g_watchAddr,
                       *(float*)g_watchAddr);
    CONTEXT ctx = *ep->ContextRecord;
    for (int depth = 0; depth < 8 && ctx.Rip && len < (int)sizeof line - 40; depth++) {
        CrashFrame(line + len, sizeof line - len, ctx.Rip);
        len = (int)strlen(line);
        DWORD64 imageBase;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fn) { ctx.Rip = *(DWORD64*)ctx.Rsp; ctx.Rsp += 8; continue; }
        void* hd; DWORD64 est;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &hd, &est, nullptr);
    }
    Log("%s", line);
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void WatchTick() {
    if (g_s.tick == g_watchFrom && !g_watchAddr) {
        uint8_t* gl = (uint8_t*)g_s.loop;
        if (gl) Log("watch: GameLoop+0xd0 flags %08x", *(uint32_t*)(gl + 0xd0));
        // town -1: the weather object (GameLoop+0x130, +0x18 = temperature) instead of a villager
        uint8_t* c = g_watchTown < 0 ? (gl ? *(uint8_t**)(gl + 0x130) : nullptr) : (uint8_t*)towns::CitizenAt(g_watchTown, g_watchIndex);
        if (c) {
            g_watchAddr = c + g_watchOff;
            Log("watch: armed on town %d villager %d +%#x at tick %llu, value %08x", g_watchTown, g_watchIndex, g_watchOff,
                g_s.tick, *(uint32_t*)g_watchAddr);
            SetWatch(g_watchAddr);
        }
    } else if (g_watchAddr && g_s.tick >= g_watchTo) {
        SetWatch(nullptr);
        g_watchAddr = nullptr;
        g_watchFrom = ~0ull;
        Log("watch: off");
    }
}

static LONG CALLBACK CrashLogger(EXCEPTION_POINTERS* ep) {
    static volatile LONG count = 0;
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == 0x40010006 || code == 0x4001000a || code == 0x406d1388 || code == EXCEPTION_SINGLE_STEP)
        return EXCEPTION_CONTINUE_SEARCH;  // debug output, thread names, simwatch steps
    if (InterlockedIncrement(&count) > 5) return EXCEPTION_CONTINUE_SEARCH;
    char line[1024];
    int len = snprintf(line, sizeof line, "!! EXCEPTION %08lx (%llx %llx) thread %lu at", code,
        (unsigned long long)ep->ExceptionRecord->ExceptionInformation[0],
        (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1], GetCurrentThreadId());
    CONTEXT ctx = *ep->ContextRecord;
    if (!ctx.Rip) { ctx.Rip = *(DWORD64*)ctx.Rsp; ctx.Rsp += 8; }  // called a null pointer: start at the caller
    for (int depth = 0; depth < 24 && ctx.Rip && len < (int)sizeof line - 40; depth++) {
        CrashFrame(line + len, sizeof line - len, ctx.Rip);
        len = (int)strlen(line);
        DWORD64 imageBase;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fn) { ctx.Rip = *(DWORD64*)ctx.Rsp; ctx.Rsp += 8; continue; }
        void* hd; DWORD64 est;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &hd, &est, nullptr);
    }
    Log("%s", line);
    return EXCEPTION_CONTINUE_SEARCH;
}

void InstallHooks() {
    g_mainThread = GetCurrentThreadId();
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCWSTR)&CrashLogger, &self);
        if (self) ModuleRange(self, g_selfBase, g_selfEnd);
        AddVectoredExceptionHandler(1, CrashLogger);  // first: the game's own handler writes crash.dmp and exits
    }
    HMODULE exe = GetModuleHandleW(nullptr), rt = GetModuleHandleW(gamebuild::RuntimeDll());
    ModuleRange(exe, g_exeBase, g_exeEnd);
    if (rt) ModuleRange(rt, g_rtBase, g_rtEnd);

    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    SYSTEMTIME st;
    GetLocalTime(&st);
    swprintf_s(wcsrchr(path, L'\\') + 1, 64, L"bmp_ticks_%04d%02d%02d_%02d%02d%02d%s.log",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, SlotSuffix());
    CleanOldLogs(path);
    // the per-tick log grows ~1 GB an hour of play (a user had a 3.8 GB one): only for tests (bmp_autotest.txt) or with
    // <game>\bmp_tickslog.txt. Desyncs in normal play are diagnosed from the bmp_desync_*.log ring dumps.
    if (GameFileExists(L"bmp_autotest.txt") || GameFileExists(L"bmp_tickslog.txt")) g_ticks = _wfsopen(path, L"w", _SH_DENYNO);
    if (g_ticks) fprintf(g_ticks, "# tick calls hash chain sim sim_calls sim_hash sim_chain outside off\n");
    // the per-draw log (~150 MB a session) only for tests: the harness (bmp_autotest.txt) or <game>\bmp_drawlog.txt
    memcpy(wcsstr(path, L"bmp_ticks_"), L"bmp_draws_", 10 * sizeof(wchar_t));
    if (GameFileExists(L"bmp_autotest.txt") || GameFileExists(L"bmp_drawlog.txt")) {
        g_draws = _wfsopen(path, L"w", _SH_DENYNO);
        if (g_draws) fprintf(g_draws, "# tick F(actory handle tag)/S(im) caller_id value\n");
    }
    {  // debug: <game>\bmp_posdump.txt -> bmp_pos_*.log = every villager's exact x/y at the end of every tick
        wchar_t flag[MAX_PATH];
        wcscpy_s(flag, path);
        wcscpy_s(wcsrchr(flag, L'\\') + 1, 64, L"bmp_loadallocs.txt");
        g_loadAllocLog = GetFileAttributesW(flag) != INVALID_FILE_ATTRIBUTES;
        wcscpy_s(wcsrchr(flag, L'\\') + 1, 64, L"bmp_posdump.txt");
        if (GetFileAttributesW(flag) != INVALID_FILE_ATTRIBUTES) {
            memcpy(wcsstr(path, L"bmp_draws_"), L"bmp_pos___", 10 * sizeof(wchar_t));
            g_posDump = _wfsopen(path, L"w", _SH_DENYNO);
        }
        // debug: <game>\bmp_statedump.txt "FROM TO STEP" -> bmp_state<sfx>.log = villager objects at those ticks of the
        // first session (towns::DumpCitizens; tools/state_diff.py)
        wcscpy_s(wcsrchr(flag, L'\\') + 1, 64, L"bmp_statedump.txt");
        FILE* sf = nullptr;
        if (!_wfopen_s(&sf, flag, L"r") && sf) {
            fscanf_s(sf, "%llu %llu %llu", &g_stateFrom, &g_stateTo, &g_stateStep);
            fclose(sf);
            wchar_t sp[MAX_PATH];
            wcscpy_s(sp, flag);
            swprintf_s(wcsrchr(sp, L'\\') + 1, 64, L"bmp_state%s.log", SlotSuffix());
            g_stateDump = _wfsopen(sp, L"w", _SH_DENYNO);
            if (!g_stateStep) g_stateStep = 1;
        }
        wcscpy_s(wcsrchr(flag, L'\\') + 1, 64, L"bmp_watch.txt");
        FILE* wf = nullptr;
        if (!_wfopen_s(&wf, flag, L"r") && wf) {
            if (fscanf_s(wf, "%d %d %i %llu %llu", &g_watchTown, &g_watchIndex, &g_watchOff, &g_watchFrom, &g_watchTo) == 5)
                AddVectoredExceptionHandler(1, WatchHandler);
            else g_watchFrom = ~0ull;
            fclose(wf);
        }
    }
    p_InternalFactoryGet = (InternalFactoryGet_t)RuntimeExport("?Get@InternalFactory@Object@@SAPEAV12@XZ");

    Log("MH_Initialize: %s", MH_StatusToString(MH_Initialize()));
    Hook("?Initialize@Random@System@@AEAAXI@Z", (void*)h_RandInit, (void**)&o_RandInit);
    Hook("??0Random@System@@QEAA@XZ", (void*)h_RandTimeCtor, (void**)&o_RandTimeCtor);
    Hook("??0Random@System@@QEAA@I@Z", (void*)h_RandSeedCtor, (void**)&o_RandSeedCtor);
    Hook("?Serialize@Random@System@@QEAAXAEAVSerializer@IO@@@Z", (void*)h_RandSerialize, (void**)&o_RandSerialize);
    Hook("?Update@Scene@Audio@@QEAA_NVTime@System@@0AEAVRandom@4@AEBVVector3@Math@@M@Z", (void*)h_AudioSceneUpdate,
         (void**)&o_AudioSceneUpdate);

    Hook("?Run@Loader@Engine@@UEAAXXZ", (void*)h_LoaderRun, (void**)&o_LoaderRun);
    Hook("?Load@External@Object@@SAPEAV12@AEBVFilename@System@@PEAV12@@Z", (void*)h_ExternalLoad, (void**)&o_ExternalLoad);
    Hook("?LoadResources@UpdateLoop@Engine@@QEAAXXZ", (void*)h_LoadResources, (void**)&o_LoadResources);
    Hook("?Startup@LoaderLoop@Engine@@UEAAXXZ", (void*)h_LoaderStartup, (void**)&o_LoaderStartup);
    Hook("?Update@MeshInstance@Graphics@@UEAAXAEBVTime@System@@@Z", (void*)h_MeshInstanceUpdate, (void**)&o_MeshInstanceUpdate);

    Log("simheap init: %s", simheap::Init() ? "ok" : "FAILED");
    {
        wchar_t name[64];
        swprintf_s(name, L"bmp_heapjitter%s.txt", SlotSuffix());
        g_heapJitter = GameFileExists(name);
        if (g_heapJitter) Log("test: engine heap jitter ON (%ls)", name);
        swprintf_s(name, L"bmp_heapnudge%s.txt", SlotSuffix());
        if (GameFileExists(name)) {
            std::string t = ReadGameFile(name);
            g_heapNudgeAt = t.empty() ? 3000 : strtoull(t.c_str(), nullptr, 10);
            Log("test: sim-heap nudge at tick %llu (%ls)", g_heapNudgeAt, name);
        }
    }
    Hook("?MemFree@MemoryAllocator@System@@SAXPEAX@Z", (void*)h_MemFree, (void**)&o_MemFree);
    Hook("?Free@MemoryAllocator@System@@QEAAXPEAX@Z", (void*)h_AllocatorFree, (void**)&o_AllocatorFree);
    Hook("?MemAlloc@MemoryAllocator@System@@SAPEAX_KH@Z", (void*)h_MemAlloc, (void**)&o_MemAlloc);
    Hook(m_RandU, (void*)h_RandU, (void**)&o_RandU);
    Hook(m_RandMax, (void*)h_RandMax, (void**)&o_RandMax);
    Hook(m_RandRange, (void*)h_RandRange, (void**)&o_RandRange);
    Hook(m_RandFloat, (void*)h_RandFloat, (void**)&o_RandFloat);
    Hook(m_RandFloatTol, (void*)h_RandFloatTol, (void**)&o_RandFloatTol);
    Hook(m_RandTol, (void*)h_RandTol, (void**)&o_RandTol);

    auto nt = (IMAGE_NT_HEADERS*)((uint8_t*)exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
    // Steam (kExeTimestamp) or GOG build: exe addresses go through Rva(). test: <game>\bmp_fakeexe.txt = unknown build
    Log("game build: %s (exe timestamp %#x)", gamebuild::Name(), nt->FileHeader.TimeDateStamp);
    if (gamebuild::Get() == gamebuild::kUnknown) {
        Log("!! exe timestamp %#x is neither the Steam (%#x) nor the GOG build of Banished 1.0.7 - exe hooks disabled",
            nt->FileHeader.TimeDateStamp, kExeTimestamp);
        // tell the player once per start: otherwise nothing happens and the mod's main-menu notice even claims the
        // DLL is missing
        std::thread([] {
            // wait for the game's window and attach the box to it - a box without owner opens behind the game
            HWND owner = nullptr;
            for (int i = 0; i < 60 && !owner; i++) {
                Sleep(500);
                EnumWindows([](HWND h, LPARAM l) -> BOOL {
                    DWORD pid = 0;
                    GetWindowThreadProcessId(h, &pid);
                    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
                    RECT r;
                    if (!GetWindowRect(h, &r) || r.right - r.left < 640) return TRUE;  // not the launcher / a splash
                    *(HWND*)l = h;
                    return FALSE;
                }, (LPARAM)&owner);
            }
            Sleep(3000);  // let the main menu appear first
            MessageBoxW(owner,
                L"Banished Multiplayer works with the Steam and GOG versions of Banished 1.0.7.\n\n"
                L"This copy of the game has a different program file, so the multiplayer stays switched off. "
                L"The game itself runs normally.\n\n"
                L"To remove the mod, delete dinput8.dll from the Banished folder.",
                L"Banished Multiplayer", MB_OK | MB_ICONINFORMATION | MB_TOPMOST | MB_SETFOREGROUND);
        }).detach();
        return;
    }
    towns::Install();  // fixed exe addresses: only on the known build
    gamewin::Install();
    tint::Install();
    HookAt("GameLoop::Update", (void*)(g_exeBase + Rva(kRvaGameLoopUpdate)), (void*)h_GameLoopUpdate, (void**)&o_GameLoopUpdate);
    HookAt("World::Update", (void*)(g_exeBase + Rva(kRvaWorldUpdate)), (void*)h_WorldUpdate, (void**)&o_WorldUpdate);
    HookAt("TerrainPoint", (void*)(g_exeBase + Rva(0xa48b0)), (void*)h_TerrainPoint, (void**)&o_TerrainPoint);
    HookAt("MapConnectivity", (void*)(g_exeBase + Rva(0xa1950)), (void*)h_MapConnect, (void**)&o_MapConnect);
    HookAt("ProductionUI rows", (void*)(g_exeBase + Rva(0x7af20)), (void*)h_ProductionRows, (void**)&o_ProductionRows);
    Hook("?Intersect@Scene@Picking@@QEBAHAEBVVector3@Math@@0AEBVScalar@4@AEAV34@2@Z", (void*)h_PickIntersect, (void**)&o_PickIntersect);
    Hook("?RootAnimationOffset@MeshInstance@Graphics@@QEBA?AVVector3@Math@@XZ", (void*)h_RootOffset, (void**)&o_RootOffset);
    p_EnableRootOnly = (void (*)(bool))RuntimeExport("?EnableRootOnlyUpdates@MeshInstance@Graphics@@SAX_N@Z");
    Hook("?Update@AnimationManager@Graphics@@QEAAXAEBVTime@System@@_N@Z", (void*)h_AnimMgrUpdate, (void**)&o_AnimMgrUpdate);
    HookAt("SimStepEnd", (void*)(g_exeBase + Rva(kRvaStepEnd)), (void*)h_StepEnd, (void**)&o_StepEnd);
    HookAt("EntityManager::AddEntity", (void*)(g_exeBase + Rva(kRvaAddEntity)), (void*)h_AddEntity, (void**)&o_AddEntity);
    HookAt("EntityManager::PerFrameSetSync", (void*)(g_exeBase + Rva(kRvaPerFrameSetSync)), (void*)h_PerFrameSetSync, (void**)&o_PerFrameSetSync);
    HookAt("ComponentEntity::FlagsChanged", (void*)(g_exeBase + Rva(kRvaEntityFlagsChanged)), (void*)h_EntityFlagsChanged, (void**)&o_EntityFlagsChanged);
    HookAt("TreeErase", (void*)(g_exeBase + Rva(kRvaTreeErase)), (void*)h_TreeErase, (void**)&o_TreeErase);
    HookAt("EntityManager::RemoveEntity", (void*)(g_exeBase + Rva(kRvaRemoveEntity)), (void*)h_RemoveEntity, (void**)&o_RemoveEntity);
    if (o_RandSeedCtor) {
        o_RandSeedCtor(g_uiRng, 0x0011AA11);
        o_RandSeedCtor(g_uiTagRng, 0x0022BB22);
        g_uiRngReady = true;
    }
    Hook("?Update@MainLoop@Engine@@QEAA_NAEBVTime@System@@@Z", (void*)h_MainLoopUpdate, (void**)&o_MainLoopUpdate);
    Hook("?Update@AchievementManager@Achievement@@QEAAXAEBVTime@System@@@Z", (void*)h_AchievementUpdate, (void**)&o_AchievementUpdate);
    if (gamebuild::Get() == gamebuild::kSteam)  // Steam achievements: the GOG build has no such export
        Hook("?OnUserStatsReceived@AchievementManager@Achievement@@QEAAXPEAUUserStatsReceived_t@@@Z", (void*)h_StatsReceived,
             (void**)&o_StatsReceived);
    gameload::Init();
    keyboard::Install();  // text boxes type with the player's keyboard layout (keyboard.cpp)
    {
        wchar_t dbg[MAX_PATH];
        GetModuleFileNameW(nullptr, dbg, MAX_PATH);
        wcscpy_s(wcsrchr(dbg, L'\\') + 1, 64, L"bmp_debug_addentity.txt");
        FILE* f = nullptr;
        if (!_wfopen_s(&f, dbg, L"r") && f) { fscanf_s(f, "%d", &g_debugAddEntity); fclose(f); }
    }
    net::Init();
    overlay::Init();
    actionprobe::Install(g_exeBase, ActionSink);
    {
        wchar_t cmdLog[MAX_PATH];
        GetModuleFileNameW(nullptr, cmdLog, MAX_PATH);
        swprintf_s(wcsrchr(cmdLog, L'\\') + 1, 64, L"bmp_commands%s.log", SlotSuffix());
        commands::Install(g_exeBase, cmdLog);
        p_PausesGame = (PausesGame_t)RuntimeExport("?PausesGame@UserInterface@UI@@QEBA_NXZ");
    }
    {
        wchar_t flag[MAX_PATH];
        GetModuleFileNameW(nullptr, flag, MAX_PATH);
        wchar_t* name = wcsrchr(flag, L'\\') + 1;
        wcscpy_s(name, 64, L"bmp_poswatch.txt");
        g_posWatch = GetFileAttributesW(flag) != INVALID_FILE_ATTRIBUTES;
        if (g_posWatch) {
            FILE* f = nullptr;
            char mode[16] = "";
            if (_wfopen_s(&f, flag, L"r") == 0 && f) { fgets(mode, sizeof mode, f); fclose(f); }
            towns::g_copyZ = mode[0] == 'z';  // "z": watch heights (reported in the x slot)
            Log("POSWATCH armed (%s)", towns::g_copyZ ? "heights" : "x/y");
        }
        wcscpy_s(name, 64, L"bmp_simwatch.txt");
        if (GetFileAttributesW(flag) != INVALID_FILE_ATTRIBUTES) {
            swprintf_s(name, 64, L"bmp_writes%s.log", SlotSuffix());
            g_watch = simwatch::Init(flag);
        }
    }
    // Terrain-overlay renderer + decal system (exe+0xb0000..0xb6fff): sim code notifies them about terrain/road/
    // decal changes, and their bookkeeping is consumed per rendered frame - membership of their structures
    // depends on frame timing, so none of it may live in the sim heap. (0xb6ce0 = TerrainGenerator: excluded.)
    {
        static const uintptr_t kVisualEntries[] = {0xb0620, 0xb2260, 0xb4220, 0xb4840, 0xb55b0, 0xb5700, 0xb59a0,
                                                   0xb60b0, 0xb6120, 0xb61d0, 0xb6250, 0xb6460, 0xb65b0};
        for (uintptr_t rva : kVisualEntries) WrapVisual((void*)(g_exeBase + Rva(rva)), "terrain/decal entry");
        Log("visual scope: %zu terrain/decal entry points wrapped", sizeof kVisualEntries / sizeof kVisualEntries[0]);
    }
    autotest::Init();
}
