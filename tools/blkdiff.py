#!/usr/bin/env python3
"""Block patch between two same-size images (for tools/blkapply.c).

    tools/blkdiff.py OLD.img NEW.img OUT.patch

Writes "OP5BLKP1" | u64 size | u64 count, then per changed 4096-byte block:
u64 offset | old block | new block (little endian).
"""
import struct
import sys

BLK = 4096
CHUNK = 64 * 1024 * 1024


def main() -> int:
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        return 2
    old_path, new_path, out_path = sys.argv[1:]
    with open(old_path, "rb") as fo, open(new_path, "rb") as fn:
        fo.seek(0, 2)
        fn.seek(0, 2)
        size = fo.tell()
        if size != fn.tell() or size % BLK:
            print(f"size mismatch or not 4 KiB aligned: {size} vs {fn.tell()}", file=sys.stderr)
            return 1
        fo.seek(0)
        fn.seek(0)
        recs = []
        pos = 0
        while pos < size:
            a = fo.read(CHUNK)
            b = fn.read(CHUNK)
            if len(a) != len(b) or not a:
                print("short read", file=sys.stderr)
                return 1
            if a != b:
                for i in range(0, len(a), BLK):
                    if a[i:i + BLK] != b[i:i + BLK]:
                        recs.append((pos + i, a[i:i + BLK], b[i:i + BLK]))
            pos += len(a)
    with open(out_path, "wb") as out:
        out.write(b"OP5BLKP1" + struct.pack("<QQ", size, len(recs)))
        for off, a, b in recs:
            out.write(struct.pack("<Q", off) + a + b)
    print(f"{len(recs)} changed blocks of {size // BLK} -> {out_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
