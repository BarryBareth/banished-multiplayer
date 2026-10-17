"""Compare two determinism-probe runs (last session in each).
Usage: python -I compare_ticks.py <ticksA.log> <ticksB.log> [session [sessionB]]   (session: -1 = last (default), -2 = the one before...)
Reports the first diverging tick for all draws and for sim-only draws (handle tags excluded), villager positions
(pos= / p= fields, newer logs), and if matching bmp_draws_*.log files exist, the first differing individual draw.
Only the requested session is kept in memory (logs of long sessions are hundreds of MB)."""
import sys, os

def session_count(path):
    with open(path) as f:
        return sum(1 for line in f if line.startswith('# session'))

def load(path, sess):
    want = sess if sess >= 0 else session_count(path) + sess
    cur, idx = None, -1
    with open(path) as f:
        for line in f:
            if line.startswith('# session'):
                idx += 1
                if idx > want: break
                cur = [] if idx == want else None
            elif cur is not None and line.startswith('#h '):  # per-tick terrain-height queries (newer logs)
                h = line.split()
                if cur and int(h[1]) == cur[-1]['tick']: cur[-1]['heights'] = ' '.join(h[2:4])
            elif cur is not None and not line.startswith('#'):
                p = line.split()
                row = {'tick': int(p[0]), 'calls': int(p[1]), 'hash': p[2], 'line': line.strip()[:200]}
                if len(p) > 6 and p[4] == 'sim':
                    row.update(simcalls=int(p[5]), simhash=p[6])
                if 'em=' in line:
                    em = line[line.index('em='):].strip()
                    row['em'] = em.split(' pos=')[0]
                if ' pos=' in line:
                    rest = line[line.index(' pos=') + 5:].split()
                    row['pos'] = rest[0]
                    row['p'] = rest[1][2:].split(',') if len(rest) > 1 and rest[1].startswith('p=') else []
                cur.append(row)
    return cur or []

def first_diff(ta, tb, key):
    n = min(len(ta), len(tb))
    for i in range(n):
        if ta[i].get(key) != tb[i].get(key):
            return i
    return None

def draws(path, sess=-1):
    if not os.path.exists(path): return None
    want = sess if sess >= 0 else session_count(path) + sess
    out, idx = None, -1
    with open(path) as f:
        for l in f:
            if l.startswith('# session'):
                idx += 1
                if idx > want: break
                out = [] if idx == want else None
            elif out is not None and not l.startswith('#'): out.append(l.split())
    return out or None

def heapkeys(rows):
    """comparison keys like the game's drift check: sim-heap allocations (M) by tick + size + offset (not the calling
    code - a pointer-keyed tree inserts the same node from two call sites), and a run of in-sim frees (X) between two
    allocations as a set (reuse is lowest-address-first, so their order doesn't matter)"""
    keys = [None] * len(rows)
    i = 0
    while i < len(rows):
        r = rows[i]
        if r[1] == 'X':
            j = i
            while j < len(rows) and rows[j][1] == 'X' and rows[j][0] == r[0]: j += 1
            run = tuple(sorted(x[3] for x in rows[i:j]))
            for k in range(i, j): keys[k] = (r[0], 'X', run)
            i = j
            continue
        keys[i] = tuple(r[:4]) if r[1] == 'M' else tuple(r)
        i += 1
    return keys

def main(a, b, sess=-1, sessb=None):
    sessb = sess if sessb is None else sessb
    ta, tb = load(a, sess), load(b, sessb)
    n = min(len(ta), len(tb))
    print(f'compared {n} ticks (A has {len(ta)}, B has {len(tb)})')
    for key, label in [('hash', 'all draws'), ('simhash', 'sim draws (no handle tags)'), ('calls', 'draw counts'),
                       ('em', 'entity manager state'), ('pos', 'villager positions'), ('heights', 'terrain-height queries')]:
        if key in ('pos', 'heights') and not any(key in r for r in ta[:2]):
            continue
        i = first_diff(ta, tb, key)
        if i is None:
            print(f'  {label:28}: IDENTICAL')
            continue
        print(f'  {label:28}: diverged at tick {ta[i]["tick"]}')
        print(f'      A: {ta[i]["line"]}\n      B: {tb[i]["line"]}')
        if key == 'pos':
            pa, pb = ta[i].get('p', []), tb[i].get('p', [])
            diff = [k for k in range(min(len(pa), len(pb))) if pa[k].split('/')[0] != pb[k].split('/')[0]]  # x/y part
            print(f'      villagers: A {len(pa)}, B {len(pb)}; differing (town:index): {[pa[k].split(":")[0] if ":" in pa[k] else k for k in diff][:20]}')
            for k in diff[:5]:
                print(f'        #{k}: A {pa[k]}  B {pb[k]}')
    dpath = lambda p: p.replace('bmp_ticks_', 'bmp_draws_') if 'bmp_ticks_' in p else p.replace('_ticks.log', '_draws.log')
    da, db = draws(dpath(a), sess), draws(dpath(b), sessb)
    if da and db:
        for label, fa, fb in [('all draws', da, db),
                              ('sim draws', [d for d in da if d[1] == 'S'], [d for d in db if d[1] == 'S']),
                              ('in-sim entity add/remove', [d for d in da if d[1] in 'ER'], [d for d in db if d[1] in 'ER']),
                              ('AI action start/end', [d for d in da if d[1] in 'az'], [d for d in db if d[1] in 'az']),
                              ('sim-heap allocations', [d for d in da if d[1] == 'M'], [d for d in db if d[1] == 'M']),
                              ('sim-heap frees in sim', [d for d in da if d[1] == 'X'], [d for d in db if d[1] == 'X'])]:
            m = min(len(fa), len(fb))
            ka, kb = heapkeys(fa), heapkeys(fb)
            j = next((k for k in range(m) if ka[k] != kb[k]), None)
            if j is None:
                print(f'  per-draw log ({label}): first {m} identical')
                continue
            print(f'  per-draw log ({label}): first difference at #{j} (tick kind caller value | stack):')
            for k in range(max(0, j - 4), min(m, j + 5)):
                mark = '>>' if k == j else '  '
                print(f'    {mark} A {" ".join(fa[k]):62} B {" ".join(fb[k])}')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else -1,
         int(sys.argv[4]) if len(sys.argv) > 4 else None)  # optional 4th: B's session if it differs
