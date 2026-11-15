#!/usr/bin/env python3
"""Install the SSH server into a MOUNTED iOS root (run as root, via sudo).

    sudo python3 tools/op5-ssh-install.py ROOT BINDIR [PUBKEY]

ROOT    mount point of the iOS root volume (HFS+, mounted with owners)
BINDIR  directory with the signed iOS binaries: dropbear (dropbearmulti), sh (dash)
PUBKEY  public key to authorise for root (default: ~SUDO_USER/.ssh/id_ed25519.pub)

Installs:
  /usr/local/bin/dropbear (+ scp, dropbearkey links to it)   root:wheel 0755
  /bin/sh (dash)                                              root:wheel 0755
  /private/etc/shells contains /bin/sh
  /private/etc/dropbear/ (host keys made on first start, dropbear -R)  0700
  /private/var/root/.ssh/authorized_keys                      root 0700/0600
  launchd job org.op5.dropbear in /System/Library/LaunchDaemons and in the
  launchd cache /System/Library/xpc/launchd.plist (iOS 16 reads the cache):
  dropbear -F -R -p 192.168.7.2:22, KeepAlive (it retries until the
  USB Ethernet address exists).
Running it twice gives the same result.
"""
import os
import plistlib
import pwd
import shutil
import sys

LABEL = "org.op5.dropbear"
JOB_PATH = "/System/Library/LaunchDaemons/org.op5.dropbear.plist"
JOB = {
    "Label": LABEL,
    "ProgramArguments": [
        # No -E/-s: they only exist with syslog / password auth compiled in.
        "/usr/local/bin/dropbear", "-F", "-R", "-p", "192.168.7.2:22",
    ],
    "RunAtLoad": True,
    "KeepAlive": True,
    "UserName": "root",
    "StandardErrorPath": "/dev/console",
    "StandardOutPath": "/dev/console",
}


def put_file(src: str, dst: str, mode: int) -> None:
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    tmp = dst + ".op5new"
    shutil.copyfile(src, tmp)
    os.chown(tmp, 0, 0)
    os.chmod(tmp, mode)
    os.replace(tmp, dst)
    print("installed", dst)


def put_link(target: str, path: str) -> None:
    if os.path.islink(path) or os.path.exists(path):
        os.remove(path)
    os.symlink(target, path)
    os.lchown(path, 0, 0)
    print("link", path, "->", target)


def ensure_shells(root: str) -> None:
    path = os.path.join(root, "private/etc/shells")
    lines = open(path).read().splitlines() if os.path.exists(path) else []
    if "/bin/sh" in lines:
        return
    with open(path, "a") as f:
        f.write("/bin/sh\n")
    os.chown(path, 0, 0)
    os.chmod(path, 0o644)
    print("added /bin/sh to /etc/shells")


def authorize(root: str, pubkey: str) -> None:
    key = open(pubkey).read().strip()
    if "PRIVATE KEY" in key or not key.startswith(("ssh-", "ecdsa-")):
        raise SystemExit(f"{pubkey} does not look like a public key")
    ssh = os.path.join(root, "private/var/root/.ssh")
    os.makedirs(ssh, exist_ok=True)
    os.chown(ssh, 0, 0)
    os.chmod(ssh, 0o700)
    path = os.path.join(ssh, "authorized_keys")
    with open(path, "w") as f:
        f.write(key + "\n")
    os.chown(path, 0, 0)
    os.chmod(path, 0o600)
    print("authorized_keys: 1 key from", pubkey)


def install_job(root: str) -> None:
    job_file = os.path.join(root, JOB_PATH.lstrip("/"))
    with open(job_file, "wb") as f:
        plistlib.dump(JOB, f, fmt=plistlib.FMT_XML)
    os.chown(job_file, 0, 0)
    os.chmod(job_file, 0o644)
    cache = os.path.join(root, "System/Library/xpc/launchd.plist")
    st = os.stat(cache)
    with open(cache, "rb") as f:
        plist = plistlib.load(f)
    plist["LaunchDaemons"][JOB_PATH] = dict(JOB)
    tmp = cache + ".op5new"
    with open(tmp, "wb") as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    os.chown(tmp, st.st_uid, st.st_gid)
    os.chmod(tmp, st.st_mode & 0o7777)
    os.replace(tmp, cache)
    print("launchd job", LABEL, "in", JOB_PATH, "and in the launchd cache")


def check_root_shell(root: str) -> None:
    for name in ("private/etc/master.passwd", "private/etc/passwd"):
        path = os.path.join(root, name)
        if not os.path.exists(path):
            continue
        for line in open(path):
            if line.startswith("root:"):
                print(f"{name}: root shell = {line.strip().split(':')[-1]}")


def main() -> int:
    if len(sys.argv) not in (3, 4):
        print(__doc__, file=sys.stderr)
        return 2
    if os.geteuid() != 0:
        print("run with sudo (files must be owned by root)", file=sys.stderr)
        return 1
    root, bindir = sys.argv[1], sys.argv[2]
    if len(sys.argv) == 4:
        pubkey = sys.argv[3]
    else:
        home = pwd.getpwnam(os.environ.get("SUDO_USER", "root")).pw_dir
        pubkey = os.path.join(home, ".ssh/id_ed25519.pub")
    if not os.path.exists(os.path.join(root, "System/Library/xpc/launchd.plist")):
        print(f"{root} is not a mounted iOS root", file=sys.stderr)
        return 1
    put_file(os.path.join(bindir, "dropbear"), os.path.join(root, "usr/local/bin/dropbear"), 0o755)
    put_link("dropbear", os.path.join(root, "usr/local/bin/scp"))
    put_link("dropbear", os.path.join(root, "usr/local/bin/dropbearkey"))
    put_file(os.path.join(bindir, "sh"), os.path.join(root, "bin/sh"), 0o755)
    ensure_shells(root)
    keydir = os.path.join(root, "private/etc/dropbear")
    os.makedirs(keydir, exist_ok=True)
    os.chown(keydir, 0, 0)
    os.chmod(keydir, 0o700)
    authorize(root, pubkey)
    install_job(root)
    check_root_shell(root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
