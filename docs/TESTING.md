# Testing

Determinism bugs only show when two games are compared, so the tools run **several game instances on one PC** and
compare everything they did, tick by tick. The game window(s) open on your screen while a test runs; don't use the
mouse on them.

Setup once: `steam_appid.txt` (containing `242920`) in the Banished folder lets a second instance start without
Steam relaunching it (`nettest.ps1` creates it). Close your own Banished first: the scripts refuse to run otherwise.

## nettest.ps1: two or three players on one PC

```
.\tools\nettest.ps1 -InGameHost -Ticks 20000
```

Slot 0 hosts (in-process, TCP 7777), slot 1 joins, both play the save given with `-Save` (the file name without
`.sav` from `Documents\Banished\Save`; the default `Save0` is probably not one of yours) or a new map with
`-Separate` / `-SharedNew`, and at the end the tick logs are compared:

```
compare p0 vs p1:
  all draws                   : IDENTICAL
  per-draw log (AI action start/end): first 17096 identical
```

Useful switches (see the `param()` block at the top of the script for all):

| | |
|---|---|
| `-Players 3` | a third instance |
| `-Separate 'seed,size,start,terrain,climate'` | separate towns on a new map, e.g. `'12345,1,2,0,0'` (quote it); `-NoPick` places the towns automatically |
| `-SharedNew '...'` | one shared town on a new map |
| `-ResyncAt '3000,2000'` | the host forces resyncs (ticks restart in every new session) |
| `-LateJoin 30` | slot 1 joins 30 s after the host started (give `-Ticks` enough room) |
| `-DropAt1 T`, `-StallAt1 T` | slot 1 loses its connection / freezes for 6 s at tick T |
| `-Jitter` | slots 1+ scramble their memory layout like a different PC would - use it for any cross-PC desync |
| `-UiScript1 tests\uitests\uichaos.txt` | slot 1 clicks through windows and moves the camera (catches UI code that changes the simulation) |
| `-ViaRelay [-RelayAddr host:port]` | everything through a relay (a local one is started if no address is given) |
| `-Calm` | slot 1 idle instead of random clicks / speed changes |
| `-Names 'Ann,Bob'`, `-NoMod1`, `-FakeVersion1 X`, `-Extra 'key@1=value'` | names, a player without the mod file, a wrong version, any extra setting |

Logs land in `recon\runs\net\` (not part of the repository).

## uitest.ps1: the UI suite

```
.\tools\uitest.ps1
```

About 6 minutes: the host builds and uses workplaces, fields, orchards, pastures, the town hall and a trading post
through the game's own window handlers, the guest tries the same on the host's buildings (must be refused), and the
script checks the expected results and that both sims stayed identical. Ends with `UI TEST PASSED`. Run it after
changing anything that touches windows or commands.

UI scripts (`tests\uitests\*.txt`) are one step per line, `TICK verb arguments`; the verbs are documented at the top
of [`src/uiscript.cpp`](../src/uiscript.cpp) (build, open, press, state, expect, screenshot, mouse, browse, camtour,
leave, ...).

## autotest.ps1: one PC, two runs

```
.\tools\autotest.ps1 -Ticks 20000
```

Runs the same save twice (idle vs. random clicks, camera and speed changes) and compares - the original determinism
test, without networking.

## When games drift apart

Every 600 ticks the players compare a hash of the simulation's random draws (a mismatch triggers a resync) and of
its memory layout (a mismatch is logged as a "sim-heap drift", without a resync). Both write their recent history to
`Banished\bmp_desync_<time>.log`. Compare two players' files with

```
python -I tools\ringdiff.py host.log guest.log
```

which prints the first record that differs (an RNG draw, a job offer, an AI action, an allocation) with its code
address. `!! sim memory freed outside the simulation` in `bmp.log` means window or camera code changed simulation
data on one PC only - the usual cause of drifts.
