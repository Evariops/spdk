/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (c) 2026 Evariops.
 *
 * spdk-takeover-proof: proves, on a node, that one image's spdk_tgt takes over the ublk disks
 * another image's spdk_tgt serves, and gives them back. Linked statically: it runs in the
 * container of an image that does not carry it, after "install" copied it there.
 *
 *   spdk-takeover-proof install <dir>
 *   spdk-takeover-proof <clean|start|create|takeover|return> [--tgt PATH] [--dir DIR] [--core N]
 *       [--ids A,B] [--identities I,J] [--zero-copy] [--report PATH]
 *
 *   clean     a device left under the proof's ids is recovered and stopped
 *   start     spdk_tgt initialises and stops cleanly
 *   create    disk A on an lvol of a file, pattern 1 written, released to the kernel (quiesce)
 *   takeover  A recovered under its identity, pattern 1 checked, pattern 2 written; disk B
 *             created (with zero copy when asked), pattern 3 written; both released
 *   return    A and B recovered, patterns 2 and 3 checked, both stopped
 *
 * A phase reports one JSON object in --report (a container's termination message) and exits 0
 * when it passed or does not apply, 1 when it failed. A create that does not apply (the image
 * serves no ublk) makes the takeover prove the image against itself, and the return not apply.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PROOF_VERSION 1
#define PATTERN_BYTES (8u << 20)
#define CHUNK (1u << 20)
#define LVOL_MIB 16
#define FILE_BYTES (64ull << 20)
/* The pools of a proof: a disk's poll group keeps one 1 MiB request's buffers in reserve (fork
 * 0058), and two disks are served at once. The process runs without hugepages, on 1 GiB of
 * ordinary memory it touches as it uses it. */
#define IOBUF_POOLS "{\"small_pool_count\":2048,\"large_pool_count\":256}"

static const char *g_phase = "?";
static const char *g_tgt = "/usr/local/bin/spdk_tgt";
static const char *g_dir = "/proof";
static const char *g_report = "/dev/termination-log";
static int g_core;
static unsigned g_id_a = 1048064, g_id_b = 1048065;
static unsigned long long g_identity_a = 0x5052304641ull, g_identity_b = 0x5052304642ull;
static int g_zero_copy;

static char g_detail[2048];
static long long g_started_ms;
static pid_t g_tgt_pid = -1;
static int g_sock = -1;
static unsigned g_rpc_id;

static long long
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void
note(const char *fmt, ...)
{
	size_t len = strlen(g_detail);
	va_list ap;

	if (len > 0 && len < sizeof(g_detail) - 2) {
		g_detail[len++] = ';';
		g_detail[len++] = ' ';
		g_detail[len] = '\0';
	}
	va_start(ap, fmt);
	vsnprintf(g_detail + len, sizeof(g_detail) - len, fmt, ap);
	va_end(ap);
}

static void
json_string(FILE *out, const char *s)
{
	fputc('"', out);
	for (; *s; s++) {
		if (*s == '"' || *s == '\\') {
			fputc('\\', out);
			fputc(*s, out);
		} else if ((unsigned char)*s < 0x20) {
			fprintf(out, "\\u%04x", (unsigned char)*s);
		} else {
			fputc(*s, out);
		}
	}
	fputc('"', out);
}

/* The phase's report, and its exit status. */
static int
report(const char *result)
{
	FILE *out = fopen(g_report, "w");

	if (out == NULL) {
		out = stdout;
	}
	fprintf(out, "{\"proof\":%d,\"phase\":", PROOF_VERSION);
	json_string(out, g_phase);
	fprintf(out, ",\"result\":");
	json_string(out, result);
	fprintf(out, ",\"ms\":%lld,\"detail\":", now_ms() - g_started_ms);
	json_string(out, g_detail);
	fprintf(out, "}\n");
	if (out != stdout) {
		fclose(out);
	}
	fprintf(stderr, "%s: %s: %s\n", g_phase, result, g_detail);
	return strcmp(result, "failed") == 0 ? 1 : 0;
}

static void tgt_kill(void);

static void __attribute__((noreturn, format(printf, 1, 2)))
fail(const char *fmt, ...)
{
	char why[512];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(why, sizeof(why), fmt, ap);
	va_end(ap);
	note("%s", why);
	tgt_kill();
	exit(report("failed"));
}

static void __attribute__((noreturn))
not_applicable(const char *why)
{
	note("%s", why);
	tgt_kill();
	exit(report("not-applicable"));
}

/* ── markers between phases, in the shared directory ─────────────────────────────────── */

static void
mark(const char *name)
{
	char path[512];
	int fd;

	snprintf(path, sizeof(path), "%s/%s", g_dir, name);
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		close(fd);
	}
}

