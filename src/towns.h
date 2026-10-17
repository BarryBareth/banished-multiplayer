#pragma once
#include <string>
#include <cstdint>
#include <vector>

// Separate towns on one map. Banished keeps all town-wide state (storage lists, citizens, jobs, limits, unlocked
// seeds/livestock...) in one "Town" object at GameLoop+0xf0 that the game code always reads through the GameLoop
// global. Every player gets an own Town object; whenever the simulation runs something on behalf of a player
// (an entity's update, the town's step, a command, entity removal) that player's Town is swapped into GameLoop+0xf0.
// Outside the simulation it points at the local player's Town, so the UI shows your own town.
// Entities belong to the player whose context created them (natural map objects: player 0's context).
namespace towns {
void Install();                     // hooks (call once from InstallHooks)
void Configure(int count, int local, bool pickStarts = false);  // before the game loads: 1 = normal shared game
// Start-location picking: towns other than town 0 start empty; their player settles them later (a command).
bool PickMode();
bool Settled(int town);
bool CameraCell(int* x, int* y);    // map cell under the screen centre (camera look-at point)
bool CanSettle(int town, int x, int y, const char** why);
bool Settle(int town, int x, int y); // simulation (command): place the town's start there

// Goods (resource descriptions = RawMaterialDescription externals). Storage = Town+0x00 entities' StorageComponent.
struct Good { void* desc; int count; };
void DebugStorages();                           // uiscript `storages`: storage lists and market stock
std::vector<Good> Goods(int town);              // what the town has in storage (UI and simulation)
int CountOf(int town, void* desc);
// simulation (command): A gives nA of descA to B, B gives nB of descB to A - all or nothing
// The goods are dropped as piles next to the giver's storage; the giver's laborers carry them to the receiver's.
bool Trade(int a, void* descA, int nA, int b, void* descB, int nB, const char** why);
// Standing trades: the same Trade at the start of every season (period 1) or year (2), first one at once.
bool AddAgreement(int a, void* descA, int nA, int b, void* descB, int nB, int period);  // simulation (command)
bool EndAgreement(int town, int id);                                                    // simulation (command)
struct AgreementInfo { int id, a, b; void* descA; void* descB; int nA, nB, period, lastOk; };
std::vector<AgreementInfo> Agreements();  // UI
struct Delivery { int from, to; void* desc; int count, piles; bool stuck; };  // stuck: no walking route between the towns
std::vector<Delivery> Deliveries();             // trade goods still waiting on the ground (UI)
void DeliverNow(int from);
// Footprints (world rectangle + ground height) of other towns' buildings, fields and other structures (2+ cells),
// for the overlay's town colours. Main thread, outside the simulation; read-only.
struct Footprint { int town; float x0, y0, x1, y1, z; };
void OtherTownFootprints(std::vector<Footprint>& out);                      // simulation (command): hand over from's waiting goods directly
int Count();                        // towns in the current session (1 = off)
// The game's event log ("a child was born", ...) is shared; each entry is attributed to the town it happened in.
struct EventInfo { int index, town, cell; const char* text; };  // town -1 = unknown (older save)
std::vector<EventInfo> Events(int max);  // UI: newest first
int EventTown(int index);
void SetEventsMineOnly(bool on);         // UI: popups only for the local town's events
bool EventsMineOnly();
void FocusCell(int cell);              // UI: camera to a map cell (y * size + x)
void ShareStorage(int owner, void* storage, int target, bool on);  // simulation (command): one building usable by target
bool IsOwnStorage(void* entity);                 // a storage building (in its owner's storage list)
bool SharedWith(void* storage, int town);
// shared workplaces: another town's villagers of the profession may be hired at one of the owner's workplaces
void ShareWork(int owner, void* workplace, int target, bool on);  // simulation (command)
bool WorkSharedWith(void* workplace, int town);
bool IsOwnWorkplace(void* entity);                 // one of the local town's workplace buildings
int WorkersOfTownAt(void* workplace, int town);    // town's villagers working there
int StockAt(void* building);                       // tests: goods kept in that building
int ForeignWorkersAt(void* workplace);             // UI: other towns' villagers working there
std::string StorageSummary(int town);  // tests: "N storages (own a, from others b), goods total G"
// Probe: " pos=<hash> p=t:h16,..." - every villager's exact position (town:16-bit hash, in each town's citizen list
// order). Read-only; written to the tick log so cross-PC desyncs show the first villager that drifted.
void PositionHashes(char* out, size_t n);
int CopyPositions(float* out, int maxVillagers);
extern bool g_copyZ;  // debug (POSWATCH "z"): CopyPositions reports heights in the x slot  // x, y per villager (same order); returns the count
int Local();                        // this machine's town
void SetPlayers(int n);             // players in the session: loading a multi-town save makes a town for each
void SetLocal(int town);            // the session learned this machine's town late (a hosted multi-town save)
void SetTownPlayer(int town, const std::string& name);  // who plays that town (saved with the game, BPNM)
std::string TownPlayer(int town);                       // "" = unknown (old save / nobody yet)
int Current();                      // town whose context is active
void Switch(int town);              // make `town` current (no-op when separate towns are off)
int OwnerOf(void* entity);          // entity object -> town (0 when unknown)
int OwnerOfComponent(void* component);
int OwnerOfObject(void* obj);       // entity or component -> town; -1 when obj is neither
void OnEntityAdded(void* entity);   // AddEntity: the entity belongs to the current town
void ClaimNaturals(int town, int startCell, int endCell);  // trees/rocks/ore in the drag rectangle -> town
void TablesChangedBySim();          // commands: a command just ran (resource-table watch)
void OnLoadStart();                 // a load begins: the old session's towns are gone
void OnSessionStart();              // new GameLoop running: camera to the local town's start
void OnFrame();                     // main thread, every GameLoop frame (outside the sim)
void StepTowns(void* origTown, const int64_t* dt, void (*step)(void*, const int64_t*));  // run the town step per town

struct Scope {                      // switch for a block, restore after
    int prev;
    explicit Scope(int t) : prev(Current()) { Switch(t); }
    ~Scope() { Switch(prev); }
};
}
