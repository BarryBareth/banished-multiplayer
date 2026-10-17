# How it works

Banished has no multiplayer and wasn't designed for it, and its mod kit is data-only. So the mod is a DLL that the
game loads at start-up (`dinput8.dll`, a proxy for the system DirectInput DLL that the engine imports), which hooks
the game's code with [MinHook](https://github.com/TsudaKageyu/minhook). Addresses in the source are offsets into
the Steam build 1.0.7 (2017-09-10) of `Application-steam-x64.exe` and `Runtime-steam-x64.dll`; the engine DLL exports
most of its C++ API by name, the game logic in the exe was mapped by reverse engineering.

## Deterministic lockstep

Every player's game runs the **whole simulation**. Only player **commands** travel over the network (place a
building, set workers, trade, ...), each tagged with the sim tick it runs at, a little in the future (input delay
~150 ms). A game only simulates tick N once it has every player's commands up to N, so all games execute the same
commands at the same ticks. Every 600 ticks the players compare hashes; on a mismatch the host saves, sends the save to
everyone and everybody reloads it ("resync"). The same mechanism lets players join a running game.

That only works if the simulation is **deterministic**: the same commands must give the same result on every PC.
Banished's simulation runs at a fixed 60 steps per second (game speed = steps per frame), but many things leaked
outside state into it. The fixes, roughly in the order they were found (`src/hooks.cpp`, `src/visualstub.cpp`):

- **Random numbers.** `System::Random` objects seeded from the clock get deterministic seeds; the UI, audio and
  render-driven animation events drew from the simulation's generator and now use their own.
- **Memory addresses.** The game keeps many sets ordered by raw pointer value (entity update order!). Its allocator is
  deterministic, but UI, camera and loading allocations interleave with simulation ones. All allocations made by game
  code inside a simulation step go to a separate deterministic heap (`src/simheap.cpp`, lowest-address-first reuse,
  a fresh region per loaded game); frees from outside the simulation are never reused.
- **Pointer-keyed trees in game logic.** A few loops pick "the first" element of a tree keyed by description pointers,
  whose order differs per PC (the monthly labor rebalance) - patched to a stable order.
- **Frame-dependent updates.** Character movement was fully updated only on the last sim step of each rendered frame;
  terrain/decal dirty sets, per-frame entity sets and lazily rebuilt data (map connectivity, production statistics)
  were touched by rendering or open windows. These are run every step, or kept out of the simulation.
- **Threads.** Pathfinding ran on a worker thread; it's switched to synchronous solving.

Desync diagnosis: a ring buffer of the last ~500k simulation events (random draws with their call sites, job offers,
AI actions, allocations) is written to `bmp_desync_*.log` when games disagree; `tools/ringdiff.py` finds the first
difference between two players' files.

## Commands

`src/commands.cpp` intercepts the game's player actions at the point where they'd change the simulation - the
placement and drag tools' commit functions, and the event handlers of every building window (workers, limits, crops,
trade, ...) - turns them into text commands, sends them, and executes them from the network stream inside the next
simulation step. Window commands refer to objects by their offset in the deterministic heap (the same on every PC).

## Separate towns

Banished has exactly one town: a `Town` object holding citizens, storages, known resources and statistics, which the
game always reaches through one global. `src/towns.cpp` creates one per player and swaps the global around: every
entity has an owner, its updates run in its owner's town, citizens only take their own town's jobs, and storage
searches, deliveries and statistics stay per town. On top of that: start-location picking, trades (goods are carried
physically between towns), shared storage buildings and workplaces, per-town roads, event log attribution, and an
extra block at the end of the save file that stores all of it.

## Networking

`src/net.cpp`: the host's game is the hub; other players connect to it

- through **Steam** (`src/steam.cpp`, a lobby + SteamNetworkingMessages restricted to Valve's relays, so no IP
  addresses are exchanged),
- through a **relay server** (`tools/bmp_relay.py`): host and guests connect out to it and use a room code,
- or **directly** over TCP.

Every message is a text line. The host relays commands with a global sequence number, collects "turns" (promises that
a player's commands up to a tick are sent), compares hashes, and drives resyncs and joins. Players must run the
identical DLL (the build id is a hash of its code).

## User interface

The mod's windows are real game dialogs defined in the companion data mod (`mod/static/Dialog/Bmp*.rsc`), opened and
filled by the DLL (`src/gamedlg.cpp`, `src/mpdialog.cpp`, `src/ingame.cpp`). A [Dear ImGui](https://github.com/ocornut/imgui)
overlay (`src/overlay.cpp`) is the fallback when the data mod is missing, and draws town colours.