static int
marked(const char *name)
{
	char path[512];

	snprintf(path, sizeof(path), "%s/%s", g_dir, name);
	return access(path, F_OK) == 0;
}

/* ── spdk_tgt ─────────────────────────────────────────────────────────────────────────── */

static void
sock_path(char *buf, size_t len)
{
	snprintf(buf, len, "%s/spdk.sock", g_dir);
}

static int
tgt_alive(void)
{
	int status;

	return g_tgt_pid > 0 && waitpid(g_tgt_pid, &status, WNOHANG) == 0;
}

static void
tgt_start(void)
{
	char sock[256], log[512], mask[32];
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	long long deadline;
	int fd;

	sock_path(sock, sizeof(sock));
	unlink(sock);
	snprintf(log, sizeof(log), "%s/%s-spdk.log", g_dir, g_phase);
	snprintf(mask, sizeof(mask), "[%d]", g_core);

	g_tgt_pid = fork();
	if (g_tgt_pid < 0) {
		fail("fork: %s", strerror(errno));
	}
	if (g_tgt_pid == 0) {
		fd = open(log, O_WRONLY | O_CREAT | O_APPEND, 0644);
		if (fd >= 0) {
			dup2(fd, 1);
			dup2(fd, 2);
		}
		setsid();
		execl(g_tgt, g_tgt, "--no-huge", "-s", "1024", "-m", mask, "-r", sock, "--wait-for-rpc",
		      "--num-trace-entries", "0", (char *)NULL);
		_exit(127);
	}

	if (strlen(sock) >= sizeof(addr.sun_path)) {
		fail("the socket path %s is longer than a Unix socket's", sock);
	}
	memcpy(addr.sun_path, sock, strlen(sock) + 1);
	deadline = now_ms() + 30000;
	while (now_ms() < deadline) {
		if (!tgt_alive()) {
			g_tgt_pid = -1;
			fail("%s exited before it answered (see %s)", g_tgt, log);
		}
		fd = socket(AF_UNIX, SOCK_STREAM, 0);
		if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
			g_sock = fd;
			return;
		}
		if (fd >= 0) {
			close(fd);
		}
		usleep(100000);
	}
	fail("%s did not answer on %s within 30 s", g_tgt, sock);
}

/* SIGTERM, then SIGKILL past 10 s. Returns the exit status, or -1 when it had to be killed. */
static int
tgt_stop(void)
{
	long long deadline = now_ms() + 10000;
	int status = 0;

	if (g_sock >= 0) {
		close(g_sock);
		g_sock = -1;
	}
	if (g_tgt_pid <= 0) {
		return 0;
	}
	kill(g_tgt_pid, SIGTERM);
	while (now_ms() < deadline) {
		if (waitpid(g_tgt_pid, &status, WNOHANG) == g_tgt_pid) {
			g_tgt_pid = -1;
			return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
		}
		usleep(100000);
	}
	kill(g_tgt_pid, SIGKILL);
	waitpid(g_tgt_pid, &status, 0);
	g_tgt_pid = -1;
	return -1;
}

static void
tgt_kill(void)
{
	int status;

	if (g_sock >= 0) {
		close(g_sock);
		g_sock = -1;
	}
	if (g_tgt_pid > 0) {
		kill(g_tgt_pid, SIGKILL);
		waitpid(g_tgt_pid, &status, 0);
		g_tgt_pid = -1;
	}
}

/* ── JSON-RPC ─────────────────────────────────────────────────────────────────────────── */

