"""Translate Steam-build exe RVAs to the GOG build of Banished 1.0.7 (both built 2017-09-10 from the same code).

    python -I tools/gog_map.py STEAM.exe GOG.exe FUNCMAP rva [rva ...]   ('-' reads rvas from stdin)

FUNCMAP comes from tools/gog_match.py. Code addresses map through the matched function that contains them (same
offset when the function bodies line up, else a byte alignment of the two bodies); functions without unwind info
(leaf functions) through an alignment of the gap between two matched functions. Data addresses (.rdata/.data) map
through the RIP-relative references to them from matched functions that line up byte for byte (majority vote)."""
import bisect, collections, difflib, struct, sys


def load_pe(p):
    d = open(p, 'rb').read()
    o = struct.unpack_from('<I', d, 0x3c)[0]
    ns, so = struct.unpack_from('<H', d, o + 6)[0], struct.unpack_from('<H', d, o + 20)[0]
    secs = [struct.unpack_from('<8sIIII', d, o + 24 + so + i * 40) for i in range(ns)]

    def off(rva):
        for n, vs, va, rs, ra in secs:
            if va <= rva < va + min(vs, rs):
                return rva - va + ra
    return d, off, {n.rstrip(b'\0').decode(): (va, vs) for n, vs, va, rs, ra in secs}


class Mapper:
    def __init__(self, steam, gog, funcmap):
        self.S, self.G = load_pe(steam), load_pe(gog)
        self.fm = sorted(tuple(int(x, 16) for x in l.split()[:4]) + (float(l.split()[4]),) for l in open(funcmap))
        self.starts = [f[0] for f in self.fm]
        self.refs = None

    def rd(self, X, rva, n):
        o = X[1](rva)
        return X[0][o:o + n] if o is not None else b''

    def in_text(self, r):
        va, vs = self.S[2]['.text']
        return va <= r < va + vs

    def code(self, r):
        S, G, fm = self.S, self.G, self.fm
        i = bisect.bisect_right(self.starts, r) - 1
        if i < 0:
            return None, 'nofunc'
        s0, s1, g0, g1, sc = fm[i]
        if not s0 <= r < s1:  # leaf function without unwind info: align the gap up to the next function
            if i + 1 >= len(fm):
                return None, 'gap'
            n0, _, ng0, _, _ = fm[i + 1]
            a, b, k = self.rd(S, s1, n0 - s1), self.rd(G, g1, ng0 - g1), r - s1
            for x, y, n in difflib.SequenceMatcher(None, a, b, autojunk=False).get_matching_blocks():
                if x <= k < x + n:
                    return g1 + y + (k - x), 'leaf'
            return None, 'gap'
        k = r - s0
        if s1 - s0 == g1 - g0 and sc > 0.9:
            return g0 + k, 'func %.2f' % sc
        a, b = self.rd(S, s0, s1 - s0), self.rd(G, g0, g1 - g0)
        for x, y, n in difflib.SequenceMatcher(None, a, b, autojunk=False).get_matching_blocks():
            if x <= k < x + n:
                return g0 + y + (k - x), 'inner %.2f' % sc
        return None, 'unaligned %.2f' % sc

    def _build_refs(self):
        S, G = self.S, self.G
        lo, hi = S[2]['.rdata'][0], S[2]['.rsrc'][0]
        self.refs = collections.defaultdict(collections.Counter)
        for s0, s1, g0, g1, sc in self.fm:
            if s1 - s0 != g1 - g0 or sc < 0.8:
                continue
            a, b = self.rd(S, s0, s1 - s0), self.rd(G, g0, g1 - g0)
            for k in range(len(a) - 3):
                da, db = struct.unpack_from('<i', a, k)[0], struct.unpack_from('<i', b, k)[0]
                for imm in (0, 1, 2, 4):  # displacement followed by an immediate of this size
                    ts = s0 + k + 4 + imm + da
                    if lo <= ts < hi:
                        self.refs[ts][g0 + k + 4 + imm + db] += 1

    def data(self, r):
        if self.refs is None:
            self._build_refs()
        for d in range(0x400):  # nearest referenced address at or below r, same offset from it
            c = self.refs.get(r - d)
            if c:
                tg, n = c.most_common(1)[0]
                tot = sum(c.values())
                if n >= 2 and n / tot > 0.6:
                    return tg + d, 'ref %d/%d -%x' % (n, tot, d)
        return None, 'noref'

    def map(self, r):
        g, how = self.code(r) if self.in_text(r) else self.data(r)
        same = None
        if g is not None:
            a, b = self.rd(self.S, r, 16), self.rd(self.G, g, 16)
            same = sum(x == y for x, y in zip(a, b)) if a else None
        return g, how, same


if __name__ == '__main__':
    m = Mapper(*sys.argv[1:4])
    args = sys.argv[4:]
    if args == ['-']:
        args = sys.stdin.read().split()
    for a in args:
        g, how, same = m.map(int(a, 16))
        print('%x\t%s\t%s\t%s' % (int(a, 16), '%x' % g if g is not None else '-', how,
                                  '-' if same is None else '%d/16' % same))
