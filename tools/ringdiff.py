"""Diff two desync/drift ring dumps (bmp_desync_*.log) from different PCs: first differing record + context.
Only the last session in each file counts (records after the last tick decrease).
  python -I tools/ringdiff.py A.log B.log [context]
Tags (caller column): 41/5a action start/end, 4a job offer, 49 idle pick, 50 profession, 54 theft watch,
4d sim-heap alloc (value = offset, a = caller code id), 58 sim-heap free in sim; others = RNG draws (caller code id).
Compared like the drift check: allocations by size + offset (not the calling code: the same allocation can come from
two insert sites of a pointer-keyed tree), and a run of frees between two allocations as a set (reuse is lowest-address-
first, so their order doesn't matter)."""
import sys
def load(p):
    out = []
    with open(p, errors='replace') as f:
        for l in f:
            if l.startswith('#') or not l.strip(): continue
            out.append(l.rstrip('\n'))
    last = 0
    for i in range(1, len(out)):
        if int(out[i].split()[0]) < int(out[i - 1].split()[0]): last = i
    return out[last:]
def norm(lines):
    """comparison keys, one per line: frees in a run get the run's sorted content, allocations drop caller + stack"""
    keys = [None] * len(lines)
    i = 0
    while i < len(lines):
        t = lines[i].split()
        if len(t) > 2 and t[1] == '58000000':
            j = i
            while j < len(lines) and lines[j].split()[1] == '58000000' and lines[j].split()[0] == t[0]: j += 1
            run = ' '.join(sorted(l.split()[2] for l in lines[i:j]))
            for k in range(i, j): keys[k] = (t[0], 'frees', run, k - i)
            i = j
            continue
        keys[i] = (t[0], t[1], t[2]) if len(t) > 2 and t[1].startswith('4d') else lines[i]
        i += 1
    return keys
a, b = load(sys.argv[1]), load(sys.argv[2])
ka, kb = norm(a), norm(b)
ctx = int(sys.argv[3]) if len(sys.argv) > 3 else 15
ta = int(a[0].split()[0]); tb = int(b[0].split()[0])
start = max(ta, tb) + 1
ia = next(i for i, l in enumerate(a) if int(l.split()[0]) >= start)
ib = next(i for i, l in enumerate(b) if int(l.split()[0]) >= start)
while int(a[ia].split()[0]) != int(b[ib].split()[0]):
    if int(a[ia].split()[0]) < int(b[ib].split()[0]): ia += 1
    else: ib += 1
print('A from tick', a[ia].split()[0], 'to', a[-1].split()[0], '| B from', b[ib].split()[0], 'to', b[-1].split()[0])
n = min(len(a) - ia, len(b) - ib)
for k in range(n):
    if ka[ia + k] != kb[ib + k]:
        print('first difference at entry', k)
        for j in range(max(0, k - ctx), min(n, k + 10)):
            m = '>>' if j == k else '  '
            print(m, 'A', a[ia + j]); print('   B', b[ib + j])
        break
else:
    print('identical for', n)
