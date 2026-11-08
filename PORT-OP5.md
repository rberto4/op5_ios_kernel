# OnePlus 5 (cheeseburger) port

## Can it work?

Probably, with real driver work. The deciding facts:

- **CPU: same class as topaz, where iOS 16 already runs.** The Snapdragon 835's Kryo 280 is 4 Gold cores (Cortex-A73 class, MPIDR `0x100`-`0x103`) and 4 Silver cores (Cortex-A53 class, `0x0`-`0x3`), all ARMv8.0. Topaz's Kryo 260 is the same pairing and layout. There, Gold has the 16K translation granule and Silver does not; on a Hi3660 (A73 + A53) Linux saw the same split. iOS 16 needs 16K pages, so XNU runs on the four Gold cores. **Probe page 1 has to confirm this for Kryo 280**; if Gold lacked 16K, only topaz's experimental 4K-page kernel (patches 0140-0142) would remain.
- **ARMv8.0 gaps are solved upstream:** no LSE atomics, no PAN, no RCpc. Topaz patch 0081 builds XNU for `cortex-a53`, drops PAN and emulates the userland's LSE and `ldapr` instructions.
- **Storage is UFS 2.1**, like sweet, whose UFS driver this tree carries.
- **The bootloader can `fastboot boot`** a v0 boot image and leaves the panel lit (mainline Linux uses its framebuffer as simplefb).
- **Board drivers are new work:** the touch controller sits on an older I2C block (BLSP QUP, not GENI), its power rails are owned by RPM (not RPMh), and the PMIC arbiter is v3 (not v5). None of sweet's or topaz's input drivers apply as is.
- **The panel is 16:9 (1080x1920)**, so this port presents an **iPhone 8** (750x1334, also 16:9) and scales it up 1.44x to the full panel (see "Display").

## Hardware map

Sources: mainline `msm8998.dtsi` and `msm8998-oneplus-common.dtsi`, the OnePlus msm8998 kernel (`cheeseburger.dtsi`, Synaptics node, panel `dsi-panel-samsung_s6e3fa5_1080p_cmd.dtsi`), LineageOS `msm8998-common` BoardConfig. Values marked *probe* are to be confirmed by the stage 0 probe.

| Item | OnePlus 5 (MSM8998) | topaz (SM6225) |
| --- | --- | --- |
| Boot image | v0, `Image.gz` + appended DTB, base 0, page 4096 | v4, payload behind `_end` |
| CPU | Kryo 280: Silver `0x0`-`0x3`, Gold `0x100`-`0x103`; 16K on Gold only (*probe*) | Kryo 260, same layout |
| RAM | 6 GB (this phone) or 8 GB from `0x80000000`; carve-outs all below 4 GB; XNU gets `0x1_00000000` + `0xfcc00000`, inside the 6 GB model's 4 GB upper bank (*probe*) | `0x1_40000000` + 4 GB |
| Framebuffer | `0x9d400000`, 1080x1920, a8r8g8b8, stride 4320; Samsung S6E3FA5 **command-mode** panel (*probe*) | `0x5c000000`, 1080x2400 video mode |
| GIC v3 | GICD `0x17a00000`, GICR `0x17b00000` (8 x `0x20000`) | `0xf200000` / `0xf300000` |
| Timer | 19.2 MHz, virtual timer PPI 3 = INTID 19 | same |
| PSCI | 1.0 via `smc` | same |
| UFS | UFS 2.1 host `0x1da4000` (PHY `0x1da7000`) | UFS 3.0 `0x4804000` |
| Reset | PS_HOLD `0x10ac000`, IMEM restart reason `0x146bf65c` | `0x440b000`, `0xc12565c` |
| PMIC bus | SPMI arbiter **v3**: core `0x800f000`, obsrvr `0x9400000`, chnls `0x8400000`, intr `0xa400000`, cnfg `0x800a000` | v5 |
| Keys | Power: PM8998 PON (`0x810` bit 0). Vol-: PM8998 GPIO5, Vol+: GPIO6 (active low) | PM6125 |
| Touch | Synaptics S3320 (RMI4, F01/F12), I2C `0x20` on BLSP1 QUP5 (`0xc179000`, QUP v2.2.1); SDA/SCL GPIO 87/88 (func 3), reset GPIO 89, IRQ GPIO 125; vdd L28 3.0 V, vio L6 1.8 V | FocalTech FT5452 on GENI QUP0 SE2 |
| Regulators | RPM over GLINK: msg RAM `0x778000`, APCS IPC `0x17911000` | RPM |
| TLMM | `0x3400000`, tiles WEST `+0x100000`, NORTH `+0x500000`, EAST `+0x900000` | 3 tiles, other offsets |
| Clocks | GCC `0x100000` (BLSP1 AHB CBCR `0x17004`, QUP5 I2C `0x21008`, UFS AHB `0x7500c`) | |
| Gold CPU clock | OSM, not cpufreq-hw | cpufreq-hw `0xf523000` |
| GPU, Wi-Fi | Adreno 540; WCN3990 (needs the modem up, MBA self-auth) | Adreno 610; WCN3988 via PAS |
| Reserved GPIOs | 0-3, 81-84 (secure: never touch) | |