static char g_resp[1 << 20];

/* The value of a top-level key of the response, or NULL. */
static const char *
top_level(const char *resp, const char *key)
{
	size_t klen = strlen(key);
	int depth = 0, in_string = 0;
	const char *p, *start = NULL;

	for (p = resp; *p; p++) {
		if (in_string) {
			if (*p == '\\' && p[1]) {
				p++;
			} else if (*p == '"') {
				in_string = 0;
				if (depth == 1 && start != NULL && (size_t)(p - start) == klen &&
				    strncmp(start, key, klen) == 0) {
					const char *q = p + 1;

					while (*q == ' ' || *q == '\n' || *q == '\t' || *q == '\r') {
						q++;
					}
					if (*q == ':') {
						return q + 1;
					}
				}
			}
		} else if (*p == '"') {
			in_string = 1;
			start = p + 1;
		} else if (*p == '{' || *p == '[') {
			depth++;
		} else if (*p == '}' || *p == ']') {
			depth--;
		}
	}
	return NULL;
}

/* Sends a request and reads its answer; returns 0, or the error's code. */
static int
rpc(const char *method, const char *params)
{
	char req[4096];
	size_t len = 0;
	int depth = 0, in_string = 0, started = 0, n;
	const char *error;

	if (params != NULL) {
		n = snprintf(req, sizeof(req), "{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"%s\",\"params\":%s}",
			     ++g_rpc_id, method, params);
	} else {
		n = snprintf(req, sizeof(req), "{\"jsonrpc\":\"2.0\",\"id\":%u,\"method\":\"%s\"}", ++g_rpc_id, method);
	}
	if (write(g_sock, req, n) != n) {
		fail("%s: cannot send: %s", method, strerror(errno));
	}

	for (;;) {
		ssize_t got = read(g_sock, g_resp + len, sizeof(g_resp) - 1 - len);
		ssize_t i;

		if (got <= 0) {
			fail("%s: the connection closed without an answer", method);
		}
		for (i = 0; i < got; i++) {
			char c = g_resp[len + i];

			if (in_string) {
				if (c == '\\') {
					i++;
				} else if (c == '"') {
					in_string = 0;
				}
			} else if (c == '"') {
				in_string = 1;
			} else if (c == '{') {
				depth++;
				started = 1;
			} else if (c == '}') {
				depth--;
			}
		}
		len += got;
		g_resp[len] = '\0';
		if (started && depth == 0) {
			break;
		}
		if (len >= sizeof(g_resp) - 1) {
			fail("%s: the answer is larger than %zu bytes", method, sizeof(g_resp));
		}
	}

	error = top_level(g_resp, "error");
	if (error == NULL) {
		return 0;
	}
	error = strstr(error, "\"code\"");
	return error != NULL ? atoi(strchr(error, ':') + 1) : -1;
}

/* The answer's error message, for a report. */
static const char *
rpc_message(void)
{
	static char message[256];
	const char *m = strstr(g_resp, "\"message\"");
	size_t i = 0;

	message[0] = '\0';
	if (m != NULL && (m = strchr(m + 9, '"')) != NULL) {
		for (m++; *m && *m != '"' && i < sizeof(message) - 1; m++) {
			message[i++] = *m;
		}
		message[i] = '\0';
	}
	return message;
}

static void
rpc_must(const char *method, const char *params)
{
	int rc = rpc(method, params);

	if (rc != 0) {
		fail("%s: %d %s", method, rc, rpc_message());
	}
}

#define METHOD_NOT_FOUND (-32601)
#define INVALID_PARAMS (-32602)

/* ── the disk's bytes ─────────────────────────────────────────────────────────────────── */

static void
fill(uint8_t *buf, size_t len, uint64_t seed, uint64_t offset)
{
	uint64_t x = seed ^ (offset * 0x9e3779b97f4a7c15ull);
	size_t i;

	for (i = 0; i + 8 <= len; i += 8) {
		x ^= x >> 12;
		x ^= x << 25;
		x ^= x >> 27;
		uint64_t v = x * 0x2545f4914f6cdd1dull;
		memcpy(buf + i, &v, 8);
	}
}

