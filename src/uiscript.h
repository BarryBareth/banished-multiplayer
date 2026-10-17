#pragma once

// UI test scripts: drive building windows the way a player's clicks do, so the command layer's window hooks run
// exactly as in real play. Armed by bmp_autotest.txt "uiscript@N=<file>" (per game instance slot N).
namespace uiscript {
bool Load(const wchar_t* path);
void OnFrame();  // main thread, every GameLoop frame after the sim steps, before the UI update
void OnNewSession();
}
