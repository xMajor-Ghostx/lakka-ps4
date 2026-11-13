// PS4 Linux loader PKG - generic installer (OpenOrbis homebrew, needs GoldHEN).
//
// Copies whatever make-pkg.sh put into the package to where the Linux loader expects
// it, optionally reserves one file of a fixed size on the PS4 drive, then hands the
// loader payload to GoldHEN's BinLoader. Nothing distro-specific lives here: the
// initramfs decides what to do with the files.
//
//   assets/linux/manifest.txt      what to copy / reserve (written by make-pkg.sh)
//   assets/linux/build-id          hash of the package files (skip copy when unchanged)
//   /data/linux/boot/<file>        every "file=" entry (bzImage, initramfs.cpio.gz, images...)
//   /data/linux/boot/<payload>     the loader payload; a copy also goes to /data/payloads
//   /data/linux/boot/<reserve>     optional: zero-filled once by OrbisOS (fast), never
//                                  deleted or resized by this app afterwards
//
// manifest.txt, one entry per line:
//   payload=<name>                 required, the loader .bin (also a package file)
//   file=<name>                    repeatable, copied to /data/linux/boot/<name>
//   reserve=<name> <MiB>           optional
//   keep_free_gb=<n>               optional, space the reserve always leaves free (10)
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <orbis/CommonDialog.h>
#include <orbis/MsgDialog.h>
#include <orbis/Sysmodule.h>
#include <orbis/SystemService.h>
#include <orbis/libkernel.h>
#include <orbis/Net.h>

#ifndef APP_TITLE
#define APP_TITLE "Linux for PS4"
#endif
#ifndef TITLE_ID
#error "TITLE_ID must be defined (make-pkg.sh passes it)"
#endif

// Inside the app sandbox the package is at /app0; after the GoldHEN jailbreak the real
// root is visible instead. The first location that has the manifest wins.
static const char *src_candidates[] = {
	"/app0/assets/linux",
	"/mnt/sandbox/pfsmnt/" TITLE_ID "-app0/assets/linux",
	"/mnt/sandbox/" TITLE_ID "_000/app0/assets/linux",
};
static char SRC[128];
#ifdef TEMPLATE_DUMMY
// Built from the template's dummy files: everything goes to a separate folder, nothing is
// put in GoldHEN's payload menu and nothing is booted, so the real install is untouched.
#define DST "/data/linux/boot-template"
static const char *payload_dirs[] = { DST };
#else
#define DST "/data/linux/boot"
static const char *payload_dirs[] = { DST, "/data/payloads" };
#endif
#define N_PAYLOAD_DIRS (sizeof(payload_dirs) / sizeof(payload_dirs[0]))
#define GB (1024ULL * 1024ULL * 1024ULL)
#define MANIFEST "manifest.txt"
#define BUILD_ID "build-id"

#define MAX_FILES 32
#define NAME_MAX_LEN 64
static char files[MAX_FILES][NAME_MAX_LEN];
static int n_files;
static char payload_name[NAME_MAX_LEN];
// Install menu + reserve (same flow as the ChromeOS for PS4 app), all from the manifest.
static char reserve_name[NAME_MAX_LEN];          // empty = no menu, no reserve
static char done_marker[NAME_MAX_LEN] = "install-done";
static uint64_t reserve_extra_mb;                // added to the chosen size (image headers etc.)
static uint64_t keep_free_gb = 10;
static uint64_t min_gb = 8, rec_gb = 32;
static uint64_t size_steps[8] = { 32, 64, 128, 256 };
static int n_steps = 4;
static int allow_usb = 1;

// ---------------------------------------------------------------- raw syscalls
static long orbis_syscall3(long num, long a1, long a2, long a3) {
	long ret;
	unsigned char err;
	__asm__ volatile("syscall\n\tsetc %1"
	                 : "=a"(ret), "=r"(err)
	                 : "a"(num), "D"(a1), "S"(a2), "d"(a3)
	                 : "rcx", "r11", "memory");
	return err ? -ret : ret;   // FreeBSD: carry set, rax = errno
}

// Everything goes to the GoldHEN klog (port 3232).
static void klog(const char *fmt, ...) {
	// One write per line: two calls (prefix, text) got split apart by other klog output.
	static const char prefix[] = "[ps4-linux-pkg] ";
	char buf[512];
	memcpy(buf, prefix, sizeof(prefix) - 1);
	size_t cap = sizeof(buf) - (sizeof(prefix) - 1) - 2;
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf + sizeof(prefix) - 1, cap + 1, fmt, ap);
	va_end(ap);
	if (n < 0) return;
	if ((size_t)n > cap) n = (int)cap;
	n += (int)sizeof(prefix) - 1;
	buf[n] = '\n'; buf[n + 1] = 0;
	sceKernelDebugOutText(0, buf);
}

// ---------------------------------------------------------------- dialogs
static void dialog_param_init(OrbisMsgDialogParam *p) {
	memset(p, 0, sizeof(*p));
	p->baseParam.size = sizeof(OrbisCommonDialogBaseParam);
	p->baseParam.magic = (uint32_t)(ORBIS_COMMON_DIALOG_MAGIC_NUMBER + (uint64_t)&p->baseParam);
	p->size = sizeof(*p);
}

