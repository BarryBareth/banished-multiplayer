"""Resolve offsets inside our dinput8.dll to function+line using its PDB (dbghelp).
Usage: python -I sym.py <dll> <offset_hex> [...]"""
import ctypes, ctypes.wintypes as wt, sys, os

dbg = ctypes.WinDLL('dbghelp')

class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', wt.ULONG),
                ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', wt.ULONG),
                ('Scope', wt.ULONG), ('Tag', wt.ULONG), ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG),
                ('Name', ctypes.c_char * 512)]

class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.DWORD), ('Key', ctypes.c_void_p), ('LineNumber', wt.DWORD),
                ('FileName', ctypes.c_char_p), ('Address', ctypes.c_uint64)]

def main(dll, offsets):
    h = ctypes.c_void_p(0x1234)
    dbg.SymSetOptions(0x2 | 0x10)  # UNDNAME | LOAD_LINES
    dbg.SymInitialize(h, os.path.dirname(dll).encode(), False)
    base = 0x10000000
    dbg.SymLoadModuleEx.restype = ctypes.c_uint64
    dbg.SymLoadModuleEx(h, None, dll.encode(), None, ctypes.c_uint64(base), 0, None, 0)
    for off in offsets:
        addr = ctypes.c_uint64(base + int(off, 16))
        si = SYMBOL_INFO(); si.SizeOfStruct = 88; si.MaxNameLen = 500
        disp = ctypes.c_uint64()
        name = si.Name.decode() if dbg.SymFromAddr(h, addr, ctypes.byref(disp), ctypes.byref(si)) else '?'
        ln = IMAGEHLP_LINE64(); ln.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
        d32 = wt.DWORD()
        where = f'{os.path.basename(ln.FileName.decode())}:{ln.LineNumber}' if dbg.SymGetLineFromAddr64(h, addr, ctypes.byref(d32), ctypes.byref(ln)) else ''
        print(f'+{off}: {name}+{disp.value:#x} {where}')

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2:])