static void
device(char *path, size_t len, unsigned id)
{
	snprintf(path, len, "/dev/ublkb%u", id);
}

static int
device_held(unsigned id)
{
	char path[64];

	snprintf(path, sizeof(path), "/sys/class/ublk-char/ublkc%u", id);
	return access(path, F_OK) == 0;
}

static void
wait_device(unsigned id)
{
	char path[64];
	struct stat st;
	long long deadline = now_ms() + 10000;

	device(path, sizeof(path), id);
	while (now_ms() < deadline) {
		if (stat(path, &st) == 0 && S_ISBLK(st.st_mode)) {
			return;
		}
		usleep(50000);
	}
	fail("%s did not appear", path);
}

static void
wait_gone(unsigned id)
{
	long long deadline = now_ms() + 10000;

	while (now_ms() < deadline) {
		if (!device_held(id)) {
			return;
		}
		usleep(50000);
	}
	fail("ublk %u is still held by the kernel 10 s after its stop", id);
}

/* Writes or checks PATTERN_BYTES of seed's bytes, in 1 MiB requests, with O_DIRECT. */
static void
pattern(unsigned id, uint64_t seed, int write)
{
	char path[64];
	uint8_t *buf, *want;
	uint64_t off;
	int fd;

	device(path, sizeof(path), id);
	if (posix_memalign((void **)&buf, 4096, CHUNK) != 0 || posix_memalign((void **)&want, 4096, CHUNK) != 0) {
		fail("no memory for the pattern");
	}
	fd = open(path, (write ? O_WRONLY : O_RDONLY) | O_DIRECT);
	if (fd < 0) {
		fail("open %s: %s", path, strerror(errno));
	}
	for (off = 0; off < PATTERN_BYTES; off += CHUNK) {
		fill(want, CHUNK, seed, off);
		if (write) {
			if (pwrite(fd, want, CHUNK, (off_t)off) != CHUNK) {
				fail("write of %s at %llu: %s", path, (unsigned long long)off, strerror(errno));
			}
		} else {
			if (pread(fd, buf, CHUNK, (off_t)off) != CHUNK) {
				fail("read of %s at %llu: %s", path, (unsigned long long)off, strerror(errno));
			}
			if (memcmp(buf, want, CHUNK) != 0) {
				fail("%s at %llu does not hold what was written there", path, (unsigned long long)off);
			}
		}
	}
	if (write && fdatasync(fd) != 0) {
		fail("flush of %s: %s", path, strerror(errno));
	}
	close(fd);
	free(buf);
	free(want);
}

/* ── the steps every phase shares ─────────────────────────────────────────────────────── */

static int
kernel_has_ublk(void)
{
	return access("/dev/ublk-control", F_OK) == 0;
}

/* The pools sized for a proof, then the subsystems initialised. */
static void
initialise(void)
{
	rpc_must("iobuf_set_options", IOBUF_POOLS);
	rpc_must("framework_start_init", NULL);
}

/* The ublk target created; returns 0, or the target's error code. */
static int
serve(int zero_copy)
{
	char params[128];
	int rc;

	if (zero_copy) {
		snprintf(params, sizeof(params), "{\"cpumask\":\"[%d]\",\"zero_copy\":true}", g_core);
		rc = rpc("ublk_create_target", params);
		if (rc != INVALID_PARAMS) {
			return rc;
		}
		note("zero copy is unknown to this image");
	}
	snprintf(params, sizeof(params), "{\"cpumask\":\"[%d]\"}", g_core);
	return rpc("ublk_create_target", params);
}

/* The lvstore's bdev over the file; the store and its lvols load from it. */
static void
open_store(void)
{
	char params[512];
	int rc;

	snprintf(params, sizeof(params), "{\"filename\":\"%s/proof.img\",\"name\":\"proof0\",\"block_size\":4096}", g_dir);
	rc = rpc("bdev_uring_create", params);
	if (rc == METHOD_NOT_FOUND) {
		rpc_must("bdev_aio_create", params);
	} else if (rc != 0) {
		fail("bdev_uring_create: %d %s", rc, rpc_message());
	}
	rpc_must("bdev_wait_for_examine", NULL);
}

