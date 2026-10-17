// Sim-heap write watch (see simwatch.h).
#include "simwatch.h"
#include "simheap.h"
#include "common.h"
#include "autotest.h"
#include <share.h>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace simwatch {

static uint8_t* g_base;
static size_t g_protected;          // bytes currently read-only (0 = unprotected)
static uintptr_t g_selfLo, g_selfHi;  // our own DLL: writes from here (simheap free-list bookkeeping) are fine
static uintptr_t g_exeLo, g_exeHi, g_rtLo, g_rtHi;
static FILE* g_log;
static SRWLOCK g_lock = SRWLOCK_INIT;
static thread_local uint8_t* t_pendingPage;

struct Site { uint32_t frames[16]; int n; uint64_t count, firstTick, lastTick; };
static std::unordered_map<uint64_t, Site>* g_sites;

static void Range(HMODULE m, uintptr_t& lo, uintptr_t& hi) {
    auto nt = (IMAGE_NT_HEADERS*)((uint8_t*)m + ((IMAGE_DOS_HEADER*)m)->e_lfanew);
    lo = (uintptr_t)m;
    hi = lo + nt->OptionalHeader.SizeOfImage;
}
static uint32_t Id(uintptr_t a) {
    if (a >= g_exeLo && a < g_exeHi) return 0x10000000u | uint32_t(a - g_exeLo);
    if (a >= g_rtLo && a < g_rtHi) return 0x20000000u | uint32_t(a - g_rtLo);
    return 0;
}

// Unwind the faulting thread's stack from its CONTEXT (x64 unwind tables).
static int Unwind(const CONTEXT* fault, uint32_t* out, int max) {
    CONTEXT ctx = *fault;
    int n = 0;
    for (int depth = 0; depth < 32 && n < max && ctx.Rip; depth++) {
        uint32_t id = Id(ctx.Rip);
        if (id) out[n++] = id;
        DWORD64 imageBase;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, nullptr);
        if (!fn) {  // leaf function: return address on top of stack
            ctx.Rip = *(DWORD64*)ctx.Rsp;
            ctx.Rsp += 8;
            continue;
        }
        void* handlerData;
        DWORD64 establisher;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
    }
    return n;
}

static LONG CALLBACK Handler(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code == EXCEPTION_SINGLE_STEP && t_pendingPage) {
        DWORD old;
        if (g_protected) VirtualProtect(t_pendingPage, 4096, PAGE_READONLY, &old);
        t_pendingPage = nullptr;
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code != EXCEPTION_ACCESS_VIOLATION || ep->ExceptionRecord->NumberParameters < 2) return EXCEPTION_CONTINUE_SEARCH;
    if (ep->ExceptionRecord->ExceptionInformation[0] != 1) return EXCEPTION_CONTINUE_SEARCH;  // only writes
    uint8_t* addr = (uint8_t*)ep->ExceptionRecord->ExceptionInformation[1];
    if (!g_protected || addr < g_base || addr >= g_base + g_protected) return EXCEPTION_CONTINUE_SEARCH;

    uintptr_t rip = ep->ContextRecord->Rip;
    if (!(rip >= g_selfLo && rip < g_selfHi)) {
        Site s{};
        s.n = Unwind(ep->ContextRecord, s.frames, 16);
        uint64_t key = 1469598103934665603ull;
        for (int i = 0; i < s.n; i++) key = (key ^ s.frames[i]) * 1099511628211ull;
        uint64_t tick = ProbeTick();
        AcquireSRWLockExclusive(&g_lock);
        auto it = g_sites->find(key);
        if (it == g_sites->end()) {
            s.count = 1;
            s.firstTick = s.lastTick = tick;
            g_sites->emplace(key, s);
            if (g_log) {
                // which object was written: its vtable (if it has one) and the field offset
                size_t bsize = 0;
                uint8_t* obj = (uint8_t*)simheap::BlockOf(addr, &bsize);
                uint32_t vt = obj ? Id(*(uintptr_t*)obj) : 0;
                fprintf(g_log, "NEW tick=%llu obj=%s+%x field=+%llx objsize=%zu thread=%lu stack:", tick,
                        vt ? ((vt >> 28) == 1 ? "exe" : "rt") : "data", vt & 0xfffffff,
                        obj ? (unsigned long long)(addr - obj) : 0ull, bsize, GetCurrentThreadId());
                for (int i = 0; i < s.n; i++) fprintf(g_log, " %s+%x", (s.frames[i] >> 28) == 1 ? "exe" : "rt", s.frames[i] & 0xfffffff);
                fputc('\n', g_log);
                fflush(g_log);
            }
        } else {
            it->second.count++;
            it->second.lastTick = tick;
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    // let this one instruction through: unprotect the page, single-step, re-protect
    uint8_t* page = (uint8_t*)((uintptr_t)addr & ~(uintptr_t)4095);
    DWORD old;
    VirtualProtect(page, 4096, PAGE_READWRITE, &old);
    t_pendingPage = page;
    ep->ContextRecord->EFlags |= 0x100;  // trap flag
    return EXCEPTION_CONTINUE_EXECUTION;
}

bool Init(const wchar_t* logPath) {
    HMODULE self;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&Init, &self);
    Range(self, g_selfLo, g_selfHi);
    Range(GetModuleHandleW(nullptr), g_exeLo, g_exeHi);
    if (HMODULE rt = GetModuleHandleW(L"Runtime-steam-x64.dll")) Range(rt, g_rtLo, g_rtHi);
    g_sites = new std::unordered_map<uint64_t, Site>();
    g_log = _wfsopen(logPath, L"w", _SH_DENYNO);
    AddVectoredExceptionHandler(1, Handler);
    Log("simwatch: armed, log %ls", logPath);
    return true;
}

void Protect() {
    size_t committed;
    simheap::Range(&g_base, &committed);
    if (!g_base || !committed || g_protected) return;
    DWORD old;
    if (VirtualProtect(g_base, committed, PAGE_READONLY, &old)) g_protected = committed;
}

void Unprotect() {
    if (!g_protected) return;
    DWORD old;
    VirtualProtect(g_base, g_protected, PAGE_READWRITE, &old);
    g_protected = 0;
}

void Report() {
    if (!g_log) return;
    AcquireSRWLockShared(&g_lock);
    std::vector<const Site*> v;
    for (auto& [k, s] : *g_sites) v.push_back(&s);
    std::sort(v.begin(), v.end(), [](auto a, auto b) { return a->count > b->count; });
    fprintf(g_log, "=== report at tick %llu: %zu distinct write sites ===\n", ProbeTick(), v.size());
    for (auto s : v) {
        fprintf(g_log, "%10llu  ticks %llu-%llu :", s->count, s->firstTick, s->lastTick);
        for (int i = 0; i < s->n; i++) fprintf(g_log, " %s+%x", (s->frames[i] >> 28) == 1 ? "exe" : "rt", s->frames[i] & 0xfffffff);
        fputc('\n', g_log);
    }
    fflush(g_log);
    ReleaseSRWLockShared(&g_lock);
}

}  // namespace simwatch
