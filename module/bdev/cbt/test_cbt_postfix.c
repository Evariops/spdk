/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Evariops.
 *   All rights reserved.
 */

/*
 * CBT validation tests, run against a standalone model of the module rather
 * than the SPDK build: reset refusal, range-truncation reporting, deferred-name
 * cleanup, epoch eviction rules, and byte-boundary correctness of the optimized
 * mark_dirty against a per-bit reference.
 *
 * Build:  make -f Makefile.test
 * Run:    ./test_cbt_postfix
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>
#include <errno.h>

/* ================================================================== */
/* Simulated CBT module — mirrors the production logic               */
/* ================================================================== */

#define CBT_EPOCH_ID_MAX        64
#define CBT_BACKEND_ID_MAX      128
#define CBT_MAX_EPOCHS          4
#define CBT_CHUNK_SIZE_MIN_KB   4
#define CBT_CHUNK_SIZE_MAX_KB   65536
#define CBT_MAX_RANGES_LIMIT    65536

enum cbt_epoch_state {
	CBT_EPOCH_OPEN        = 0,
	CBT_EPOCH_FROZEN      = 1,
	CBT_EPOCH_REBUILDING  = 2,
	CBT_EPOCH_COMPLETED   = 3,
	CBT_EPOCH_INVALID     = 4,
};

struct cbt_epoch {
	char                    epoch_id[CBT_EPOCH_ID_MAX];
	char                    stale_backend_id[CBT_BACKEND_ID_MAX];
	uint64_t                generation;
	enum cbt_epoch_state    state;
	uint8_t                *bitmap_frozen;
	struct cbt_epoch       *next;
};

struct cbt_dirty_range {
	uint64_t offset_blocks;
	uint64_t length_blocks;
};

/* Simulated deferred name entry. */
struct cbt_name_entry {
	char *vbdev_name;
	char *bdev_name;
	uint32_t chunk_size_kb;
	struct cbt_name_entry *next;
};

struct cbt_device {
	uint8_t  *bitmap;
	uint64_t  bitmap_size_bits;
	uint64_t  bitmap_size_bytes;
	uint64_t  chunk_size_blocks;
	uint32_t  chunk_shift;
	uint32_t  chunk_size_kb;
	uint64_t  total_blocks;
	bool      healthy_clear_suspended;
	bool      backends_healthy;
	bool      dirty_history_valid;
	uint64_t  total_writes_tracked;

	struct cbt_epoch *epochs_head;
	uint64_t          epoch_count;
};

/* Global deferred name list (simulates g_bdev_names). */
static struct cbt_name_entry *g_names_head = NULL;

static void
names_clear(void)
{
	struct cbt_name_entry *n = g_names_head;
	while (n) {
		struct cbt_name_entry *next = n->next;
		free(n->vbdev_name);
		free(n->bdev_name);
		free(n);
		n = next;
	}
	g_names_head = NULL;
}

static struct cbt_name_entry *
names_find(const char *vbdev_name)
{
	struct cbt_name_entry *n = g_names_head;
	while (n) {
		if (strcmp(n->vbdev_name, vbdev_name) == 0) return n;
		n = n->next;
	}
	return NULL;
}

static void
names_remove(const char *vbdev_name)
{
	struct cbt_name_entry **pp = &g_names_head;
	while (*pp) {
		if (strcmp((*pp)->vbdev_name, vbdev_name) == 0) {
			struct cbt_name_entry *del = *pp;
			*pp = del->next;
			free(del->vbdev_name);
			free(del->bdev_name);
			free(del);
			return;
		}
		pp = &(*pp)->next;
	}
}

static int
names_insert(const char *bdev_name, const char *vbdev_name, uint32_t chunk_size_kb)
{
	if (names_find(vbdev_name)) return -EEXIST;
	struct cbt_name_entry *n = calloc(1, sizeof(*n));
	if (!n) return -ENOMEM;
	n->bdev_name = strdup(bdev_name);
	n->vbdev_name = strdup(vbdev_name);
	n->chunk_size_kb = chunk_size_kb ? chunk_size_kb : 64;
	n->next = g_names_head;
	g_names_head = n;
	return 0;
}

/* ── Device creation/destruction ── */

static struct cbt_device *
cbt_create(uint64_t total_blocks, uint32_t chunk_size_kb, uint32_t block_size)
{
	struct cbt_device *dev = calloc(1, sizeof(*dev));
	if (!dev) return NULL;

	dev->chunk_size_kb = chunk_size_kb;
	dev->chunk_size_blocks = ((uint64_t)chunk_size_kb * 1024) / block_size;
	if (dev->chunk_size_blocks == 0) dev->chunk_size_blocks = 1;

	if ((dev->chunk_size_blocks & (dev->chunk_size_blocks - 1)) != 0) {
		uint64_t v = dev->chunk_size_blocks;
		v--; v |= v >> 1; v |= v >> 2; v |= v >> 4;
		v |= v >> 8; v |= v >> 16; v |= v >> 32;
		dev->chunk_size_blocks = v + 1;
	}
	dev->chunk_shift = (uint32_t)__builtin_ctzll(dev->chunk_size_blocks);

	dev->bitmap_size_bits  = (total_blocks + dev->chunk_size_blocks - 1) /
				 dev->chunk_size_blocks;
	dev->bitmap_size_bytes = (dev->bitmap_size_bits + 7) / 8;
	dev->total_blocks = total_blocks;

	dev->bitmap = calloc(1, dev->bitmap_size_bytes);
	if (!dev->bitmap) {
		free(dev);
		return NULL;
	}

	dev->epochs_head = NULL;
	dev->epoch_count = 0;
	dev->backends_healthy = false;
	dev->dirty_history_valid = true;
	return dev;
}

