#pragma once
#include "net.h"
// The in-game multiplayer windows in the game's own style (session bar, trade, offers, settling); see ingame.cpp.
namespace ingame {
bool Frame(const net::UiInfo& u);  // every frame (main thread): open / close them; false = mod missing, use the overlay
bool Native();
void ToggleTrade();
}
