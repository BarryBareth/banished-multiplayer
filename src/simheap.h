#pragma once
#include <cstddef>
#include <cstdint>

// Deterministic heap for simulation objects. The game orders entity sets by raw pointer value, so
// sim objects must get addresses whose relative order depends only on the sim's own alloc/free
// sequence - never on UI/camera allocations interleaved in the shared engine heap.
namespace simheap {
bool Init();
bool Contains(const void* p);
uint64_t Offset(const void* p);                   // position inside the current generation (deterministic object id)
void* FromOffset(uint64_t offset);               // inverse of Offset
// Start a new generation (every game load): fresh bump region, empty free lists; blocks of older generations are
// never handed out again. A session's allocation history then no longer depends on what the process did before.
void NewGeneration();
void Range(uint8_t** base, size_t* committed);    // committed part of the arena
void* Alloc(size_t size, int align);              // nullptr if the request can't be served (caller falls back)
void Free(void* p, bool reuse = true);            // reuse=false: quarantine (never handed out again)
void* BlockOf(const void* p, size_t* size);      // start of the live block containing p (nullptr if unknown)

struct Stats { uint64_t allocs, frees, live, liveBytes, committed, fallbacks, quarantined, generation, released; };
Stats GetStats();
}