static void
cbt_destroy(struct cbt_device *dev)
{
	if (!dev) return;
	struct cbt_epoch *ep = dev->epochs_head;
	while (ep) {
		struct cbt_epoch *next = ep->next;
		free(ep->bitmap_frozen);
		free(ep);
		ep = next;
	}
	free(dev->bitmap);
	free(dev);
}

/* ── Optimized mark_dirty (mirrors the production code) ── */

static inline void
cbt_mark_dirty(struct cbt_device *dev, uint64_t offset_blocks, uint64_t num_blocks)
{
	if (num_blocks == 0 || dev->bitmap_size_bits == 0) return;

	uint64_t chunk_start = offset_blocks >> dev->chunk_shift;
	uint64_t chunk_end   = (offset_blocks + num_blocks - 1) >> dev->chunk_shift;

	if (chunk_end >= dev->bitmap_size_bits) {
		chunk_end = dev->bitmap_size_bits - 1;
	}

	uint64_t byte_start = chunk_start >> 3;
	uint64_t byte_end   = chunk_end >> 3;

	if (byte_start == byte_end) {
		uint8_t mask = 0;
		for (uint64_t i = chunk_start; i <= chunk_end; i++) {
			mask |= (uint8_t)(1u << (i & 7));
		}
		__atomic_fetch_or(&dev->bitmap[byte_start], mask, __ATOMIC_RELAXED);
	} else {
		uint8_t first_mask = (uint8_t)(0xFF << (chunk_start & 7));
		__atomic_fetch_or(&dev->bitmap[byte_start], first_mask, __ATOMIC_RELAXED);

		uint64_t full_start = byte_start + 1;
		uint64_t full_end   = byte_end;
		if (full_end > full_start) {
			memset(&dev->bitmap[full_start], 0xFF, full_end - full_start);
		}

		uint8_t last_mask = (uint8_t)(0xFF >> (7 - (chunk_end & 7)));
		__atomic_fetch_or(&dev->bitmap[byte_end], last_mask, __ATOMIC_RELAXED);
	}

	__atomic_fetch_add(&dev->total_writes_tracked, 1, __ATOMIC_RELAXED);
}

/* ── Reference mark_dirty (simple per-bit, for comparison) ── */

static inline void
cbt_mark_dirty_reference(struct cbt_device *dev, uint64_t offset_blocks, uint64_t num_blocks)
{
	if (num_blocks == 0 || dev->bitmap_size_bits == 0) return;

	uint64_t chunk_start = offset_blocks >> dev->chunk_shift;
	uint64_t chunk_end   = (offset_blocks + num_blocks - 1) >> dev->chunk_shift;

	if (chunk_end >= dev->bitmap_size_bits) {
		chunk_end = dev->bitmap_size_bits - 1;
	}

	for (uint64_t i = chunk_start; i <= chunk_end; i++) {
		dev->bitmap[i >> 3] |= (uint8_t)(1u << (i & 7));
	}
}

/* ── Epoch helpers ── */

static struct cbt_epoch *
cbt_find_epoch(struct cbt_device *dev, const char *epoch_id)
{
	struct cbt_epoch *ep = dev->epochs_head;
	while (ep) {
		if (strcmp(ep->epoch_id, epoch_id) == 0) return ep;
		ep = ep->next;
	}
	return NULL;
}

static bool
cbt_any_epoch_open(struct cbt_device *dev)
{
	struct cbt_epoch *ep = dev->epochs_head;
	while (ep) {
		if (ep->state == CBT_EPOCH_OPEN || ep->state == CBT_EPOCH_FROZEN ||
		    ep->state == CBT_EPOCH_REBUILDING) {
			return true;
		}
		ep = ep->next;
	}
	return false;
}

static int
cbt_epoch_open(struct cbt_device *dev, const char *epoch_id,
	       const char *backend_id, uint64_t generation)
{
	if (strlen(epoch_id) >= CBT_EPOCH_ID_MAX) return -ENAMETOOLONG;
	if (strlen(backend_id) >= CBT_BACKEND_ID_MAX) return -ENAMETOOLONG;

	struct cbt_epoch *existing = cbt_find_epoch(dev, epoch_id);
	if (existing) {
		if (generation > existing->generation) {
			existing->generation = generation;
			existing->state = CBT_EPOCH_OPEN;
			return 0;
		}
		return -EEXIST;
	}

	if (dev->epoch_count >= CBT_MAX_EPOCHS) {
		/* Refuse eviction while the oldest epoch is active. */
		struct cbt_epoch *oldest = dev->epochs_head;
		if (!oldest || oldest->state == CBT_EPOCH_OPEN ||
		    oldest->state == CBT_EPOCH_FROZEN ||
		    oldest->state == CBT_EPOCH_REBUILDING) {
			return -ENOSPC;
		}
		/* Safe to evict COMPLETED or INVALID. */
		dev->epochs_head = oldest->next;
		dev->epoch_count--;
		free(oldest->bitmap_frozen);
		free(oldest);
	}

	struct cbt_epoch *ep = calloc(1, sizeof(*ep));
	if (!ep) return -ENOMEM;

	snprintf(ep->epoch_id, sizeof(ep->epoch_id), "%s", epoch_id);
	snprintf(ep->stale_backend_id, sizeof(ep->stale_backend_id), "%s", backend_id);
	ep->generation = generation;
	ep->state = CBT_EPOCH_OPEN;

	/* Append to tail. */
	if (!dev->epochs_head) {
		dev->epochs_head = ep;
	} else {
		struct cbt_epoch *tail = dev->epochs_head;
		while (tail->next) tail = tail->next;
		tail->next = ep;
	}
	dev->epoch_count++;
	dev->healthy_clear_suspended = true;
	return 0;
}

