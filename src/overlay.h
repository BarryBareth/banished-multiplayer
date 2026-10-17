#pragma once
#include <string>
#include <cstdint>

// In-game multiplayer UI: a Dear ImGui overlay drawn on top of the game's frame (IDXGISwapChain::Present hook).
// Main menu: host panel (save picker, invite, start, player list). In game: compact session bar (players, status,
// invite, resync). Button clicks are handed to the network code on the main thread (net::Ui*).
namespace overlay {
void Init();                              // install the Present/ResizeBuffers hooks (call from InstallHooks)
void FilterMouse(uint8_t* mouseState);    // hide clicks/wheel from the game while the cursor is over our UI
void RequestScreenshot(const wchar_t* path);  // tests: the next finished frame -> PNG
bool TakePasteKey();                      // Ctrl+V / Shift+Insert reached the game window since the last call
std::string RenderInfo();                 // bug reports: DX11 overlay state + the d3d9/d3d11/dxgi modules in use
void GameFrame();                         // main loop, end of every frame: the game-styled multiplayer windows (any renderer)
}