// Returns 1 for OK/first button, 2 for second button, 0 for cancel.
static int dialog(OrbisMsgDialogButtonType type, const char *b1, const char *b2, const char *fmt, ...) {
	static char msg[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	OrbisMsgDialogParam p;
	OrbisMsgDialogUserMessageParam um;
	OrbisMsgDialogButtonsParam bp;
	OrbisMsgDialogResult r;
	dialog_param_init(&p);
	memset(&um, 0, sizeof(um));
	memset(&bp, 0, sizeof(bp));
	p.mode = ORBIS_MSG_DIALOG_MODE_USER_MSG;
	um.buttonType = type;
	um.msg = msg;
	if (type == ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS) {
		bp.msg1 = b1;
		bp.msg2 = b2;
		um.buttonsParam = &bp;
	}
	p.userMsgParam = &um;
	sceMsgDialogInitialize();
	if (sceMsgDialogOpen(&p) < 0) {
		sceMsgDialogTerminate();
		return 0;
	}
	while (sceMsgDialogUpdateStatus() != ORBIS_COMMON_DIALOG_STATUS_FINISHED)
		sceKernelUsleep(10000);
	memset(&r, 0, sizeof(r));
	sceMsgDialogGetResult(&r);
	sceMsgDialogTerminate();
	klog("dialog type %d result=%d buttonId=%d", (int)type, (int)r.result, (int)r.buttonId);
	if (r.buttonId == ORBIS_MSG_DIALOG_BUTTON_ID_BUTTON1) return 1;
	if (r.buttonId == ORBIS_MSG_DIALOG_BUTTON_ID_BUTTON2) return 2;
	return 0;
}

// Two-button dialog that closes itself after secs seconds: returns 3 on timeout,
// otherwise like dialog(). Lets a saved choice continue on its own (boot stays hands-free)
// while still offering a way to change it.
static int dialog_timed(int secs, const char *b1, const char *b2, const char *fmt, ...) {
	static char msg[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	OrbisMsgDialogParam p;
	OrbisMsgDialogUserMessageParam um;
	OrbisMsgDialogButtonsParam bp;
	OrbisMsgDialogResult r;
	dialog_param_init(&p);
	memset(&um, 0, sizeof(um));
	memset(&bp, 0, sizeof(bp));
	p.mode = ORBIS_MSG_DIALOG_MODE_USER_MSG;
	um.buttonType = ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS;
	um.msg = msg;
	bp.msg1 = b1;
	bp.msg2 = b2;
	um.buttonsParam = &bp;
	p.userMsgParam = &um;
	sceMsgDialogInitialize();
	if (sceMsgDialogOpen(&p) < 0) { sceMsgDialogTerminate(); return 3; }
	uint64_t t0 = sceKernelGetProcessTime();
	int timed_out = 0;
	while (sceMsgDialogUpdateStatus() != ORBIS_COMMON_DIALOG_STATUS_FINISHED) {
		if (!timed_out && sceKernelGetProcessTime() - t0 >= (uint64_t)secs * 1000000ULL) {
			timed_out = 1;
			sceMsgDialogClose();
		}
		sceKernelUsleep(10000);
	}
	memset(&r, 0, sizeof(r));
	sceMsgDialogGetResult(&r);
	sceMsgDialogTerminate();
	klog("timed dialog result=%d buttonId=%d timed_out=%d", (int)r.result, (int)r.buttonId, timed_out);
	if (timed_out) return 3;
	if (r.buttonId == ORBIS_MSG_DIALOG_BUTTON_ID_BUTTON1) return 1;
	if (r.buttonId == ORBIS_MSG_DIALOG_BUTTON_ID_BUTTON2) return 2;
	return 0;
}

static void quit(void) {
	sceSystemServiceLoadExec("exit", NULL);
	for (;;) sceKernelUsleep(1000000);
}

static void fail(const char *fmt, const char *arg) {
	dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL, fmt, arg);
	quit();
}

static int progress_open(const char *msg) {
	OrbisMsgDialogParam p;
	OrbisMsgDialogProgressBarParam pb;
	dialog_param_init(&p);
	memset(&pb, 0, sizeof(pb));
	p.mode = ORBIS_MSG_DIALOG_MODE_PROGRESS_BAR;
	pb.barType = ORBIS_MSG_DIALOG_PROGRESSBAR_TYPE_PERCENTAGE;
	pb.msg = msg;
	p.progBarParam = &pb;
	sceMsgDialogInitialize();
	return sceMsgDialogOpen(&p);
}

static void progress_set(uint32_t pct, const char *msg) {
	sceMsgDialogProgressBarSetValue(0, pct);
	if (msg) sceMsgDialogProgressBarSetMsg(0, msg);
	sceMsgDialogUpdateStatus();
}

static void progress_close(void) {
	sceMsgDialogClose();
	while (sceMsgDialogUpdateStatus() != ORBIS_COMMON_DIALOG_STATUS_FINISHED)
		sceKernelUsleep(10000);
	sceMsgDialogTerminate();
}

// ---------------------------------------------------------------- files
static uint64_t fd_size(int fd) {
	off_t cur = lseek(fd, 0, SEEK_CUR);
	off_t end = lseek(fd, 0, SEEK_END);
	if (cur >= 0) lseek(fd, cur, SEEK_SET);
	return end > 0 ? (uint64_t)end : 0;
}

static uint64_t file_size(const char *path) {
	int fd = open(path, O_RDONLY);
	if (fd < 0) return 0;
	uint64_t n = fd_size(fd);
	close(fd);
	return n;
}

static int exists(const char *path) {
	struct stat st;
	return stat(path, &st) == 0;
}

static int read_small(const char *path, char *buf, size_t cap) {
	int fd = open(path, O_RDONLY);
	if (fd < 0) return -1;
	ssize_t n = read(fd, buf, cap - 1);
	close(fd);
	if (n <= 0) return -1;
	buf[n] = 0;
	return 0;
}

// FreeBSD statfs (396), fstatfs (397), getfsstat (395) called directly; the sandbox
// refuses them (EPERM), so this only works after the GoldHEN jailbreak.
#define STATFS_SIZE 472   // FreeBSD 9 struct statfs; f_mntonname @384, f_fstypename @280

static int fs_parse(const char *how, const unsigned char *b, uint64_t *free_b) {
	uint64_t bsize, blocks; int64_t bavail;
	memcpy(&bsize, b + 16, 8); memcpy(&blocks, b + 32, 8); memcpy(&bavail, b + 48, 8);
	if (bavail < 0) bavail = 0;
	if (bsize == 0 || blocks == 0) { klog("%s: empty result", how); return -1; }
	*free_b = (uint64_t)bavail * bsize;
	klog("%s: free %llu MB of %llu MB", how, (unsigned long long)(*free_b >> 20),
	     (unsigned long long)((blocks * bsize) >> 20));
	return 0;
}

static uint64_t free_bytes(const char *path) {
	static uint64_t sf[64];
	static unsigned char all[64 * STATFS_SIZE];
	uint64_t f = 0;
	memset(sf, 0, sizeof(sf));
	long r = orbis_syscall3(396, (long)path, (long)sf, 0);
	if (r >= 0 && fs_parse("statfs", (unsigned char *)sf, &f) == 0) return f;
	if (r < 0) klog("statfs(%s) failed: errno %ld", path, -r);

	int fd = open(path, O_RDONLY);
	if (fd >= 0) {
		memset(sf, 0, sizeof(sf));
		r = orbis_syscall3(397, (long)fd, (long)sf, 0);
		close(fd);
		if (r >= 0 && fs_parse("fstatfs", (unsigned char *)sf, &f) == 0) return f;
		if (r < 0) klog("fstatfs(%s) failed: errno %ld", path, -r);
	}

	memset(all, 0, sizeof(all));
	r = orbis_syscall3(395, (long)all, (long)sizeof(all), 2 /* MNT_NOWAIT */);
	if (r < 0) { klog("getfsstat failed: errno %ld", -r); return 0; }
	for (long i = 0; i < r && i < 64; i++) {
		const char *on = (const char *)all + i * STATFS_SIZE + 384;
		if (strcmp(on, "/user") == 0 || strcmp(on, "/data") == 0)
			return fs_parse("getfsstat", all + i * STATFS_SIZE, &f) == 0 ? f : 0;
	}
	klog("getfsstat: no /user or /data mount");
	return 0;
}

static void mkdirs(const char *path) {
	char tmp[256];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++) {
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, 0777);
			*p = '/';
		}
	}
	mkdir(tmp, 0777);
}