static void
lvol(const char *name)
{
	char params[256];

	snprintf(params, sizeof(params), "{\"name\":\"proof/%s\"}", name);
	if (rpc("bdev_get_bdevs", params) == 0) {
		return;
	}
	snprintf(params, sizeof(params), "{\"lvol_name\":\"%s\",\"size_in_mib\":%d,\"lvs_name\":\"proof\",\"thin_provision\":true}",
		 name, LVOL_MIB);
	rpc_must("bdev_lvol_create", params);
}

/* ublk_start_disk or ublk_recover_disk under an identity; an image before identities is asked
 * without one. */
static void
disk(const char *method, const char *name, unsigned id, unsigned long long identity)
{
	char params[256];
	int rc;

	snprintf(params, sizeof(params), "{\"bdev_name\":\"proof/%s\",\"ublk_id\":%u,\"identity\":%llu}", name, id, identity);
	rc = rpc(method, params);
	if (rc == INVALID_PARAMS) {
		note("this image carries no identity");
		snprintf(params, sizeof(params), "{\"bdev_name\":\"proof/%s\",\"ublk_id\":%u}", name, id);
		rc = rpc(method, params);
	}
	if (rc != 0) {
		fail("%s %s as ublk %u: %d %s", method, name, id, rc, rpc_message());
	}
	wait_device(id);
}

/* Leaves the disk to the kernel, its requests held: quiesce where the image has it. Returns 0
 * when the image cannot: the caller then kills the process, as a crash would leave the disk; a
 * SIGTERM to an image before fork 0069 would delete it. */
static int
release(unsigned id)
{
	char params[128];
	int rc;

	snprintf(params, sizeof(params), "{\"ublk_id\":%u,\"timeout_ms\":5000}", id);
	rc = rpc("ublk_quiesce_disk", params);
	if (rc == METHOD_NOT_FOUND) {
		note("this image cannot quiesce: ublk %u released by the process's death", id);
		return 0;
	}
	if (rc != 0) {
		fail("ublk_quiesce_disk %u: %d %s", id, rc, rpc_message());
	}
	return 1;
}

/* Ends the process once its disks are released: stopped when every release quiesced, killed
 * otherwise. */
static void
leave(int quiesced)
{
	if (quiesced) {
		tgt_stop();
	} else {
		tgt_kill();
	}
}

static void
stop_disk(unsigned id)
{
	char params[64];

	snprintf(params, sizeof(params), "{\"ublk_id\":%u}", id);
	rpc_must("ublk_stop_disk", params);
	wait_gone(id);
}

/* ── the phases ───────────────────────────────────────────────────────────────────────── */

static int
phase_clean(void)
{
	unsigned ids[2] = { g_id_a, g_id_b };
	char params[128];
	int i, held = 0;

	for (i = 0; i < 2; i++) {
		held += device_held(ids[i]);
	}
	if (!held) {
		note("nothing under ublk %u and %u", g_id_a, g_id_b);
		return report("ok");
	}
	tgt_start();
	initialise();
	if (serve(0) != 0) {
		fail("ublk_create_target: %s", rpc_message());
	}
	for (i = 0; i < 2; i++) {
		if (!device_held(ids[i])) {
			continue;
		}
		/* Any bdev serves a recovery that is only there to stop the device: no volume's. */
		snprintf(params, sizeof(params), "{\"name\":\"clean%d\",\"num_blocks\":16384,\"block_size\":4096}", i);
		rpc_must("bdev_null_create", params);
		snprintf(params, sizeof(params), "{\"bdev_name\":\"clean%d\",\"ublk_id\":%u}", i, ids[i]);
		if (rpc("ublk_recover_disk", params) != 0) {
			fail("ublk %u is held and cannot be recovered (%s): another process serves it", ids[i], rpc_message());
		}
		stop_disk(ids[i]);
		note("ublk %u was left from an earlier proof: stopped", ids[i]);
	}
	tgt_stop();
	return report("ok");
}

