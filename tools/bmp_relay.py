"""Banished Multiplayer relay server: lets players meet when nobody can accept incoming connections.

The host's game connects OUT to this server and opens a room; friends connect OUT too and join the room by its code.
The relay only forwards text lines - the host's game stays the authority (lockstep, saves, resyncs all run there).

Protocol (one text line each, '\\n'-terminated):
  host  -> relay  RELAY HOST <room> [<token>]  relay -> host   RELAY OK <room> | RELAY ERR <reason>
                 (a host that lost its connection reopens its room with the same token: the stale room - and its
                 guests, who reconnect by themselves - is closed first)
  guest -> relay  RELAY JOIN <room>            relay -> guest  RELAY OK <room> | RELAY ERR <reason>
                                               relay -> host   RELAY CONN <id>
  guest -> relay  <line>                       relay -> host   RELAY FROM <id> <line>
  host  -> relay  RELAY TO <id> <line>         relay -> guest  <line>
  host  -> relay  RELAY KICK <id>              (guest disconnected)
  guest disconnects                            relay -> host   RELAY DROP <id>
  host disconnects                             every guest of the room is disconnected

Public-use limits (all configurable): connections and rooms per IP address, a handshake deadline, an idle timeout,
a line length cap and a traffic cap per connection. Every minute a stats line is logged.

Usage:  python3 -I bmp_relay.py [--port 7780] [--max-rooms 500]
Players need TCP access to the port (open/forward it on the machine running this).
"""
import argparse
import asyncio
import collections
import logging
import time

MAX_GUESTS = 16


class Limits:
    line = 1024 * 1024          # bytes per line (save transfer lines are ~60 KB)
    per_ip = 12                 # concurrent connections per IP address
    rooms_per_ip = 3
    handshake = 15              # seconds to send RELAY HOST/JOIN
    idle = 30 * 60              # seconds without any line before a connection is dropped
    rate = 4 * 1024 * 1024      # bytes per second per connection (average over 10 s)
    max_rooms = 500


class Room:
    def __init__(self, code, host_writer, ip, token=''):
        self.code = code
        self.token = token
        self.host = host_writer
        self.ip = ip
        self.guests = {}               # id -> writer
        self.next_id = 1


rooms = {}                             # code -> Room
conns_per_ip = collections.Counter()
stats = collections.Counter()


def send(writer, line):
    if writer is None or writer.is_closing():
        return
    data = line.encode('utf-8', 'replace') + b'\n'
    stats['bytes_out'] += len(data)
    writer.write(data)


async def drain(writer):
    try:
        await asyncio.wait_for(writer.drain(), 30)
    except (ConnectionError, RuntimeError, asyncio.TimeoutError):
        writer.close()


class Reader:
    """readline with an idle timeout and a per-connection traffic budget."""
    def __init__(self, reader):
        self.r = reader
        self.window = time.monotonic()
        self.used = 0

    async def line(self, timeout):
        data = await asyncio.wait_for(self.r.readline(), timeout)
        stats['bytes_in'] += len(data)
        now = time.monotonic()
        if now - self.window > 10:
            self.window, self.used = now, 0
        self.used += len(data)
        if self.used > Limits.rate * 10:
            raise ConnectionError('traffic limit')
        return data


async def handle(reader, writer):
    peer = writer.get_extra_info('peername')
    ip = peer[0] if peer else '?'
    if conns_per_ip[ip] >= Limits.per_ip:
        writer.close()
        stats['refused_ip'] += 1
        return
    conns_per_ip[ip] += 1
    stats['connections'] += 1
    r = Reader(reader)
    try:
        try:
            first = (await r.line(Limits.handshake)).decode('utf-8', 'replace').strip()
        except (asyncio.TimeoutError, asyncio.LimitOverrunError, ValueError, ConnectionError):
            return
        parts = first.split(' ')
        if len(parts) in (3, 4) and parts[0] == 'RELAY' and parts[1] == 'HOST':
            await run_host(parts[2], r, writer, ip, parts[3] if len(parts) == 4 else '')
        elif len(parts) == 3 and parts[0] == 'RELAY' and parts[1] == 'JOIN':
            await run_guest(parts[2], r, writer, ip)
        else:
            send(writer, 'RELAY ERR expected RELAY HOST/JOIN')
            await drain(writer)
    finally:
        conns_per_ip[ip] -= 1
        if conns_per_ip[ip] <= 0:
            del conns_per_ip[ip]
        writer.close()


