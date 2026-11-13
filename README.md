# Lakka for PS4 1.00

Lakka 6.1 (RetroArch) on a jailbroken PS4, installed from one PKG.

## What you need
- A PS4 on firmware 5.05 up to 14.00 (the range the Linux loader supports) with GoldHEN
  (tested: 2.4), BinLoader enabled (port 9090).
- A TV on HDMI, a controller (DualShock 4 over USB or Bluetooth).
- Free space on the PS4 drive: ~1.2 GB for the files, plus the storage you choose, plus
  10 GB that the app always leaves free.

## Install
1. Install `IV0000-LAKK00001_00-LAKKAPS4LOADER00.pkg` (GoldHEN > Package Installer).
2. Launch **Lakka for PS4**. It copies the Lakka files to `/data/linux/boot`.
3. Choose the storage size (**Install / Next size**). Sizes that don't fit right now are
   marked with how much more space they need.
4. The app reserves the space (one time), waits until the drive is idle, and starts Linux.
5. First boot: "Formatting Lakka storage (one time)", orange LED. Then Lakka's menu.

Later launches skip the copy and boot within seconds. To change the storage size later,
delete `lakka-storage.img` and `lakka-installed` in `/data/linux/boot` over FTP and launch the
app again - **this erases ROMs, saves and settings**.

## Using it
- ROMs: over the network (`\\LAKKA\ROMs`, SMB) or on a USB stick (FAT32, exFAT, NTFS, ext4).
- Back to OrbisOS: Lakka's Main Menu > **Shutdown** (the PS4 turns off) or **Restart**.
- ChromeOS for PS4 can stay installed: whichever app you launch decides what boots.

## Files on the PS4 drive (`/data/linux/boot`)
| File | What it is |
|---|---|
| `bzImage`, `initramfs.cpio.gz` | PS4 Linux kernel and initramfs (Lakka build) |
| `SYSTEM` | Lakka 6.1's system image, unmodified |
| `lakka-ps4-layer.sqfs` | PS4 graphics drivers (Mesa 25.1.9 + PS4 support), read-only |
| `lakka-storage.img` | Lakka's storage (ext4): ROMs, saves, settings |
| `lakka-installed` | written once the storage exists: the app stops asking |
| `lakka-error.txt` | written when a boot fails: the reason |
| `Linux_Loader_1G_VRAM.bin` | the Linux loader payload (also in `/data/payloads`) |

## Troubleshooting
- **The boot stops with an error**: it is shown on the TV and saved to
  `/data/linux/boot/lakka-error.txt`; the PS4 restarts into OrbisOS after 60 s. Read the
  file over FTP.
- **"The PS4 drive file system is not clean"**: boot OrbisOS once and shut it down normally.
- **"The Lakka storage ... is not empty either: not formatting it"**: the storage file holds
  something that is not a Lakka storage. Nothing was erased. To start over, delete
  `lakka-storage.img` and `lakka-installed` (erases saves).
- **"The payload could not be started"** in the app: GoldHEN's BinLoader is off. Restart
  the PS4, load GoldHEN, launch the app again (or start the payload from GoldHEN's menu).
- App logs: GoldHEN klog (port 3232), prefix `[ps4-linux-pkg]`.

## Building
Everything is in `lakka-ps4-1.00-src.tar.xz`:
1. `mesa/build-mesa.sh` - Mesa 25.1.9 + `mesa-25.1.9-ps4-liverpool.patch` in Docker (Debian 12),
   linked against Lakka's own `libLLVM.so.19.1` (extracted from SYSTEM).
2. `scripts/make-layer.sh` - the PS4 layer.
3. `scripts/build-kernel.sh` - Strawberry kernel (`~/linux`) + `scripts/lakka.cfg`.
4. `scripts/build-initramfs.sh` - PS4 initramfs + `initramfs/overlay*` (45-lakka.sh).
5. `ps4-linux-pkg/make-pkg.sh lakka.conf` - the PKG (OpenOrbis toolchain).

Upstream sources:
- Lakka 6.1: https://github.com/libretro/Lakka-LibreELEC/releases/tag/v6.1 (source: the
  same repository at tag v6.1)
- Strawberry kernel: https://gitlab.com/rmuxnet/linux
- Mesa 25.1.9: https://archive.mesa3d.org/mesa-25.1.9.tar.xz
- PS4 initramfs: https://github.com/GattoDev-debug/initramfs-ps4
- Linux loader: https://github.com/ps4-linux/ps4-linux-loader
- OpenOrbis toolchain: https://github.com/OpenOrbis ; GoldHEN: https://github.com/GoldHEN

Private use only - see RELEASE-NOTES.txt and CREDITS.md.
