#!/usr/bin/env python3
"""Unslid dyld-cache addresses -> image + symbol, without ipsw.

  dsc-sym.py DIR ADDR...          DIR = dylibs extracted from the cache (Xcode's
                                  dsc_extractor.bundle, see NOTES-OP5.md)
  dsc-sym.py DIR --log LOG [NAME] SMP/CRASH lines of the SWEET console log
                                  (slide taken from their "slide 0x..." line)

Each extracted dylib keeps its cache address as __TEXT's vmaddr, so an index of
those ranges (DIR/.index, made once) finds the image, and atos the symbol.
"""
import os, re, struct, subprocess, sys, bisect

def text_range(path):
    with open(path, 'rb') as f:
        h = f.read(32)
        if len(h) < 32 or struct.unpack('<I', h[:4])[0] != 0xfeedfacf:
            return None
        ncmds, sizeofcmds = struct.unpack('<II', h[16:24])
        cmds = f.read(sizeofcmds)
    off = 0
    for _ in range(ncmds):
        cmd, size = struct.unpack('<II', cmds[off:off + 8])
        if cmd == 0x19 and cmds[off + 8:off + 24].rstrip(b'\0') == b'__TEXT':
            vm, vs = struct.unpack('<QQ', cmds[off + 24:off + 40])
            return vm, vm + vs
        off += size
    return None

def index(d):
    ip = os.path.join(d, '.index')
    if not os.path.exists(ip):
        rows = []
        for root, _, files in os.walk(d):
            for n in files:
                p = os.path.join(root, n)
                if os.path.islink(p) or n.startswith('.'):
                    continue
                r = text_range(p)
                if r:
                    rows.append((r[0], r[1], os.path.relpath(p, d)))
        rows.sort()
        with open(ip, 'w') as f:
            for a, b, p in rows:
                f.write(f'{a:x} {b:x} {p}\n')
    rows = []
    for l in open(ip):
        a, b, p = l.rstrip('\n').split(' ', 2)
        rows.append((int(a, 16), int(b, 16), p))
    return rows

def sym(d, rows, starts, addr):
    i = bisect.bisect_right(starts, addr) - 1
    if i < 0 or addr >= rows[i][1]:
        return f'{addr:#x} ?'
    a, _, p = rows[i]
    r = subprocess.run(['atos', '-o', os.path.join(d, p), '-l', hex(a), hex(addr)], capture_output=True, text=True)
    return f'{addr:#x} {os.path.basename(p)}: {r.stdout.strip()}'

def main():
    d = sys.argv[1]
    rows = index(d)
    starts = [r[0] for r in rows]
    if sys.argv[2] == '--log':
        want = sys.argv[4] if len(sys.argv) > 4 else None
        slide = {}
        for line in open(sys.argv[3], errors='replace'):
            m = re.search(r'(?:SMP|CRASH) (\w+)\[\d+\].*slide (0x[0-9a-f]+)', line)
            if m:
                slide[m.group(1)] = int(m.group(2), 16)
                if not want or m.group(1) == want:
                    print(line.rstrip())
                continue
            m = re.search(r'(?:SMP|CRASH) (\w+) (t\d+|pc|bt)\b(.*)', line)
            if not m or (want and m.group(1) != want) or m.group(1) not in slide:
                continue
            print(f'{m.group(1)} {m.group(2)}:')
            for a in re.findall(r'0x[0-9a-f]+', m.group(3)):
                v = int(a, 16) - slide[m.group(1)]
                if 0x180000000 <= v < 0x300000000:
                    print('   ', sym(d, rows, starts, v))
    else:
        for a in sys.argv[2:]:
            print(sym(d, rows, starts, int(a, 16)))

main()