## What this fork changes from topaz

- Patch series cut after 0082. Kept: sweet 0001-0079 (generic bring-up; sweet's SM7150 key/touch/USB drivers only run with `sweethw=1`) and topaz 0080-0082 (CPU topology, ARMv8.0, boot on Gold). Moved to `xnu/patches-topaz/`: everything from 0083 (SM6225 touch, keys, MDP scanout, modem/Wi-Fi, GPU, audio, 4K pages). Several of those poke SM6225 MMIO and would hang or reset this phone.
- Patches 0010/0012 (early framebuffer console): `0x9d400000`, 1080x1920. XNU maps it through one 32 MB block, which must stay 32 MB aligned: the block base stays `0x9c000000` (it covers the splash buffer, and nothing XPU-protected lies in it) and the pixel pointer gets the `0x1400000` offset. The whole series applies cleanly to xnu-8796.141.3 (checked).
- Patch 0080: CPU name Kryo 280.
- `shim/`: OnePlus 5 addresses; always hands over to Gold `0x100` (the ADT and patch 0082 expect XNU there); no SM6225 cpufreq write; boot.img v0 with `Image.gz` + appended DTB (payload in the ramdisk, or `EMBED=1` behind `_end`); the stage 0 probe (`probe.c`) when there is no payload.
- `tools/mkadt.py`: GIC, UFS, framebuffer, RAM, synthetic ECID, Kryo 280; no `wlan` node (no Wi-Fi driver, so `wifid` should not start); `--midmem` refuses to run (on MSM8998 RAM from `0x80000000` holds the hyp/TZ/modem carve-outs).
- `tools/unpack_bootv0.py`, `tools/mkdtb-mainline.sh`: DTBs for the boot image.

## Stages

Each stage is a `fastboot boot` from RAM until stage 2 flashes `userdata`.

0. **Probe** (`shim/out/shim-boot.img`, no payload). Photos of the four pages. Then update `HIGH_MEM_SIZE` (or `HIGHSIZE=`) in `tools/mkadt.py` from page 2 and check page 1's verdict.
1. **XNU to the root mount, no root yet.** Kernel from CI, then
   `python3 tools/mkadt.py shim/out/cb.adt --highmem` and
   `python3 tools/mkpayload.py xnu/out/kernel.development.sweet shim/out/cb.adt shim/out/payload.bin --cmdline 'debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress rd=ufsroot0 wdt=-1 wdt_test=no_user_checkin'`, then `shim/build.sh shim/out/payload.bin`.
   Success: early markers at the bottom of the screen, kernel text scrolling, the UFS driver finding `userdata`, then a panic about the missing root. Read-only: no `ufsrw=1`. If ABL drops the v0 ramdisk ("no initrd" on screen), rebuild with `EMBED=1`.
2. **iOS root and GUI, no input.** Root image from the **iPhone 8** (iPhone10,1, D20AP) IPSW of the same iOS 16.7 build, made as in [README-topaz.md](README-topaz.md) (use the D20AP Erase identity in BuildManifest.plist, and put `DeviceTree.d20ap.im4p`, extracted, at `research/ipsw-8/d20ap.adt` for `mkadt.py`). Either flash `userdata` (destructive: confirm first) or keep Android with the container file (see "Keeping Android"). Boot with `ufsrw=1 rootrw=1 launchd_unsecure_cache=1 sweetfb_noconsole=1 sweetfb_hz=30`, then `cpus=4`. Expect the lock screen after several minutes.
3. **Keys.** Small: see below.
4. **Touch.** Large: see below.
5. **Display identity / scaling.**
6. Later: Wi-Fi (modem MSS boot with MBA, then topaz's WLFW/HTT stack for the WCN3990), GPU (Adreno 540 through topaz's Mesa freedreno route), audio, battery.

## Driver work list

### Keys (patch 0060 `bsd/dev/sweet_keys.c`, SPMI)

Sweet's driver polls the PMIC through the SPMI arbiter v5 observer. For v3 (`drivers/spmi/spmi-pmic-arb.c`, `pmic_arb_v3` ops):

- APID map at `core + 0x800 + 4n` (v5: `+0x900`), PPID in bits 19:8, owner at `cnfg + 0x700 + 4n` (same).
- Observer channel at `obsrvr + 0x1000 * ee + 0x8000 * apid` (v5: `0x10000 * ee + 0x80 * apid`). Command, status (`+0x08`) and data (`+0x18`) are the same.
- PON real-time status `0x810`, bit 0 = power. Volume: PM8998 GPIO5 (`0xc400`, PPID `0x0c4`) and GPIO6 (`0xc500`), active low, instead of sweet's PM6150L GPIO2.

The probe's `spmi_rd()` already implements the v3 read; page 3's live key view confirms the bits.

### Touch

1. **Power.** L28 (3.0 V) and L6 (1.8 V) belong to RPM. If probe page 3 shows them enabled, skip this step at first. Otherwise: RPM SMD requests over GLINK (`drivers/rpmsg/qcom_glink_rpm.c`, `drivers/soc/qcom/smd-rpm.c`, `drivers/regulator/qcom_smd-regulator.c`): resource type `ldoa`, id 28 / 6, keys `swen` = 1 and `uv` = voltage. Topaz's planned `TopazRpm` (in topaz-woa) is the same protocol on SM6225.
2. **Clocks.** BLSP1 AHB (voted, `0x52004` bit 17) and BLSP1 QUP5 I2C APPS (CBCR `0x21008`, plus its RCG at 19.2 MHz); `drivers/clk/qcom/gcc-msm8998.c`.
3. **Pins.** GPIO 87/88 to function 3 (`blsp_i2c5`), pull-up; reset GPIO 89 output; IRQ GPIO 125 input (poll first, as sweet did).
4. **QUP v2 I2C** in FIFO mode with V2 tags: `drivers/i2c/busses/i2c-qup.c`. A new file next to sweet's GENI code.
5. **Synaptics RMI4**: page select register `0xff`, PDT scan from `0xe9` down, F01 (device control), F12 (2D, 10 fingers): `drivers/input/rmi4/rmi_f12.c`. OnePlus's `synaptics_driver_s3320.c` shows the register pages it uses. The capacitive Back/Recents keys come from the same chip (F1A) and can serve as a Home button.
6. **HID side**: reuse sweet's `SweetHID` digitizer service. Its panel mapping hard-codes 2400 rows (`SweetHID.cpp`, `fOffY = (fLogicalH - 2400) / 2`): use the panel height instead.

### Display (decided: iPhone 8)

The device tree (`tools/mkadt.py`) presents an **iPhone 8** (`iPhone10,1`, D20AP, 750x1334 at 2x = 375x667 points, Home-button UI). Kernel patch 0083 makes AppleCLCD report 750x1334 and scale each frame up 1.44x (nearest neighbour) into the 1080x1920 panel, so nothing is cut off; touch is normalised to the panel and needs no scaling. Checked on the host: the full-screen layer covers every panel pixel, and partial layers never read outside their rectangle. Not yet compiled (CI) or run.

Rendering 750x1334 is about 2.7x fewer pixels than the iPhone X canvas, which helps the CPU-only renderer.

**Home button.** iPhone 8 mode has no swipe-up gesture: leaving an app needs a Home press (`kHIDUsage_Csmr_Menu`, already in sweet's HID layer as `sweet_hid_key(3, ...)`). Plan: when the keys driver is ported, both volume keys pressed together send Home; once touch works, the Synaptics capacitive key next to the fingerprint button does. The fingerprint button itself is read by TrustZone and is out of reach for now.

`sweetfb_native=1` with `mkadt.py --native` presents the raw 1080x1920 panel at 3x instead (no scaling, layout untested).

## Keeping Android (no erase): the container file

Every stage boots with `fastboot boot` from RAM, so `boot`, `system`, `vendor` and the bootloaders are never written: powering on normally still starts Android. The iOS root lives in one file, `/data/ios/root.img`, inside Android's `userdata`.

**Verified on this phone (2026-10-23, rooted Pixel Experience 12):** `ioscont check /data/ios` wrote 64 MiB, the raw `userdata` blocks matched exactly (6 extents, 0 mismatched): `/data` is ext4 (blocks never move), and a directory created by root at `/data/ios` is stored unencrypted even though Android uses file-based encryption. 32 GiB free.

How it works: kernel patch 0084 gives the UFS root driver an extent map (`container-extents` in `/sweet-ufs`, from `tools/mkadt.py --container`). `ufsroot0` is the file, every I/O is translated through the map, and every write must also lie inside one of the file's extents, so nothing else in Android's data can be written. The persistent kernel log uses the last 4 MiB + 4 KiB of the container, behind the HFS+ volume. Host test: 20,000 random reads and writes over 6 scattered extents land correctly; writes outside the file or across two extents are refused. Not yet compiled (CI) or run.

**Install** (once the kernel boots; nothing else on the phone changes):

```sh
# 1. HFS+ root image, 16 GiB (README-topaz.md, iPhone 8 IPSW), then 8 MiB more for the log
truncate -s +8M ios16-root.img
# 2. stream it into the unencrypted directory as root (not via /data/local/tmp or /sdcard:
#    those are encrypted, the blocks on flash would be ciphertext)
adb exec-in "su -c 'cat > /data/ios/root.img'" < ios16-root.img
adb shell "su -c 'sync; /data/local/tmp/ioscont map /data/ios/root.img'" > root.map
# 3. device tree with the map, then payload and boot image as usual
python3 tools/mkadt.py shim/out/cb.adt --highmem --container root.map
```

Rules: never move, copy over, truncate or defragment `root.img` from Android (its blocks would change). If it is ever rewritten, run `ioscont map` again and rebuild the ADT. A factory reset deletes it. Read the kernel log afterwards from Android with `tools/sweet-log-pull.sh log.txt container`.

Other ways, not recommended: shrinking `userdata` (f2fs cannot shrink, and full-disk encryption keeps a footer at the end), or a USB stick over OTG (needs a USB host driver first; topaz-woa has one to port later).

## Probe results on a second unit (2026-11-08)

A second A5000 (6 GB, stock OxygenOS 10.0.1, bootloader unlocked) ran the stage 0 probe
(`shim/out/shim-boot.img`, default build, built on macOS with Homebrew `aarch64-elf-gcc` 16.2).
Observed (photos, not in git):

- Display: text from the top edge, no shift: splash at `0x9d400000` (ABL patches the
  `splash_region@9d600000` node to base `0x9D400000`, size `0x2400000`).
- CPU: Silver MIDR `0x51AF8014` MMFR0 `0x1122` (tg16 0); Gold MIDR `0x51AF8001` MMFR0
  `0x101122` (tg16 1). Gold hand-off, GIC (vtimer, SGI1 10/10, GICR 0x101-0x103) OK. Page 1/4
  (per-core table) was not photographed.
- Clocks: both OSMs disabled; silver PLL L `0x51`, gold PLL L `0x20`, both locked; gold RCG
  switch to GPLL0 / gold PLL works (ticks `0x753008` / `0x7270E2`).
- ABL passes the v0 ramdisk: INITRD `0x455FE000`-`0x455FE065` (the 101-byte probe ramdisk).
- RAM: `<0x40000000 0xC0000000>`, `<0x1_00000000 0xBD8C0000>` (6 GB), same as the first unit;
  all carve-outs below 4 GB.
- PMIC: SPMI v3, PM8998 (REVID type `0x51` sub `0x14` rev4 `0x02`); vol-/vol+ GPIO5/6 st1
  `0x81` idle. Key presses were not captured by the live view. BLSP1/QUP5 clocks off, TLMM
  87/88/89/125 func 0. The L28/L6 lines use SID 0 PPIDs and do not reflect the touch rails.
- UFS: UFSHCI `0x210`, cap `0x1587001F`, HCE 1, HCS `0x10F`; TUR/READ CAPACITY/READ rc 0;
  LUN0 last LBA `0xD7D7FF`; GPT 32 entries; userdata LBA `0x14F40`, 54920 MiB, ext4.
- After the last page this unit booted Android instead of fastboot: its ABL (OOS 10.0.1)
  ignores the `0x77665500` IMEM restart reason. Re-enter fastboot with power + vol up.

Every measured value matches the first unit.

Stage 1 on the same unit (2026-11-08, video): kernel `kernel-run40186605448` (0089, sha256
`66f2a902...`), `mkadt.py --highmem`, boot-args `debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress
rd=ufsroot0 wdt=-1 wdt_test=no_user_checkin sweetlog=0 sweettouch=0` (UFS read-only, touch and its
RPM rail votes off). XNU came up on gold `0x100` (`1/1 CPUs initialized`), AppleCLCD 1080x1920
at `0x9d400000` presented as 750x1334, `SWEET UFS: "userdata" ... 54920 MiB, read-only`, `SWEET
KEYS` started (arb `0x30000000`, vol-/vol+ `0x81`), then `BSD root: ufsroot0` and the expected
`unknown Volume Signature : 20a5` / `mount(N) failed` loop (43 retries, no panic).

Stage 2 on the same unit (2026-11-08, videos): root image built on macOS with
`tools/mkiosroot-macos.sh` (iPhone10,1 16.7.16 20H392, 16 GiB + 8 MiB, sha256 `c0a46efb...`),
streamed into `/data/ios/root.img` from TWRP (`ioscont check` OK on OxygenOS 10 FBE; 137 extents),
ADT `--highmem --container`, kernel 0089, boot-args as stage 2 with `cpus=4 sweetfb_hwscale=0`,
gold at 614.4 MHz, touch on. Observed: `4/4 CPUs initialized`, Apple logo, iPhone 8 home screen,
App Library; touch and volume keys work; the two default widgets stay white ("Waiting...").
Note: `adb exec-in` dropped the last 51363 bytes of the 16 GiB stream (all zero); always check
size and sha256 on the phone after the copy.

## Open questions the probe answers

All answered on 2026-10-23 (first unit) and confirmed on 2026-11-08 (second unit), see above
and NOTES.md.

- Gold `TGran16` (page 1 VERDICT); Silver too (if yes, all 8 cores could run).
- Does the command-mode panel show CPU writes (any page visible at all), and is `0x9d400000` the right base (the OnePlus kernel's own DT says `0x9d600000`; text shifted up by about 485 rows would mean that one)?
- RAM size of the upper bank (6 GB vs 8 GB model), page 2.
- Touch rails and I2C clocks left on by ABL, page 3.
- Gold clock after PSCI `CPU_ON` (`SPEED` line, `~MHz` on page 1, rough: it assumes one add per cycle).
- UFS host state as ABL leaves it, page 4.