// Copies the open source fd to dst (via dst.part, then rename), with progress.
static int copy_file(int in, const char *dst, uint64_t *done, uint64_t total, const char *label) {
	static unsigned char buf[4 * 1024 * 1024];
	char part[256], msg[256];
	snprintf(part, sizeof(part), "%s.part", dst);
	snprintf(msg, sizeof(msg), "Copying %s ...", label);
	progress_set(total ? (uint32_t)(*done * 100 / total) : 0, msg);
	if (in < 0 || lseek(in, 0, SEEK_SET) != 0) { klog("copy %s: source seek failed errno %d", label, errno); return -1; }
	// World-writable: the files can be replaced later over FTP.
	int out = open(part, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (out < 0) { klog("copy %s: open %s failed errno %d", label, part, errno); return -1; }
	klog("copy %s -> %s (%llu bytes)", label, dst, (unsigned long long)fd_size(in));
	ssize_t n;
	int rc = 0;
	while ((n = read(in, buf, sizeof(buf))) > 0) {
		ssize_t off = 0;
		while (off < n) {
			ssize_t w = write(out, buf + off, (size_t)(n - off));
			if (w <= 0) { klog("copy %s: write failed errno %d after %llu", label, errno, (unsigned long long)*done); rc = -1; break; }
			off += w;
		}
		if (rc) break;
		*done += (uint64_t)n;
		progress_set(total ? (uint32_t)(*done * 100 / total) : 0, NULL);
	}
	if (n < 0) { klog("copy %s: read failed errno %d", label, errno); rc = -1; }
	if (fsync(out) != 0) klog("copy %s: fsync errno %d (ignored)", label, errno);
	if (close(out) != 0) { klog("copy %s: close errno %d", label, errno); rc = -1; }
	if (rc == 0) {
		uint64_t a = file_size(part), b = fd_size(in);
		if (a != b) { klog("copy %s: size mismatch %llu != %llu", label, (unsigned long long)a, (unsigned long long)b); rc = -1; }
	}
	if (rc == 0) {
		unlink(dst);
		if (rename(part, dst) != 0) { klog("copy %s: rename failed errno %d", label, errno); rc = -1; }
		else chmod(dst, 0666);
	}
	if (rc != 0) unlink(part);
	return rc;
}

// ---------------------------------------------------------------- manifest
static void copy_name(char *dst, const char *src) {
	size_t n = strcspn(src, " \t\r\n");
	if (n == 0 || n >= NAME_MAX_LEN) fail("manifest.txt has a bad file name: %s", src);
	memcpy(dst, src, n);
	dst[n] = 0;
	if (strchr(dst, '/') || strcmp(dst, ".") == 0 || strcmp(dst, "..") == 0)
		fail("manifest.txt has a bad file name: %s", dst);
}

static void load_manifest(void) {
	static char text[8192];
	char path[256];
	snprintf(path, sizeof(path), "%s/%s", SRC, MANIFEST);
	if (read_small(path, text, sizeof(text)) != 0) fail("The package has no %s.\n\nRebuild it with make-pkg.sh.", MANIFEST);
	for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
		if (strncmp(line, "payload=", 8) == 0) copy_name(payload_name, line + 8);
		else if (strncmp(line, "file=", 5) == 0) {
			if (n_files >= MAX_FILES) fail("manifest.txt lists too many files.%s", "");
			copy_name(files[n_files++], line + 5);
		} else if (strncmp(line, "reserve=", 8) == 0) copy_name(reserve_name, line + 8);
		else if (strncmp(line, "done_marker=", 12) == 0) copy_name(done_marker, line + 12);
		else if (strncmp(line, "reserve_extra_mb=", 17) == 0) reserve_extra_mb = strtoull(line + 17, NULL, 10);
		else if (strncmp(line, "keep_free_gb=", 13) == 0) keep_free_gb = strtoull(line + 13, NULL, 10);
		else if (strncmp(line, "min_gb=", 7) == 0) min_gb = strtoull(line + 7, NULL, 10);
		else if (strncmp(line, "recommended_gb=", 15) == 0) rec_gb = strtoull(line + 15, NULL, 10);
		else if (strncmp(line, "usb=", 4) == 0) allow_usb = line[4] == '1';
		else if (strncmp(line, "sizes=", 6) == 0) {
			n_steps = 0;
			for (char *p = line + 6; *p && n_steps < 8;) {
				uint64_t v = strtoull(p, &p, 10);
				if (v) size_steps[n_steps++] = v;
				if (*p == ',') p++; else break;
			}
		}
	}
	if (!payload_name[0]) fail("manifest.txt names no payload.%s", "");
	if (min_gb == 0) min_gb = 1;
	klog("manifest: %d files, payload %s, reserve %s (+%llu MB), sizes %d from %llu GB, rec %llu GB, usb %d, keep free %llu GB",
	     n_files, payload_name, reserve_name[0] ? reserve_name : "-", (unsigned long long)reserve_extra_mb, n_steps,
	     (unsigned long long)min_gb, (unsigned long long)rec_gb, allow_usb, (unsigned long long)keep_free_gb);
}