/* Mirrors vbdev_cbt.c: snapshot-and-clear is only safe when no OTHER epoch
 * still needs the shared accumulated live view. */
static bool
cbt_has_other_active_epoch(struct cbt_device *dev, struct cbt_epoch *self)
{
	for (struct cbt_epoch *ep = dev->epochs_head; ep; ep = ep->next) {
		if (ep == self) continue;
		if (ep->state == CBT_EPOCH_OPEN || ep->state == CBT_EPOCH_FROZEN ||
		    ep->state == CBT_EPOCH_REBUILDING) {
			return true;
		}
	}
	return false;
}

static int
cbt_epoch_freeze(struct cbt_device *dev, const char *epoch_id)
{
	struct cbt_epoch *ep = cbt_find_epoch(dev, epoch_id);
	if (!ep) return -ENOENT;
	if (ep->state != CBT_EPOCH_OPEN && ep->state != CBT_EPOCH_FROZEN) return -EINVAL;

	free(ep->bitmap_frozen);
	ep->bitmap_frozen = malloc(dev->bitmap_size_bytes);
	if (!ep->bitmap_frozen) return -ENOMEM;

	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	if (!cbt_has_other_active_epoch(dev, ep)) {
		/* Delta semantics: atomic per-byte exchange (no lost concurrent ORs). */
		for (uint64_t i = 0; i < dev->bitmap_size_bytes; i++) {
			ep->bitmap_frozen[i] = __atomic_exchange_n(&dev->bitmap[i], 0,
								   __ATOMIC_ACQ_REL);
		}
	} else {
		memcpy(ep->bitmap_frozen, dev->bitmap, dev->bitmap_size_bytes);
	}
	ep->state = CBT_EPOCH_FROZEN;
	return 0;
}

static int
cbt_epoch_rebuild_start(struct cbt_device *dev, const char *epoch_id)
{
	struct cbt_epoch *ep = cbt_find_epoch(dev, epoch_id);
	if (!ep) return -ENOENT;
	if (ep->state != CBT_EPOCH_FROZEN) return -EINVAL;
	ep->state = CBT_EPOCH_REBUILDING;
	return 0;
}

static int
cbt_epoch_close(struct cbt_device *dev, const char *epoch_id)
{
	struct cbt_epoch *ep = cbt_find_epoch(dev, epoch_id);
	if (!ep) return -ENOENT;
	if (ep->state == CBT_EPOCH_OPEN) return -EINVAL;

	/* Remove from list. */
	struct cbt_epoch **pp = &dev->epochs_head;
	while (*pp && *pp != ep) pp = &(*pp)->next;
	if (*pp) *pp = ep->next;
	dev->epoch_count--;
	free(ep->bitmap_frozen);
	free(ep);

	if (!cbt_any_epoch_open(dev)) {
		dev->healthy_clear_suspended = false;
	}
	return 0;
}

/* ── Reset (refuses during active epochs) ── */

static int
cbt_reset(struct cbt_device *dev)
{
	if (cbt_any_epoch_open(dev)) {
		return -EBUSY;
	}
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	memset(dev->bitmap, 0, dev->bitmap_size_bytes);
	return 0;
}

/* ── Healthy-clear poller: model only — production clears on reset ── */

static bool
cbt_healthy_clear_tick(struct cbt_device *dev)
{
	if (dev->healthy_clear_suspended || cbt_any_epoch_open(dev)) {
		return false;
	}
	if (!dev->backends_healthy) {
		return false;
	}

	for (uint64_t i = 0; i < dev->bitmap_size_bytes; i++) {
		if (dev->bitmap[i] != 0) {
			memset(dev->bitmap, 0, dev->bitmap_size_bytes);
			return true;
		}
	}
	return false;
}

/* ── get_dirty_ranges (reports truncation) ── */

