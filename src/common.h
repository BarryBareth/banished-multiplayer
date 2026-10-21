#pragma once
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <string>
#include "gamebuild.h"

// Several game instances can run from the same folder (local multiplayer testing): each claims a slot at
// startup (named mutex) and suffixes its files with SlotSuffix() ("" for slot 0, "_p1" for slot 1, ...).
extern int g_slot;
void SlotClaim();
const wchar_t* SlotSuffix();

// Logging to <game dir>\bmp<suffix>.log (flushed per line so crashes keep the tail).
void LogInit();
void Log(const char* fmt, ...);

// Resolve an export from the engine DLL (gamebuild::RuntimeDll()) by mangled name; logs and returns nullptr if missing.
void* RuntimeExport(const char* mangled);

// Install all hooks. Called once, on the game's main thread.
void InstallHooks();
namespace keyboard { void Install(); }  // keyboard.cpp: text boxes follow the player's keyboard layout

// The companion data mod (WinData\bmpShare.pkm) as this player has it: kModOk = installed and the build this DLL
// expects, kModMissing = not installed, kModOther = another build of it (older or newer copy). Main thread, once the
// game has started (before that: kModOk).
enum { kModOk, kModMissing, kModOther };
int CompanionMod();
int CompanionModKnown();
// The other enabled mods ("file:size;..." in load order, the companion mod left out): every player in a game must have
// the same. Refresh on the main thread; ModsSignature returns the last answer (any thread).
void RefreshModsSignature();
std::string ModsSignature();
bool ModsKnown();  // RefreshModsSignature has read the list (it runs when the game starts loading its data)
bool ModsChangedSinceStart();  // mods switched on/off since the game started: multiplayer needs a restart
