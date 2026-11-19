"""Symbolicate SMP sampler lines: dscsym.py CACHE DSC_X LOG [proc...]"""
import bisect, re, struct, subprocess, sys

def images(cache):
    with open(cache, 'rb') as f:
        h = f.read(0x200)
        imgs_off, imgs_cnt = struct.unpack_from('<II', h, 0x18)
        if imgs_cnt == 0:  # newer header: imagesOffset/imagesCount at 0x1c0
            imgs_off, imgs_cnt = struct.unpack_from('<II', h, 0x1c0)
        f.seek(imgs_off)
        raw = f.read(imgs_cnt * 32)
        out = []
        for i in range(imgs_cnt):
            addr, _, _, path_off, _ = struct.unpack_from('<QQQII', raw, i * 32)
            f.seek(path_off)
            path = f.read(256).split(b'\0', 1)[0].decode()
            out.append((addr, path))
    return sorted(out)

_syms = {}
def syms(dscx, path):
    if path not in _syms:
        r = subprocess.run(['xcrun', 'nm', '-n', '--defined-only', dscx + path], capture_output=True, text=True)
        lst = []
        for l in r.stdout.splitlines():
            p = l.split(None, 2)
            if len(p) == 3 and p[1] in 'tTsS':
                lst.append((int(p[0], 16), p[2]))
        _syms[path] = lst
    return _syms[path]

def resolve(imgs, keys, dscx, a):
    i = bisect.bisect_right(keys, a) - 1
    if i < 0:
        return '?'
    path = imgs[i][1]
    s = syms(dscx, path)
    j = bisect.bisect_right([x[0] for x in s], a) - 1
    name = path.rsplit('/', 1)[-1]
    return f'{name} {s[j][1]}+{a - s[j][0]:#x}' if j >= 0 else f'{name} +{a - imgs[i][0]:#x}'

def main():
    cache, dscx, log = sys.argv[1:4]
    want = sys.argv[4:]
    imgs = images(cache)
    keys = [x[0] for x in imgs]
    slide = 0
    seen = set()
    for line in open(log, errors='replace'):
        if not line.startswith('SMP') or 'sample 1' in line:
            if 'sample 1' in line:
                break
            continue
        m = re.search(r'slide (0x[0-9a-f]+)', line)
        if m:
            slide = int(m.group(1), 16)
        if want and not any(f'SMP {w} ' in line or f'SMP {w}[' in line for w in want):
            continue
        print(line.rstrip()[:90])
        for a in re.findall(r'0x[0-9a-f]{8,}', line):
            v = int(a, 16) - slide
            if 0x180000000 <= v < 0x300000000:
                print(f'    {resolve(imgs, keys, dscx, v)}')

main()