static int
phase_start(void)
{
	int status;

	tgt_start();
	initialise();
	rpc_must("spdk_get_version", NULL);
	note("started");
	if (kernel_has_ublk()) {
		char params[64];

		snprintf(params, sizeof(params), "{\"cpumask\":\"[%d]\"}", g_core);
		if (rpc("ublk_create_target", params) == 0) {
			note("ublk target created");
			if (rpc("ublk_get_target", NULL) == 0) {
				note("ublk_get_target answers");
			}
			rpc_must("ublk_destroy_target", NULL);
		} else {
			note("no ublk target (%s)", rpc_message());
		}
	}
	status = tgt_stop();
	if (status != 0) {
		fail("spdk_tgt did not stop cleanly (status %d)", status);
	}
	note("stopped cleanly");
	return report("ok");
}

/* Disk A, pattern 1, released. */
static void
create_a(void)
{
	int fd, rc;
	char path[512];

	snprintf(path, sizeof(path), "%s/proof.img", g_dir);
	fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || ftruncate(fd, (off_t)FILE_BYTES) != 0) {
		fail("the store's file %s: %s", path, strerror(errno));
	}
	close(fd);

	tgt_start();
	initialise();
	rc = serve(0);
	if (rc == METHOD_NOT_FOUND) {
		not_applicable("this image serves no ublk");
	} else if (rc != 0) {
		fail("ublk_create_target: %d %s", rc, rpc_message());
	}
	open_store();
	rpc_must("bdev_lvol_create_lvstore", "{\"bdev_name\":\"proof0\",\"lvs_name\":\"proof\",\"cluster_sz\":1048576}");
	lvol("a");
	disk("ublk_start_disk", "a", g_id_a, g_identity_a);
	pattern(g_id_a, g_identity_a, 1);
	note("ublk %u written", g_id_a);
	leave(release(g_id_a));
	note("released to the kernel");
}

static int
phase_create(void)
{
	if (!kernel_has_ublk()) {
		mark("create.na");
		not_applicable("the node's kernel has no ublk driver");
	}
	create_a();
	mark("create.ok");
	return report("ok");
}

static int
phase_takeover(void)
{
	int rc, quiesced;

	if (!kernel_has_ublk()) {
		not_applicable("the node's kernel has no ublk driver");
	}
	if (marked("create.na") || !marked("create.ok")) {
		/* Nothing of the current image's to take over: the image proves itself. */
		create_a();
		note("the image took over its own disk");
	}

	/* The order of a process that restarts: the store and its lvols load, then the target. B's
	 * lvol is there before the target too: a disk with zero copy over a bdev registered first is
	 * the case that failed every large request before fork 0067's fix. */
	tgt_start();
	initialise();
	open_store();
	lvol("b");
	rc = serve(g_zero_copy);
	if (rc == METHOD_NOT_FOUND) {
		fail("this image serves no ublk: it cannot take over ublk %u", g_id_a);
	} else if (rc != 0) {
		fail("ublk_create_target: %d %s", rc, rpc_message());
	}
	disk("ublk_recover_disk", "a", g_id_a, g_identity_a);
	pattern(g_id_a, g_identity_a, 0);
	note("ublk %u recovered, its bytes intact", g_id_a);
	pattern(g_id_a, g_identity_a + 1, 1);

	disk("ublk_start_disk", "b", g_id_b, g_identity_b);
	pattern(g_id_b, g_identity_b, 1);
	if (rpc("ublk_get_disks", NULL) == 0) {
		note("ublk %u created%s", g_id_b,
		     strstr(g_resp, "\"enabled\": true") != NULL || strstr(g_resp, "\"enabled\":true") != NULL ? ", zero copy enabled" : "");
	}
	quiesced = release(g_id_a);
	quiesced &= release(g_id_b);
	leave(quiesced);
	mark("takeover.ok");
	note("both released to the kernel");
	return report("ok");
}

