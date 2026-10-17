"""Bug report receiver for Banished Multiplayer (the mod's "Send" button, src/report.cpp).

  python -I tools/bmp_bugs.py --port 8781 --dir /opt/bmp-bugs/reports [--ntfy https://ntfy.example/topic]

POST /report  body = the report .zip (Content-Type application/zip, header X-BMP-Build)  ->  200 {"id": "..."}
GET  /health  -> 200 ok
GET  /latest.json, /download/<file>  -> the release info and zips in --public (tools/make_release.ps1 makes them,
     deploy/bugs/publish_release.ps1 puts them there); the DLL's update check reads /latest.json

Meant to sit behind a reverse proxy / Cloudflare tunnel (listens on 127.0.0.1 only by default). Limits: 25 MB per
report, 6 reports per hour per client (CF-Connecting-IP / X-Forwarded-For / socket address), 2 GB in the folder;
the body must be a zip that holds report.txt. Standard library only."""
import argparse, io, json, os, re, secrets, sys, threading, time, urllib.request, zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

MAX_BYTES = 25 * 1024 * 1024
PER_HOUR = 6
DIR_CAP = 2 * 1024 * 1024 * 1024

args = None
lock = threading.Lock()
recent = {}  # client -> [timestamps]


def client_of(h):
    for key in ('CF-Connecting-IP', 'X-Forwarded-For'):
        v = h.headers.get(key)
        if v:
            return v.split(',')[0].strip()
    return h.client_address[0]


def folder_size(d):
    return sum(e.stat().st_size for e in os.scandir(d) if e.is_file())


def notify(text):
    if not args.ntfy:
        return
    try:
        urllib.request.urlopen(urllib.request.Request(args.ntfy, data=text.encode(), headers={'Title': 'Banished MP', 'Tags': 'bug'}), timeout=10)
    except Exception as e:
        print('ntfy failed:', e, flush=True)


class Handler(BaseHTTPRequestHandler):
    server_version = 'bmp-bugs/1'

    def reply(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == '/health':
            return self.reply(200, {'ok': True})
        name = 'latest.json' if self.path == '/latest.json' else self.path[10:] if self.path.startswith('/download/') else ''
        # plain file names only (no paths): latest.json and the release zips
        if not re.fullmatch(r'latest\.json|BanishedMultiplayer-[0-9.]+\.zip', name):
            return self.reply(404, {'error': 'not found'})
        path = os.path.join(args.public, name)
        if not os.path.isfile(path):
            return self.reply(404, {'error': 'not found'})
        with open(path, 'rb') as f:
            body = f.read()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json' if name.endswith('.json') else 'application/zip')
        if name.endswith('.json'):
            self.send_header('Cache-Control', 'no-cache')
        else:
            self.send_header('Content-Disposition', f'attachment; filename="{name}"')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        if self.path != '/report':
            return self.reply(404, {'error': 'not found'})
        try:
            n = int(self.headers.get('Content-Length', '0'))
        except ValueError:
            n = 0
        if n <= 0 or n > MAX_BYTES:
            return self.reply(413, {'error': 'size'})
        who = client_of(self)
        now = time.time()
        with lock:
            stamps = [t for t in recent.get(who, []) if now - t < 3600]
            if len(stamps) >= PER_HOUR:
                return self.reply(429, {'error': 'too many reports, try later'})
            stamps.append(now)
            recent[who] = stamps
        data = self.rfile.read(n)
        try:
            z = zipfile.ZipFile(io.BytesIO(data))
            names = z.namelist()
            head = z.read('report.txt').decode('utf-8', 'replace')
        except Exception:
            return self.reply(400, {'error': 'not a report'})
        with lock:
            if folder_size(args.dir) + n > DIR_CAP:
                return self.reply(507, {'error': 'storage full'})
            rid = time.strftime('%Y%m%d-%H%M%S', time.gmtime()) + '-' + secrets.token_hex(3)
            with open(os.path.join(args.dir, rid + '.zip'), 'wb') as f:
                f.write(data)
        kind = next((l[6:] for l in head.splitlines() if l.startswith('kind: ')), '?')
        build = self.headers.get('X-BMP-Build', '?')[:40]
        print(f'{rid} {kind} build {build} {n // 1024} KB files {len(names)}', flush=True)
        # only the report's type, number and build go out (ntfy.sh is a public server): no player text
        notify(f'{"Crash" if kind == "crash" else "Bug"} report {rid} - build {build}, {n // 1024} KB')
        self.reply(200, {'id': rid})

    def log_message(self, fmt, *a):
        pass  # one line per stored report above is enough


def main():
    global args
    p = argparse.ArgumentParser()
    p.add_argument('--port', type=int, default=8781)
    p.add_argument('--bind', default='127.0.0.1')
    p.add_argument('--dir', default='reports')
    p.add_argument('--public', default='public')
    p.add_argument('--ntfy', default=os.environ.get('BMP_BUGS_NTFY', ''))
    args = p.parse_args()
    os.makedirs(args.dir, exist_ok=True)
    print(f'bmp-bugs on {args.bind}:{args.port}, reports in {os.path.abspath(args.dir)}', flush=True)
    ThreadingHTTPServer((args.bind, args.port), Handler).serve_forever()


if __name__ == '__main__':
    sys.exit(main())
