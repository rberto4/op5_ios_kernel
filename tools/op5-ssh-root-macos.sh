#!/bin/bash
# Install SSH (dropbear + dash) into a COPY of the phone's container on the Mac,
# then make a block patch against the untouched copy (applied on the phone by
# shim/out/blkapply from TWRP; the container never moves, root.map stays valid).
#
#   sudo tools/op5-ssh-root-macos.sh PRISTINE.img WORK.img BINDIR OUT.patch
#
# PRISTINE.img  the container as pulled from the phone (never modified)
# WORK.img      created here as an APFS clone of PRISTINE (cp -c), then modified
# BINDIR        signed iOS binaries (dropbear, sh)
# Journal: fsck_hfs -fy replays/repairs before the mount (iOS is always reset,
# never cleanly unmounted).
set -euo pipefail
[ $# = 4 ] || { echo "usage: sudo $0 PRISTINE.img WORK.img BINDIR OUT.patch" >&2; exit 2; }
[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
pristine=$1 work=$2 bindir=$3 out=$4
here=$(cd "$(dirname "$0")" && pwd)
rm -f "$work"
cp -c "$pristine" "$work"
dev=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount -noverify "$work" | awk 'NR==1{print $1}')
mnt=$(mktemp -d /tmp/op5root.XXXXXX)
cleanup() {
	umount "$mnt" 2>/dev/null || diskutil unmount force "$mnt" 2>/dev/null || true
	hdiutil detach "$dev" 2>/dev/null || hdiutil detach -force "$dev" 2>/dev/null || true
	rmdir "$mnt" 2>/dev/null || true
}
trap cleanup EXIT
echo "device $dev"
fsck_hfs -fy "${dev/disk/rdisk}" || echo "fsck_hfs exit $? (continuing; the mount decides)"
mount -t hfs -o nobrowse "$dev" "$mnt"
python3 -I "$here/op5-ssh-install.py" "$mnt" "$bindir"
sync
cleanup
trap - EXIT
python3 -I "$here/blkdiff.py" "$pristine" "$work" "$out"
chown "${SUDO_UID:-0}:${SUDO_GID:-0}" "$out" "$work"
echo "done: $out"
