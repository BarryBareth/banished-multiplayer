#pragma once
#include <string>
// Bug reports: a game-styled window (Dialog/BmpReport.rsc) with "what were you doing" + Save to Desktop / Send.
// The report is a .zip of this PC's mod logs, recent desync records and (after a crash) crash.dmp, scrubbed of
// player names, Steam ids, the Windows user name, IP addresses and room/lobby codes. Send uploads it over HTTPS
// (WinHTTP) to the report server (tools/bmp_bugs.py); a failed send saves it to the Desktop instead.
namespace report {
void Open(bool crash);             // show the window (buttons: Multiplayer window, in-game bar; crash prompt)
void Frame(void* ui, bool inMenu); // main thread, every frame: keeps the window; at the menu, offers it once after a crash
void Test(bool send, const std::string& text);  // tests: Save / Send as if clicked (window open or not)
bool Busy();                       // a report is being built / sent
}
