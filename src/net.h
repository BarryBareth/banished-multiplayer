#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Lockstep networking (protocol: tools/relay.py; the host can also run the relay in-process).
// In-game (no config): main menu, Ctrl+Shift+H = host the newest save through a friends-only Steam lobby
// (N = other save, I = invite overlay, S = start); friends join through the Steam invite / "Join Game".
// Status is shown in the window title. Test/LAN config <game>\bmp_net.txt (keys may be per slot: key@N=...):
//   relay=127.0.0.1:7777     join a relay (tools/relay.py) or a game hosting over TCP
//   host=7777                host in-process, accept TCP players on this port
//   players=2                host: start automatically when this many players are present
//   save=Save0               host: save to send to everybody (they all load it as Multiplayer.sav)
//   delay=30                 command delay in sim ticks (latency budget)
namespace net {
void Init();                         // read config, connect (background), call once from InstallHooks
bool Active();
bool Started();                      // relay said START (every player connected): ok to load the save
void Frame();                        // main thread, every frame: process incoming messages
uint64_t Delay();                    // command delay in ticks
uint64_t CommandSlot(uint64_t tick);  // slot for a command issued now: tick + Delay(), after every promised turn
void SendCommand(uint64_t slot, const std::string& line);  // line = command text without the slot
// Lockstep gate: how many more sim ticks this client may run (everyone has promised commands up to then).
int64_t TicksAllowed(uint64_t currentTick);
void AfterFrame(uint64_t currentTick);                      // send TURN (no more local commands before tick+delay)
void OnTickHash(uint64_t tick, uint64_t hash, uint64_t heap);  // periodic desync check (+ sim-heap drift)
void DebugStatus(uint64_t tick, bool uiPaused, int speed);
// Shared speed/pause: the game's own speed control (speedCtl+0x34 speed, +0x30 paused) is kept equal to the
// network state; a local change (player click, hotkey, event autopause) is sent and reverted until it returns.
void SyncSpeed(uint8_t* speedCtl);
const std::string& SaveName();       // save every player loads (sent by the host); empty = harness decides

// ---- UI (overlay.cpp). Getters are cheap snapshots; actions are queued and run in Frame() on the main thread.
struct UiPlayer { int id; std::string name; bool ready; bool synced; };
struct UiInfo {
    bool active, hosting, started, inGame, inMenu, steamOk, resyncing, lobby;
    int me, players;
    std::string status, hostSave;
    std::string version;  // this DLL's build id (players must match)
    std::vector<UiPlayer> roster;
    uint64_t tick, desyncs;
    int speed;
    bool paused;
    bool separate;   // separate towns on one map
    int town, towns; // my town index / number of towns
    int hostVia;     // hosting: 0 Steam, 1 direct, 2 relay (-1 not hosting)
    std::string room;  // relay room code (host or guest)
    bool relayUp;    // hosting via relay: the relay opened the room
    int listenPort;  // direct hosting port (settings)
};
// connection settings (main menu), saved in <game>\bmp_settings.txt
struct Settings { int method; int port; std::string relay; std::string name; };  // method: 0 Steam, 1 direct, 2 relay
Settings GetSettings();
void SetSettings(const Settings& s);
UiInfo GetUiInfo();
std::vector<std::string> UiSaves();          // host candidates, newest first
// trade offers (separate towns): from/to are player ids
struct TradeOffer { int id, from, to; std::string give, ask; int nGive, nAsk, period; };  // period 0 once, 1 every season, 2 every year
std::vector<TradeOffer> IncomingOffers();          // offers to me, waiting for an answer
void SendOffer(int toPlayer, const std::string& givePath, int nGive, const std::string& askPath, int nAsk, int period = 0);
void AnswerOffer(int id, bool accept);
int TownOfPlayer(int player);
void SendPresence(const std::string& line);        // CUR / MARK lines (presence.cpp), only in a started session
int Me();                                          // my player id (-1 = none)
std::string TradeNews();                           // latest trade message for the UI ("" if none)
void SetTradeNews(const std::string& s);
enum UiAction { kUiHost, kUiInvite, kUiStart, kUiResync, kUiLeave, kUiJoin };  // kUiJoin arg: D|host:port or R|room code  // kUiHost arg: "S|<save>" or "N|seed,size,start,terrain,climate"
void UiDo(UiAction a, const std::string& arg = "");
}