// ---------------------------------------------------------------- build stamp
// build-id is copied last, so a matching stamp means the previous copy completed.
static int already_installed(const int *src_fd, int payload_fd) {
	char a[128], b[128], path[256];
	snprintf(path, sizeof(path), "%s/%s", SRC, BUILD_ID);
	if (read_small(path, a, sizeof(a)) != 0) { klog("no build-id in package: copying"); return 0; }
	if (read_small(DST "/" BUILD_ID, b, sizeof(b)) != 0) { klog("no build-id on console: copying"); return 0; }
	if (strcmp(a, b) != 0) { klog("build-id differs: copying"); return 0; }
	for (int i = 0; i < n_files; i++) {
		snprintf(path, sizeof(path), DST "/%s", files[i]);
		if (file_size(path) != fd_size(src_fd[i])) { klog("%s missing or wrong size: copying", files[i]); return 0; }
	}
	for (size_t i = 0; i < N_PAYLOAD_DIRS; i++) {
		snprintf(path, sizeof(path), "%s/%s", payload_dirs[i], payload_name);
		if (file_size(path) != fd_size(payload_fd)) { klog("%s missing or wrong size: copying", path); return 0; }
	}
	klog("files already installed (build-id match): skipping copy");
	return 1;
}

// ---------------------------------------------------------------- GoldHEN jailbreak
// syscall 500 cmd 2/3 (GoldHEN SDK). Only used for the reserve: the sandbox refuses the
// free-space syscalls, and reserving blind could fill the PS4 drive.
#define GOLDHEN_SDK_CMD_JAILBREAK 2
#define GOLDHEN_SDK_CMD_UNJAILBREAK 3
struct jailbreak_backup {
	uint32_t cr_uid, cr_ruid, cr_rgid, cr_groups;
	uint64_t cr_paid;
	uint64_t cr_caps[2];
	void *cr_prison, *fd_cdir, *fd_jdir, *fd_rdir;
	uint8_t spare[128];   // headroom in case a GoldHEN version stores more
};
static struct jailbreak_backup jb;
static int jailbroken;

static int goldhen_jailbreak(void) {
	if (jailbroken) return 0;
	memset(&jb, 0, sizeof(jb));
	long r = orbis_syscall3(500, GOLDHEN_SDK_CMD_JAILBREAK, (long)&jb, 0);
	klog("GoldHEN jailbreak: %ld", r);
	if (r != 0) return -1;
	jailbroken = 1;
	return 0;
}

// Back into the sandbox before the payload: booting straight from a jailbroken process
// gave no HDMI on the Slim, the same files booted fine after unjailbreak + sync.
static void goldhen_unjailbreak(void) {
	if (!jailbroken) return;
	long r = orbis_syscall3(500, GOLDHEN_SDK_CMD_UNJAILBREAK, (long)&jb, 0);
	klog("GoldHEN unjailbreak: %ld", r);
	if (r == 0) jailbroken = 0;
	sync();
	sceKernelUsleep(5 * 1000000);
}

// ---------------------------------------------------------------- reserve
// The loader stops OrbisOS abruptly; booting right after a big write crashed Linux until
// the drive was given time. A small file is written + fsync'd every 2 s: when that is
// fast three times in a row (and at least 30 s passed) the drive is idle.
#define PROBE_FILE DST "/.pkg-probe"
#define PROBE_FAST_US 150000ULL
#define PROBE_NEED 3
#define SETTLE_MIN_S 30        // after a reserve (the crash case: big write, then boot)
#define SETTLE_COPY_MIN_S 10   // after copying files only (a big image can be GBs too)
// Set when this launch wrote to the PS4 drive / already waited for it: boot() never hands
// the drive to Linux while OrbisOS may still be writing.
static int wrote_drive, settled;
#define SETTLE_MAX_S 600
#define RESERVE_MBPS 50   // estimate only (stock 5400 RPM drive); the live ETA is measured

static uint64_t probe_us(void) {
	static char block[4096];
	uint64_t t0 = sceKernelGetProcessTime();
	int fd = open(PROBE_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) return ~0ULL;
	block[0]++;
	ssize_t w = write(fd, block, sizeof(block));
	int f = fsync(fd);
	close(fd);
	if (w != (ssize_t)sizeof(block) || f != 0) return ~0ULL;
	return sceKernelGetProcessTime() - t0;
}

