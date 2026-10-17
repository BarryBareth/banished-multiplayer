#pragma once
#include <cstdint>

// Automated determinism test driver. Active only when <game>\bmp_autotest.txt exists:
//   save=dettest      save to load from the main menu (case-insensitive match on save name)
//   mode=idle|chaos   idle: fixed speed, no input. chaos: random camera/UI/tool/speed input like a player
//   ticks=20000       quit the game after this many sim ticks
//   speed=5           sim ticks per frame for idle mode (chaos picks 1/2/5/10 at random)
//   seed=1            chaos RNG seed
namespace autotest {
void Init();                         // read config, install input hooks (call from InstallHooks)
bool Active();
void Frame();                        // every MainLoop::Update, main thread, before the original
void OnSimTick(uint64_t tick);       // after each completed sim tick
void ScriptMouse(int x, int y, bool leftDown);  // test scripts: the game sees the mouse here (x < 0: real mouse again)
}

// provided by hooks.cpp
void* ProbeGameLoop();
void ProbeDumpRecentDraws(const char* why);  // desync: the last in-sim draws -> <game>\bmp_desync_<time>.log
void ProbeNote(uint32_t tag, uint64_t value, int32_t a, int32_t b);  // an extra event in that record (sim only)
uint64_t ProbeSessionCount();  // increments when a game session starts
bool ProbeInSim();
bool ProbeUiPaused();  // a UI dialog is pausing the game
bool ProbeIsMainThread();
void ProbeNonSimScope(bool enter);  // UI-side work from inside a tick (engine heap, UI RNG)
uint64_t ProbeTick();
void ProbeFlush();
void ProbeSetAllocLog(uint64_t from, uint64_t to);
