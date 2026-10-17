#pragma once

// Run an engine function (any signature) in "visual scope": sim-heap routing/RNG treat it as non-simulation work.
extern volatile long g_visualStub;
void WrapVisual(void* target, const char* name);
