#pragma once
#include <string>

// Other players' cursors and map pings (purely visual - nothing here touches the simulation).
// Each player sends where their mouse points on the map ("CUR x y", ~8 per second while it moves, "CUR -" when it
// leaves the map or the game loses focus) and Alt + left click sends a ping ("MARK x y"); the host relays both like trade
// offers. Remote cursors and pings are drawn by the DX11 overlay in the sender's town colour (player colour when sharing
// one town); a ping off screen gets an arrow at the screen edge, and clicking a ping's label flies the camera there.
// bmp_settings.txt: cursors=0 hides other players' cursors (pings stay).
namespace presence {
void Frame(bool inSession);                 // main thread, every frame (overlay::GameFrame): local cursor + ping input
bool BlockLeftClick();                      // input hook: the game must not see an Alt+click (it would select / build)
void Draw(float width, float height, float scale);  // overlay (ImGui frame): remote cursors + pings
// net (main thread): a player's cursor / ping arrived. colour = town index (or player id when sharing one town)
void OnCursor(int player, const std::string& name, int colour, bool on, float x, float y);
void OnPing(int player, const std::string& name, int colour, float x, float y);
// tests (uiscript): act as if the local mouse pointed at / pinged this cell
void TestCursor(int cellX, int cellY);
void TestPing(int cellX, int cellY);
void TestProbe(float screenX, float screenY);  // log the mouse-ray hit for a screen pixel and its projection back
}
