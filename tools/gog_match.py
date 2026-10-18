"""Match functions between the Steam and GOG builds of Application-x64 via .pdata order + byte similarity.
Writes recon/gog/funcmap.tsv: steam_start steam_end gog_start gog_end score"""
import struct, sys, difflib
def load(p):
    d = open(p, 'rb').read(); o = struct.unpack_from('<I', d, 0x3c)[0]
    ns = struct.unpack_from('<H', d, o+6)[0]; so = struct.unpack_from('<H', d, o+20)[0]
    secs = [struct.unpack_from('<8sIIII', d, o+24+so+i*40) for i in range(ns)]
    dd = o+24+112
    prva, psz = struct.unpack_from('<II', d, dd+3*8)
    def off(rva):
        for n, vs, va, rs, ra in secs:
            if va <= rva < va+max(vs, rs): return rva-va+ra
    p0 = off(prva); fns = []
    for i in range(psz//12):
        b, e, u = struct.unpack_from('<III', d, p0+i*12)
        fns.append((b, e))
    # pdata can have chained entries for the same function; keep unique starts in order
    fns = sorted(set(fns))
    return d, off, fns, secs
S = load(sys.argv[1]); G = load(sys.argv[2])
def body(X, f): return X[0][X[1](f[0]):X[1](f[0])+f[1]-f[0]]
sk = [(f[1]-f[0], body(S, f)[:6]) for f in S[2]]
gk = [(f[1]-f[0], body(G, f)[:6]) for f in G[2]]
sm = difflib.SequenceMatcher(None, sk, gk, autojunk=False)
pairs = {}
for a, b, n in sm.get_matching_blocks():
    for k in range(n): pairs[a+k] = b+k
# fill gaps between matched blocks: equal-count gaps pair in order if lengths are close
ops = sm.get_opcodes()
for tag, i1, i2, j1, j2 in ops:
    if tag == 'replace':
        for k in range(min(i2-i1, j2-j1)): pairs.setdefault(i1+k, j1+k)
out = []
for i, j in sorted(pairs.items()):
    a, b = body(S, S[2][i]), body(G, G[2][j])
    eq = sum(x == y for x, y in zip(a, b)) / max(len(a), len(b))
    out.append((S[2][i], G[2][j], eq))
with open(sys.argv[3], 'w') as f:
    for s, g, e in out: f.write('%x\t%x\t%x\t%x\t%.3f\n' % (s[0], s[1], g[0], g[1], e))
good = sum(e > 0.8 for _, _, e in out)
print('steam fns', len(S[2]), 'gog fns', len(G[2]), 'paired', len(out), 'score>0.8', good)
