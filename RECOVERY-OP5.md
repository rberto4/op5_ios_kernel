# OnePlus 5 (cheeseburger): recovery runbook

`RECOVERY.md` is topaz's (Redmi Note 12 4G). This one is for the OnePlus 5 (A5000) as this port uses
it: bootloader unlocked, stock OxygenOS, every iOS test a `fastboot boot` from RAM, the iOS root in
`/data/ios/root.img` (container, PORT-OP5.md "Keeping Android"). Nothing in that flow writes `boot`,
`recovery`, `system`, `vendor`, the bootloaders or the GPT, so most failures end at level 0 or 1.

Go down the levels in order. **Every command that writes a partition is marked WRITE: run it only
after deciding to, one at a time.** `persist`, `modemst1`, `modemst2`, `fsg`, `fsc` hold calibration
and the radio's EFS (IMEI); they cannot be downloaded anywhere, only restored from your own backup.

## Backups this runbook relies on

Taken before any test (Fase 1, 2026-11-08), kept outside git and copied off the Mac:

| File | What | How it was taken |
| --- | --- | --- |
| `backup/{persist,modemst1,modemst2,fsg,fsc,boot}.img` + `SHA256SUMS` | Partition images | TWRP `dd`, three-way sha256 check |
| `backup/partition-table.txt`, `backup/proc-partitions.txt` | by-name links, sizes | TWRP |
| `backup/gpt/sd{a..f}.{head,tail}.bin` + `SHA256SUMS` | First and last 1 MiB of every UFS LUN (primary and backup GPT) | TWRP `dd`, read twice |

On the unit this was written for, `fsg` is all zeros: `modemst1`/`modemst2` have no on-device golden
copy, so their backup is the only one.

## Level 0: iOS (or a probe) hangs

Hold **power + volume up** for 10-15 s: the phone resets into fastboot. Holding **power** alone
resets it into Android. Nothing to repair: the test ran from RAM. Note: this ABL (OxygenOS 10.0.1)
ignores the "reboot to bootloader" reason the shim writes, so a probe that ends by itself boots
Android, not fastboot.

## Level 1: Android does not boot after a test

`boot`/`system` were not written, so look at `userdata` first.

```sh
fastboot boot twrp-3.4.0-1-cheeseburger.img        # RAM only
adb shell 'mount | grep " /data "; ls -l /data/ios'
```

- Do **not** accept "Format data" / factory reset prompts (Android or TWRP): they erase the
  container and every app's data.
- If the container is suspected (a kernel wrote outside it), compare its map with the one the ADT
  was built from: `adb shell /tmp/ioscont map /data/ios/root.img` vs `research/root.map`.
- Removing the iOS root frees the space and returns Android to its earlier state
  (WRITE, userdata only): `adb shell rm /data/ios/root.img`.

## Level 2: calibration / radio data damaged (no IMEI, sensors, Wi-Fi MAC)

