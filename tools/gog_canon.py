"""Rewrite the code ids in a GOG player's ring dump (bmp_desync_*.log) as Steam-build ids, so tools/ringdiff.py can
compare it with a Steam player's. Code ids: 1xxxxxxx = exe+x, 2xxxxxxx = engine DLL+x. Rewritten: the caller column
of draw records, the caller (column a, decimal) of sim-heap allocation records (tag 4d) and the stack after '|'.
Each id moves through the function that contains it (recon/gog/funcmap.tsv for the exe, funcmap_rt.tsv for the engine
DLL, from tools/gog_match.py); addresses outside matched functions are kept.
    python -I tools/gog_canon.py IN.log OUT.log"""
import bisect, os, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def back_map(path):
    rows = sorted((int(p[2], 16), int(p[3], 16), int(p[0], 16), int(p[1], 16))
                  for p in (l.split() for l in open(path)))
    return rows, [r[0] for r in rows]


maps = {1: back_map(os.path.join(root, 'recon', 'gog', 'funcmap.tsv')),
        2: back_map(os.path.join(root, 'recon', 'gog', 'funcmap_rt.tsv'))}


def canon(v):
    kind, a = v >> 28, v & 0xfffffff
    if kind not in maps:
        return v
    rows, starts = maps[kind]
    i = bisect.bisect_right(starts, a) - 1
    if i < 0 or not rows[i][0] <= a < rows[i][1]:
        return v
    g0, g1, s0, s1 = rows[i]
    return (kind << 28) | min(s0 + (a - g0), s1 - 1)


with open(sys.argv[1], errors='replace') as f, open(sys.argv[2], 'w') as o:
    for line in f:
        if line.startswith('#') or '|' not in line:
            o.write(line)
            continue
        head, stack = line.rstrip('\n').split('|', 1)
        t = head.split()
        if len(t) >= 5:
            tag = int(t[1], 16)
            if tag >> 28 in (1, 2):
                t[1] = '%08x' % canon(tag)
            elif t[1].startswith(('4d', '58')):
                t[3] = str(canon(int(t[3])))
        st = ' '.join('%08x' % canon(int(x, 16)) for x in stack.split())
        o.write(' '.join(t) + ' | ' + st + '\n')