static void settle_drive(int min_s) {
	char msg[160];
	sync();
	if (progress_open("Finishing writing to the PS4 drive ...") < 0) { sceKernelUsleep(60 * 1000000); return; }
	uint64_t t0 = sceKernelGetProcessTime();
	int fast = 0, secs = 0;
	for (;;) {
		uint64_t us = probe_us();
		secs = (int)((sceKernelGetProcessTime() - t0) / 1000000);
		klog("settle %3d s: probe fsync %llu ms", secs, (unsigned long long)(us == ~0ULL ? 0 : us / 1000));
		fast = (us != ~0ULL && us < PROBE_FAST_US) ? fast + 1 : 0;
		if (fast >= PROBE_NEED && secs >= min_s) break;
		if (secs >= SETTLE_MAX_S) { klog("settle: still busy after %d s, booting anyway", secs); break; }
		snprintf(msg, sizeof(msg), "Finishing writing to the PS4 drive ... %d s", secs);
		progress_set((uint32_t)(secs * 100 / SETTLE_MAX_S), msg);
		sync();
		sceKernelUsleep(2 * 1000000);
	}
	unlink(PROBE_FILE);
	sync();
	progress_close();
	settled = 1;
	klog("settled after %d s", secs);
}

static const char *errno_text(int e) {
	switch (e) {
	case 28: return "The PS4 drive is full.";
	case 5:  return "The PS4 drive reported a read/write error.";
	case 13: case 1: return "Access to the PS4 drive was refused.";
	default: return "Writing to the PS4 drive failed.";
	}
}

// ---- install choice (same flow as the ChromeOS for PS4 app)
// Saved for the initramfs, which then skips those questions on the TV:
//   /data/linux/boot/install-choice:  target=internal size_gib=<n> | target=external
// The initramfs writes <done_marker> after a successful install: then the app never
// asks or reserves again.
#define CHOICE_FILE DST "/install-choice"
#define DRYRUN_FLAG DST "/pkg-dryrun"

static uint64_t img_bytes(uint64_t gib) {
	return gib * GB + reserve_extra_mb * 1048576ULL;
}

static int write_choice(const char *text) {
	int fd = open(CHOICE_FILE ".part", O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0) { klog("choice: open failed errno %d", errno); return -1; }
	ssize_t n = write(fd, text, strlen(text));
	fsync(fd);
	close(fd);
	if (n != (ssize_t)strlen(text) || rename(CHOICE_FILE ".part", CHOICE_FILE) != 0) {
		klog("choice: write failed errno %d", errno);
		unlink(CHOICE_FILE ".part");
		return -1;
	}
	chmod(CHOICE_FILE, 0666);
	klog("choice saved: %s", text);
	return 0;
}

static uint64_t choice_gib(const char *c) {
	const char *p = strstr(c, "size_gib=");
	return p ? strtoull(p + 9, NULL, 10) : 0;
}

// Asks where to install and how big; returns GiB for the PS4 drive, 0 for USB.
// Bytes already reserved on the PS4 drive (set by the caller): picking USB gives them back,
// so the confirmation says so.
static uint64_t reserved_now;

static uint64_t ask_install(uint64_t avail) {
	int r;
	while (allow_usb) {
		r = dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS, "PS4 drive", "USB drive",
		           "%s - where should it be installed?\n\n"
		           "PS4 drive: games and saves stay.\n"
		           "USB drive: that drive is ERASED. You pick it on the TV after the restart "
		           "(needs a USB keyboard).", APP_TITLE);
		if (r == 0) quit();
		if (r == 1) break;
		char extra[160] = "";
		if (reserved_now)
			snprintf(extra, sizeof(extra), "\n\nThe %llu GB already reserved on the PS4 drive will be deleted.",
			         (unsigned long long)(reserved_now / GB));
		r = dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS, "Use a USB drive", "Back",
		           "Install %s to a USB drive?\n\n"
		           "After the restart you pick the drive on the TV (USB keyboard needed). "
		           "EVERYTHING on the drive you pick is ERASED.%s\n\n"
		           "You can still change this the next time the app starts.", APP_TITLE, extra);
		if (r == 1) return 0;
		if (r == 0) quit();
		// Back: ask again
	}
	uint64_t max_gib = 0;
	while (img_bytes(max_gib + 1) <= avail) max_gib++;
	klog("sizes: avail %llu MB -> max %llu GB", (unsigned long long)(avail >> 20), (unsigned long long)max_gib);
	if (max_gib < min_gb) {
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "Not enough free space on the PS4 drive: %s needs at least %llu GB "
		       "(%llu GB recommended) plus %llu GB that always stay free.\n\n"
		       "Free up space%s.", APP_TITLE, (unsigned long long)min_gb, (unsigned long long)rec_gb,
		       (unsigned long long)keep_free_gb, allow_usb ? ", or install to a USB drive" : "");
		quit();
	}
	// Every configured size is listed, also the ones that do not fit right now (marked, with
	// how much more free space they need), plus "all the free space". Ascending order.
	uint64_t sizes[9];
	int n = 0;
	for (int i = 0; i < n_steps; i++)
		if (size_steps[i] >= min_gb && size_steps[i] != max_gib) sizes[n++] = size_steps[i];
	sizes[n++] = max_gib;
	for (int i = 1; i < n; i++)   // insertion sort, n <= 9
		for (int j = i; j > 0 && sizes[j - 1] > sizes[j]; j--) { uint64_t t = sizes[j]; sizes[j] = sizes[j - 1]; sizes[j - 1] = t; }
	int start = 0;   // recommended if it fits, else the biggest that fits
	for (int i = 0; i < n; i++) if (sizes[i] <= max_gib) start = i;
	for (int i = 0; i < n; i++) if (sizes[i] == rec_gb && rec_gb <= max_gib) start = i;

	for (int i = start;; i = (i + 1) % n) {
		uint64_t g = sizes[i], mins = img_bytes(g) / 1048576 / RESERVE_MBPS / 60 + 1;
		int fits = g <= max_gib;
		char tag[96];
		if (!fits) {
			uint64_t need = img_bytes(g) - avail;
			snprintf(tag, sizeof(tag), "  (does not fit: needs %llu GB more free space)",
			         (unsigned long long)((need + GB - 1) / GB));
		} else snprintf(tag, sizeof(tag), "%s", g == max_gib ? "  (all the free space)" : (g == rec_gb ? "  (recommended)" : ""));
		r = dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS, "Install", n > 1 ? "Next size" : "Cancel",
		           "Storage for %s:\n\n"
		           "        %llu GB%s\n\n"
		           "Reserving it takes up to about %llu min (one time; less on a fast drive). "
		           "%llu GB of the PS4 drive always stay free (free now for %s: %llu GB).%s",
		           APP_TITLE, (unsigned long long)g, tag, (unsigned long long)mins,
		           (unsigned long long)keep_free_gb, APP_TITLE, (unsigned long long)max_gib,
		           n > 1 ? "\n\n\"Next size\" shows the other sizes." : "");
		if (r == 0 || (r == 2 && n == 1)) quit();
		if (r == 2) continue;
		if (fits) return g;
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "%llu GB does not fit on the PS4 drive right now.\n\n"
		       "Free up about %llu GB (delete games or media), then launch the app again - "
		       "or pick a smaller size.", (unsigned long long)g,
		       (unsigned long long)((img_bytes(g) - avail + GB - 1) / GB));
		i = (i + n - 1) % n;   // stay on this size
	}
}

