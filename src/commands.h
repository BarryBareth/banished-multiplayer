#pragma once
#include <cstdint>
#include <string>

// Lockstep command layer, step 1: player actions (building placement, drag-tool commits) issued outside a sim
// tick are queued and executed at the start of a later sim tick, inside the simulation, so every machine can
// apply them at the same tick. Executed commands are logged to <game>\bmp_commands.log.
namespace commands {
void Install(uintptr_t exeBase, const wchar_t* logPath);
void OnTickStart(uint64_t tick);   // inside the sim tick, before World::Update
void ExecuteAllNow(uint64_t tick); // paused frame: apply everything queued (caller provides sim scope)
bool Pending();
bool LoadReplay(const wchar_t* path);  // schedule commands from a bmp_commands.log-format file
bool ScheduleLine(const char* line, int player = 0);  // schedule one "<slot> <command>" line (network) for a player/town
void* ToolFor(uint32_t toolType, const std::string& path);  // the cached replay tool (tests: placement checks)
bool Submit(const std::string& text);  // a player action as command text ("place ...", "dragN ...") - test scripts
void FlushNetOutbox();                // send replay commands loaded while networked (after START)
void OnNewSession();
void ClearScheduled();
void RequestDeliverNow();          // separate towns: hand over the local town's waiting trade goods directly
void RequestSettle(int x, int y);  // separate towns: settle the local town at this cell (lockstep command)
void* OpenStorageWindow(float* x, float* y);  // overlay: an open window of one of the local town's storage buildings
void RequestShareStorage(void* storage, int town, bool on);  // separate towns: one of the local town's storage buildings usable by `town` (lockstep)
void* OpenWorkplaceWindow(float* x, float* y);  // overlay: an open window of one of the local town's workplaces
void RequestShareWork(void* building, int town, bool on);  // separate towns: town may staff this workplace (lockstep)
// separate towns: accept a trade - partner town gives nGive of givePath, the local town gives nAsk of askPath
void RequestTrade(int partnerTown, const std::string& givePath, int nGive, const std::string& askPath, int nAsk);
// separate towns: the same trade every season (period 1) or year (2), from now on, until a partner ends it
void RequestAgreement(int partnerTown, const std::string& givePath, int nGive, const std::string& askPath, int nAsk, int period);
void RequestEndAgreement(int id);
std::string DescPath(const void* desc);         // resource description -> its path (stable across machines)
void* DescFromPath(const std::string& path);
std::string DescName(const void* desc);         // short readable name             // drop every scheduled (network) command - a resync restarts the session               // a game is being loaded: drop per-session caches (tool objects, queue)
bool Executing();                  // true while a queued command is being applied
}
