// One big reserved region, bump allocation, 16-byte size classes. Free blocks are reused lowest address first, so
// reuse depends only on WHICH blocks are free, not on the order they were freed: the game destroys some trees keyed
// by engine-heap pointers (descriptions), whose shape - and so the order their nodes are freed in - differs between
// machines (seen 2026-10-11: same 3 blocks freed in a different order -> LIFO reuse diverged -> entity order desync).
// No randomization -> identical alloc/free sets yield identical relative addresses.
#include "simheap.h"
#include <windows.h>
#include <map>
#include <set>

namespace simheap {

static const size_t kReserve = 16ull << 30;     // 16 GB of address space (reserved, committed on demand)
static const size_t kCommitStep = 64ull << 20;  // commit in 64 MB steps
static const size_t kHeader = 16;               // [u64 classSize][u64 magic] before each block
static const uint64_t kMagic = 0x51A5EA9E51A5EA9Eull;
static const size_t kSmallMax = 64 * 1024;      // classes up to 64 KB use the flat table

static uint8_t* g_base;
static uint8_t* g_top;        // next unused byte
static uint8_t* g_committed;  // end of committed memory
static uint8_t* g_genBase;    // start of the current generation
using FreeSet = std::set<uint8_t*>;              // free blocks of one size class, lowest address first
static FreeSet* g_small[kSmallMax / 16 + 1];
static std::map<size_t, FreeSet>* g_large;
static SRWLOCK g_lock = SRWLOCK_INIT;
static Stats g_stats;

// Old generations' memory goes back to Windows: live blocks are counted per 1 MB chunk, and at each new generation the
// chunks of generations before the previous one that hold no live block are decommitted (the previous generation is
// kept whole in case something still glances at the old game's objects while the new one starts). Before this, every
// load / resync / join-in-progress left its whole arena committed: memory grew all evening.
static const size_t kChunk = 1ull << 20;
static int32_t* g_chunkLive;      // live blocks touching each chunk
static uint8_t* g_chunkGone;      // 1 = decommitted
static uint8_t* g_prevGenBase;    // start of the previous generation (kept)
static void CountChunks(const uint8_t* block, size_t bytes, int d) {
    size_t c0 = (size_t)(block - g_base) / kChunk, c1 = (size_t)(block + bytes - 1 - g_base) / kChunk;
    for (size_t c = c0; c <= c1; c++) g_chunkLive[c] += d;
}
static bool ChunkGone(const void* p) { return g_chunkGone[(size_t)((const uint8_t*)p - g_base) / kChunk] != 0; }

bool Init() {
    if (g_base) return true;
    g_base = (uint8_t*)VirtualAlloc(nullptr, kReserve, MEM_RESERVE, PAGE_NOACCESS);
    g_top = g_committed = g_genBase = g_prevGenBase = g_base;
    g_large = new std::map<size_t, FreeSet>();
    g_chunkLive = new int32_t[kReserve / kChunk]();
    g_chunkGone = new uint8_t[kReserve / kChunk]();
    return g_base != nullptr;
}

bool Contains(const void* p) { return g_base && (const uint8_t*)p >= g_base && (const uint8_t*)p < g_top; }

void Range(uint8_t** base, size_t* committed) { *base = g_base; *committed = (size_t)(g_committed - g_base); }

uint64_t Offset(const void* p) { return (uint64_t)((const uint8_t*)p - g_genBase); }
void* FromOffset(uint64_t offset) { return g_genBase + offset; }

void NewGeneration() {
    AcquireSRWLockExclusive(&g_lock);
    // next commit-step boundary (committed memory grows from g_base in kCommitStep steps)
    size_t used = (size_t)(g_top - g_base);
    uint8_t* keepFrom = g_genBase;  // the generation that just ended stays; everything before it may go
    static const bool keepAll = GetFileAttributesW(L"bmp_nodecommit.txt") != INVALID_FILE_ATTRIBUTES;  // test
    if (keepAll) keepFrom = g_base;
    for (size_t c = 0; g_base + (c + 1) * kChunk <= keepFrom && g_base + (c + 1) * kChunk <= g_committed; c++) {
        if (g_chunkLive[c] || g_chunkGone[c]) continue;
        if (VirtualFree(g_base + c * kChunk, kChunk, MEM_DECOMMIT)) {
            g_chunkGone[c] = 1;
            g_stats.released += kChunk;
        }
    }
    g_prevGenBase = keepFrom;
    g_top = g_genBase = g_base + (used + kCommitStep - 1) / kCommitStep * kCommitStep;
    if (g_committed < g_top) g_committed = g_top;
    for (FreeSet* s : g_small) if (s) s->clear();
    g_large->clear();
    g_stats.generation++;
    ReleaseSRWLockExclusive(&g_lock);
}

static FreeSet& FreeSetOf(size_t cls) {
    if (cls <= kSmallMax) {
        FreeSet*& s = g_small[cls / 16];
        if (!s) s = new FreeSet();
        return *s;
    }
    return (*g_large)[cls];
}

void* Alloc(size_t size, int align) {
    if (!g_base || align > 16) {
        InterlockedIncrement64((LONG64*)&g_stats.fallbacks);
        return nullptr;
    }
    size_t cls = (size + 15) & ~size_t(15);
    if (cls == 0) cls = 16;
    AcquireSRWLockExclusive(&g_lock);
    uint8_t* block;
    FreeSet& fs = FreeSetOf(cls);
    if (!fs.empty()) {
        block = *fs.begin();
        fs.erase(fs.begin());
    } else {
        size_t need = kHeader + cls;
        if (g_top + need > g_base + kReserve) {
            ReleaseSRWLockExclusive(&g_lock);
            InterlockedIncrement64((LONG64*)&g_stats.fallbacks);
            return nullptr;
        }
        while (g_top + need > g_committed) {
            VirtualAlloc(g_committed, kCommitStep, MEM_COMMIT, PAGE_READWRITE);
            g_committed += kCommitStep;
            g_stats.committed += kCommitStep;
        }
        block = g_top;
        g_top += need;
    }
    ((uint64_t*)block)[0] = cls;
    ((uint64_t*)block)[1] = kMagic;
    CountChunks(block, kHeader + cls, +1);
    g_stats.allocs++;
    g_stats.live++;
    g_stats.liveBytes += cls;
    ReleaseSRWLockExclusive(&g_lock);
    return block + kHeader;
}

void Free(void* p, bool reuse) {
    uint8_t* block = (uint8_t*)p - kHeader;
    AcquireSRWLockExclusive(&g_lock);
    if (ChunkGone(block)) { ReleaseSRWLockExclusive(&g_lock); return; }  // a stale pointer into released memory
    if (((uint64_t*)block)[1] == kMagic) {
        size_t cls = ((uint64_t*)block)[0];
        ((uint64_t*)block)[1] = 0;  // catches double frees
        CountChunks(block, kHeader + cls, -1);
        if (reuse && block >= g_genBase) {  // older generations: never reused
            FreeSetOf(cls).insert(block);
        } else {
            g_stats.quarantined += cls;
        }
        g_stats.frees++;
        g_stats.live--;
        g_stats.liveBytes -= cls;
    }
    ReleaseSRWLockExclusive(&g_lock);
}

// Blocks are 16-aligned with a [classSize][magic] header: scan backwards for the header that covers p.
void* BlockOf(const void* p, size_t* size) {
    const uint8_t* a = (const uint8_t*)((uintptr_t)p & ~(uintptr_t)15);
    for (int i = 0; i < 4096 && a >= g_base; i++, a -= 16) {
        if (ChunkGone(a)) return nullptr;
        const uint64_t* h = (const uint64_t*)a;
        if (h[1] == kMagic && a + kHeader + h[0] > (const uint8_t*)p) {
            if (size) *size = (size_t)h[0];
            return (void*)(a + kHeader);
        }
    }
    return nullptr;
}

Stats GetStats() {
    AcquireSRWLockShared(&g_lock);
    Stats s = g_stats;
    ReleaseSRWLockShared(&g_lock);
    return s;
}

}  // namespace simheap
