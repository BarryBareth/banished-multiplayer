# Bug reports

The **bug** button (Multiplayer window and the in-game bar) opens a small window: the player writes what they were
doing, then clicks **Send** (uploads the report) or **Save** (puts it on the Desktop as
`BanishedMP-bug-<time>.zip`, e.g. to attach to a GitHub issue). After a crash, the game offers the same window once,
the next time it starts.

## What a report contains

A zip with:

| File | |
|---|---|
| `report.txt` | type (bug / crash), time (UTC), mod build, a session id (the same on every player's report of one game), the multiplayer state, the Windows build, the graphics setup, the player's text |
| `bmp.log`, `bmp_prev.log` | the mod's log of this and the previous run (last 3 MB each) |
| `bmp_commands.log` | the players' recorded actions (last 1 MB) |
| `bmp_desync_*.log` | the two newest "out of sync" records of the last two days, if any |
| `crash.dmp` | the game's own crash file: after a crash, or if one is less than a day old |

Before anything leaves the PC, the text files are cleaned: the Windows user name, every player name, Steam IDs, other
long numeric IDs, IP addresses (except 127.0.0.1), room codes and join/relay addresses are replaced by placeholders.
`crash.dmp` is a small memory snapshot of the game's threads made by the game itself; it isn't cleaned and can contain
bits of text the game had in memory. Players who want to check first can use **Save** and look inside the zip.

## Where Send goes

The address is `kDefaultUrl` in [`src/report.cpp`](../src/report.cpp). A player (or a fork) can send elsewhere with a
line in `Banished\bmp_settings.txt`:

```
bugurl=https://your.server/report
```

If sending fails, the report is saved to the Desktop instead.

## Running a receiver

[`tools/bmp_bugs.py`](../tools/bmp_bugs.py): Python 3 standard library only.

```
POST /report   body = the report zip (header X-BMP-Build)  ->  200 {"id": "20261016-124322-43eeaa"}
GET  /health   ->  200 {"ok": true}
GET  /latest.json, /download/BanishedMultiplayer-<version>.zip   (optional, see below)
```

It accepts at most 25 MB per report and 6 reports per hour per client, keeps at most 2 GB, and only stores zips that
contain `report.txt`. It listens on **127.0.0.1 only**: put a reverse proxy with HTTPS in front of it (nginx, Caddy,
a Cloudflare tunnel, ...). The per-client limit uses `CF-Connecting-IP` / `X-Forwarded-For`, so don't expose the port
directly.

On Linux with systemd, from a copy of this repository:

```
cd server/bugs
sudo sh install.sh
```

Reports land in `/var/lib/bmp-bugs/reports/<id>.zip`; `journalctl -u bmp-bugs -f` shows one line per report.

### Phone notifications (optional)

Put `BMP_BUGS_NTFY=https://ntfy.sh/<a long random topic>` into `/etc/bmp-bugs.env` and restart the service: every
report then sends a push to that [ntfy](https://ntfy.sh) topic (subscribe to it in the ntfy app). The push holds only
the report type, its id, the build and the size - never the player's text. Anyone who knows the topic can read and send
to it, so keep it secret.

### Serving your own update info (optional)

The mod's update check normally reads this repository's newest GitHub release. A player can point it at your
receiver instead with `updateurl=https://your.server/latest.json` in `bmp_settings.txt`; the receiver serves
`latest.json` (`{"version": "0.9.1", "url": "https://...", "notes": "..."}`) and release zips from its `--public`
folder.
