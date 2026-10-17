#pragma once
// The companion mod's buttons inside the game's own windows (event log filter, workplace sharing).
namespace gamewin {
void Install();          // hooks (from InstallHooks)
bool HasLogButtons();    // the event log window has the mod's Mine / Everyone buttons
bool HasWorkButton();    // workplace windows have the mod's Share button
bool WorkEvent(void* workPlaceUI, int evt, int id);  // from commands' WorkPlaceUI event hook: true = the Share toggle
void LayoutFrame();      // in-game frame: snapshot my window layout (kept over resyncs / joins)
}