static uint64_t avail_bytes(uint64_t free_b, const char *path, const char *part) {
	uint64_t avail = free_b + file_size(path) + file_size(part), keep = keep_free_gb * GB;
	return avail > keep ? avail - keep : 0;
}

// Dry run (flag file /data/linux/boot/pkg-dryrun, e.g. made over FTP): the real questions
// with the real sizes, then a summary. Writes, reserves and boots nothing; removes the flag.
static void dry_run(void) {
	char path[256], part[256];
	klog("DRY RUN: nothing is written, reserved or booted");
	unlink(DRYRUN_FLAG);
	if (!reserve_name[0]) {
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL, "Dry run: this package has no install menu (no reserve).%s", "");
		quit();
	}
	snprintf(path, sizeof(path), DST "/%s", reserve_name);
	snprintf(part, sizeof(part), "%s.part", path);
	if (goldhen_jailbreak() != 0) fail("Dry run: the GoldHEN jailbreak failed.%s", "");
	uint64_t free_b = free_bytes(DST);
	if (!free_b) fail("Dry run: the free space could not be read.%s", "");
	reserved_now = file_size(path) + file_size(part);
	uint64_t gib = ask_install(avail_bytes(free_b, path, part));
	uint64_t want = img_bytes(gib);
	if (gib == 0) {
		klog("DRY RUN result: USB drive");
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "Dry run - nothing was changed.\n\nChosen: USB drive.\n"
		       "A real run would reserve nothing and ask for the drive on the TV.%s", "");
	} else {
		klog("DRY RUN result: PS4 drive, %llu GB -> reserve %llu bytes, free %llu MB",
		     (unsigned long long)gib, (unsigned long long)want, (unsigned long long)(free_b >> 20));
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "Dry run - nothing was changed.\n\nChosen: PS4 drive, %llu GB storage.\n"
		       "A real run would reserve %llu MB (about %llu min).\nPS4 drive free now: %llu MB.",
		       (unsigned long long)gib, (unsigned long long)(want >> 20),
		       (unsigned long long)(want / 1048576 / RESERVE_MBPS / 60 + 1), (unsigned long long)(free_b >> 20));
	}
	quit();
}

static void ask_slow_install(const char *why) {
	klog("fast reserve not possible: %s", why);
	int r = dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_2BUTTONS, "Continue", "Cancel",
	               "%s\n\n%s can still be installed, but Linux then has to prepare the space "
	               "itself, which takes much longer. The questions are then asked on the TV "
	               "(USB keyboard).\n\nContinue with the slower install?", why, APP_TITLE);
	if (r != 1) quit();
}

