#pragma once
#include <cstdint>

// Write-watch for the sim heap: outside sim ticks the arena is read-only; any write faults, is logged with its
// call stack (deduplicated), then allowed via single-step. Every site found = code that mutates simulation state
// from UI/tools/rendering = a future lockstep command boundary (or a visual write to classify).
namespace simwatch {
bool Init(const wchar_t* logPath);
void Protect();    // call when leaving sim code (end of the frame's last sim step)
void Unprotect();  // call before running sim code (first sim step of a frame)
void Report();     // dump the deduplicated write sites to the log file
}
