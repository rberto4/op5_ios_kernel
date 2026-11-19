#!/usr/bin/env python3
"""Build the minimal Apple Device Tree that the SWEET xnu needs on cheeseburger.

Only what the early boot path actually reads (see NOTES.md); every MMIO
address comes from the OnePlus 5 (cheeseburger, MSM8998) device tree: mainline
msm8998.dtsi / msm8998-oneplus-common.dtsi and the OnePlus msm8998 kernel.
Usage:  mkadt.py OUT.adt [--dump] [--qemu] [--highmem] [--native] [--container MAP]

--container MAP: the iOS root is a file in Android's userdata (PORT-OP5.md
"Keeping Android"); MAP is the output of `ioscont map /data/ios/root.img`.
"""
import hashlib, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import adt  # noqa: E402

# Kernel-managed RAM without --highmem: low-bank fallback above the OnePlus
# carve-outs (ramoops/param/mtp end at 0xacf00000). Not verified on a phone;
# use --highmem.
PHYS_BASE = 0xb0000000
MEM_SIZE = 0x4d000000
# --highmem (phone only): the upper DDR bank. Probe page 2 on the 6 GB A5000
# (2026-10-23): ABL's /memory is <0x40000000 0xC0000000>, <0x1_00000000
# 0xBD8C0000>, and every carve-out (hyp, TZ, modem, ADSP, splash, OnePlus
# ramoops/param/mtp) is below 4 GB. XNU gets the upper bank rounded down to
# 32 MB: 0xBC000000 (3 GB; an iPhone 8 has 2 GB). The 8 GB model has more,
# set HIGHSIZE= after checking its probe page 2.
HIGH_PHYS_BASE = 0x100000000
HIGH_MEM_SIZE = int(__import__("os").environ.get("HIGHSIZE", "0xbc000000"), 16)
# topaz's --midmem (RAM from 0x80000000) is NOT safe here: on MSM8998 that
# range holds the hyp/TZ/modem carve-outs (XPU-protected), see build().
# /arm-io children are relative to this; it must not be 0 (serial_init bails)
SOC_BASE = 0x01000000

# interrupt-controller@17a00000 (GICR 0x17b00000, 8 x 0x20000)
GICD, GICD_SIZE = 0x17a00000, 0x10000
GICR, GICR_SIZE = 0x17b00000, 0x100000
# XBL splash framebuffer (cont_splash_mem, mainline simplefb): 1080x1920 a8r8g8b8
FB, FB_SIZE = 0x9d400000, 0x800000
SWEET_ECID = 0x4348454553450001  # 'CHEESE' + 1, synthetic
FB_W, FB_H, FB_STRIDE = 1080, 1920, 1080 * 4
# Internal UFS 2.1 host (ufshc@1da4000); QEMU virt has none
UFS, UFS_SIZE = 0x01da4000, 0x2500
TIMEBASE = 19200000
VTIMER_INTID = 19
# QEMU virt (-M virt,gic-version=3 -cpu cortex-a76): GICD/GICR and the generic timer's CNTFRQ
QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE = 0x08000000, 0x080a0000, 62500000
QEMU_VTIMER_INTID = 27

BOOT_MANIFEST_HASH = hashlib.sha384(b'cheeseburger boot manifest').digest()

# cheeseburger presents an iPhone 8 (iPhone10,1 / D20AP): 16:9 like the
# OnePlus 5 panel, 750x1334 @2x scaled up 1.44x by the kernel (patch 0083),
# Home-button UI. The userland comes from the iPhone 8 IPSW. MobileGestalt
# derives product type, device class, artwork etc. from the root identity
# and /product of the real D20AP device tree. That tree is an Apple artifact
# (kept out of git): extract it from the IPSW with
#   tools/remotezip.py URL DeviceTree.d20ap.im4p DIR
#   ipsw img4 im4p extract DIR/Firmware/all_flash/DeviceTree.d20ap.im4p -o d20ap.adt
D20AP_ADT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '../research/ipsw-8/d20ap.adt')
LOGICAL_W, LOGICAL_H, DISPLAY_SCALE = 750, 1334, 2