static void install_setup(void) {
	static unsigned char zero[4 * 1024 * 1024];   // bss: all zeros
	char path[256], part[256], marker[256], choice[128], msg[200];
	if (!reserve_name[0]) return;
	snprintf(marker, sizeof(marker), DST "/%s", done_marker);
	if (exists(marker)) { klog("%s exists: installed, nothing to ask or reserve", marker); return; }
	snprintf(path, sizeof(path), DST "/%s", reserve_name);
	snprintf(part, sizeof(part), "%s.part", path);

	uint64_t gib = 0;
	if (read_small(CHOICE_FILE, choice, sizeof(choice)) == 0) {
		int ext = strstr(choice, "target=external") != NULL;
		gib = ext ? 0 : choice_gib(choice);
		klog("choice from an earlier launch: %s %llu GB", ext ? "USB" : "PS4 drive", (unsigned long long)gib);
		char what[96];
		if (ext) snprintf(what, sizeof(what), "a USB drive (it is ERASED)");
		else snprintf(what, sizeof(what), "the PS4 drive, %llu GB", (unsigned long long)gib);
		int r = dialog_timed(10, "Continue", "Change",
		                     "%s is set to install to %s.\n\n"
		                     "Continues automatically in 10 seconds. Press \"Change\" to pick again.",
		                     APP_TITLE, what);
		if (r == 0) quit();
		if (r == 2) {
			unlink(CHOICE_FILE);   // the reserve stays until a different choice is confirmed
			klog("choice cleared by the user");
			gib = 0;
		} else {
			if (ext) { klog("choice: USB drive (nothing to reserve)"); return; }
			if (gib && file_size(path) == img_bytes(gib)) return;   // reserved
		}
	}
	if (goldhen_jailbreak() != 0) {
		ask_slow_install("GoldHEN did not give the app access to the PS4 drive's free space "
		                 "(is GoldHEN 2.4 or newer running?).");
		return;
	}
	uint64_t free_b = free_bytes(DST);
	if (!free_b) { goldhen_unjailbreak(); ask_slow_install("The free space of the PS4 drive could not be read."); return; }
	uint64_t avail = avail_bytes(free_b, path, part);

	if (!gib) {
		reserved_now = file_size(path) + file_size(part);
		gib = ask_install(avail);
		if (gib == 0) {
			unlink(path); unlink(part);
			write_choice("target=external\n");
			return;
		}
		snprintf(choice, sizeof(choice), "target=internal\nsize_gib=%llu\n", (unsigned long long)gib);
		if (write_choice(choice) != 0)
			fail("Could not save your choice to /data/linux/boot.%s\n\nCheck that the PS4 drive is not full, then try again.", "");
	}
	uint64_t want = img_bytes(gib);
	if (want > avail) {
		klog("%llu GB no longer fits", (unsigned long long)gib);
		unlink(CHOICE_FILE); unlink(part);
		fail("The size chosen earlier no longer fits on the PS4 drive.%s\n\nLaunch the app again to choose a smaller size.", "");
	}
	if (exists(path) && file_size(path) != want) { unlink(path); klog("old reserve has another size: removed"); }
	if (exists(path)) { settle_drive(SETTLE_MIN_S); return; }

	int out = open(part, O_WRONLY | O_CREAT, 0666);
	if (out < 0) { klog("open %s failed errno %d", part, errno); fail("Could not create the space in /data/linux/boot.%s", ""); }
	uint64_t have = fd_size(out);
	if (have > want) {   // a bigger size was chosen before: start over
		close(out); unlink(part);
		out = open(part, O_WRONLY | O_CREAT, 0666);
		if (out < 0) fail("Could not create the space in /data/linux/boot.%s", "");
		have = 0;
	}
	have -= have % sizeof(zero);   // resume from a whole number of buffers
	if (lseek(out, (off_t)have, SEEK_SET) != (off_t)have) {
		close(out); unlink(part);
		fail("Could not continue the earlier reservation; it was removed.%s\n\nLaunch the app again.", "");
	}
	klog("reserving %llu MB (%llu GB) at %s (resume at %llu MB, free %llu MB)", (unsigned long long)(want >> 20),
	     (unsigned long long)gib, path, (unsigned long long)(have >> 20), (unsigned long long)(free_b >> 20));
	snprintf(msg, sizeof(msg), "Reserving %llu GB on the PS4 drive for %s (one time only) ...", (unsigned long long)gib, APP_TITLE);
	if (progress_open(msg) < 0) { close(out); fail("Could not open the progress dialog.%s", ""); }
	uint64_t t0 = sceKernelGetProcessTime(), done = have, last = 0;
	int werr = 0;
	while (done < want) {
		size_t n = (want - done) < sizeof(zero) ? (size_t)(want - done) : sizeof(zero);
		ssize_t w = write(out, zero, n);
		if (w <= 0) { werr = errno ? errno : 5; klog("reserve write failed errno %d at %llu MB", werr, (unsigned long long)(done >> 20)); break; }
		done += (uint64_t)w;
		uint64_t now = sceKernelGetProcessTime();
		if (now - last >= 2000000ULL) {   // live remaining time from this drive's speed
			last = now;
			uint64_t el = (now - t0) / 1000000, mb = (done - have) >> 20;
			if (el >= 5 && mb) {
				uint64_t left = ((want - done) >> 20) * el / mb;
				snprintf(msg, sizeof(msg), "Reserving %llu GB on the PS4 drive for %s (one time only) ...\n"
				         "%llu MB/s, about %llu min %02llu s left", (unsigned long long)gib, APP_TITLE,
				         (unsigned long long)(mb / el), (unsigned long long)(left / 60), (unsigned long long)(left % 60));
				progress_set((uint32_t)(done * 100 / want), msg);
			} else progress_set((uint32_t)(done * 100 / want), NULL);
		}
	}
	if (fsync(out) != 0) klog("reserve fsync errno %d (ignored)", errno);
	close(out);
	progress_close();
	uint64_t secs = (sceKernelGetProcessTime() - t0) / 1000000;
	klog("reserved %llu MB in %llu s (%llu MB/s)", (unsigned long long)((done - have) >> 20), (unsigned long long)secs,
	     (unsigned long long)(secs ? ((done - have) >> 20) / secs : 0));
	if (werr) {
		if (werr == 28) {   // full: give the space back, a smaller size is needed
			unlink(part); unlink(CHOICE_FILE);
			fail("%s\n\nThe reservation was removed. Free up space or launch the app again and choose a smaller size.", errno_text(werr));
		}
		fail("%s\n\nWhat was reserved so far is kept: launch the app again to continue.", errno_text(werr));
	}
	if (file_size(part) != want || rename(part, path) != 0) {
		klog("reserve finish failed (size %llu, errno %d)", (unsigned long long)file_size(part), errno);
		fail("The space was written but could not be finished.%s\n\nLaunch the app again.", "");
	}
	chmod(path, 0666);
	klog("reserve of %llu GB complete", (unsigned long long)gib);
	wrote_drive = 1;
	settle_drive(SETTLE_MIN_S);
}

