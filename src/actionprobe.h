#pragma once
#include <cstdint>
#include <utility>

namespace actionprobe {
// kind 'a' = action started (vf4), 'z' = action update ended/failed (vf5 result != 1)
using Sink = void (*)(char kind, int type, uint64_t actionId, uint64_t result);
void Install(uintptr_t exeBase, Sink sink);
const char* Name(int type);
// called after a DropOffRawMaterial action started (it moves the carried goods into the storage) - towns.cpp ends a
// trade courier's errand there (the probe owns the hook on that function)
void SetDropOffHandler(void (*fn)(void* action));
}
