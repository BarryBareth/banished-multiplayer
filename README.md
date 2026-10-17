# Banished Multiplayer

Play [Banished](https://store.steampowered.com/app/242920/Banished/) together with other players:

- **Share one town**: everybody runs the same town together.
- **Separate towns**: everybody builds their own town on the same map, and you can trade goods, share storage
  buildings and lend each other workplaces.

Everyone's game runs the whole simulation in lockstep, and only the players' actions travel over the network. If the
games ever drift apart, the host's game is reloaded on everyone's PC automatically (a "resync"). Players can join a
game that's already running.

> Banished has no multiplayer, and its official mod kit can't add code. This mod is a DLL (`dinput8.dll`) that the
> game loads at start-up and that hooks into the game's code, plus a normal data mod (`bmpShare.pkm`) with the
> mod's windows and buttons. It is not made by or affiliated with Shining Rock Software.

## What you need

- The **Steam version of Banished** (1.0.7, the normal 64-bit game). Other versions are not supported.
- Windows 10 or 11.
- **Everybody in a game needs the same version of this mod.** The game tells you when the host's version differs.

## Install

1. Download the newest `BanishedMultiplayer-<version>.zip` from the
   [Releases page](../../releases/latest).
2. Close Banished. Unpack the zip anywhere and double-click **Install.bat**. It finds Banished by itself (or asks
   where it is) and copies two files:
   - `dinput8.dll` → the Banished folder (next to `Application-steam-x64.exe`)
   - `bmpShare.pkm` → `Banished\WinData`
3. Start Banished from Steam. The main menu now has a **Multiplayer** button.

If you subscribed to the mod on the **Steam Workshop**, Steam already gives you `bmpShare.pkm`. You still need the
DLL from the zip above; Install.bat notices the Workshop copy and only installs the DLL.

Windows or your antivirus may warn about Install.bat or the DLL because they're new and unsigned. Hooking into a
game's code is what antivirus programs look for, so this is expected; the full source code is in this repository.

To install by hand, copy the two files to the places above. To uninstall, run **Uninstall.bat** (or delete the two
files). Your settings and logs (`bmp_*.txt`, `bmp*.log` in the Banished folder) are left behind; delete them if
you like.

## Playing

Main menu → **Multiplayer**:

| Option | What it does |
|---|---|
| **Share one town** → New town / Load town | A fresh map, or one of the host's saves, that everybody runs together |
| **Separate towns** → New map / Continue a saved game | Each player gets their own town. A saved game remembers which town is whose. "Players choose where their town starts" lets each player pick a spot first |

**Connect through** one of three ways:

| | How | Needs |
|---|---|---|
| **Steam** | The host clicks **Invite players**; the others accept, or right-click the host in their Steam friends list → **Join Game** | Everyone on Steam. Traffic goes through Valve's relays, so nobody's IP address is shared |
| **Relay server** | The host gets a **room code** (copy button next to it) and sends it to the others; they paste it under **Join a game** | Everybody enters the same **Relay address**: a relay server somebody in your group runs - double-click `BanishedRelay.exe` from the release zip ([details](docs/RELAY.md)) |
| **Direct (IP)** | The others type the host's public IP address and port | The host forwards the port (default 7777, TCP) on their router. Players see each other's IP address |

In the game, the multiplayer bar at the top of the screen shows the players and has:

- **Trade** (separate towns): offer goods to another player, once or every season/year.
- **Share** on storage barns, stockpiles and carts: let another player's villagers use them. Workplaces can be
  shared the same way.
- **Resync**: reloads everybody from the host's game. It also happens by itself when the mod notices the games
  drifting apart.
- **Leave**, and the **bug** button.

Game speed and pause are shared: when one player changes them, it changes for everybody.

## Problems

- **No Multiplayer button / a message saying a file is missing:** `bmpShare.pkm` is missing or from another version
  than your `dinput8.dll`. Install both from the same release.
- **"This mod needs one more file" on the main menu:** you have the mod file but not the DLL. Install the zip above.
- **"Different mod version":** you and the host run different versions. Install the same release.
- **Something went wrong:** click the **bug** button (in the Multiplayer window or the bar), write what you were
  doing and click **Send**, or **Save** to keep the report as a zip and attach it to a
  [GitHub issue](../../issues). After a crash the game offers to send a report the next time you start it.
  Names, Steam IDs and addresses are removed from the logs in a report; a crash report also contains the game's crash
  file (`crash.dmp`), which can hold bits of text the game had in memory. See [what a report contains](docs/BUG_REPORTS.md).

The mod checks once per start whether a newer release is out and says so on the main menu. To switch that off, add
the line `updatecheck=0` to `Banished\bmp_settings.txt`.

## For developers

- [Building the mod](docs/BUILDING.md): the DLL needs Visual Studio; the mod file needs the Banished Mod Kit.
- [Running a relay server](docs/RELAY.md) and [the bug report receiver](docs/BUG_REPORTS.md).
- [How it works](docs/HOW_IT_WORKS.md): lockstep, determinism, towns.
- [Testing](docs/TESTING.md): the multi-instance test tools.
- [Making a release](docs/RELEASING.md), including the Steam Workshop upload.

Third-party code: see [THIRD_PARTY.md](THIRD_PARTY.md).

## License

[MIT](LICENSE). Banished is (c) Shining Rock Software LLC; this project is not affiliated with it.