// ---------------------------------------------------------------- payload auto-boot
// Hands the loader to GoldHEN's BinLoader (TCP 9090 on the console itself).
static int send_payload(int fd) {
	static unsigned char buf[512 * 1024];
	uint64_t size = fd_size(fd);
	if (size == 0 || size > sizeof(buf) || lseek(fd, 0, SEEK_SET) != 0) return -1;
	ssize_t n = read(fd, buf, (size_t)size);
	if (n != (ssize_t)size) { klog("payload read %ld of %llu", (long)n, (unsigned long long)size); return -1; }
	sceNetInit();
	int s = sceNetSocket("linux-loader", ORBIS_NET_AF_INET, ORBIS_NET_SOCK_STREAM, 0);
	if (s < 0) { klog("sceNetSocket failed 0x%x", s); return -1; }
	struct { uint8_t len; uint8_t family; uint16_t port; uint32_t addr; uint8_t zero[8]; } sa;
	memset(&sa, 0, sizeof(sa));
	sa.len = sizeof(sa);
	sa.family = ORBIS_NET_AF_INET;
	sa.port = sceNetHtons(9090);
	sa.addr = 0x0100007F;  // 127.0.0.1 in network byte order
	int r = sceNetConnect(s, (const OrbisNetSockaddr *)&sa, sizeof(sa));
	if (r < 0) { klog("connect 127.0.0.1:9090 failed 0x%x (GoldHEN BinLoader off?)", r); sceNetSocketClose(s); return -1; }
	size_t off = 0;
	while (off < (size_t)n) {
		int w = sceNetSend(s, buf + off, (size_t)n - off, 0);
		if (w <= 0) { klog("send failed 0x%x at %zu", w, off); sceNetSocketClose(s); return -1; }
		off += (size_t)w;
	}
	sceNetSocketClose(s);
	klog("payload sent (%zu bytes)", off);
	return 0;
}

static void boot(int payload_fd, const char *what) {
	goldhen_unjailbreak();
	sync();   // always, jailbroken or not
	if (wrote_drive && !settled) settle_drive(SETTLE_COPY_MIN_S);
#ifdef TEMPLATE_DUMMY
	klog("template build: not sending the dummy payload");
	dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
	       "Template build: %s in " DST ".\n\nA real build would now send %s to GoldHEN and boot Linux. "
	       "This one stops here (dummy files).", what, payload_name);
	quit();
#endif
	if (send_payload(payload_fd) != 0)
		dialog(ORBIS_MSG_DIALOG_BUTTON_TYPE_OK, NULL, NULL,
		       "%s, but the payload could not be started.\n\nStart it yourself: GoldHEN > Payloads > %s",
		       what, payload_name);
	else
		for (;;) sceKernelUsleep(1000000);   // the loader takes over the console
	quit();
}

// ---------------------------------------------------------------- main
int main(void) {
	char path[256];

	sceSysmoduleLoadModule(ORBIS_SYSMODULE_MESSAGE_DIALOG);
	sceCommonDialogInitialize();
	klog("start: %s (build " __DATE__ " " __TIME__ ")", APP_TITLE);

	SRC[0] = 0;
	for (size_t i = 0; i < sizeof(src_candidates) / sizeof(src_candidates[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", src_candidates[i], MANIFEST);
		if (exists(path)) { snprintf(SRC, sizeof(SRC), "%s", src_candidates[i]); break; }
	}
	if (!SRC[0]) fail("Cannot find the installer's files.%s\n\nReinstall the package and try again.", "");
	klog("package files at %s", SRC);
	load_manifest();

	// Opened once: after the jailbreak /app0 is no longer the package root.
	int src_fd[MAX_FILES], payload_fd;
	uint64_t total = 0;
	for (int i = 0; i < n_files; i++) {
		snprintf(path, sizeof(path), "%s/%s", SRC, files[i]);
		src_fd[i] = open(path, O_RDONLY);
		if (src_fd[i] < 0) fail("The installer package is incomplete: %s is missing.", files[i]);
		total += fd_size(src_fd[i]);
	}
	snprintf(path, sizeof(path), "%s/%s", SRC, payload_name);
	payload_fd = open(path, O_RDONLY);
	if (payload_fd < 0) fail("The installer package is incomplete: %s is missing.", payload_name);
	total += N_PAYLOAD_DIRS * fd_size(payload_fd);
	klog("files total %llu bytes", (unsigned long long)total);
	klog("free space on the PS4 drive: %llu MB (0 = unknown)", (unsigned long long)(free_bytes(DST) >> 20));
	if (exists(DRYRUN_FLAG)) dry_run();   // never returns

	if (already_installed(src_fd, payload_fd)) {
		install_setup();   // no-op once installed
		boot(payload_fd, "The files are installed");
	}
	unlink(DST "/" BUILD_ID);   // invalidate the stamp until this copy completes

	uint64_t free_user = free_bytes("/data");   // /user is not visible in the app sandbox (0 = unknown)
	if (free_user && free_user < total + GB)
		fail("Not enough free space on the PS4 drive for the " APP_TITLE " files.%s", "");

	mkdirs(DST);
	for (size_t i = 0; i < N_PAYLOAD_DIRS; i++) mkdirs(payload_dirs[i]);
	if (progress_open("Copying " APP_TITLE " ...") < 0) fail("Could not open the progress dialog.%s", "");
	uint64_t done = 0;
	for (int i = 0; i < n_files; i++) {
		snprintf(path, sizeof(path), DST "/%s", files[i]);
		if (copy_file(src_fd[i], path, &done, total, files[i]) != 0) {
			progress_close();
			fail("Copying %s failed (details in the GoldHEN log, port 3232).\n\nIs the PS4 drive full?", files[i]);
		}
	}
	for (size_t i = 0; i < N_PAYLOAD_DIRS; i++) {
		snprintf(path, sizeof(path), "%s/%s", payload_dirs[i], payload_name);
		if (copy_file(payload_fd, path, &done, total, "loader payload") != 0) {
			progress_close();
			fail("Copying the loader payload to %s failed.", payload_dirs[i]);
		}
	}
	snprintf(path, sizeof(path), "%s/%s", SRC, BUILD_ID);
	int id_fd = open(path, O_RDONLY);
	if (id_fd >= 0) {
		uint64_t d0 = 0;
		if (copy_file(id_fd, DST "/" BUILD_ID, &d0, 0, "build stamp") != 0) klog("build-id copy failed (next launch copies again)");
		close(id_fd);
	}
	progress_set(100, "Starting ...");
	progress_close();
	wrote_drive = 1;   // files were copied this launch: wait for the drive before booting

	install_setup();
	boot(payload_fd, "The files are in place");
	return 0;
}
