"""Find code that really differs between the Steam and GOG builds (not just moved). For every matched function pair of
the same length, every differing byte must lie in a 4-byte RIP-relative displacement whose two targets correspond
(code: through the function map; data: through tools/gog_map.py's reference vote). Anything else is reported.
    python -I tools/gog_codediff.py STEAM.exe GOG.exe [FUNCMAP] [--all]"""
import bisect, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gog_map import Mapper

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
args = [a for a in sys.argv[1:] if not a.startswith('--')]
fmap = args[2] if len(args) > 2 else os.path.join(root, 'recon', 'gog', 'funcmap.tsv')
m = Mapper(args[0], args[1], fmap)
S, G = m.S, m.G
tlo, thi = S[2]['.text'][0], S[2]['.text'][0] + S[2]['.text'][1]
m._build_refs()
# data delta lookup: sorted (steam, gog) anchors from the reference votes
anch = sorted((ts, c.most_common(1)[0][0]) for ts, c in m.refs.items() if c.most_common(1)[0][1] >= 2)
akeys = [a[0] for a in anch]


def imports(X):
    """IAT slot rva -> imported name, and import thunk (jmp [rip+slot]) rva -> the same name"""
    d, off = X[0], X[1]
    o = struct.unpack_from('<I', d, 0x3c)[0]
    irva = struct.unpack_from('<I', d, o + 24 + 112 + 8)[0]
    i, names = off(irva), {}
    while True:
        ilt, _, _, name, iat = struct.unpack_from('<5I', d, i)
        if not name:
            break
        t, slot = off(ilt or iat), iat
        while True:
            v = struct.unpack_from('<Q', d, t)[0]
            if not v:
                break
            names[slot] = '#%d' % (v & 0xffff) if v >> 63 else d[off(v & 0x7fffffff) + 2:].split(b'\0')[0].decode()
            t += 8
            slot += 8
        i += 20
    va, vs = X[2]['.text']
    code = d[off(va):off(va) + vs]
    for k in range(len(code) - 6):
        if code[k] == 0xff and code[k + 1] == 0x25:
            tgt = va + k + 6 + struct.unpack_from('<i', code, k + 2)[0]
            if tgt in names:
                names[va + k] = names[tgt]
    return names


imp_s, imp_g = imports(S), imports(G)


def target_ok(ts, tg):
    if ts in imp_s or tg in imp_g:
        return imp_s.get(ts) == imp_g.get(tg)
    if tlo <= ts < thi:
        g, _ = m.code(ts) if False else (None, None)
        i = bisect.bisect_right(m.starts, ts) - 1
        if i >= 0 and m.fm[i][0] <= ts < m.fm[i][1]:
            return tg - m.fm[i][2] == ts - m.fm[i][0]
        if i >= 0:  # leaf function in a gap: same distance from the previous function's end
            return tg - m.fm[i][3] == ts - m.fm[i][1] or (i + 1 < len(m.fm) and m.fm[i + 1][2] - tg == m.fm[i + 1][0] - ts)
        return False
    i = bisect.bisect_right(akeys, ts) - 1
    if i < 0:
        return False
    return tg - anch[i][1] == ts - anch[i][0] or (i + 1 < len(anch) and anch[i + 1][1] - tg == anch[i + 1][0] - ts)


bad = []
for s0, s1, g0, g1, sc in m.fm:
    if s1 - s0 != g1 - g0:
        continue
    a, b = m.rd(S, s0, s1 - s0), m.rd(G, g0, g1 - g0)
    k, n = 0, len(a)
    unexplained = []
    while k < n:
        if a[k] == b[k]:
            k += 1
            continue
        done = False
        for w in range(max(0, k - 3), k + 1):  # a 4-byte displacement covering byte k
            if w + 4 > n:
                continue
            da, db = struct.unpack_from('<i', a, w)[0], struct.unpack_from('<i', b, w)[0]
            if 0 < da < 0x300000 and target_ok(da, db):  # image-relative address (jump tables: [base + idx*4 + rva])
                k = w + 4
                done = True
                break
            for imm in (0, 1, 2, 4):
                ts, tg = s0 + w + 4 + imm + da, g0 + w + 4 + imm + db
                if (ts == 0 and tg == 0) or target_ok(ts, tg) or (not tlo <= ts < thi and (ts - tg) % 4 == 0 and 0 < ts < 0x1fc000
                                         and len(m.rd(S, ts, 16)) == 16 and m.rd(S, ts, 16) == m.rd(G, tg, 16)):
                    k = w + 4
                    done = True
                    break
            if done:
                break
        if not done:
            unexplained.append(k)
            k += 1
    if unexplained:
        bad.append((s0, s1, g0, len(unexplained), unexplained[:6]))
print('%d same-length functions with unexplained byte differences' % len(bad))
for s0, s1, g0, cnt, at in bad if '--all' in sys.argv else bad[:80]:
    print('  steam %x-%x gog %x: %d bytes, at +%s' % (s0, s1, g0, cnt, ' +'.join('%x' % x for x in at)))