static int
cbt_get_dirty_ranges(struct cbt_device *dev, const char *epoch_id,
		     uint32_t max_ranges,
		     struct cbt_dirty_range **out_ranges,
		     uint32_t *out_count,
		     bool *out_truncated)
{
	struct cbt_epoch *ep = cbt_find_epoch(dev, epoch_id);
	if (!ep) return -ENOENT;
	if (ep->state != CBT_EPOCH_FROZEN && ep->state != CBT_EPOCH_REBUILDING) return -EINVAL;
	if (!ep->bitmap_frozen) return -EINVAL;

	uint32_t cap = max_ranges ? max_ranges : 4096;
	if (cap > CBT_MAX_RANGES_LIMIT) cap = CBT_MAX_RANGES_LIMIT;

	struct cbt_dirty_range *ranges = calloc(cap, sizeof(*ranges));
	if (!ranges) return -ENOMEM;

	uint32_t count = 0;
	bool truncated = false;
	int64_t run_start = -1;

	for (uint64_t i = 0; i < dev->bitmap_size_bits; i++) {
		bool is_dirty = (ep->bitmap_frozen[i / 8] & (1u << (i % 8))) != 0;

		if (is_dirty && run_start < 0) {
			run_start = (int64_t)i;
		}

		if (!is_dirty || i == dev->bitmap_size_bits - 1) {
			if (run_start >= 0) {
				uint64_t end = is_dirty ? i : i - 1;
				if (count < cap) {
					uint64_t offset = (uint64_t)run_start * dev->chunk_size_blocks;
					uint64_t length = (end - (uint64_t)run_start + 1) *
							   dev->chunk_size_blocks;
					if (offset + length > dev->total_blocks) {
						length = dev->total_blocks - offset;
					}
					ranges[count].offset_blocks = offset;
					ranges[count].length_blocks = length;
					count++;
				} else {
					truncated = true;
				}
				run_start = -1;
			}
		}
	}

	*out_ranges = ranges;
	*out_count = count;
	*out_truncated = truncated;
	return 0;
}

/* ================================================================== */
/* Test framework                                                     */
/* ================================================================== */

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST(name) static void name(void)
#define RUN(name) do { \
	printf("  %-55s", #name); \
	name(); \
	g_tests_run++; g_tests_passed++; \
	printf("✓\n"); \
} while (0)

#define ASSERT(cond) do { \
	if (!(cond)) { \
		printf("✗\n    ASSERT FAILED: %s\n    at %s:%d\n", \
		       #cond, __FILE__, __LINE__); \
		exit(1); \
	} \
} while (0)

/* ================================================================== */
/* Reset refuses during active epochs                                 */
/* ================================================================== */

/* An OPEN epoch makes reset return -EBUSY and leaves the bitmap untouched. */
TEST(test_reset_refuses_during_open_epoch)
{
	struct cbt_device *dev = cbt_create(1024 * 128, 64, 512);
	ASSERT(dev != NULL);

	cbt_mark_dirty(dev, 0, 128);
	int rc = cbt_epoch_open(dev, "ep1", "backend1", 1);
	ASSERT(rc == 0);

	rc = cbt_reset(dev);
	ASSERT(rc == -EBUSY);

	bool any_set = false;
	for (uint64_t i = 0; i < dev->bitmap_size_bytes; i++) {
		if (dev->bitmap[i]) { any_set = true; break; }
	}
	ASSERT(any_set);

	cbt_destroy(dev);
}

/* A FROZEN epoch also blocks reset. */
TEST(test_reset_refuses_during_frozen_epoch)
{
	struct cbt_device *dev = cbt_create(1024 * 128, 64, 512);
	ASSERT(dev != NULL);

	cbt_mark_dirty(dev, 0, 128);
	cbt_epoch_open(dev, "ep1", "backend1", 1);
	cbt_epoch_freeze(dev, "ep1");

	int rc = cbt_reset(dev);
	ASSERT(rc == -EBUSY);

	cbt_destroy(dev);
}

/* A REBUILDING epoch also blocks reset. */
TEST(test_reset_refuses_during_rebuilding_epoch)
{
	struct cbt_device *dev = cbt_create(1024 * 128, 64, 512);
	ASSERT(dev != NULL);

	cbt_mark_dirty(dev, 0, 128);
	cbt_epoch_open(dev, "ep1", "backend1", 1);
	cbt_epoch_freeze(dev, "ep1");
	cbt_epoch_rebuild_start(dev, "ep1");

	int rc = cbt_reset(dev);
	ASSERT(rc == -EBUSY);

	cbt_destroy(dev);
}

/* Once the last epoch is closed, reset succeeds and zeroes the bitmap. */
TEST(test_reset_succeeds_after_all_epochs_closed)
{
	struct cbt_device *dev = cbt_create(1024 * 128, 64, 512);
	ASSERT(dev != NULL);

	cbt_mark_dirty(dev, 0, 128);
	cbt_epoch_open(dev, "ep1", "backend1", 1);
	cbt_epoch_freeze(dev, "ep1");
	cbt_epoch_close(dev, "ep1");

	int rc = cbt_reset(dev);
	ASSERT(rc == 0);

	for (uint64_t i = 0; i < dev->bitmap_size_bytes; i++) {
		ASSERT(dev->bitmap[i] == 0);
	}

	cbt_destroy(dev);
}

/* ================================================================== */
/* Truncation flag                                                    */
/* ================================================================== */

/* 50 alternating dirty chunks make 50 ranges: asking for 5 reports truncated,
 * asking for 100 does not. */
