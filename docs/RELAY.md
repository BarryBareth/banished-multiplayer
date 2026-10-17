# Running a relay server

A relay lets players meet when nobody can accept incoming connections: the host's game and every other player's
game connect **out** to the relay, and the relay forwards their messages. It's the easiest way to play without
Steam or router setup.

The relay only passes text lines along. The host's game stays in charge of everything (lockstep, saves, resyncs),
so the relay needs very little: a small Linux server or VPS is plenty. A game with one resync moves about 10 MB
through it.

**Privacy:** players connect to the relay, not to each other, so players don't see each other's IP addresses. The
relay's operator does see the IP address of everyone who connects. Running a relay on your home connection
publishes your home IP address to everyone who uses it; use a VPS if you don't want that.

## What you need

- A machine with **Python 3.8 or newer** that players can reach from the internet: a VPS, or a PC/server at home with
  a port forward.
- **TCP port 7780** open (or any port you choose).

The relay is one file with no dependencies: [`tools/bmp_relay.py`](../tools/bmp_relay.py).

## Quick start (any OS)

```
python3 -I tools/bmp_relay.py --port 7780
```

Players then enter `your.server.address:7780` under **Relay address** in the Multiplayer window, choose **Relay
server**, and host or join with a room code.

## As a service on Linux (systemd)

From a copy of this repository on the server:

```
cd server/relay
sudo sh install.sh
```

This installs `/opt/bmp-relay/bmp_relay.py` and the service `bmp-relay` ([unit file](../server/relay/bmp-relay.service):
runs as a throwaway unprivileged user with a read-only view of the system, 300 MB memory cap), opens port 7780 in
`ufw` if it's active, and prints the server's LAN and public addresses.

```
journalctl -u bmp-relay -f        # logs: rooms opened/closed, a stats line per minute
sudo systemctl restart bmp-relay  # after updating bmp_relay.py
```

**At home:** forward TCP 7780 on your router to the server's LAN address, and give players your public address (or a
DNS name). With Cloudflare DNS, the record must be **DNS only** (grey cloud): Cloudflare's proxy and tunnels can't
carry this kind of raw TCP connection for ordinary players.

**On a VPS:** open TCP 7780 in the provider's firewall too, if it has one.

## Options

| Option | Default | |
|---|---|---|
| `--port` | 7780 | TCP port |
| `--bind` | 0.0.0.0 | address to listen on |
| `--max-rooms` | 500 | rooms open at the same time |
| `--per-ip` | 12 | connections at the same time from one IP address |

Built-in limits for a public relay (in the `Limits` class at the top of the file): 3 rooms per IP address, 16 guests
per room, 15 seconds to say host/join, 30 minutes idle timeout, 1 MB per line, 4 MB/s per connection.

## Protocol

One text line each, `\n`-terminated:

```
host  -> relay  RELAY HOST <room> [<token>]  relay -> host   RELAY OK <room> | RELAY ERR <reason>
guest -> relay  RELAY JOIN <room>            relay -> guest  RELAY OK <room> | RELAY ERR <reason>
                                             relay -> host   RELAY CONN <id>
guest -> relay  <line>                       relay -> host   RELAY FROM <id> <line>
host  -> relay  RELAY TO <id> <line>         relay -> guest  <line>
host  -> relay  RELAY KICK <id>
guest disconnects                            relay -> host   RELAY DROP <id>
host disconnects                             every guest of the room is disconnected
```

A host that lost its connection reopens its room with the same token; its guests reconnect by themselves.
