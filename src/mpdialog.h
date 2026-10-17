#pragma once
// The multiplayer window as a real game dialog (companion mod: Dialog/BmpMultiplayer.rsc): host / join / lobby.
namespace mpdialog {
bool Open(void* userInterface);  // main thread; false when the mod's dialog isn't available (overlay fallback)
void Close();
bool IsOpen();
void MenuTest(void* userInterface, void (*screenshot)(const wchar_t* path));  // tests (bmp_menutest.txt)
}