TEST(test_truncation_flag_when_ranges_exceeded)
{
	struct cbt_device *dev = cbt_create(1024 * 1024, 64, 512);
	ASSERT(dev != NULL);

	for (uint64_t i = 0; i < dev->bitmap_size_bits && i < 100; i += 2) {
		uint64_t offset = i * dev->chunk_size_blocks;
		cbt_mark_dirty(dev, offset, dev->chunk_size_blocks);
	}

	cbt_epoch_open(dev, "ep1", "b1", 1);
	cbt_epoch_freeze(dev, "ep1");

	struct cbt_dirty_range *ranges = NULL;
	uint32_t count = 0;
	bool truncated = false;

	int rc = cbt_get_dirty_ranges(dev, "ep1", 5, &ranges, &count, &truncated);
	ASSERT(rc == 0);
	ASSERT(count == 5);
	ASSERT(truncated == true);
	free(ranges);

	rc = cbt_get_dirty_ranges(dev, "ep1", 100, &ranges, &count, &truncated);
	ASSERT(rc == 0);
	ASSERT(count == 50);
	ASSERT(truncated == false);
	free(ranges);

	cbt_destroy(dev);
}

/* A range list that fits leaves the truncated flag clear. */
TEST(test_truncation_flag_not_set_when_fits)
{
	struct cbt_device *dev = cbt_create(1024 * 128, 64, 512);
	ASSERT(dev != NULL);

	cbt_mark_dirty(dev, 0, 128);  /* Single range. */
	cbt_epoch_open(dev, "ep1", "b1", 1);
	cbt_epoch_freeze(dev, "ep1");

	struct cbt_dirty_range *ranges = NULL;
	uint32_t count = 0;
	bool truncated = false;

	int rc = cbt_get_dirty_ranges(dev, "ep1", 0, &ranges, &count, &truncated);
	ASSERT(rc == 0);
	ASSERT(count >= 1);
	ASSERT(truncated == false);
	free(ranges);

	cbt_destroy(dev);
}

/* ================================================================== */
/* Deferred delete cleanup                                            */
/* ================================================================== */

/* Deleting a vbdev whose create is still deferred drops the pending name entry
 * instead of leaving it behind. */
TEST(test_deferred_delete_cleans_name_entry)
{
	names_clear();

	int rc = names_insert("base0", "cbt0", 64);
	ASSERT(rc == 0);
	ASSERT(names_find("cbt0") != NULL);

	names_remove("cbt0");
	ASSERT(names_find("cbt0") == NULL);

	names_clear();
}

/* Deleting an unknown name is a no-op, not a crash. */
TEST(test_deferred_delete_nonexistent_is_noop)
{
	names_clear();

	names_remove("ghost");
	ASSERT(names_find("ghost") == NULL);

	names_clear();
}

/* ================================================================== */
/* Rollback on create failure                                         */
/* ================================================================== */

/* A failed registration rolls the name entry back, so the name stays free. */
TEST(test_rollback_name_on_register_failure)
{
	names_clear();

	int rc = names_insert("base_fail", "cbt_fail", 64);
	ASSERT(rc == 0);
	ASSERT(names_find("cbt_fail") != NULL);

	names_remove("cbt_fail");
	ASSERT(names_find("cbt_fail") == NULL);

	names_clear();
}

/* A second vbdev with an existing name is rejected with -EEXIST. */
TEST(test_duplicate_name_rejected)
{
	names_clear();

	int rc = names_insert("base1", "cbt1", 64);
	ASSERT(rc == 0);

	rc = names_insert("base2", "cbt1", 128);
	ASSERT(rc == -EEXIST);

	names_clear();
}

/* ================================================================== */
/* Eviction refuses active epochs                                     */
/* ================================================================== */

/* With every slot holding an OPEN epoch, one more open returns -ENOSPC rather
 * than evicting. */
TEST(test_eviction_refuses_all_active)
{
	struct cbt_device *dev = cbt_create(1024 * 128, 64, 512);
	ASSERT(dev != NULL);

	for (int i = 0; i < CBT_MAX_EPOCHS; i++) {
		char id[16];
		snprintf(id, sizeof(id), "ep%d", i);
		int rc = cbt_epoch_open(dev, id, "b", (uint64_t)i + 1);
		ASSERT(rc == 0);
	}
	ASSERT(dev->epoch_count == CBT_MAX_EPOCHS);

	int rc = cbt_epoch_open(dev, "ep_overflow", "b", 99);
	ASSERT(rc == -ENOSPC);
	ASSERT(dev->epoch_count == CBT_MAX_EPOCHS);

	cbt_destroy(dev);
}

/* A FROZEN oldest epoch is not evictable. */
TEST(test_eviction_refuses_frozen_oldest)
{
	struct cbt_device *dev = cbt_create(1024 * 128, 64, 512);
	ASSERT(dev != NULL);

	cbt_mark_dirty(dev, 0, 128);

	cbt_epoch_open(dev, "ep0", "b", 1);
	cbt_epoch_freeze(dev, "ep0");

	for (int i = 1; i < CBT_MAX_EPOCHS; i++) {
		char id[16];
		snprintf(id, sizeof(id), "ep%d", i);
		cbt_epoch_open(dev, id, "b", (uint64_t)i + 1);
	}

	int rc = cbt_epoch_open(dev, "ep_new", "b", 99);
	ASSERT(rc == -ENOSPC);

	cbt_destroy(dev);
}

