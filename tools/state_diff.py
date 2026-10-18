"""Compare two villager-state dumps (bmp_state*.log from <game>\\bmp_statedump.txt, towns::DumpCitizens): the first tick
where they differ and which words (offset, both values, as hex and float).
    python -I tools/state_diff.py A.log B.log [max_lines]"""
import struct, sys


def load(p):
    out = {}
    for l in open(p):
        t = l.split()
        out[(int(t[0]), int(t[1]), int(t[2]), t[3])] = t[5:] if t[4].startswith('vt') else t[4:]
    return out


def f32(h):
    try:
        return '%.6g' % struct.unpack('<f', struct.pack('<I', int(h, 16)))[0]
    except ValueError:
        return h


a, b = load(sys.argv[1]), load(sys.argv[2])
limit = int(sys.argv[3]) if len(sys.argv) > 3 else 40
ticks = sorted({k[0] for k in a} & {k[0] for k in b})
print('ticks in both: %d (%s..%s)' % (len(ticks), ticks[0] if ticks else '-', ticks[-1] if ticks else '-'))
shown = 0
for tick in ticks:
    keys = sorted(k for k in a if k[0] == tick)
    diffs = []
    for k in keys:
        wa, wb = a[k], b.get(k)
        if wb is None:
            diffs.append('%s missing in B' % (k,))
            continue
        for i, (x, y) in enumerate(zip(wa, wb)):
            if x != y:
                diffs.append('town %d villager %d %s +%#x: A %s (%s)  B %s (%s)' % (k[1], k[2], k[3], i * 4, x, f32(x), y, f32(y)))
    if diffs:
        print('first difference at tick %d: %d words' % (tick, len(diffs)))
        for d in diffs[:limit]:
            print('  ' + d)
        break
else:
    print('no differences')