def d20ap_node(want, path=D20AP_ADT):
    """Properties of a top-level node (product, buttons) of the real D20AP
    tree, without the syscfg placeholders iBoot would fill in (flag bit set)
    and the node bookkeeping. None if the tree or the node is missing."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /{want} from D20AP', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == want.encode() + b'\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    print(f'warning: {path}: no /{want} node', file=sys.stderr)
    return None

def d20ap_child(parent, want, path=D20AP_ADT):
    """Properties of the child node WANT of the top-level node PARENT of the
    real D20AP tree (as d20ap_node). None if absent."""
    if not os.path.exists(path):
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == parent.encode() + b'\0':
            for g in c.children:
                if dict((k, v) for k, f, v in g.props).get('name') == want.encode() + b'\0':
                    return [(k, v) for k, f, v in g.props if not f and k not in ('name', 'AAPL,phandle')]
    return None

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


FIEMAP_UNSAFE = 0x2 | 0x4 | 0x8 | 0x80 | 0x100 | 0x200 | 0x800  # unknown, delalloc, encoded, encrypted, not aligned, inline, tail, unwritten


def container_extents(path):
    """ioscont map output -> (file block, partition block, count) triples in
    4 KiB blocks, checked: aligned, written, gap-free from 0."""
    ext, total = [], 0
    for line in open(path):
        f = line.split()
        if not f or f[0] != 'extent':
            continue
        lo, ph, ln, flags = (int(f[i], 0) for i in (1, 2, 3, 5))
        if flags & FIEMAP_UNSAFE:
            raise SystemExit(f'{path}: {line.strip()}: unsafe flags {flags:#x}: write the image into '
                             'the file first (dd conv=notrunc), then map it again')
        if lo % 4096 or ph % 4096 or ln % 4096:
            raise SystemExit(f'{path}: extent not 4 KiB aligned: {line.strip()}')
        if lo != total:
            raise SystemExit(f'{path}: gap or overlap at file offset {lo:#x} (expected {total:#x})')
        ext.append((lo // 4096, ph // 4096, ln // 4096))
        total += ln
    if not ext:
        raise SystemExit(f'{path}: no extents')
    print(f'container: {len(ext)} extents, {total >> 20} MiB', file=sys.stderr)
    return ext


def build(seed=None, qemu=False, highmem=False, native=False, midmem=False, container=None):
    """qemu=True: same tree, but with QEMU virt's GIC addresses and CNTFRQ (tools/qemu-sweet.py)."""
    if midmem:
        raise SystemExit('--midmem is topaz-only: on MSM8998 RAM from 0x80000000 holds XPU carve-outs')
    phys_base, mem_size = (HIGH_PHYS_BASE, HIGH_MEM_SIZE) if highmem else (PHYS_BASE, MEM_SIZE)
    gicd, gicr, timebase = (QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE) if qemu else (GICD, GICR, TIMEBASE)
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('firmware-version', cstr('cheeseburger-shim')),
        ('system-firmware-version', cstr('cheeseburger-shim')),
        # iBoot's SHA-384 of the boot manifest; /sbin/mount -P 1 exits unless
        # it exists (bind mounts /private/preboot/<hex>/usr/standalone/firmware,
        # tolerating a missing directory). Fixed stand-in value.
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        # libimg4 (cryptexd) aborts "failed to get crypto algorithm" without
        # it; real A11+ device trees say sha2-384.
        ('crypto-hash-method', cstr('sha2-384')),
        # MobileGestalt's screen class: display-scale (u32, default 0) comes
        # from /chosen, the pixel size from IOMobileFramebuffer. Scale 0 makes
        # SpringBoard's FBSDisplayMode "infxinf@0x" invalid. iPhone 8:
        # 750x1334 @2x = 375x667 points (3 with --native: 1080x1920 @3x).
        ('display-scale', u32(3 if native else DISPLAY_SCALE)),
        # XNU derives IOPlatformUUID (gethostuuid) from these on arm; without
        # them CFPreferences cannot use kCFPreferencesCurrentHost (no ByHost
        # files), so e.g. DataMigration never records LastSystemVersion and
        # migrates on every boot. Synthetic ECID (not the phone's); chip-id
        # of the A11 that the iPhone 8 userland expects.
        ('unique-chip-id', struct.pack('<Q', SWEET_ECID)),
        ('chip-id', u32(0x8015)),
        # MobileGestalt's network addresses (UniqueDeviceID etc.): iBoot sets
        # these on an iPhone. Without mac-address-wifi0 MobileGestalt waits
        # for an IONetworkController and times out on every question, so
        # lsd's LaunchServices seed never ends (sampler 2026-11-18: lsd in
        # GenerateBuiltInAppInstallSessionID -> MG semaphore; Bergamota
        # mkadt.py). Synthetic locally administered addresses, not the phone's.
        ('mac-address-wifi0', bytes([0x02, 0x4f, 0x50, 0x35, 0x00, 0x01])),
        ('mac-address-bluetooth0', bytes([0x02, 0x4f, 0x50, 0x35, 0x00, 0x02])),
        ('mac-address-ethernet0', bytes([0x02, 0x4f, 0x50, 0x35, 0x00, 0x03])),
        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1):
        # MobileGestalt reads these from /chosen, and mobileactivationd's
        # DeviceType then takes its native "Hactivation" branch (state
        # Activated without an activation record; no SEP, no baseband here).
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        # Local boot policy (kern_csr.c): SIP relaxed like `csrutil disable`
        # (lp-sip0: untrusted kexts, unrestricted FS, task_for_pid, kernel
        # debugger, dtrace, nvram), lp-sip1: unauthenticated (unsealed) root.
        # The iOS shared cache is in no trust cache, so dyld can only map it
        # with CSR_ALLOW_UNRESTRICTED_FS (vm_unix.c shared region checks).
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # SMP as on topaz: only the four gold (A73 class) cores, MPIDR 0x100-0x103.
    # Kryo 280 has the same layout as topaz's Kryo 260; the shim boots xnu on
    # gold CPU 0x100 and parks the silver cluster (no 16K granule, to be
    # confirmed by probe page 1). One cluster. The kernel defaults to one
    # and uses cpus=N to select an initial prefix (N <= 8).
    # QEMU retains its original single-CPU tree: its MPIDRs differ from sweet.
    cpu_nodes = []
    for core in range(1 if qemu else 4):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('qcom,kryo280') + cstr('qcom,kryo470') + cstr('ARM,v8')),
            ('reg', u32(core if qemu else 0x100 | core)),  # actual MPIDR Aff1:Aff0, NOT logical ID
            ('cpu-id', u32(core)),   # descriptive: XNU assigns IDs by DT order
            ('cluster-id', u32(0)),  # descriptive; see ml_parse_cpu_topology
            ('cluster-type', u32(0)),  # homogeneous CLUSTER_TYPE_SMP, not AMP
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(0)),  # gold L2
            ('l3-cache-id', u32(0)),
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(2457600000)),  # Kryo 280 gold max
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(QEMU_VTIMER_INTID if qemu else VTIMER_INTID)),
        ('reg', u64(gicd - SOC_BASE, GICD_SIZE, gicr - SOC_BASE, GICR_SIZE)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        # No hardware: watchdogd checks IODeviceTree:/arm-io/wdt for no-pmu and
        # otherwise crashes trying to set the PMU boot stage (customer builds
        # ignore the -wdt_no_pmu boot-arg).
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        # Main display: IOMobileFramebuffer.framework counts /arm-io children
        # with this device_type and reads the canvas size from the node
        # (the presented iPhone 8 size, see SweetFramebuffer.cpp);
        # the kernel's AppleCLCD matches the compatible string.
        # No "wlan" node: launchd only starts wifid with one, and there is no
        # Wi-Fi driver for the WCN3990 yet (topaz's lives in patches-topaz/).
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            # --native: present the real 1080x1920 panel. Without it iOS draws the
            # 750x1334 iPhone 8 canvas, scaled up to the whole panel (patch 0083).
            ('canvas-width', u32(FB_W if native else LOGICAL_W)),
            ('canvas-height', u32(FB_H if native else LOGICAL_H)),
        ])])

    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
    ])

    # bsd/dev/sweet_ufs.c: the UFS host ABL leaves running (absolute physical reg).
    ufs = node('sweet-ufs', [
        ph(),
        ('compatible', cstr('qcom,ufshc')),
        ('reg', u64(UFS, UFS_SIZE)),
    ] + ([('container-extents', u64(*[v for e in container for v in e]))] if container else []))

    # pmap_compute_max_asids requires the node even when no overrides are set.
    # An absent pmap-max-asids property selects XNU's own MAX_ASIDS default.
    # No effaceable storage: keybagd then reads /private/var/keybags/systembag.kb
    # as a plain plist instead of decrypting it with the effaceable key.
    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    # MobileGestalt HomeButtonType = IODeviceTree:/buttons home-button-type
    # (D22AP: 2, no Home button; absent = 0 = Home-button device). Take the
    # real D20AP /buttons; the fallback leaves home-button-type out, which
    # also means a Home-button device.
    buttons_props = d20ap_node('buttons') or [
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold') + cstr('menu')),
        ('opposed-power-vol-buttons', u32(1)),
    ]
    buttons = node('buttons', [ph()] + buttons_props)

    product_props = d20ap_node('product')
    # Our framebuffer is sRGB 8-bit. With the iPhone 8's 'P3' MobileGestalt says
    # HasExtendedColorDisplay and WebKit renders page tiles into 10-bit surfaces
    # ('w30r', 'b3a8') that this kernel does not provide: Safari pages stay blank.
    if product_props:
        product_props = [(k, cstr('sRGB') if k == 'artwork-display-gamut' else v) for k, v in product_props]
    # /product/haptics (closed-loop 1), as the iPhone 8 has it: MobileGestalt's
    # DeviceSupportsClosedLoopHaptics. With it iOS plays its haptics as
    # waveforms on the audio server's "Actuator" (userland/audio), which the
    # kernel plays on the PMI8998 motor (xnu 0141, /dev/sweet-haptic). As
    # ios-apollo's mkadt.py. MobileGestalt caches the answer: reset its cache
    # once (NOTES-OP5.md, 0141 test).
    haptics = d20ap_child('product', 'haptics') if product_props and not os.environ.get('NO_HAPTICS') else None
    if product_props and haptics is None:
        print('warning: the D20AP tree has no /product/haptics: iOS will not play haptics', file=sys.stderr)
    product = [node('product', [ph()] + product_props, [node('haptics', [ph()] + haptics)] if haptics else [])] \
        if product_props else []

    # Identity of the iPhone 8 whose userland runs here: compatible[0] is
    # hw.model/hw.target (D20AP), model is hw.machine/hw.product
    # (iPhone10,1). 'xiaomi,sweet' and qcom,kryo470 stay because the
    # SweetPlatformExpert and CPU IONameMatch still use them.
    return node('device-tree', [
        ph(),
        ('compatible', cstr('D20AP') + cstr('iPhone10,1') + cstr('oneplus,cheeseburger') + cstr('xiaomi,sweet') + cstr('qcom,msm8998') + cstr('AppleARM')),
        ('model', cstr('iPhone10,1')),
        ('target-type', cstr('D20')),
        ('target-sub-type', cstr('D20AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons] + product + ([] if qemu else [ufs]))


if __name__ == '__main__':
    cmap = sys.argv[sys.argv.index('--container') + 1] if '--container' in sys.argv else None
    root = build(qemu='--qemu' in sys.argv, highmem='--highmem' in sys.argv,
                 native='--native' in sys.argv, midmem='--midmem' in sys.argv,
                 container=container_extents(cmap) if cmap else None)
    blob = adt.serialize(root)
    open(sys.argv[1], 'wb').write(blob)
    if '--dump' in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f'{sys.argv[1]}: {len(blob)} bytes', file=sys.stderr)
