"""Writes src/gog_rvas.h: the Steam -> GOG exe address table the DLL uses on the GOG build (src/gamebuild.cpp).

    python -I tools/gog_match.py STEAM.exe GOG.exe recon/gog/funcmap.tsv     (once: matches the two builds' functions)
    python -I tools/gog_table.py STEAM.exe GOG.exe

Collects every hex literal in src/ that falls inside the Steam exe's image (code + data), every vtable the code
references (commands send vtable addresses, always as Steam addresses), the call sites of the time-seeded Random()
constructor (hooks.cpp seeds those per site) and, if present, the types from recon/types.tsv; maps them with
tools/gog_map.py and fails when an address that the source uses can't be mapped. Run it again whenever new exe
addresses are added to the source."""
import glob, os, re, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gog_map import Mapper, load_pe

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
steam, gog = sys.argv[1], sys.argv[2]
m = Mapper(steam, gog, os.path.join(root, 'recon', 'gog', 'funcmap.tsv'))
secs = m.S[2]
text_lo, text_hi = secs['.text'][0], secs['.text'][0] + secs['.text'][1]
data_lo, data_hi = secs['.rdata'][0], secs['.rsrc'][0]

used = set()
for f in glob.glob(os.path.join(root, 'src', '*.cpp')) + glob.glob(os.path.join(root, 'src', '*.h')):
    if os.path.basename(f) in ('gog_rvas.h', 'modversion.h', 'repo_gen.h'):
        continue
    for line in open(f, encoding='utf-8', errors='replace'):
        code = line.split('//')[0]
        for h in re.findall(r'\b0x([0-9a-fA-F]{4,6})\b', code):
            v = int(h, 16)
            if text_lo <= v < text_hi or data_lo <= v < data_hi:
                used.add(v)
types, vtables = set(), set()
types_tsv = os.path.join(root, 'recon', 'types.tsv')  # the type registry dump (not in the public repository)
for l in (open(types_tsv).read().splitlines()[1:] if os.path.exists(types_tsv) else []):
    p = l.split('\t')
    for h in (p[3], p[4], p[5]):
        v = int(h, 16)
        if v and (text_lo <= v < text_hi or data_lo <= v < data_hi):
            types.add(v)
    if int(p[5], 16):
        vtables.add(int(p[5], 16))

# return addresses of every call of System::Random::Random() (time-seeded): hooks.cpp seeds these per call site, from
# the Steam address of the site, so both builds must agree on it
def import_slots(X, name):
    d, off = X[0], X[1]
    o = struct.unpack_from('<I', d, 0x3c)[0]
    i, slots = off(struct.unpack_from('<I', d, o + 24 + 112 + 8)[0]), set()
    while True:
        ilt, _, _, nm, iat = struct.unpack_from('<5I', d, i)
        if not nm:
            break
        t, slot = off(ilt or iat), iat
        while True:
            v = struct.unpack_from('<Q', d, t)[0]
            if not v:
                break
            if not v >> 63 and d[off(v & 0x7fffffff) + 2:].split(b'\0')[0].decode() == name:
                slots.add(slot)
            t, slot = t + 8, slot + 8
        i += 20
    return slots


def call_returns(X, name):
    slots = import_slots(X, name)
    va, vs = X[2]['.text']
    code = X[0][X[1](va):X[1](va) + vs]
    thunks = {va + k for k in range(len(code) - 6) if code[k] == 0xff and code[k + 1] == 0x25
              and va + k + 6 + struct.unpack_from('<i', code, k + 2)[0] in slots}
    out = []
    for k in range(len(code) - 6):
        if code[k] == 0xff and code[k + 1] == 0x15 and va + k + 6 + struct.unpack_from('<i', code, k + 2)[0] in slots:
            out.append(va + k + 6)
        elif code[k] == 0xe8 and va + k + 5 + struct.unpack_from('<i', code, k + 1)[0] in thunks:
            out.append(va + k + 5)
    return out


rand_sites = call_returns(m.S, '??0Random@System@@QEAA@XZ')
used |= set(rand_sites)