static int
phase_return(void)
{
	int rc;

	if (!kernel_has_ublk()) {
		not_applicable("the node's kernel has no ublk driver");
	}
	if (marked("create.na") || !marked("create.ok")) {
		not_applicable("the current image serves no ublk: a return to it is not proven");
	}
	if (!marked("takeover.ok")) {
		fail("no takeover to return from");
	}
	tgt_start();
	initialise();
	open_store();
	rc = serve(0);
	if (rc != 0) {
		fail("ublk_create_target: %d %s", rc, rpc_message());
	}
	disk("ublk_recover_disk", "a", g_id_a, g_identity_a);
	pattern(g_id_a, g_identity_a + 1, 0);
	disk("ublk_recover_disk", "b", g_id_b, g_identity_b);
	pattern(g_id_b, g_identity_b, 0);
	note("ublk %u and %u recovered, their bytes intact", g_id_a, g_id_b);
	stop_disk(g_id_a);
	stop_disk(g_id_b);
	tgt_stop();
	note("both stopped");
	return report("ok");
}

static int
install(const char *to)
{
	char self[512], dest[512], buf[65536];
	ssize_t len, got;
	int in, out;

	len = readlink("/proc/self/exe", self, sizeof(self) - 1);
	if (len < 0) {
		fail("readlink /proc/self/exe: %s", strerror(errno));
	}
	self[len] = '\0';
	snprintf(dest, sizeof(dest), "%s/spdk-takeover-proof", to);
	in = open(self, O_RDONLY);
	out = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0755);
	if (in < 0 || out < 0) {
		fail("copy to %s: %s", dest, strerror(errno));
	}
	while ((got = read(in, buf, sizeof(buf))) > 0) {
		if (write(out, buf, got) != got) {
			fail("copy to %s: %s", dest, strerror(errno));
		}
	}
	close(in);
	close(out);
	note("installed in %s", to);
	return report("ok");
}

static void
usage(void)
{
	fprintf(stderr, "usage: spdk-takeover-proof install <dir>\n"
		"       spdk-takeover-proof <clean|start|create|takeover|return> [--tgt PATH] [--dir DIR] [--core N]\n"
		"           [--ids A,B] [--identities I,J] [--zero-copy] [--report PATH]\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	int i;

	g_started_ms = now_ms();
	signal(SIGPIPE, SIG_IGN);
	if (argc < 2) {
		usage();
	}
	g_phase = argv[1];
	if (strcmp(g_phase, "install") == 0) {
		if (argc < 3) {
			usage();
		}
		for (i = 3; i + 1 < argc; i += 2) {
			if (strcmp(argv[i], "--report") == 0) {
				g_report = argv[i + 1];
			}
		}
		return install(argv[2]);
	}
	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--zero-copy") == 0) {
			g_zero_copy = 1;
		} else if (i + 1 < argc && strcmp(argv[i], "--tgt") == 0) {
			g_tgt = argv[++i];
		} else if (i + 1 < argc && strcmp(argv[i], "--dir") == 0) {
			g_dir = argv[++i];
		} else if (i + 1 < argc && strcmp(argv[i], "--core") == 0) {
			g_core = atoi(argv[++i]);
		} else if (i + 1 < argc && strcmp(argv[i], "--report") == 0) {
			g_report = argv[++i];
		} else if (i + 1 < argc && strcmp(argv[i], "--ids") == 0) {
			if (sscanf(argv[++i], "%u,%u", &g_id_a, &g_id_b) != 2) {
				usage();
			}
		} else if (i + 1 < argc && strcmp(argv[i], "--identities") == 0) {
			if (sscanf(argv[++i], "%llu,%llu", &g_identity_a, &g_identity_b) != 2) {
				usage();
			}
		} else {
			usage();
		}
	}

	if (strcmp(g_phase, "clean") == 0) {
		return phase_clean();
	} else if (strcmp(g_phase, "start") == 0) {
		return phase_start();
	} else if (strcmp(g_phase, "create") == 0) {
		return phase_create();
	} else if (strcmp(g_phase, "takeover") == 0) {
		return phase_takeover();
	} else if (strcmp(g_phase, "return") == 0) {
		return phase_return();
	}
	usage();
	return 2;
}
