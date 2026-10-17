# Texts for mod sites

Copy-paste texts for the places the mod is published. The Steam Workshop page text is in [WORKSHOP.md](WORKSHOP.md).
Pictures: `mod/assets/preview.jpg` (square, 512 px) and `mod/assets/icon_large.png`; add your own screenshots too.

Upload the **release zip** (`BanishedMultiplayer-<version>.zip` from the GitHub release) on Nexus Mods and
banishedinfo.com. The Steam Workshop only takes `bmpShare.pkm` (uploaded from inside the game).

## Short summary (Nexus "brief overview", max 255 characters)

```
Multiplayer for Banished: share one town or each build your own town on the same map, with trading and shared storage. Steam, relay server or direct connection. Open source.
```

## Nexus Mods (BBCode)

Suggested category: *Gameplay* (or *Utilities*). Requirements: none besides the game.

```
[size=5][b]Banished Multiplayer[/b][/size]

Play Banished together with other players:
[list]
[*][b]Share one town[/b] - everybody runs the same town together.
[*][b]Separate towns[/b] - everybody builds their own town on the same map, and you can trade goods, share storage buildings and lend each other workplaces.
[/list]

Every player's game runs the whole simulation in sync and only the players' actions travel over the network. If the games ever drift apart, the host's game is reloaded on everyone's PC automatically. Players can join a game that's already running.

[size=4][b]Install[/b][/size]
[list=1]
[*]Close Banished.
[*]Unpack the zip anywhere and double-click [b]Install.bat[/b]. It copies dinput8.dll into the Banished folder and bmpShare.pkm into Banished\WinData.
[*]Start Banished. The main menu now has a [b]Multiplayer[/b] button.
[/list]
To uninstall, run Uninstall.bat. To install by hand, copy the two files to the places above.

[b]Requirements:[/b] Banished 1.0.7 (64-bit), Windows 10/11. Everybody in a game needs the same version of the mod - the game tells you if someone's differs.

[size=4][b]Connecting[/b][/size]
[list]
[*][b]Steam[/b] - invite players through Steam; nobody's IP address is shared.
[*][b]Relay server[/b] - one person in your group runs [b]BanishedRelay.exe[/b] (included) and everybody connects to it with a room code. Handy when the host can't forward a port.
[*][b]Direct (IP)[/b] - the host forwards TCP port 7777 and the others type the host's address.
[/list]

[size=4][b]About the DLL[/b][/size]
Banished's mod kit can't add code, so the multiplayer is a DLL (dinput8.dll) that the game loads at start-up and that hooks into the game. Antivirus programs sometimes warn about DLLs that hook into games; the full source code is on GitHub:
[url=https://github.com/BarryBareth/banished-multiplayer]github.com/BarryBareth/banished-multiplayer[/url]

[size=4][b]Bugs[/b][/size]
Use the bug button in the Multiplayer window or the in-game bar ([b]Send[/b] or [b]Save[/b] a report), or open an issue on GitHub. The mod tells you on the main menu when a new version is out.

Not affiliated with Shining Rock Software.
```

## Plain text (banishedinfo.com, forums, anywhere)

```
BANISHED MULTIPLAYER

Play Banished together with other players:
- Share one town: everybody runs the same town together.
- Separate towns: everybody builds their own town on the same map - trade goods, share storage buildings and lend each other workplaces.

Every player's game runs the whole simulation in sync; only the players' actions travel over the network. If the games ever drift apart, the host's game is reloaded on everyone's PC automatically. Players can join a game that's already running.

INSTALL
1. Close Banished.
2. Unpack the zip anywhere and double-click Install.bat (it copies dinput8.dll into the Banished folder and bmpShare.pkm into Banished\WinData).
3. Start Banished. The main menu now has a Multiplayer button.
Uninstall: run Uninstall.bat.

Needs Banished 1.0.7 (64-bit) and Windows 10/11. Everybody in a game needs the same version of the mod.

CONNECTING
- Steam: invite players through Steam; nobody's IP address is shared.
- Relay server: one person runs BanishedRelay.exe (included) and everybody connects to it with a room code.
- Direct (IP): the host forwards TCP port 7777; the others type the host's address.

The multiplayer is a DLL that hooks into the game (Banished's mod kit can't add code). Antivirus programs sometimes warn about that; the full source code is at https://github.com/BarryBareth/banished-multiplayer

Bugs: use the bug button in the game, or open an issue on GitHub.
Not affiliated with Shining Rock Software.
```