# every vtable the code references (an .rdata address used by matched code whose first slot points into .text):
# commands identify objects by their vtable, sent as the Steam address (commands.cpp VtableRva -> SteamRva)
m._build_refs()
for ts, votes in m.refs.items():
    if not data_lo <= ts < secs['.data'][0]:
        continue
    q = m.rd(m.S, ts, 8)
    if len(q) == 8 and text_lo <= struct.unpack('<Q', q)[0] - 0x140000000 < text_hi:
        types.add(ts)
        vtables.add(ts)

rows, bad = {}, []
for v in sorted(used | types):
    g, how, same = m.map(v)
    ok = g is not None and (same is None or same >= 11 or not how.startswith(('func', 'inner', 'leaf')))
    if ok:
        rows[v] = g
    elif v in used:
        bad.append('%x %s %s' % (v, how, same))
# vtables by content: translate the first slots through the function map and find that pointer sequence in the GOG
# build's .rdata (the reference vote can land on a neighbour when a vtable is referenced from few places)
g_rdata = m.G[2]['.rdata']
g_blob = m.rd(m.G, g_rdata[0], g_rdata[1])


def vt_by_content(v):
    seq = b''
    for i in range(4):
        q = m.rd(m.S, v + i * 8, 8)
        if len(q) < 8:
            return None
        p = struct.unpack('<Q', q)[0] - 0x140000000
        if not text_lo <= p < text_hi:
            break
        g, _ = m.code(p)
        if g is None:
            return None
        seq += struct.pack('<Q', g + 0x140000000)
    if len(seq) < 16:
        return None
    hits, k = [], g_blob.find(seq)
    while k >= 0 and len(hits) < 3:
        if k % 8 == 0:
            hits.append(g_rdata[0] + k)
        k = g_blob.find(seq, k + 1)
    return hits[0] if len(hits) == 1 else None


for v in sorted(vtables):
    g = vt_by_content(v)
    if g is not None and rows.get(v) != g:
        if v in rows:
            print('vtable %x: reference vote %x, content %x - using the content' % (v, rows[v], g))
        rows[v] = g

# a vtable must be a vtable in both builds: its first slots point at matching functions
vt_bad = []
for v in sorted(vtables):
    if v in rows and len(m.rd(m.S, v, 8)) == 8 and len(m.rd(m.G, rows[v], 8)) == 8:
        ps = struct.unpack('<Q', m.rd(m.S, v, 8))[0] - 0x140000000
        pg = struct.unpack('<Q', m.rd(m.G, rows[v], 8))[0] - 0x140000000
        if text_lo <= ps < text_hi and m.code(ps)[0] not in (pg, None):
            if not (0x3a000 <= pg < 0x3c000):  # import thunks are laid out differently, see gog_map.py
                vt_bad.append('%x -> %x slot0 %x vs %x%s' % (v, rows[v], ps, pg, '' if v in used else ' (left out)'))
                if v not in used:
                    del rows[v]

d, _, _ = load_pe(gog)
o = struct.unpack_from('<I', d, 0x3c)[0]
stamp = struct.unpack_from('<I', d, o + 8)[0]
out = os.path.join(root, 'src', 'gog_rvas.h')
with open(out, 'w', newline='\n') as f:
    f.write('// Generated by tools/gog_table.py - do not edit. Steam exe RVA -> GOG exe RVA (Banished 1.0.7, GOG build\n')
    f.write('// TimeDateStamp %#x). Sorted by the Steam RVA.\n#pragma once\n#include <cstdint>\n' % stamp)
    f.write('static const uint32_t kGogExeStamp = %#x;\n' % stamp)
    f.write('static const uint32_t kGogRvas[][2] = {\n')
    items = sorted(rows.items())
    for i in range(0, len(items), 6):
        f.write('    ' + ' '.join('{%#x, %#x},' % kv for kv in items[i:i + 6]) + '\n')
    f.write('};\n')
print('%d addresses used by the source, %d from types.tsv, %d in the table -> %s' % (len(used), len(types), len(rows), out))
for b in bad:
    print('UNMAPPED (used by the source):', b)
for b in vt_bad:
    print('vtable check failed:', b)
sys.exit(1 if bad or any('left out' not in b for b in vt_bad) else 0)
