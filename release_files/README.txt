BANISHED MULTIPLAYER  (version {VERSION})
========================================

Play Banished together with other players: share one town, or each build your own town on the same map
(and trade with each other).

Needs: the Steam version of Banished (1.0.7, the normal 64-bit game). Everybody in a game needs the
same version of this mod.

Project page (downloads, instructions, source code): {REPO}


INSTALL
-------
1. Close Banished.
2. Unpack this zip anywhere and double-click Install.bat.
   It finds Banished by itself (or asks where it is) and copies two files:
     dinput8.dll   -> the Banished folder
     bmpShare.pkm  -> Banished\WinData
3. Start Banished from Steam. The main menu now has a Multiplayer button.

Windows may warn about Install.bat or the DLL because they're new and unsigned. The mod works by
loading dinput8.dll into the game; that is also why antivirus programs sometimes ask about it.

To install by hand, copy the two files to the places above.
To uninstall, double-click Uninstall.bat (or delete the two files).


PLAYING
-------
Main menu -> Multiplayer:

  Share one town    everybody runs the same town together
  Separate towns    everybody builds their own town on one map. "Players choose where their town
                    starts" lets each player pick their spot before the game begins.

  New town / Load town            (one shared town) start a fresh map or share one of the host's saves
  New map / Continue a saved game (separate towns) the same; a saved game with separate towns
                                  remembers which town is whose.

Connect through:
  Steam           easiest. Host, then "Invite players" (Steam overlay), or the others right-click
                  you in their Steam friends list -> Join Game.
  Relay server    the host gets a room code: copy it, send it to the other players, they paste it
                  under "Join a game". Everybody enters the same relay server under "Relay
                  address" - one somebody in your group runs (see RUNNING YOUR OWN RELAY below).
  Direct (IP)     the others type the host's IP address. The host's router must forward the port
                  (default 7777).


RUNNING YOUR OWN RELAY
----------------------
A relay helps when the host can't forward a port: everybody connects to the relay instead of to
the host. One person in your group - with a PC whose router they can set up, or a server - runs it:

1. Double-click BanishedRelay.exe (in this zip). It shows the address to give the others, e.g.
   203.0.113.5:7780. Leave the window open while you play; closing it stops the relay.
2. On that PC's router, forward TCP port 7780 to the PC ("on this network" address in the
   window), and click Allow when Windows Firewall asks.
3. Everybody (the relay's owner too) enters that address under "Relay address", chooses
   "Relay server", and the host hosts as usual.

Everyone who connects sees the relay PC's IP address. On a Linux server or VPS, use the Python
version and installer from the project page instead (docs/RELAY.md).

In the game the multiplayer bar (top of the screen) shows the players and has Trade, Invite,
Resync, Leave and the bug button.
  Trade           separate towns: offer goods to another player, once or every season/year.
  Share           storage barns, stockpiles and carts have a Share button: let another player's
                  villagers use them.
  Resync          if your games ever drift apart, this reloads everybody from the host's game. It
                  also happens by itself when the mod notices a problem.

Speed and pause work for everybody at once.


PROBLEMS
--------
- No Multiplayer button: bmpShare.pkm is missing or from another version - the game tells you
  at the main menu. Run Install.bat again.
- "Different mod version": you and the host have different versions. Both install the same zip.
- Something went wrong: press the bug button (the bug icon in the multiplayer window or bar),
  write what you were doing, and press Send. If the game crashed, it offers to send a report the
  next time you start it. The logs in a report have names, Steam IDs and addresses taken out. After a crash the
  report also holds the game's own crash file (crash.dmp, a small snapshot of the game's memory), which can contain
  bits of text the game had in memory. Use "Save" instead of "Send" to look at a report first.

The mod checks once per start whether a newer version is out and tells you at the main menu.
To switch that off, add the line  updatecheck=0  to Banished\bmp_settings.txt.
