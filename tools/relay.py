"""Lockstep relay for the Banished multiplayer mod.

Usage: python -I relay.py [--port 7777] [--players 2] [--host 127.0.0.1]

Line protocol (UTF-8, '\n'-terminated):
  client -> relay   HELLO <name>
  relay  -> client  WELCOME <playerId> <playerCount>
  relay  -> all     START                         (once every expected player has joined)
  client -> relay   CMD <slot> <command line>     (a bmp_commands.log-format line without the leading slot)
  relay  -> all     CMD <player> <seq> <slot> <command line>   (global order = seq; echoed to the sender too)
  client -> relay   TURN <tick>                   (promise: no more CMDs with slot <= tick from this client)
  relay  -> all     TURN <player> <tick>
  client -> relay   CTL speed <n> | CTL pause <0|1>
  relay  -> all     CTL <player> speed <n> | CTL <player> pause <0|1>
  client -> relay   HASH <tick> <hash>
  relay  -> all     HASH <player> <tick> <hash>
The relay never interprets game state; it only orders and fans out messages."""
import asyncio, argparse, time

class Relay:
    def __init__(self, players):
        self.expected = players
        self.clients = {}          # player id -> writer
        self.seq = 0
        self.started = False
        self.hashes = {}           # tick -> {player: hash}

    def log(self, msg):
        print(time.strftime('%H:%M:%S'), msg, flush=True)

    async def send_all(self, line):
        data = (line + '\n').encode()
        for w in list(self.clients.values()):
            try:
                w.write(data)
                await w.drain()
            except Exception:
                pass

    async def handle(self, reader, writer):
        pid = None
        try:
            hello = (await reader.readline()).decode().strip()
            if not hello.startswith('HELLO'):
                return
            pid = next(i for i in range(64) if i not in self.clients)
            self.clients[pid] = writer
            writer.write(f'WELCOME {pid} {self.expected}\n'.encode())
            await writer.drain()
            self.log(f'player {pid} joined ({hello[6:]}), {len(self.clients)}/{self.expected}')
            if not self.started and len(self.clients) >= self.expected:
                self.started = True
                self.log('all players present -> START')
                await self.send_all('START')
            while True:
                line = (await reader.readline()).decode().rstrip('\n')
                if not line:
                    break
                kind, _, rest = line.partition(' ')
                if kind == 'CMD':
                    self.seq += 1
                    await self.send_all(f'CMD {pid} {self.seq} {rest}')
                elif kind == 'TURN':
                    await self.send_all(f'TURN {pid} {rest}')
                elif kind == 'CTL':   # shared speed / pause: 'CTL speed <n>' / 'CTL pause <0|1>'
                    await self.send_all(f'CTL {pid} {rest}')
                elif kind == 'HASH':
                    tick, h = rest.split()
                    seen = self.hashes.setdefault(int(tick), {})
                    seen[pid] = h
                    if len(seen) == len(self.clients):
                        ok = len(set(seen.values())) == 1
                        self.log(f'tick {tick}: hashes {"MATCH" if ok else "DESYNC " + str(seen)}')
                        del self.hashes[int(tick)]
                    await self.send_all(f'HASH {pid} {rest}')
        except (ConnectionError, asyncio.IncompleteReadError):
            pass
        finally:
            if pid is not None and self.clients.get(pid) is writer:
                del self.clients[pid]
                self.log(f'player {pid} left')
            writer.close()

async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=7777)
    ap.add_argument('--players', type=int, default=2)
    a = ap.parse_args()
    relay = Relay(a.players)
    server = await asyncio.start_server(relay.handle, a.host, a.port)
    relay.log(f'relay listening on {a.host}:{a.port}, waiting for {a.players} players')
    async with server:
        await server.serve_forever()

if __name__ == '__main__':
    asyncio.run(main())