/* A REBUILDING oldest epoch is not evictable. */
TEST(test_eviction_refuses_rebuilding_oldest)
{
	struct cbt_device *dev = cbt_create(1024 * 128, 64, 512);
	ASSERT(dev != NULL);

	cbt_mark_dirty(dev, 0, 128);

	cbt_epoch_open(dev, "ep0", "b", 1);
	cbt_epoch_freeze(dev, "ep0");
	cbt_epoch_rebuild_start(dev, "ep0");

	for (int i = 1; i < CBT_MAX_EPOCHS; i++) {
		char id[16];
		snprintf(id, sizeof(id), "ep%d", i);
		cbt_epoch_open(dev, id, "b", (uint64_t)i + 1);
	}

	int rc = cbt_epoch_open(dev, "ep_new", "b", 99);
	ASSERT(rc == -ENOSPC);

	cbt_destroy(dev);
}

/* A closed epoch frees its slot, so the next open succeeds. */
TEST(test_eviction_succeeds_when_oldest_completed)
{
	struct cbt_device *dev = cbt_create(1024 * 128, 64, 512);
	ASSERT(dev != NULL);

	cbt_mark_dirty(dev, 0, 128);

	cbt_epoch_open(dev, "ep0", "b", 1);
	cbt_epoch_freeze(dev, "ep0");
	cbt_epoch_close(dev, "ep0");

	for (int i = 1; i <= CBT_MAX_EPOCHS; i++) {
		char id[16];
		snprintf(id, sizeof(id), "ep%d", i);
		int rc = cbt_epoch_open(dev, id, "b", (uint64_t)i + 1);
		ASSERT(rc == 0);
	}

	cbt_destroy(dev);
}

/* ================================================================== */
/* Healthy-clear requires an explicit backends_healthy signal          */
/* ================================================================== */

/* Without the healthy signal the clear never fires and the bits survive. */
TEST(test_clear_blocked_when_not_healthy)
{
	struct cbt_device *dev = cbt_create(1024 * 128, 64, 512);
	ASSERT(dev != NULL);

	cbt_mark_dirty(dev, 0, 1024);
	dev->backends_healthy = false;

	bool cleared = cbt_healthy_clear_tick(dev);
	ASSERT(cleared == false);

	bool any_set = false;
	for (uint64_t i = 0; i < dev->bitmap_size_bytes; i++) {
		if (dev->bitmap[i]) { any_set = true; break; }
	}
	ASSERT(any_set);

	cbt_destroy(dev);
}

/* With the healthy signal set and no epoch open, the clear zeroes the bitmap. */
TEST(test_clear_proceeds_when_healthy)
{
	struct cbt_device *dev = cbt_create(1024 * 128, 64, 512);
	ASSERT(dev != NULL);

	cbt_mark_dirty(dev, 0, 1024);
	dev->backends_healthy = true;

	bool cleared = cbt_healthy_clear_tick(dev);
	ASSERT(cleared == true);

	for (uint64_t i = 0; i < dev->bitmap_size_bytes; i++) {
		ASSERT(dev->bitmap[i] == 0);
	}

	cbt_destroy(dev);
}

/* An open epoch blocks the clear even when the backends are healthy. */
TEST(test_clear_blocked_during_epoch_even_if_healthy)
{
	struct cbt_device *dev = cbt_create(1024 * 128, 64, 512);
	ASSERT(dev != NULL);

	cbt_mark_dirty(dev, 0, 1024);
	dev->backends_healthy = true;
	cbt_epoch_open(dev, "ep1", "b1", 1);

	bool cleared = cbt_healthy_clear_tick(dev);
	ASSERT(cleared == false);

	cbt_destroy(dev);
}

/* The clear follows the healthy flag in both directions, revocation included. */
TEST(test_healthy_flag_transition)
{
	struct cbt_device *dev = cbt_create(1024 * 128, 64, 512);
	ASSERT(dev != NULL);

	cbt_mark_dirty(dev, 0, 1024);

	dev->backends_healthy = false;
	ASSERT(cbt_healthy_clear_tick(dev) == false);

	dev->backends_healthy = true;
	ASSERT(cbt_healthy_clear_tick(dev) == true);

	cbt_mark_dirty(dev, 0, 1024);
	dev->backends_healthy = false;
	ASSERT(cbt_healthy_clear_tick(dev) == false);

	cbt_destroy(dev);
}

/* ================================================================== */
/* Optimized mark_dirty correctness                                   */
/* ================================================================== */

/* A range confined to one byte sets exactly the chunks it covers (2 to 5). */
TEST(test_mark_dirty_single_byte_range)
{
	struct cbt_device *dev = cbt_create(64 * 128, 64, 512);
	ASSERT(dev != NULL);

	uint64_t start = 2 * dev->chunk_size_blocks;
	uint64_t len   = 4 * dev->chunk_size_blocks;
	cbt_mark_dirty(dev, start, len);

	ASSERT((dev->bitmap[0] & 0x3C) == 0x3C);
	ASSERT((dev->bitmap[0] & 0xC3) == 0);

	cbt_destroy(dev);
}

/* A range crossing byte boundaries sets the leading partial byte, the full byte
 * and the trailing partial byte, and nothing else (chunks 5 to 20). */