Check `*#06#` in Android. To restore from the backup (TWRP in RAM; WRITE, one partition at a time,
after checking the image's sha256 against `backup/SHA256SUMS`):

```sh
shasum -a 256 -c backup/SHA256SUMS
adb push backup/modemst1.img /tmp/ && adb shell 'sha256sum /tmp/modemst1.img'
adb shell 'dd if=/tmp/modemst1.img of=/dev/block/bootdevice/by-name/modemst1 bs=1048576 && sync'   # WRITE
```

Same for `modemst2`, `fsc`, `persist` (not `fsg` on a unit where it is zero). Read back with
`sha256sum /dev/block/bootdevice/by-name/<p>` and compare.

## Level 3: a GPT is damaged (fastboot "partition not found", nothing boots)

topaz lost its LUN0 GPT to a UFS write bug (RECOVERY.md). The container write guard (patch 0084)
exists to prevent that here, but if it happens: the 1 MiB dumps contain the GPT **and the start of
the first partitions** (on LUN0 `ssd` at block 6 and `persist` from block 8), so restore **only the
GPT blocks**, never the whole dump.

- Primary GPT: blocks 0-5 of `sdX.head.bin` (protective MBR, header at LBA 1, entries from LBA 2;
  the first partition starts at LBA 6 on every LUN of this unit; check with
  `python3 -c` on the header's `first_usable_lba` before writing).
- Backup GPT: the header is the last block of the LUN and its entries sit just before it
  (header field `partition_entries_lba`); take the same blocks from `sdX.tail.bin`.

Measured on this unit's dumps (4 KiB blocks; checked 2026-11-08):

| LUN | Primary GPT | First partition | Backup GPT (entries .. header) |
| --- | --- | --- | --- |
| sda | 0-5 | 6 | 14145531 .. 14145535 (last 5 blocks) |
| sdb, sdc | 0-5 | 6 | 1019 .. 1023 |
| sdd | 0-5 | 6 | 32763 .. 32767 |
| sde (64 entries) | 0-5 | 6 | 1048571 .. 1048575 |
| sdf | 0-5 | 6 | 393211 .. 393215 |

So: primary = the first 6 blocks of `sdX.head.bin`, backup = the last 5 blocks of `sdX.tail.bin`.

```sh
dd if=backup/gpt/sda.head.bin of=gpt0-primary.bin bs=4096 count=6
adb push gpt0-primary.bin /tmp/
adb shell 'dd if=/tmp/gpt0-primary.bin of=/dev/block/sda bs=4096 count=6 conv=fsync'   # WRITE
```

Then `fastboot reboot bootloader` and check `fastboot getvar partition-size:userdata`.

## Level 4: no fastboot at all (hard brick): EDL + MsmDownloadTool

The Snapdragon 835's emergency download mode (EDL, USB id 05c6:9008) is in the boot ROM and works
even with every partition destroyed. OnePlus's MsmDownloadTool (Windows) reflashes the whole phone
to a stock OxygenOS build.

**Prepare now, not when it is needed:**

1. On the Windows 11 PC, get the OnePlus 5 MsmDownloadTool package and the Qualcomm HS-USB
   QDLoader 9008 driver. They are not hosted by OnePlus any more; the usual source is the XDA
   OnePlus 5 unbrick thread. Check the package against the checksum published there, scan it,
   and keep a copy offline. Record it here:

   | Item | File | Version | SHA-256 |
   | --- | --- | --- | --- |
   | MsmDownloadTool package | TODO | TODO | TODO |
   | QDLoader 9008 driver | TODO | TODO | TODO |

2. Test the path once without flashing: power the phone off; hold **volume up** and plug the USB
   cable into the PC (do not press power). The screen stays black; Windows' Device Manager shows
   "Qualcomm HS-USB QDLoader 9008" (or "QUSB_BULK" without the driver). Leave EDL by holding
   **power + volume up** for 10-15 s. Do not press Start in the tool during this test.

**Unbrick (erases everything):**

1. Enter EDL as above, start MsmDownloadTool as administrator, wait for the port to show as
   connected, press **Start**. About 5 minutes; do not unplug. The phone boots stock OxygenOS.
2. The tool rewrites every partition, possibly including `persist` and the modem EFS from the
   package's defaults, and relocks the bootloader. Afterwards check `*#06#` (IMEI), Wi-Fi,
   sensors. If anything is missing: unlock again (Developer options, OEM unlocking;
   `fastboot oem unlock`, which wipes data), then level 2 from the backup.

## Never

- `fastboot flash`/`erase` of anything, `fastboot oem lock`, without a decision for that command.
- Writing `persist`, `modemst1`, `modemst2`, `fsg`, `fsc` except as a level 2 restore.
- Restoring a whole 1 MiB GPT dump (it overwrites the first partitions' data).
- An iOS kernel with `ufsrw=1` on a container whose map was not rebuilt after `root.img` changed.
