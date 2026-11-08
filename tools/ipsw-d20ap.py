#!/usr/bin/env python3
"""IPSW helpers for the iPhone 8 (D20AP) root, standard library + macOS only.

  ipsw-d20ap.py paths BuildManifest.plist [BOARD]
      print the OS, SystemOS, AppOS and DeviceTree paths of BOARD's (default
      d20ap) Erase identity, one "KEY PATH" per line
  ipsw-d20ap.py im4p IN.im4p OUT
      write the IM4P payload, LZFSE-decompressed when it is LZFSE
      (macOS compression_tool), e.g. DeviceTree.d20ap.im4p -> d20ap.adt

Replaces `ipsw img4 im4p extract` for the device tree (no extra tools).
"""
import os
import plistlib
import subprocess
import sys
import tempfile

MANIFEST_KEYS = {
    'OS': 'OS',
    'SystemOS': 'Cryptex1,SystemOS',
    'AppOS': 'Cryptex1,AppOS',
    'DeviceTree': 'DeviceTree',
}


def erase_identity(manifest: dict, board: str) -> dict:
    for ident in manifest['BuildIdentities']:
        info = ident.get('Info', {})
        if info.get('DeviceClass', '').lower() == board and info.get('RestoreBehavior') == 'Erase':
            return ident
    raise SystemExit(f'no {board} Erase identity in the manifest')


def paths(manifest_path: str, board: str) -> None:
    with open(manifest_path, 'rb') as f:
        manifest = plistlib.load(f)
    ident = erase_identity(manifest, board)
    for key, entry in MANIFEST_KEYS.items():
        path = ident['Manifest'].get(entry, {}).get('Info', {}).get('Path')
        if not path:
            raise SystemExit(f'{board}: no {entry} path')
        print(key, path)


def der_header(buf: bytes, pos: int) -> tuple[int, int, int]:
    """(tag, content length, content offset) of the DER element at pos."""
    tag = buf[pos]
    n = buf[pos + 1]
    pos += 2
    if n & 0x80:
        count = n & 0x7f
        n = int.from_bytes(buf[pos:pos + count], 'big')
        pos += count
    return tag, n, pos


def im4p_payload(buf: bytes) -> bytes:
    tag, _, pos = der_header(buf, 0)
    if tag != 0x30:
        raise SystemExit('not a DER SEQUENCE')
    fields = []
    for _ in range(4):  # IA5 "IM4P", IA5 type, IA5 description, OCTET STRING payload
        tag, length, start = der_header(buf, pos)
        fields.append((tag, buf[start:start + length]))
        pos = start + length
    if fields[0] != (0x16, b'IM4P') or fields[3][0] != 0x04:
        raise SystemExit('not an IM4P')
    return fields[3][1]


def im4p(src: str, dst: str) -> None:
    with open(src, 'rb') as f:
        payload = im4p_payload(f.read())
    if not payload.startswith(b'bvx2'):
        with open(dst, 'wb') as f:
            f.write(payload)
        print(f'{dst}: {len(payload)} bytes (not compressed)')
        return
    with tempfile.NamedTemporaryFile(delete=False) as tmp:
        tmp.write(payload)
    try:
        subprocess.run(['compression_tool', '-decode', '-a', 'lzfse', '-i', tmp.name, '-o', dst],
                       check=True)
    finally:
        os.unlink(tmp.name)
    print(f'{dst}: {os.path.getsize(dst)} bytes (LZFSE payload {len(payload)})')


def main() -> None:
    if len(sys.argv) >= 3 and sys.argv[1] == 'paths':
        paths(sys.argv[2], sys.argv[3].lower() if len(sys.argv) > 3 else 'd20ap')
    elif len(sys.argv) == 4 and sys.argv[1] == 'im4p':
        im4p(sys.argv[2], sys.argv[3])
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main()