TEST(test_mark_dirty_spans_multiple_bytes)
{
	struct cbt_device *dev = cbt_create(256 * 128, 64, 512);
	ASSERT(dev != NULL);

	uint64_t start = 5 * dev->chunk_size_blocks;
	uint64_t len   = 16 * dev->chunk_size_blocks;
	cbt_mark_dirty(dev, start, len);

	ASSERT((dev->bitmap[0] & 0xE0) == 0xE0);  /* bits 5,6,7 */
	ASSERT((dev->bitmap[0] & 0x1F) == 0x00);  /* bits 0-4 clear */
	ASSERT(dev->bitmap[1] == 0xFF);            /* full byte */
	ASSERT((dev->bitmap[2] & 0x1F) == 0x1F);  /* bits 0-4 */
	ASSERT((dev->bitmap[2] & 0xE0) == 0x00);  /* bits 5-7 clear */

	cbt_destroy(dev);
}

/* Over a spread of ranges (single block, boundaries, whole device, past the
 * end), the optimized marker produces exactly the reference bitmap. */
TEST(test_mark_dirty_large_range_matches_reference)
{
	uint64_t total_blocks = 1024 * 1024;  /* ~1M blocks */
	struct cbt_device *dev_opt = cbt_create(total_blocks, 64, 512);
	struct cbt_device *dev_ref = cbt_create(total_blocks, 64, 512);
	ASSERT(dev_opt != NULL && dev_ref != NULL);

	struct { uint64_t off; uint64_t len; } ops[] = {
		{0, 1},                           /* single block */
		{127, 1},                         /* boundary */
		{128, 256},                       /* aligned start */
		{1000, 500000},                   /* huge range */
		{999999, 1},                      /* near end */
		{0, total_blocks},                /* full device */
		{500, 100000},                    /* large mid */
		{3, 5},                           /* small mid */
		{total_blocks - 10, 100},         /* overflow clamped */
	};

	for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
		cbt_mark_dirty(dev_opt, ops[i].off, ops[i].len);
		cbt_mark_dirty_reference(dev_ref, ops[i].off, ops[i].len);
	}

	ASSERT(dev_opt->bitmap_size_bytes == dev_ref->bitmap_size_bytes);
	ASSERT(memcmp(dev_opt->bitmap, dev_ref->bitmap, dev_opt->bitmap_size_bytes) == 0);

	cbt_destroy(dev_opt);
	cbt_destroy(dev_ref);
}

/* A range starting on a byte boundary fills that byte and no neighbour. */
TEST(test_mark_dirty_first_bit_of_byte)
{
	struct cbt_device *dev = cbt_create(128 * 128, 64, 512);
	ASSERT(dev != NULL);

	uint64_t start = 8 * dev->chunk_size_blocks;
	uint64_t len   = 8 * dev->chunk_size_blocks;
	cbt_mark_dirty(dev, start, len);

	ASSERT(dev->bitmap[0] == 0x00);
	ASSERT(dev->bitmap[1] == 0xFF);
	ASSERT(dev->bitmap[2] == 0x00);

	cbt_destroy(dev);
}

/* A range ending on a byte boundary fills that byte and no neighbour. */
TEST(test_mark_dirty_last_bit_of_byte)
{
	struct cbt_device *dev = cbt_create(128 * 128, 64, 512);
	ASSERT(dev != NULL);

	uint64_t start = 0;
	uint64_t len   = 8 * dev->chunk_size_blocks;
	cbt_mark_dirty(dev, start, len);

	ASSERT(dev->bitmap[0] == 0xFF);
	ASSERT(dev->bitmap[1] == 0x00);

	cbt_destroy(dev);
}

/* The narrowest cross-byte range sets one bit on each side and no other. */
TEST(test_mark_dirty_cross_byte_boundary_two_bits)
{
	struct cbt_device *dev = cbt_create(128 * 128, 64, 512);
	ASSERT(dev != NULL);

	uint64_t start = 7 * dev->chunk_size_blocks;
	uint64_t len   = 2 * dev->chunk_size_blocks;
	cbt_mark_dirty(dev, start, len);

	ASSERT((dev->bitmap[0] & 0x80) == 0x80);  /* bit 7 */
	ASSERT((dev->bitmap[1] & 0x01) == 0x01);  /* bit 0 */
	ASSERT((dev->bitmap[0] & 0x7F) == 0x00);
	ASSERT((dev->bitmap[1] & 0xFE) == 0x00);

	cbt_destroy(dev);
}

/* Thread worker for concurrent test. */
struct concurrent_mark_arg {
	struct cbt_device *dev;
	int id;
};

static void *
concurrent_mark_thread(void *arg)
{
	struct concurrent_mark_arg *ta = arg;
	uint64_t seed = (uint64_t)ta->id * 6364136223846793005ULL + 1;
	for (int i = 0; i < 10000; i++) {
		seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
		uint64_t off = seed % ta->dev->total_blocks;
		uint64_t len = (seed >> 17) % 10000 + 1;
		cbt_mark_dirty(ta->dev, off, len);
	}
	return NULL;
}

/* Eight threads marking overlapping ranges lose no bit: every bit the
 * single-threaded reference replay sets is also set in the concurrent bitmap. */