async def run_host(code, r, writer, ip, token=''):
    code = code.upper()
    old = rooms.get(code)
    if old is not None and token and old.token == token:  # the same host is back: drop the stale room
        rooms.pop(code, None)
        old.host.close()
        for g in list(old.guests.values()):
            g.close()
        logging.info('room %s reopened by its host (%d stale guests disconnected)', code, len(old.guests))
    if code in rooms:
        send(writer, 'RELAY ERR room code already in use')
    elif len(rooms) >= Limits.max_rooms:
        send(writer, 'RELAY ERR the relay is full - try again later')
    elif sum(1 for x in rooms.values() if x.ip == ip) >= Limits.rooms_per_ip:
        send(writer, 'RELAY ERR too many games from your address')
    elif not (3 <= len(code) <= 16 and code.isalnum()):
        send(writer, 'RELAY ERR bad room code')
    else:
        room = Room(code, writer, ip, token)
        rooms[code] = room
        stats['rooms_opened'] += 1
        logging.info('room %s opened by %s', code, ip)
        send(writer, 'RELAY OK ' + code)
        await drain(writer)
        try:
            while True:
                line = await r.line(Limits.idle)
                if not line:
                    break
                text = line.decode('utf-8', 'replace').rstrip('\r\n')
                if text.startswith('RELAY TO '):
                    _, _, gid, *rest = text.split(' ', 3)
                    g = room.guests.get(int(gid)) if gid.isdigit() else None
                    if g is not None:
                        send(g, rest[0] if rest else '')
                        await drain(g)
                elif text.startswith('RELAY KICK '):
                    gid = text[11:]
                    g = room.guests.pop(int(gid), None) if gid.isdigit() else None
                    if g is not None:
                        g.close()
        except (ConnectionError, asyncio.TimeoutError, asyncio.LimitOverrunError, ValueError):
            pass
        finally:
            if rooms.get(code) is room:  # (a reopened room belongs to the new connection)
                rooms.pop(code, None)
            for g in list(room.guests.values()):
                g.close()
            logging.info('room %s closed (%d guests disconnected)', code, len(room.guests))
    await drain(writer)


async def run_guest(code, r, writer, ip):
    room = rooms.get(code.upper())
    if room is None:
        send(writer, 'RELAY ERR no game with that room code - check the code and that the host is waiting')
        await drain(writer)
        return
    if len(room.guests) >= MAX_GUESTS:
        send(writer, 'RELAY ERR the game is full')
        await drain(writer)
        return
    gid = room.next_id
    room.next_id += 1
    room.guests[gid] = writer
    stats['guests_joined'] += 1
    logging.info('room %s: guest %d joined from %s', room.code, gid, ip)
    send(writer, 'RELAY OK ' + room.code)
    send(room.host, 'RELAY CONN %d' % gid)
    await drain(writer)
    await drain(room.host)
    try:
        while True:
            line = await r.line(Limits.idle)
            if not line:
                break
            send(room.host, 'RELAY FROM %d %s' % (gid, line.decode('utf-8', 'replace').rstrip('\r\n')))
            await drain(room.host)
    except (ConnectionError, asyncio.TimeoutError, asyncio.LimitOverrunError, ValueError):
        pass
    finally:
        if room.guests.pop(gid, None) is not None:
            send(room.host, 'RELAY DROP %d' % gid)
            await drain(room.host)
        logging.info('room %s: guest %d left', room.code, gid)


async def report():
    while True:
        await asyncio.sleep(60)
        logging.info('stats: %d rooms, %d guests, %d connections from %d addresses; total in %.1f MB out %.1f MB, '
                     'refused %d', len(rooms), sum(len(x.guests) for x in rooms.values()), sum(conns_per_ip.values()),
                     len(conns_per_ip), stats['bytes_in'] / 1e6, stats['bytes_out'] / 1e6, stats['refused_ip'])


async def main():
    ap = argparse.ArgumentParser(description='Banished Multiplayer relay server')
    ap.add_argument('--port', type=int, default=7780)
    ap.add_argument('--bind', default='0.0.0.0')
    ap.add_argument('--max-rooms', type=int, default=Limits.max_rooms)
    ap.add_argument('--per-ip', type=int, default=Limits.per_ip, help='concurrent connections per IP address')
    a = ap.parse_args()
    Limits.max_rooms, Limits.per_ip = a.max_rooms, a.per_ip
    logging.basicConfig(level=logging.INFO, format='%(asctime)s %(message)s')
    server = await asyncio.start_server(handle, a.bind, a.port, limit=Limits.line)
    logging.info('relay listening on %s:%d (max %d rooms, %d connections per address)', a.bind, a.port, Limits.max_rooms,
                 Limits.per_ip)
    asyncio.get_running_loop().create_task(report())
    async with server:
        await server.serve_forever()


if __name__ == '__main__':
    asyncio.run(main())
