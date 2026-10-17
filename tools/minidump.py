"""Minimal minidump reader: exception code/address + module-relative return-address candidates from the faulting
thread's stack. Usage: python -I tools/minidump.py crash.dmp"""
import struct, sys

data = open(sys.argv[1], 'rb').read()
sig, ver, nstreams, dirrva = struct.unpack_from('<IIII', data, 0)
streams = {}
for i in range(nstreams):
    t, size, rva = struct.unpack_from('<III', data, dirrva + i * 12)
    streams[t] = (size, rva)

def mstr(rva):
    n = struct.unpack_from('<I', data, rva)[0]
    return data[rva + 4: rva + 4 + n].decode('utf-16-le')

mods = []
if 4 in streams:
    _, rva = streams[4]
    n = struct.unpack_from('<I', data, rva)[0]
    for i in range(n):
        base, size = struct.unpack_from('<QI', data, rva + 4 + i * 108)
        name_rva = struct.unpack_from('<I', data, rva + 4 + i * 108 + 20)[0]
        mods.append((base, size, mstr(name_rva).split('\\')[-1]))

def where(a):
    for b, s, nm in mods:
        if b <= a < b + s: return f'{nm}+{a - b:x}'
    return None

mem = []  # (start, bytes) from MemoryListStream (5)
if 5 in streams:
    _, rva = streams[5]
    n = struct.unpack_from('<I', data, rva)[0]
    for i in range(n):
        start, dsize, drva = struct.unpack_from('<QII', data, rva + 4 + i * 16)
        mem.append((start, data[drva: drva + dsize]))

if 6 not in streams:
    print('no exception stream'); sys.exit()
_, rva = streams[6]
tid = struct.unpack_from('<I', data, rva)[0]
code, flags, rec, addr, nparams = struct.unpack_from('<IIQQI', data, rva + 8)
params = struct.unpack_from('<15Q', data, rva + 8 + 32)
print(f'thread {tid} exception {code:08x} at {addr:x} ({where(addr)}) params {[hex(p) for p in params[:nparams]]}')
ctx_size, ctx_rva = struct.unpack_from('<II', data, rva + 8 + 152)
ctx = data[ctx_rva: ctx_rva + ctx_size]
rsp, rbp = struct.unpack_from('<QQ', ctx, 0x98), None
rsp = rsp[0]
rip = struct.unpack_from('<Q', ctx, 0xf8)[0]
regs = 'rax rcx rdx rbx rsp rbp rsi rdi r8 r9 r10 r11 r12 r13 r14 r15'.split()
vals = struct.unpack_from('<16Q', ctx, 0x78)
print('rip', hex(rip), where(rip))
print(' '.join(f'{r}={v:x}' for r, v in zip(regs, vals)))
for start, b in mem:
    if start <= rsp < start + len(b):
        off = rsp - start
        print('stack return-address candidates:')
        k = 0
        for o in range(off, min(len(b), off + 0x1800), 8):
            v = struct.unpack_from('<Q', b, o)[0]
            w = where(v)
            if w and ('.exe' in w or 'Runtime' in w or 'dinput8' in w):
                print(f'  rsp+{o - off:4x}: {w}'); k += 1
                if k > 40: break
        break