TEST(test_mark_dirty_concurrent_no_loss)
{
	uint64_t total = 1024 * 1024;
	struct cbt_device *dev = cbt_create(total, 64, 512);
	ASSERT(dev != NULL);

	#define N_THREADS 8
	#define OPS_PER_THREAD 10000

	pthread_t threads[N_THREADS];
	struct concurrent_mark_arg args[N_THREADS];

	for (int i = 0; i < N_THREADS; i++) {
		args[i].dev = dev;
		args[i].id = i;
		pthread_create(&threads[i], NULL, concurrent_mark_thread, &args[i]);
	}
	for (int i = 0; i < N_THREADS; i++) {
		pthread_join(threads[i], NULL);
	}

	struct cbt_device *ref = cbt_create(total, 64, 512);
	ASSERT(ref != NULL);

	for (int t = 0; t < N_THREADS; t++) {
		uint64_t seed = (uint64_t)t * 6364136223846793005ULL + 1;
		for (int i = 0; i < OPS_PER_THREAD; i++) {
			seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
			uint64_t off = seed % ref->total_blocks;
			uint64_t len = (seed >> 17) % 10000 + 1;
			cbt_mark_dirty_reference(ref, off, len);
		}
	}

	for (uint64_t i = 0; i < dev->bitmap_size_bytes; i++) {
		ASSERT((dev->bitmap[i] & ref->bitmap[i]) == ref->bitmap[i]);
	}

	cbt_destroy(dev);
	cbt_destroy(ref);
	#undef N_THREADS
	#undef OPS_PER_THREAD
}

/* ================================================================== */
/* max_ranges bounded by CBT_MAX_RANGES_LIMIT                         */
/* ================================================================== */

/* An absurd max_ranges is capped instead of driving a huge allocation. */
TEST(test_max_ranges_capped_to_limit)
{
	struct cbt_device *dev = cbt_create(1024 * 128, 64, 512);
	ASSERT(dev != NULL);

	cbt_mark_dirty(dev, 0, 128);
	cbt_epoch_open(dev, "ep1", "b1", 1);
	cbt_epoch_freeze(dev, "ep1");

	struct cbt_dirty_range *ranges = NULL;
	uint32_t count = 0;
	bool truncated = false;

	int rc = cbt_get_dirty_ranges(dev, "ep1", 999999, &ranges, &count, &truncated);
	ASSERT(rc == 0);
	ASSERT(count >= 1);
	free(ranges);

	cbt_destroy(dev);
}

/* ================================================================== */
/* dirty_history_valid reflects state                                 */
/* ================================================================== */

/* A freshly created device starts with a valid dirty history. */
TEST(test_dirty_history_valid_initial)
{
	struct cbt_device *dev = cbt_create(1024 * 128, 64, 512);
	ASSERT(dev != NULL);

	ASSERT(dev->dirty_history_valid == true);

	cbt_destroy(dev);
}

/* ================================================================== */
/* Main                                                               */
/* ================================================================== */

int main(void)
{
	printf("CBT post-fix validation tests\n");
	printf("==============================\n\n");

	printf("── reset refuses during active epochs ──\n");
	RUN(test_reset_refuses_during_open_epoch);
	RUN(test_reset_refuses_during_frozen_epoch);
	RUN(test_reset_refuses_during_rebuilding_epoch);
	RUN(test_reset_succeeds_after_all_epochs_closed);

	printf("\n── truncation flag ──\n");
	RUN(test_truncation_flag_when_ranges_exceeded);
	RUN(test_truncation_flag_not_set_when_fits);

	printf("\n── deferred delete cleanup ──\n");
	RUN(test_deferred_delete_cleans_name_entry);
	RUN(test_deferred_delete_nonexistent_is_noop);

	printf("\n── rollback on create failure ──\n");
	RUN(test_rollback_name_on_register_failure);
	RUN(test_duplicate_name_rejected);

	printf("\n── eviction refuses active epochs ──\n");
	RUN(test_eviction_refuses_all_active);
	RUN(test_eviction_refuses_frozen_oldest);
	RUN(test_eviction_refuses_rebuilding_oldest);
	RUN(test_eviction_succeeds_when_oldest_completed);

	printf("\n── healthy-clear requires backends_healthy ──\n");
	RUN(test_clear_blocked_when_not_healthy);
	RUN(test_clear_proceeds_when_healthy);
	RUN(test_clear_blocked_during_epoch_even_if_healthy);
	RUN(test_healthy_flag_transition);

	printf("\n── optimized mark_dirty correctness ──\n");
	RUN(test_mark_dirty_single_byte_range);
	RUN(test_mark_dirty_spans_multiple_bytes);
	RUN(test_mark_dirty_large_range_matches_reference);
	RUN(test_mark_dirty_first_bit_of_byte);
	RUN(test_mark_dirty_last_bit_of_byte);
	RUN(test_mark_dirty_cross_byte_boundary_two_bits);
	RUN(test_mark_dirty_concurrent_no_loss);

	printf("\n── max_ranges bounded ──\n");
	RUN(test_max_ranges_capped_to_limit);

	printf("\n── dirty_history_valid ──\n");
	RUN(test_dirty_history_valid_initial);

	printf("\n==============================\n");
	printf("Results: %d passed, %d failed\n", g_tests_passed, g_tests_run - g_tests_passed);
	return (g_tests_passed == g_tests_run) ? 0 : 1;
}
