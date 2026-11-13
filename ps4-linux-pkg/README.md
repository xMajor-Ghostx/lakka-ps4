# ps4-linux-pkg

Template for a PS4 Linux loader PKG. It's the ChromeOS for PS4 installer with every
ChromeOS part removed, and the same app flow:

1. Copy the package files to `/data/linux/boot` (payload also to `/data/payloads`), with a
   progress bar. Skipped when the console already has this build (`build-id` + sizes).
2. Install menu (if `RESERVE_NAME` is set): **PS4 drive / USB drive**, then the size
   (**Install / Next size**, recommended size marked, "all the free space" last).
3. Reserve: GoldHEN jailbreak to read the free space, zero-fill `<RESERVE_NAME>` from
   OrbisOS with a live MB/s + ETA (resumable), then unjailbreak.
4. Wait until the drive is idle (fsync probe, at least 30 s), then send the payload to
   GoldHEN BinLoader (127.0.0.1:9090) and boot.

The choice is saved in `/data/linux/boot/install-choice` (`target=internal` +
`size_gib=<n>`, or `target=external`). Your initramfs takes it from there. It writes
`DONE_MARKER` once installed, and after that the app only copies and boots.
Dry run: create `/data/linux/boot/pkg-dryrun` over FTP. The next launch asks the real
questions with the real sizes and changes nothing.

## Use

```sh
./make-pkg.sh                  # pkg.conf -> out/IV0000-<TITLE_ID>_00-<CONTENT_LABEL>.pkg
./make-pkg.sh other.conf       # one conf per distro
```

`drop/` ships the latest Strawberry kernel (`bzImage`), the PS4 Linux `initramfs.cpio.gz`
and the 1 GB VRAM loader. Swap them for another combo. Give each distro its own `TITLE_ID`.
All builds share `/data/linux/boot`, so the last one launched wins (the ChromeOS app copies
its own files back the next time it is launched).

**Safe test builds:** a file starting with `PS4-LINUX-PKG-DUMMY` (for example
`printf 'PS4-LINUX-PKG-DUMMY' > loader.bin`) makes `make-pkg.sh` build a test PKG. It runs the
whole flow in `/data/linux/boot-template`, adds nothing to GoldHEN's payload menu and never
boots, so a real install is left alone.

## Notes

- The kernel is installed as `bzImage` and the initramfs as `initramfs.cpio.gz`, which are
  the names the loader reads.
- Kernel command line: no `bootargs.txt`. Use `CONFIG_CMDLINE` or the payload's default.
- The payload must match the console's VRAM setting and must be 512 KB or smaller.
- Image size for a choice of N GB = N GiB + `RESERVE_EXTRA_MB`.
- The initramfs must not create directories on the PS4 drive from Linux (UFS driver).
- Logs go to the GoldHEN klog (port 3232) with the prefix `[ps4-linux-pkg]`.
