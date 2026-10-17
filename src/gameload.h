#pragma once
#include <string>
#include <vector>

// Loads a save from the main menu through the engine's own Save::SaveLoad state machine, then starts the game
// the way the Load dialog does (GameLoop ctor -> LoaderLoop -> MainLoop::SetNextLoop). Used by the autotest
// harness and by multiplayer sessions.
namespace gameload {
void Init();                         // resolve engine exports (once)
bool InMenu();                       // the main-menu loop is current
// Start loading the save whose file name matches `name` (case-insensitive; exact stem match preferred, else
// substring). Works from the main menu or in-game (like the in-game Load dialog); not while a load is running.
bool Request(const std::string& name);
// Start a brand-new map like the New Game dialog: list indices into the start-condition (MapGenerator),
// terrain and climate lists; mapSize = index into the generator's size table.
bool RequestMainMenu();              // in-game: quit to the main menu like the in-game menu's button
bool RequestNewGame(uint32_t seed, int mapSize, int startIdx, int terrainIdx, int climateIdx, bool disasters);
// New-game choices (names as the game's lists hold them): start conditions, terrains, climates, map sizes.
struct NewGameOptions { std::vector<std::string> starts, terrains, climates, sizes; };
NewGameOptions GetNewGameOptions();
enum State { kIdle, kListing, kLoading, kStarting, kDone, kFailed };
State Frame();                       // called once per MainLoop::Update (hooks.cpp), drives the request
State Current();
const char* Error();                 // reason when kFailed
// Save the running game the way autosave does (SaveLoad op 4, slot -4 -> <SaveDir>\Auto.sav). In-game only.
bool BeginAutoSave();
bool SaveBusy();                     // the save started by BeginAutoSave is still being written
std::wstring SaveDir();              // <Documents>\Banished\Save
}
