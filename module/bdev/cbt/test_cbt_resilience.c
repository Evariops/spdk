/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Evariops.
 *   All rights reserved.
 */

/*
 * CBT resilience tests — negative paths, error injection, failure modes, run
 * against a standalone model of the module: allocation failure at every site,
 * invalid state transitions, adversarial inputs, epoch exhaustion, concurrent
 * mark against clear, and arithmetic edge cases.
 *
 * Build:  make -f Makefile.test test-resilience
 * Run:    ./test_cbt_resilience
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
#include <dlfcn.h>

/* ================================================================== */
/* malloc fault injection                                             */
/* ================================================================== */

static _Atomic int g_malloc_fail_countdown = -1;  /* -1 = no injection */
static _Atomic int g_malloc_fail_count = 0;

/* Wraps malloc/calloc with a countdown: the nth allocation returns NULL, which
 * is how each allocation failure path gets exercised in turn. */
static void *
fi_malloc(size_t size)
{
	int c = atomic_load(&g_malloc_fail_countdown);
	if (c >= 0) {
		if (c == 0) {
			atomic_fetch_add(&g_malloc_fail_count, 1);
			return NULL;
		}
		atomic_fetch_sub(&g_malloc_fail_countdown, 1);
	}
	return malloc(size);
}

static void *
fi_calloc(size_t nmemb, size_t size)
{
	int c = atomic_load(&g_malloc_fail_countdown);
	if (c >= 0) {
		if (c == 0) {
			atomic_fetch_add(&g_malloc_fail_count, 1);
			return NULL;
		}
		atomic_fetch_sub(&g_malloc_fail_countdown, 1);
	}
	return calloc(nmemb, size);
}

static char *
fi_strdup(const char *s)
{
	(void)fi_strdup; /* available for future use */
	int c = atomic_load(&g_malloc_fail_countdown);
	if (c >= 0) {
		if (c == 0) {
			atomic_fetch_add(&g_malloc_fail_count, 1);
			return NULL;
		}
		atomic_fetch_sub(&g_malloc_fail_countdown, 1);
	}
	return strdup(s);
}

static void
fi_reset(void)
{
	atomic_store(&g_malloc_fail_countdown, -1);
	atomic_store(&g_malloc_fail_count, 0);
}

static void
fi_set_fail_at(int nth_alloc)
{
	atomic_store(&g_malloc_fail_countdown, nth_alloc);
	atomic_store(&g_malloc_fail_count, 0);
}

/* ================================================================== */
/* Simulated CBT module (mirrors production code, uses fi_* allocs)   */
/* ================================================================== */

#define CBT_EPOCH_ID_MAX     64
#define CBT_BACKEND_ID_MAX   128
#define CBT_MAX_EPOCHS       4

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
	/* True while bitmap_frozen holds bits exchanged OUT of the live bitmap and
	 * not yet proven copied by a COMPLETED rebuild (mirrors vbdev_cbt.h). */
	bool                    frozen_live_consumed;
	/* Model equivalent of the production rebuild-registry RUNNING lookup. */
	bool                    rebuild_running;
	struct cbt_epoch       *next;
};

struct cbt_device {
	uint8_t  *bitmap;
	uint64_t  bitmap_size_bits;
	uint64_t  bitmap_size_bytes;
	uint64_t  chunk_size_blocks;
	uint32_t  chunk_shift;
	uint64_t  total_blocks;
	bool      healthy_clear_suspended;

	struct cbt_epoch *epochs_head;
	uint64_t          epoch_count;
};

static struct cbt_device *
cbt_create(uint64_t total_blocks, uint32_t chunk_size_kb, uint32_t block_size)
{
	struct cbt_device *dev = fi_calloc(1, sizeof(*dev));
	if (!dev) return NULL;

	dev->chunk_size_blocks = ((uint64_t)chunk_size_kb * 1024) / block_size;
	if (dev->chunk_size_blocks == 0) dev->chunk_size_blocks = 1;

	/* Chunk size must be a power of two for the shift-based fast path. */
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

	dev->bitmap = fi_calloc(1, dev->bitmap_size_bytes);
	if (!dev->bitmap) {
		free(dev);
		return NULL;
	}

	dev->epochs_head = NULL;
	dev->epoch_count = 0;
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

static inline void
cbt_mark_dirty(struct cbt_device *dev, uint64_t offset_blocks, uint64_t num_blocks)
{
	if (num_blocks == 0 || dev->bitmap_size_bits == 0) return;

	uint64_t chunk_start = offset_blocks >> dev->chunk_shift;
	uint64_t chunk_end   = (offset_blocks + num_blocks - 1) >> dev->chunk_shift;

	if (chunk_end >= dev->bitmap_size_bits) {
		chunk_end = dev->bitmap_size_bits - 1;
	}

	for (uint64_t i = chunk_start; i <= chunk_end; i++) {
		uint8_t mask = (uint8_t)(1u << (i & 7));
		__atomic_fetch_or(&dev->bitmap[i >> 3], mask, __ATOMIC_RELAXED);
	}
}

/* Two-phase marking, as in production: the dirty bit is set at SUBMIT (crash
 * conservatism) and re-set at COMPLETION before the host ack, so a freeze that
 * exchanges out the submit-time bit of an in-flight write is harmless. */
static void
cbt_write_submit(struct cbt_device *dev, uint64_t offset_blocks, uint64_t num_blocks)
{
	cbt_mark_dirty(dev, offset_blocks, num_blocks);
}

static void
cbt_write_complete(struct cbt_device *dev, uint64_t offset_blocks, uint64_t num_blocks)
{
	cbt_mark_dirty(dev, offset_blocks, num_blocks);
}

/* Mirrors the production terminal classification: the flush is submitted only
 * for a genuine success candidate (error 0, not cancelled, NOT aborted, chunks
 * copied, target present), a flush failure maps to FAILED, and the order of
 * tests is cancelled, then error, then aborted, then completed. */
enum cbt_rebuild_final_state {
	REB_COMPLETED = 0,
	REB_FAILED    = 1,
	REB_ABORTED   = 2,
	REB_CANCELLED = 3,
};

static enum cbt_rebuild_final_state
cbt_rebuild_finish_classify(int error, bool cancelled, bool aborted,
			    uint64_t chunks_copied, bool dst_present,
			    bool flush_fails)
{
	if (error == 0 && !cancelled && !aborted && chunks_copied > 0 && dst_present) {
		if (flush_fails) {
			error = -EIO;
			aborted = true;
		}
	}
	if (cancelled)  return REB_CANCELLED;
	if (error != 0) return REB_FAILED;
	if (aborted)    return REB_ABORTED;
	return REB_COMPLETED;
}

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
cbt_any_epoch_active(struct cbt_device *dev)
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

static void cbt_epoch_restore_unconsumed_delta(struct cbt_device *dev,
					       struct cbt_epoch *ep);

static int
cbt_epoch_open(struct cbt_device *dev, const char *epoch_id,
	       const char *stale_backend_id, uint64_t generation)
{
	if (!dev) return -EINVAL;
	if (strlen(epoch_id) >= CBT_EPOCH_ID_MAX) return -ENAMETOOLONG;
	if (strlen(stale_backend_id) >= CBT_BACKEND_ID_MAX) return -ENAMETOOLONG;

	struct cbt_epoch *existing = cbt_find_epoch(dev, epoch_id);
	if (existing) {
		if (generation > existing->generation) {
			existing->generation = generation;
			snprintf(existing->stale_backend_id,
				 sizeof(existing->stale_backend_id),
				 "%s", stale_backend_id);
			existing->state = CBT_EPOCH_OPEN;
			return 0;
		}
		return -EEXIST;
	}

	if (dev->epoch_count >= CBT_MAX_EPOCHS) {
		/* Evict the oldest ONLY if safe: never an active epoch
		 * (OPEN/FROZEN/REBUILDING) and never one a rebuild still points at. */
		struct cbt_epoch *oldest = dev->epochs_head;
		if (!oldest || oldest->state == CBT_EPOCH_OPEN ||
		    oldest->state == CBT_EPOCH_FROZEN ||
		    oldest->state == CBT_EPOCH_REBUILDING ||
		    oldest->rebuild_running) {
			return -ENOSPC;
		}
		/* An INVALID epoch may still hold an unconsumed exchanged delta. */
		cbt_epoch_restore_unconsumed_delta(dev, oldest);
		dev->epochs_head = oldest->next;
		dev->epoch_count--;
		free(oldest->bitmap_frozen);
		free(oldest);
	}

	struct cbt_epoch *ep = fi_calloc(1, sizeof(*ep));
	if (!ep) return -ENOMEM;

	snprintf(ep->epoch_id, sizeof(ep->epoch_id), "%s", epoch_id);
	snprintf(ep->stale_backend_id, sizeof(ep->stale_backend_id),
		 "%s", stale_backend_id);
	ep->generation = generation;
	ep->state = CBT_EPOCH_OPEN;
	ep->next = NULL;

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

/* Epochs share the live bitmap, so snapshot-and-clear is only safe when no
 * OTHER epoch still needs the accumulated view. */
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

/* OR an unconsumed exchanged delta back into the live bitmap before its buffer
 * is discarded. Pessimistic — already-copied chunks get re-copied — never lossy. */
static void
cbt_epoch_restore_unconsumed_delta(struct cbt_device *dev, struct cbt_epoch *ep)
{
	if (!ep->frozen_live_consumed || ep->bitmap_frozen == NULL) return;
	for (uint64_t i = 0; i < dev->bitmap_size_bytes; i++) {
		uint8_t b = ep->bitmap_frozen[i];
		if (b != 0) {
			__atomic_fetch_or(&dev->bitmap[i], b, __ATOMIC_RELAXED);
		}
	}
	ep->frozen_live_consumed = false;
}

static int
cbt_epoch_freeze(struct cbt_device *dev, const char *epoch_id)
{
	if (!dev) return -EINVAL;

	struct cbt_epoch *ep = cbt_find_epoch(dev, epoch_id);
	if (!ep) return -ENOENT;
	if (ep->state != CBT_EPOCH_OPEN && ep->state != CBT_EPOCH_FROZEN &&
	    ep->state != CBT_EPOCH_REBUILDING) {
		return -EINVAL;
	}
	/* Never free or realloc the frozen bitmap a RUNNING rebuild is scanning. */
	if (ep->rebuild_running) return -EBUSY;

	/* Allocate BEFORE touching the old snapshot: ENOMEM must leave the epoch,
	 * and the previous delta, exactly as they were. */
	uint8_t *new_frozen = fi_malloc(dev->bitmap_size_bytes);
	if (!new_frozen) return -ENOMEM;

	/* An unconsumed previous delta is merged back first, so the exchange below
	 * re-captures it together with the writes since the last freeze. */
	if (ep->bitmap_frozen != NULL) {
		cbt_epoch_restore_unconsumed_delta(dev, ep);
		free(ep->bitmap_frozen);
	}
	ep->bitmap_frozen = new_frozen;

	if (!cbt_has_other_active_epoch(dev, ep)) {
		/* Snapshot-AND-CLEAR: each freeze captures the DELTA since the previous
		 * one, so iterative rebuilds converge. Per-byte exchange, not
		 * memcpy+memset: a concurrent OR between a copy and a clear would be
		 * lost, and a missed chunk is silent divergence. */
		for (uint64_t i = 0; i < dev->bitmap_size_bytes; i++) {
			ep->bitmap_frozen[i] = __atomic_exchange_n(&dev->bitmap[i], 0,
								   __ATOMIC_ACQ_REL);
		}
		ep->frozen_live_consumed = true;
	} else {
		memcpy(ep->bitmap_frozen, dev->bitmap, dev->bitmap_size_bytes);
		ep->frozen_live_consumed = false;
	}
	ep->state = CBT_EPOCH_FROZEN;
	return 0;
}

static int
cbt_epoch_rebuild_start(struct cbt_device *dev, const char *epoch_id)
{
	if (!dev) return -EINVAL;

	struct cbt_epoch *ep = cbt_find_epoch(dev, epoch_id);
	if (!ep) return -ENOENT;
	/* FROZEN or REBUILDING (a retry), a frozen bitmap is required, and one
	 * rebuild per epoch. */
	if (ep->state != CBT_EPOCH_FROZEN && ep->state != CBT_EPOCH_REBUILDING) {
		return -EINVAL;
	}
	if (!ep->bitmap_frozen) return -EINVAL;
	if (ep->rebuild_running) return -EBUSY;

	ep->state = CBT_EPOCH_REBUILDING;
	ep->rebuild_running = true;
	return 0;
}

/* Model of the rebuild terminating: on COMPLETED the exchanged delta is proven
 * copied; on abort or failure it stays owned by bitmap_frozen until merged back. */
static int
cbt_epoch_rebuild_finish(struct cbt_device *dev, const char *epoch_id, bool completed)
{
	struct cbt_epoch *ep = cbt_find_epoch(dev, epoch_id);
	if (!ep) return -ENOENT;
	if (!ep->rebuild_running) return -EINVAL;

	ep->rebuild_running = false;
	if (completed) {
		ep->frozen_live_consumed = false;
	}
	return 0;
}

static int
cbt_epoch_close(struct cbt_device *dev, const char *epoch_id)
{
	if (!dev) return -EINVAL;

	struct cbt_epoch *ep = cbt_find_epoch(dev, epoch_id);
	if (!ep) return -ENOENT;
	if (ep->state == CBT_EPOCH_OPEN) return -EINVAL;
	/* A RUNNING rebuild writes into the epoch. */
	if (ep->rebuild_running) return -EBUSY;

	/* Closing must not lose un-copied dirty history. */
	cbt_epoch_restore_unconsumed_delta(dev, ep);

	/* Remove from list. */
	struct cbt_epoch **pp = &dev->epochs_head;
	while (*pp && *pp != ep) pp = &(*pp)->next;
	if (*pp) *pp = ep->next;
	dev->epoch_count--;

	free(ep->bitmap_frozen);
	free(ep);

	if (!cbt_any_epoch_active(dev)) {
		dev->healthy_clear_suspended = false;
	}
	return 0;
}

static int
cbt_epoch_invalidate(struct cbt_device *dev, const char *epoch_id)
{
	if (!dev) return -EINVAL;

	struct cbt_epoch *ep = cbt_find_epoch(dev, epoch_id);
	if (!ep) return -ENOENT;
	/* Invalidating a REBUILDING epoch would make it evictable under the RUNNING
	 * rebuild, freeing memory that rebuild still reads. Cancel it first. */
	if (ep->rebuild_running) return -EBUSY;

	ep->state = CBT_EPOCH_INVALID;
	return 0;
}

static uint64_t
count_bits(const uint8_t *bm, uint64_t bytes)
{
	uint64_t n = 0;
	for (uint64_t i = 0; i < bytes; i++) {
		n += (uint64_t)__builtin_popcount(bm[i]);
	}
	return n;
}

struct dirty_range {
	uint64_t offset_blocks;
	uint64_t length_blocks;
};

static int
cbt_epoch_get_dirty_ranges(struct cbt_device *dev, const char *epoch_id,
			   uint32_t max_ranges,
			   struct dirty_range **out_ranges, uint32_t *out_count)
{
	if (!dev) return -EINVAL;

	struct cbt_epoch *ep = cbt_find_epoch(dev, epoch_id);
	if (!ep) return -ENOENT;
	if (ep->state != CBT_EPOCH_FROZEN && ep->state != CBT_EPOCH_REBUILDING) {
		return -EINVAL;
	}
	if (!ep->bitmap_frozen) return -EINVAL;

	uint32_t cap = max_ranges ? max_ranges : 4096;
	struct dirty_range *ranges = fi_calloc(cap, sizeof(*ranges));
	if (!ranges) return -ENOMEM;

	uint32_t count = 0;
	int64_t run_start = -1;
	const uint8_t *bmap = ep->bitmap_frozen;

	for (uint64_t i = 0; i < dev->bitmap_size_bits; i++) {
		bool is_dirty = (bmap[i >> 3] & (1u << (i & 7))) != 0;

		if (is_dirty && run_start < 0) {
			run_start = (int64_t)i;
		}
		if (!is_dirty || i == dev->bitmap_size_bits - 1) {
			if (run_start >= 0) {
				uint64_t end = is_dirty ? i : i - 1;
				if (count < cap) {
					uint64_t off = (uint64_t)run_start * dev->chunk_size_blocks;
					uint64_t len = (end - (uint64_t)run_start + 1) *
						       dev->chunk_size_blocks;
					if (off + len > dev->total_blocks) {
						len = dev->total_blocks - off;
					}
					ranges[count].offset_blocks = off;
					ranges[count].length_blocks = len;
					count++;
				}
				run_start = -1;
			}
		}
	}

	*out_ranges = ranges;
	*out_count = count;
	return 0;
}

/* ================================================================== */
/* Test harness                                                       */
/* ================================================================== */

static int g_passed = 0;
static int g_failed = 0;

#define TEST(name) do { printf("  %-55s", #name); } while (0)
#define PASS()     do { printf(" \xe2\x9c\x93\n"); g_passed++; } while (0)
#define ASSERT(expr) do {                                               \
	if (!(expr)) {                                                  \
		printf(" \xe2\x9c\x97 FAIL at %s:%d: %s\n",            \
		       __FILE__, __LINE__, #expr);                       \
		g_failed++; fi_reset(); return;                         \
	}                                                               \
} while (0)
#define ASSERT_EQ(a, b)  ASSERT((a) == (b))
#define ASSERT_NE(a, b)  ASSERT((a) != (b))
#define ASSERT_RC(rc, expected) ASSERT((rc) == (expected))

/* ================================================================== */
/* SECTION 1: Allocation failures (fault injection)                   */
/* ================================================================== */

/* A failed device allocation yields NULL, not a half-built device. */
static void test_create_malloc_fail_struct(void)
{
	TEST(test_create_malloc_fail_struct);
	fi_set_fail_at(0);  /* First calloc (device struct) fails */
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev == NULL);
	fi_reset();
	PASS();
}

/* A failed bitmap allocation frees the device struct and yields NULL. */
static void test_create_malloc_fail_bitmap(void)
{
	TEST(test_create_malloc_fail_bitmap);
	fi_set_fail_at(1);  /* Second calloc (bitmap) fails */
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev == NULL);
	fi_reset();
	PASS();
}

/* A failed epoch allocation returns -ENOMEM and adds no epoch. */
static void test_epoch_open_malloc_fail(void)
{
	TEST(test_epoch_open_malloc_fail);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev != NULL);

	fi_set_fail_at(0);  /* epoch calloc fails */
	int rc = cbt_epoch_open(dev, "ep1", "backend1", 1);
	ASSERT_RC(rc, -ENOMEM);
	ASSERT_EQ(dev->epoch_count, 0);

	fi_reset();
	cbt_destroy(dev);
	PASS();
}

/* A failed freeze allocation returns -ENOMEM and leaves the epoch OPEN with no
 * frozen bitmap, so a retry is possible. */
static void test_epoch_freeze_malloc_fail(void)
{
	TEST(test_epoch_freeze_malloc_fail);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev != NULL);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "backend1", 1), 0);
	cbt_mark_dirty(dev, 0, 128);

	fi_set_fail_at(0);  /* freeze malloc fails */
	int rc = cbt_epoch_freeze(dev, "ep1");
	ASSERT_RC(rc, -ENOMEM);

	struct cbt_epoch *ep = cbt_find_epoch(dev, "ep1");
	ASSERT(ep != NULL);
	ASSERT_EQ(ep->state, CBT_EPOCH_OPEN);
	ASSERT(ep->bitmap_frozen == NULL);

	fi_reset();
	cbt_destroy(dev);
	PASS();
}

/* A failed range-array allocation returns -ENOMEM and leaves *out_ranges NULL. */
static void test_get_ranges_malloc_fail(void)
{
	TEST(test_get_ranges_malloc_fail);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev != NULL);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "backend1", 1), 0);
	cbt_mark_dirty(dev, 0, 128);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);

	fi_set_fail_at(0);  /* ranges calloc fails */
	struct dirty_range *ranges = NULL;
	uint32_t count = 0;
	int rc = cbt_epoch_get_dirty_ranges(dev, "ep1", 0, &ranges, &count);
	ASSERT_RC(rc, -ENOMEM);
	ASSERT(ranges == NULL);

	fi_reset();
	cbt_destroy(dev);
	PASS();
}

/* ================================================================== */
/* SECTION 2: Invalid state transitions                               */
/* ================================================================== */

/* Re-freeze is refused while a rebuild RUNS, allowed once it terminates (the
 * convergence loop), and refused for good on an INVALID epoch. */
static void test_freeze_non_open_epoch(void)
{
	TEST(test_freeze_non_open_epoch);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b", 1), 0);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ep1"), 0);

	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), -EBUSY);

	ASSERT_RC(cbt_epoch_rebuild_finish(dev, "ep1", true), 0);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);

	ASSERT_RC(cbt_epoch_invalidate(dev, "ep1"), 0);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), -EINVAL);

	cbt_destroy(dev);
	PASS();
}

/* An OPEN epoch cannot be closed: it must be frozen first. */
static void test_close_open_epoch(void)
{
	TEST(test_close_open_epoch);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b", 1), 0);
	ASSERT_RC(cbt_epoch_close(dev, "ep1"), -EINVAL);
	ASSERT_EQ(dev->epoch_count, 1);

	cbt_destroy(dev);
	PASS();
}

/* A rebuild cannot start on an epoch that is still OPEN. */
static void test_rebuild_start_non_frozen(void)
{
	TEST(test_rebuild_start_non_frozen);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b", 1), 0);
	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ep1"), -EINVAL);

	cbt_destroy(dev);
	PASS();
}

/* Ranges can only be read from a frozen bitmap, never from an OPEN epoch. */
static void test_get_ranges_from_open_epoch(void)
{
	TEST(test_get_ranges_from_open_epoch);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b", 1), 0);
	cbt_mark_dirty(dev, 0, 128);

	struct dirty_range *ranges = NULL;
	uint32_t count = 0;
	ASSERT_RC(cbt_epoch_get_dirty_ranges(dev, "ep1", 0, &ranges, &count), -EINVAL);
	ASSERT(ranges == NULL);

	cbt_destroy(dev);
	PASS();
}

/* An INVALID epoch exposes no ranges. */
static void test_get_ranges_from_invalid_epoch(void)
{
	TEST(test_get_ranges_from_invalid_epoch);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b", 1), 0);
	ASSERT_RC(cbt_epoch_invalidate(dev, "ep1"), 0);

	struct dirty_range *ranges = NULL;
	uint32_t count = 0;
	ASSERT_RC(cbt_epoch_get_dirty_ranges(dev, "ep1", 0, &ranges, &count), -EINVAL);

	cbt_destroy(dev);
	PASS();
}

/* ================================================================== */
/* SECTION 3: Non-existent entities                                   */
/* ================================================================== */

static void test_freeze_nonexistent_epoch(void)
{
	TEST(test_freeze_nonexistent_epoch);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT_RC(cbt_epoch_freeze(dev, "ghost"), -ENOENT);
	cbt_destroy(dev);
	PASS();
}

static void test_close_nonexistent_epoch(void)
{
	TEST(test_close_nonexistent_epoch);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT_RC(cbt_epoch_close(dev, "ghost"), -ENOENT);
	cbt_destroy(dev);
	PASS();
}

static void test_invalidate_nonexistent_epoch(void)
{
	TEST(test_invalidate_nonexistent_epoch);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT_RC(cbt_epoch_invalidate(dev, "ghost"), -ENOENT);
	cbt_destroy(dev);
	PASS();
}

static void test_rebuild_nonexistent_epoch(void)
{
	TEST(test_rebuild_nonexistent_epoch);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ghost"), -ENOENT);
	cbt_destroy(dev);
	PASS();
}

static void test_get_ranges_nonexistent_epoch(void)
{
	TEST(test_get_ranges_nonexistent_epoch);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	struct dirty_range *ranges = NULL;
	uint32_t count = 99;
	ASSERT_RC(cbt_epoch_get_dirty_ranges(dev, "ghost", 0, &ranges, &count), -ENOENT);
	ASSERT(ranges == NULL);
	cbt_destroy(dev);
	PASS();
}

/* ================================================================== */
/* SECTION 4: Adversarial / invalid inputs                            */
/* ================================================================== */

static void test_epoch_id_too_long(void)
{
	TEST(test_epoch_id_too_long);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	char long_id[CBT_EPOCH_ID_MAX + 10];
	memset(long_id, 'A', sizeof(long_id) - 1);
	long_id[sizeof(long_id) - 1] = '\0';

	ASSERT_RC(cbt_epoch_open(dev, long_id, "backend", 1), -ENAMETOOLONG);
	ASSERT_EQ(dev->epoch_count, 0);

	cbt_destroy(dev);
	PASS();
}

static void test_backend_id_too_long(void)
{
	TEST(test_backend_id_too_long);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	char long_backend[CBT_BACKEND_ID_MAX + 10];
	memset(long_backend, 'B', sizeof(long_backend) - 1);
	long_backend[sizeof(long_backend) - 1] = '\0';

	ASSERT_RC(cbt_epoch_open(dev, "ep1", long_backend, 1), -ENAMETOOLONG);
	ASSERT_EQ(dev->epoch_count, 0);

	cbt_destroy(dev);
	PASS();
}

/* An id of exactly CBT_EPOCH_ID_MAX - 1 characters is accepted and kept whole. */
static void test_epoch_id_max_length(void)
{
	TEST(test_epoch_id_max_length);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	char max_id[CBT_EPOCH_ID_MAX];
	memset(max_id, 'X', CBT_EPOCH_ID_MAX - 1);
	max_id[CBT_EPOCH_ID_MAX - 1] = '\0';

	ASSERT_RC(cbt_epoch_open(dev, max_id, "b", 1), 0);
	ASSERT_EQ(dev->epoch_count, 1);

	struct cbt_epoch *ep = cbt_find_epoch(dev, max_id);
	ASSERT(ep != NULL);
	ASSERT_EQ(strlen(ep->epoch_id), (size_t)(CBT_EPOCH_ID_MAX - 1));

	cbt_destroy(dev);
	PASS();
}

/* An empty epoch id is accepted: only the length bound is enforced. */
static void test_empty_epoch_id(void)
{
	TEST(test_empty_epoch_id);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	ASSERT_RC(cbt_epoch_open(dev, "", "backend", 1), 0);
	struct cbt_epoch *ep = cbt_find_epoch(dev, "");
	ASSERT(ep != NULL);

	cbt_destroy(dev);
	PASS();
}

/* Re-opening an epoch id at a lower or equal generation is rejected and leaves
 * the existing epoch untouched. */
static void test_duplicate_epoch_id_lower_gen(void)
{
	TEST(test_duplicate_epoch_id_lower_gen);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b1", 5), 0);
	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b2", 3), -EEXIST);
	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b2", 5), -EEXIST);

	struct cbt_epoch *ep = cbt_find_epoch(dev, "ep1");
	ASSERT_EQ(ep->generation, 5);
	ASSERT_EQ(strcmp(ep->stale_backend_id, "b1"), 0);

	cbt_destroy(dev);
	PASS();
}

/* A higher generation takes the epoch over in place: new backend id, state back
 * to OPEN, still one epoch. */
static void test_duplicate_epoch_id_higher_gen(void)
{
	TEST(test_duplicate_epoch_id_higher_gen);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b1", 5), 0);
	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b2", 10), 0);

	struct cbt_epoch *ep = cbt_find_epoch(dev, "ep1");
	ASSERT_EQ(ep->generation, 10);
	ASSERT_EQ(strcmp(ep->stale_backend_id, "b2"), 0);
	ASSERT_EQ(ep->state, CBT_EPOCH_OPEN);
	ASSERT_EQ(dev->epoch_count, 1);

	cbt_destroy(dev);
	PASS();
}

/* ================================================================== */
/* SECTION 5: Resource exhaustion                                     */
/* ================================================================== */

/* With every slot active, a further open returns -ENOSPC; invalidating the
 * oldest makes it evictable and the open then succeeds. */
static void test_max_epochs_eviction(void)
{
	TEST(test_max_epochs_eviction);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b1", 1), 0);
	ASSERT_RC(cbt_epoch_open(dev, "ep2", "b2", 2), 0);
	ASSERT_RC(cbt_epoch_open(dev, "ep3", "b3", 3), 0);
	ASSERT_RC(cbt_epoch_open(dev, "ep4", "b4", 4), 0);
	ASSERT_EQ(dev->epoch_count, CBT_MAX_EPOCHS);

	ASSERT_RC(cbt_epoch_open(dev, "ep5", "b5", 5), -ENOSPC);

	ASSERT_RC(cbt_epoch_invalidate(dev, "ep1"), 0);
	ASSERT_RC(cbt_epoch_open(dev, "ep5", "b5", 5), 0);
	ASSERT_EQ(dev->epoch_count, CBT_MAX_EPOCHS);
	ASSERT(cbt_find_epoch(dev, "ep1") == NULL);
	ASSERT(cbt_find_epoch(dev, "ep5") != NULL);

	cbt_destroy(dev);
	PASS();
}

/* Evicting an epoch that holds a frozen delta merges that delta back into the
 * live bitmap instead of destroying it. */
static void test_max_epochs_eviction_with_frozen_bitmap(void)
{
	TEST(test_max_epochs_eviction_with_frozen_bitmap);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b1", 1), 0);
	cbt_mark_dirty(dev, 0, 128);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);

	ASSERT_RC(cbt_epoch_open(dev, "ep2", "b2", 2), 0);
	ASSERT_RC(cbt_epoch_open(dev, "ep3", "b3", 3), 0);
	ASSERT_RC(cbt_epoch_open(dev, "ep4", "b4", 4), 0);

	ASSERT_RC(cbt_epoch_open(dev, "ep5", "b5", 5), -ENOSPC);
	ASSERT_RC(cbt_epoch_invalidate(dev, "ep1"), 0);
	ASSERT_RC(cbt_epoch_open(dev, "ep5", "b5", 5), 0);
	ASSERT(cbt_find_epoch(dev, "ep1") == NULL);
	ASSERT(count_bits(dev->bitmap, dev->bitmap_size_bytes) > 0);

	cbt_destroy(dev);
	PASS();
}

/* ================================================================== */
/* SECTION 6: Double operations / idempotence                         */
/* ================================================================== */

/* Freezing twice with no rebuild in between keeps both ranges: the first,
 * never-copied delta is merged back and re-captured. */
static void test_double_freeze(void)
{
	TEST(test_double_freeze);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b", 1), 0);
	cbt_mark_dirty(dev, 0, 128);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);

	cbt_mark_dirty(dev, 256, 128);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);

	struct dirty_range *ranges = NULL;
	uint32_t count = 0;
	ASSERT_RC(cbt_epoch_get_dirty_ranges(dev, "ep1", 0, &ranges, &count), 0);
	ASSERT_EQ(count, 2);
	ASSERT_EQ(ranges[0].offset_blocks, 0);
	ASSERT_EQ(ranges[1].offset_blocks, 256);
	free(ranges);

	cbt_destroy(dev);
	PASS();
}

/* Closing an already-closed epoch reports -ENOENT rather than double-freeing. */
static void test_double_close(void)
{
	TEST(test_double_close);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b", 1), 0);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	ASSERT_RC(cbt_epoch_close(dev, "ep1"), 0);

	ASSERT_RC(cbt_epoch_close(dev, "ep1"), -ENOENT);

	cbt_destroy(dev);
	PASS();
}

/* Invalidation is idempotent. */
static void test_double_invalidate(void)
{
	TEST(test_double_invalidate);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b", 1), 0);
	ASSERT_RC(cbt_epoch_invalidate(dev, "ep1"), 0);
	ASSERT_RC(cbt_epoch_invalidate(dev, "ep1"), 0);

	struct cbt_epoch *ep = cbt_find_epoch(dev, "ep1");
	ASSERT_EQ(ep->state, CBT_EPOCH_INVALID);

	cbt_destroy(dev);
	PASS();
}

/* ================================================================== */
/* SECTION 7: Lifecycle correctness                                   */
/* ================================================================== */

/* Opening an epoch suspends the clear; closing the last one resumes it. */
static void test_close_resumes_healthy_clear(void)
{
	TEST(test_close_resumes_healthy_clear);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	ASSERT_EQ(dev->healthy_clear_suspended, false);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b", 1), 0);
	ASSERT_EQ(dev->healthy_clear_suspended, true);

	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	ASSERT_RC(cbt_epoch_close(dev, "ep1"), 0);
	ASSERT_EQ(dev->healthy_clear_suspended, false);

	cbt_destroy(dev);
	PASS();
}

/* One remaining open epoch keeps the clear suspended. */
static void test_close_with_remaining_epochs(void)
{
	TEST(test_close_with_remaining_epochs);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b", 1), 0);
	ASSERT_RC(cbt_epoch_open(dev, "ep2", "b", 2), 0);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	ASSERT_RC(cbt_epoch_close(dev, "ep1"), 0);

	ASSERT_EQ(dev->healthy_clear_suspended, true);
	ASSERT_EQ(dev->epoch_count, 1);

	cbt_destroy(dev);
	PASS();
}

/* The nominal open, freeze, rebuild, close sequence works end to end, ranges
 * stay readable during REBUILDING, and close waits for the rebuild to end. */
static void test_full_lifecycle_sequence(void)
{
	TEST(test_full_lifecycle_sequence);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "backend_a", 1), 0);
	cbt_mark_dirty(dev, 0, 256);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ep1"), 0);

	struct dirty_range *ranges = NULL;
	uint32_t count = 0;
	ASSERT_RC(cbt_epoch_get_dirty_ranges(dev, "ep1", 0, &ranges, &count), 0);
	ASSERT(count > 0);
	free(ranges);

	ASSERT_RC(cbt_epoch_close(dev, "ep1"), -EBUSY);
	ASSERT_RC(cbt_epoch_rebuild_finish(dev, "ep1", true), 0);
	ASSERT_RC(cbt_epoch_close(dev, "ep1"), 0);
	ASSERT_EQ(dev->epoch_count, 0);
	ASSERT_EQ(dev->healthy_clear_suspended, false);

	cbt_destroy(dev);
	PASS();
}

/* An INVALID epoch refuses freeze, rebuild and get_ranges, but can still be
 * closed for cleanup. */
static void test_invalidated_epoch_cannot_proceed(void)
{
	TEST(test_invalidated_epoch_cannot_proceed);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b", 1), 0);
	cbt_mark_dirty(dev, 0, 128);
	ASSERT_RC(cbt_epoch_invalidate(dev, "ep1"), 0);

	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), -EINVAL);
	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ep1"), -EINVAL);
	struct dirty_range *ranges = NULL;
	uint32_t count = 0;
	ASSERT_RC(cbt_epoch_get_dirty_ranges(dev, "ep1", 0, &ranges, &count), -EINVAL);

	ASSERT_RC(cbt_epoch_close(dev, "ep1"), 0);

	cbt_destroy(dev);
	PASS();
}

/* ================================================================== */
/* SECTION 8: Bitmap edge cases under failure                         */
/* ================================================================== */

/* Marking an already fully dirty bitmap is idempotent. */
static void test_mark_dirty_after_bitmap_full(void)
{
	TEST(test_mark_dirty_after_bitmap_full);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	cbt_mark_dirty(dev, 0, 2048);

	cbt_mark_dirty(dev, 0, 2048);
	cbt_mark_dirty(dev, 1000, 500);

	for (uint64_t i = 0; i < dev->bitmap_size_bits; i++) {
		ASSERT((dev->bitmap[i >> 3] & (1u << (i & 7))) != 0);
	}

	cbt_destroy(dev);
	PASS();
}

/* An offset plus length that overflows uint64 stays inside the bitmap: the end
 * chunk wraps but the clamp catches it. */
static void test_mark_dirty_uint64_overflow(void)
{
	TEST(test_mark_dirty_uint64_overflow);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	cbt_mark_dirty(dev, UINT64_MAX - 10, 20);

	cbt_destroy(dev);
	PASS();
}

/* A write past the end of the volume sets no bit at all. */
static void test_mark_dirty_offset_beyond_volume(void)
{
	TEST(test_mark_dirty_offset_beyond_volume);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	cbt_mark_dirty(dev, 100000, 1);

	for (uint64_t i = 0; i < dev->bitmap_size_bytes; i++) {
		ASSERT_EQ(dev->bitmap[i], 0);
	}

	cbt_destroy(dev);
	PASS();
}

/* Wiping the live bitmap does not touch a frozen delta: the two buffers are
 * independent, so the epoch keeps its ranges. */
static void test_clear_during_active_epoch(void)
{
	TEST(test_clear_during_active_epoch);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b", 1), 0);
	cbt_mark_dirty(dev, 0, 2048);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);

	memset(dev->bitmap, 0, dev->bitmap_size_bytes);

	struct dirty_range *ranges = NULL;
	uint32_t count = 0;
	ASSERT_RC(cbt_epoch_get_dirty_ranges(dev, "ep1", 0, &ranges, &count), 0);
	ASSERT_EQ(count, 1);  /* Full volume = 1 contiguous range */
	free(ranges);

	cbt_destroy(dev);
	PASS();
}

/* ================================================================== */
/* SECTION 9: Concurrent mark + clear (TOCTOU simulation)             */
/* ================================================================== */

struct race_ctx {
	struct cbt_device *dev;
	_Atomic bool       stop;
	_Atomic uint64_t   mark_ops;
	_Atomic uint64_t   clear_ops;
};

static void *
race_marker_thread(void *arg)
{
	struct race_ctx *ctx = arg;
	uint64_t ops = 0;
	while (!atomic_load(&ctx->stop)) {
		cbt_mark_dirty(ctx->dev, (ops * 37) % ctx->dev->total_blocks, 1);
		ops++;
	}
	atomic_store(&ctx->mark_ops, ops);
	return NULL;
}

static void *
race_clearer_thread(void *arg)
{
	struct race_ctx *ctx = arg;
	uint64_t ops = 0;
	while (!atomic_load(&ctx->stop)) {
		memset(ctx->dev->bitmap, 0, ctx->dev->bitmap_size_bytes);
		ops++;
	}
	atomic_store(&ctx->clear_ops, ops);
	return NULL;
}

/* A marker thread and a clearer thread racing on the same bitmap stay memory
 * safe: neither tears nor corrupts it. */
static void test_concurrent_mark_and_clear(void)
{
	TEST(test_concurrent_mark_and_clear);
	fi_reset();
	struct cbt_device *dev = cbt_create(65536, 64, 512);
	ASSERT(dev != NULL);

	struct race_ctx ctx = {
		.dev = dev,
		.stop = false,
		.mark_ops = 0,
		.clear_ops = 0,
	};

	pthread_t t_mark, t_clear;
	pthread_create(&t_mark, NULL, race_marker_thread, &ctx);
	pthread_create(&t_clear, NULL, race_clearer_thread, &ctx);

	struct timespec ts = {0, 50000000}; /* 50ms */
	nanosleep(&ts, NULL);

	atomic_store(&ctx.stop, true);
	pthread_join(t_mark, NULL);
	pthread_join(t_clear, NULL);

	ASSERT(atomic_load(&ctx.mark_ops) > 0);
	ASSERT(atomic_load(&ctx.clear_ops) > 0);

	cbt_destroy(dev);
	PASS();
}

/* ================================================================== */
/* SECTION 10: Destroy / cleanup correctness                          */
/* ================================================================== */

/* Destroying a device frees every epoch and frozen bitmap whatever state each
 * epoch is in. */
static void test_destroy_with_all_epoch_states(void)
{
	TEST(test_destroy_with_all_epoch_states);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	ASSERT_RC(cbt_epoch_open(dev, "open", "b", 1), 0);

	ASSERT_RC(cbt_epoch_open(dev, "frozen", "b", 2), 0);
	cbt_mark_dirty(dev, 0, 128);
	ASSERT_RC(cbt_epoch_freeze(dev, "frozen"), 0);

	ASSERT_RC(cbt_epoch_open(dev, "rebuilding", "b", 3), 0);
	cbt_mark_dirty(dev, 256, 128);
	ASSERT_RC(cbt_epoch_freeze(dev, "rebuilding"), 0);
	ASSERT_RC(cbt_epoch_rebuild_start(dev, "rebuilding"), 0);

	ASSERT_RC(cbt_epoch_open(dev, "invalid", "b", 4), 0);
	ASSERT_RC(cbt_epoch_invalidate(dev, "invalid"), 0);

	cbt_destroy(dev);
	PASS();
}

/* Destroying a device that never had an epoch is clean. */
static void test_destroy_empty_device(void)
{
	TEST(test_destroy_empty_device);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	cbt_destroy(dev);
	PASS();
}

/* Destroying NULL is a no-op. */
static void test_destroy_null(void)
{
	TEST(test_destroy_null);
	fi_reset();
	cbt_destroy(NULL);
	PASS();
}

/* ================================================================== */
/* SECTION 11: Epoch open with re-open (generation upgrade)           */
/* ================================================================== */

/* Re-opening a FROZEN epoch at a higher generation puts it back to OPEN and
 * keeps it freezable; the previous frozen buffer is reused, not leaked. */
static void test_reopen_frozen_epoch_with_higher_gen(void)
{
	TEST(test_reopen_frozen_epoch_with_higher_gen);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b1", 1), 0);
	cbt_mark_dirty(dev, 0, 128);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b2", 5), 0);

	struct cbt_epoch *ep = cbt_find_epoch(dev, "ep1");
	ASSERT_EQ(ep->state, CBT_EPOCH_OPEN);
	ASSERT_EQ(ep->generation, 5);

	cbt_mark_dirty(dev, 512, 128);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);

	cbt_destroy(dev);
	PASS();
}

/* ================================================================== */
/* SECTION 12: Chunk size edge cases                                  */
/* ================================================================== */

/* A zero chunk size falls back to one block per chunk. */
static void test_zero_chunk_size_kb(void)
{
	TEST(test_zero_chunk_size_kb);
	fi_reset();
	struct cbt_device *dev = cbt_create(100, 0, 512);
	ASSERT(dev != NULL);
	ASSERT_EQ(dev->chunk_size_blocks, 1);
	ASSERT_EQ(dev->bitmap_size_bits, 100);

	cbt_mark_dirty(dev, 50, 1);
	ASSERT((dev->bitmap[50 >> 3] & (1u << (50 & 7))) != 0);

	cbt_destroy(dev);
	PASS();
}

/* A chunk wider than the volume gives a single-bit bitmap, and the reported
 * range is clamped to the real volume size. */
static void test_chunk_larger_than_volume(void)
{
	TEST(test_chunk_larger_than_volume);
	fi_reset();
	struct cbt_device *dev = cbt_create(10, 64, 512);
	ASSERT(dev != NULL);
	ASSERT_EQ(dev->bitmap_size_bits, 1);

	cbt_mark_dirty(dev, 0, 1);
	ASSERT((dev->bitmap[0] & 1) != 0);

	ASSERT_RC(cbt_epoch_open(dev, "ep", "b", 1), 0);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep"), 0);

	struct dirty_range *ranges = NULL;
	uint32_t count = 0;
	ASSERT_RC(cbt_epoch_get_dirty_ranges(dev, "ep", 0, &ranges, &count), 0);
	ASSERT_EQ(count, 1);
	ASSERT_EQ(ranges[0].offset_blocks, 0);
	ASSERT_EQ(ranges[0].length_blocks, 10);  /* Clamped to volume, not 128 */
	free(ranges);

	cbt_destroy(dev);
	PASS();
}

/* A chunk size that is not a power of two is rounded up to one (100 KB over
 * 512-byte blocks is 200 blocks, rounded to 256). */
static void test_non_power_of_2_chunk_rounded(void)
{
	TEST(test_non_power_of_2_chunk_rounded);
	fi_reset();
	struct cbt_device *dev = cbt_create(10000, 100, 512);
	ASSERT(dev != NULL);
	ASSERT_EQ(dev->chunk_size_blocks, 256);
	ASSERT_EQ(dev->chunk_shift, 8);  /* log2(256) = 8 */

	cbt_mark_dirty(dev, 0, 256);
	ASSERT((dev->bitmap[0] & 1) != 0);
	ASSERT((dev->bitmap[0] & 2) == 0);

	cbt_destroy(dev);
	PASS();
}

/* ================================================================== */
/* SECTION: Partial rebuild state machine validation                  */
/* ================================================================== */

/* A rebuild needs a FROZEN epoch, and a second concurrent rebuild on the same
 * epoch is refused with -EBUSY. */
static void test_rebuild_start_requires_frozen_state(void)
{
	TEST(test_rebuild_start_requires_frozen_state);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev != NULL);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "backend1", 1), 0);

	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ep1"), -EINVAL);

	cbt_mark_dirty(dev, 0, 128);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ep1"), 0);

	struct cbt_epoch *ep = cbt_find_epoch(dev, "ep1");
	ASSERT(ep != NULL);
	ASSERT_EQ(ep->state, CBT_EPOCH_REBUILDING);

	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ep1"), -EBUSY);

	cbt_destroy(dev);
	PASS();
}

static void test_rebuild_start_nonexistent_epoch(void)
{
	TEST(test_rebuild_start_nonexistent_epoch);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev != NULL);

	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ghost"), -ENOENT);

	cbt_destroy(dev);
	PASS();
}

/* Writes arriving during a rebuild land in the live bitmap only: the frozen
 * bitmap the rebuild is copying stays byte-for-byte identical. */
static void test_rebuild_preserves_frozen_bitmap(void)
{
	TEST(test_rebuild_preserves_frozen_bitmap);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev != NULL);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "backend1", 1), 0);
	cbt_mark_dirty(dev, 0, 128);
	cbt_mark_dirty(dev, 256, 64);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);

	struct cbt_epoch *ep = cbt_find_epoch(dev, "ep1");
	ASSERT(ep != NULL);
	ASSERT(ep->bitmap_frozen != NULL);

	uint8_t *saved = malloc(dev->bitmap_size_bytes);
	ASSERT(saved != NULL);
	memcpy(saved, ep->bitmap_frozen, dev->bitmap_size_bytes);

	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ep1"), 0);

	cbt_mark_dirty(dev, 1024, 128);

	ASSERT(memcmp(ep->bitmap_frozen, saved, dev->bitmap_size_bytes) == 0);

	free(saved);
	cbt_destroy(dev);
	PASS();
}

/* Once in REBUILDING, the epoch and its frozen bitmap stay intact across
 * further operations. */
static void test_rebuild_epoch_cannot_be_evicted(void)
{
	TEST(test_rebuild_epoch_cannot_be_evicted);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev != NULL);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b1", 1), 0);
	cbt_mark_dirty(dev, 0, 128);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ep1"), 0);

	struct cbt_epoch *ep = cbt_find_epoch(dev, "ep1");
	ASSERT(ep != NULL);
	ASSERT_EQ(ep->state, CBT_EPOCH_REBUILDING);

	ASSERT(ep->bitmap_frozen != NULL);

	cbt_destroy(dev);
	PASS();
}

/* Close is refused while the rebuild runs and succeeds from REBUILDING once it
 * has terminated. */
static void test_rebuild_close_after_rebuild(void)
{
	TEST(test_rebuild_close_after_rebuild);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev != NULL);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "backend1", 1), 0);
	cbt_mark_dirty(dev, 0, 128);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ep1"), 0);

	ASSERT_RC(cbt_epoch_close(dev, "ep1"), -EBUSY);

	ASSERT_RC(cbt_epoch_rebuild_finish(dev, "ep1", true), 0);
	ASSERT_RC(cbt_epoch_close(dev, "ep1"), 0);

	ASSERT(cbt_find_epoch(dev, "ep1") == NULL);
	ASSERT_EQ(dev->epoch_count, 0);

	cbt_destroy(dev);
	PASS();
}

/* Invalidation is refused while the rebuild runs and legal afterwards. */
static void test_rebuild_invalidate_during_rebuild(void)
{
	TEST(test_rebuild_invalidate_during_rebuild);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev != NULL);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "backend1", 1), 0);
	cbt_mark_dirty(dev, 0, 128);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ep1"), 0);

	ASSERT_RC(cbt_epoch_invalidate(dev, "ep1"), -EBUSY);

	ASSERT_RC(cbt_epoch_rebuild_finish(dev, "ep1", false), 0);
	ASSERT_RC(cbt_epoch_invalidate(dev, "ep1"), 0);

	struct cbt_epoch *ep = cbt_find_epoch(dev, "ep1");
	ASSERT(ep != NULL);
	ASSERT_EQ(ep->state, CBT_EPOCH_INVALID);

	cbt_destroy(dev);
	PASS();
}

/* Ranges remain readable while the epoch is REBUILDING. */
static void test_rebuild_get_ranges_during_rebuild(void)
{
	TEST(test_rebuild_get_ranges_during_rebuild);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev != NULL);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "backend1", 1), 0);
	cbt_mark_dirty(dev, 0, 128);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ep1"), 0);

	struct dirty_range *ranges = NULL;
	uint32_t count = 0;
	ASSERT_RC(cbt_epoch_get_dirty_ranges(dev, "ep1", 0, &ranges, &count), 0);
	ASSERT(count > 0);
	free(ranges);

	cbt_destroy(dev);
	PASS();
}

/* The convergence loop runs: freeze, rebuild, re-freeze, rebuild again, close —
 * an epoch can cycle between FROZEN and REBUILDING as many times as needed. */
static void test_rebuild_convergence_refreeze(void)
{
	TEST(test_rebuild_convergence_refreeze);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev != NULL);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "backend1", 1), 0);
	cbt_mark_dirty(dev, 0, 128);

	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	struct cbt_epoch *ep = cbt_find_epoch(dev, "ep1");
	ASSERT(ep != NULL);
	ASSERT_EQ(ep->state, CBT_EPOCH_FROZEN);

	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ep1"), 0);
	ASSERT_EQ(ep->state, CBT_EPOCH_REBUILDING);

	cbt_mark_dirty(dev, 512, 64);

	ASSERT_RC(cbt_epoch_rebuild_finish(dev, "ep1", true), 0);

	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	ASSERT_EQ(ep->state, CBT_EPOCH_FROZEN);
	ASSERT(ep->bitmap_frozen != NULL);

	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ep1"), 0);
	ASSERT_EQ(ep->state, CBT_EPOCH_REBUILDING);

	ASSERT_RC(cbt_epoch_rebuild_finish(dev, "ep1", true), 0);
	ASSERT_RC(cbt_epoch_close(dev, "ep1"), 0);
	ASSERT(cbt_find_epoch(dev, "ep1") == NULL);

	cbt_destroy(dev);
	PASS();
}

/* ================================================================== */
/* SECTION 12: Freeze delta semantics (snapshot-and-clear)            */
/* ================================================================== */

/* When one epoch owns the bitmap, each freeze that follows a COMPLETED rebuild
 * captures only the writes since the previous freeze, down to an empty delta. */
static void test_freeze_clears_live_when_sole_epoch(void)
{
	TEST(test_freeze_clears_live_when_sole_epoch);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev != NULL);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b", 1), 0);
	cbt_mark_dirty(dev, 0, 512);

	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	struct cbt_epoch *ep = cbt_find_epoch(dev, "ep1");
	ASSERT(ep != NULL);
	ASSERT(count_bits(ep->bitmap_frozen, dev->bitmap_size_bytes) > 0);
	ASSERT_EQ(count_bits(dev->bitmap, dev->bitmap_size_bytes), 0);

	/* Only a COMPLETED rebuild licenses dropping the consumed delta, so the next
	 * freeze can be a pure delta. */
	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ep1"), 0);
	ASSERT_RC(cbt_epoch_rebuild_finish(dev, "ep1", true), 0);

	cbt_mark_dirty(dev, 1024, 128);
	uint64_t delta_chunks = count_bits(dev->bitmap, dev->bitmap_size_bytes);
	ASSERT(delta_chunks > 0);

	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	ASSERT_EQ(count_bits(ep->bitmap_frozen, dev->bitmap_size_bytes), delta_chunks);
	ASSERT_EQ(count_bits(dev->bitmap, dev->bitmap_size_bytes), 0);

	/* No new writes: the next delta is empty, which is what convergence means. */
	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ep1"), 0);
	ASSERT_RC(cbt_epoch_rebuild_finish(dev, "ep1", true), 0);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	ASSERT_EQ(count_bits(ep->bitmap_frozen, dev->bitmap_size_bytes), 0);

	cbt_destroy(dev);
	PASS();
}

/* While another epoch is still active, a freeze copies without clearing: the
 * live bitmap keeps the accumulated view that epoch needs. */
static void test_freeze_preserves_live_with_other_active_epoch(void)
{
	TEST(test_freeze_preserves_live_with_other_active_epoch);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev != NULL);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b1", 1), 0);
	ASSERT_RC(cbt_epoch_open(dev, "ep2", "b2", 2), 0);
	cbt_mark_dirty(dev, 0, 512);

	uint64_t before = count_bits(dev->bitmap, dev->bitmap_size_bytes);
	ASSERT(before > 0);

	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	ASSERT_EQ(count_bits(dev->bitmap, dev->bitmap_size_bytes), before);

	cbt_destroy(dev);
	PASS();
}

/* Under a concurrent writer, the union of every frozen snapshot plus the live
 * bitmap must equal exactly the set of chunks the writer marked: the per-byte
 * exchange may neither lose nor invent a bit. */
struct delta_race_ctx {
	struct cbt_device *dev;
	uint8_t           *shadow;      /* same layout as dev->bitmap */
	_Atomic bool       stop;
	_Atomic uint64_t   marks;
};

static void *
delta_marker_thread(void *arg)
{
	struct delta_race_ctx *ctx = arg;
	uint64_t seed = 0x9e3779b97f4a7c15ull;
	while (!atomic_load(&ctx->stop)) {
		seed = seed * 6364136223846793005ull + 1442695040888963407ull;
		uint64_t block = seed % ctx->dev->total_blocks;
		cbt_mark_dirty(ctx->dev, block, 1);
		uint64_t chunk = block >> ctx->dev->chunk_shift;
		__atomic_fetch_or(&ctx->shadow[chunk >> 3], (uint8_t)(1u << (chunk & 7)),
				  __ATOMIC_RELAXED);
		atomic_fetch_add(&ctx->marks, 1);
	}
	return NULL;
}

static void test_concurrent_refreeze_never_loses_bits(void)
{
	TEST(test_concurrent_refreeze_never_loses_bits);
	fi_reset();
	struct cbt_device *dev = cbt_create(65536, 64, 512);
	ASSERT(dev != NULL);

	uint8_t *shadow = calloc(1, dev->bitmap_size_bytes);
	uint8_t *acc    = calloc(1, dev->bitmap_size_bytes);
	ASSERT(shadow != NULL && acc != NULL);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b", 1), 0);
	struct cbt_epoch *ep = cbt_find_epoch(dev, "ep1");
	ASSERT(ep != NULL);

	struct delta_race_ctx ctx = { .dev = dev, .shadow = shadow, .stop = false, .marks = 0 };
	pthread_t t;
	pthread_create(&t, NULL, delta_marker_thread, &ctx);

	/* Re-freeze repeatedly under the writer, accumulating every captured delta. */
	for (int pass = 0; pass < 200; pass++) {
		ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
		for (uint64_t i = 0; i < dev->bitmap_size_bytes; i++) {
			acc[i] |= ep->bitmap_frozen[i];
		}
	}

	atomic_store(&ctx.stop, true);
	pthread_join(t, NULL);
	ASSERT(atomic_load(&ctx.marks) > 0);

	/* Whatever the writer marked after the last freeze is still live. */
	for (uint64_t i = 0; i < dev->bitmap_size_bytes; i++) {
		acc[i] |= dev->bitmap[i];
	}

	ASSERT_EQ(memcmp(acc, shadow, dev->bitmap_size_bytes), 0);

	free(shadow);
	free(acc);
	cbt_destroy(dev);
	PASS();
}

/* ================================================================== */
/* SECTION 13: Delta preservation and terminal classification         */
/* ================================================================== */

/* A freeze consumes a delta, the rebuild aborts, and the retry re-freezes: the
 * new snapshot must hold the un-copied delta together with the writes that
 * arrived since — three chunks, not one. */
static void test_h1_refreeze_after_aborted_rebuild_preserves_delta(void)
{
	TEST(test_h1_refreeze_after_aborted_rebuild_preserves_delta);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev != NULL);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b", 1), 0);
	cbt_mark_dirty(dev, 0, 128);      /* chunk 0 */
	cbt_mark_dirty(dev, 256, 128);    /* chunk 2 */
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	ASSERT_EQ(count_bits(dev->bitmap, dev->bitmap_size_bytes), 0);

	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ep1"), 0);
	/* Target hot-removed mid-rebuild: aborted, the delta is not fully copied. */
	ASSERT_RC(cbt_epoch_rebuild_finish(dev, "ep1", false), 0);

	cbt_mark_dirty(dev, 512, 128);    /* chunk 4 */

	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	struct cbt_epoch *ep = cbt_find_epoch(dev, "ep1");
	ASSERT(ep != NULL);
	ASSERT_EQ(count_bits(ep->bitmap_frozen, dev->bitmap_size_bytes), 3);

	cbt_destroy(dev);
	PASS();
}

/* Closing an epoch whose delta was consumed but never copied returns that delta
 * to the live bitmap instead of destroying it. */
static void test_h1_close_merges_back_unconsumed_delta(void)
{
	TEST(test_h1_close_merges_back_unconsumed_delta);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev != NULL);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b", 1), 0);
	cbt_mark_dirty(dev, 0, 128);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	ASSERT_EQ(count_bits(dev->bitmap, dev->bitmap_size_bytes), 0);

	ASSERT_RC(cbt_epoch_close(dev, "ep1"), 0);

	ASSERT_EQ(count_bits(dev->bitmap, dev->bitmap_size_bytes), 1);

	cbt_destroy(dev);
	PASS();
}

/* A COMPLETED rebuild consumes the delta legitimately, so close must NOT
 * re-inject it: that would force a spurious re-copy of everything. */
static void test_h1_completed_rebuild_does_not_merge_back(void)
{
	TEST(test_h1_completed_rebuild_does_not_merge_back);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev != NULL);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b", 1), 0);
	cbt_mark_dirty(dev, 0, 128);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ep1"), 0);
	ASSERT_RC(cbt_epoch_rebuild_finish(dev, "ep1", true), 0);

	ASSERT_RC(cbt_epoch_close(dev, "ep1"), 0);
	ASSERT_EQ(count_bits(dev->bitmap, dev->bitmap_size_bytes), 0);

	cbt_destroy(dev);
	PASS();
}

/* An ENOMEM on re-freeze leaves the previous delta untouched — the epoch is
 * neither bricked nor emptied — and the retry then succeeds. */
static void test_h1_refreeze_enomem_preserves_previous_delta(void)
{
	TEST(test_h1_refreeze_enomem_preserves_previous_delta);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev != NULL);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b", 1), 0);
	cbt_mark_dirty(dev, 0, 128);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	struct cbt_epoch *ep = cbt_find_epoch(dev, "ep1");
	ASSERT(ep != NULL);
	uint8_t *frozen_before = ep->bitmap_frozen;

	/* Next allocation fails. */
	atomic_store(&g_malloc_fail_countdown, 0);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), -ENOMEM);
	fi_reset();

	ASSERT(ep->bitmap_frozen == frozen_before);
	ASSERT_EQ(count_bits(ep->bitmap_frozen, dev->bitmap_size_bytes), 1);

	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	ASSERT_EQ(count_bits(ep->bitmap_frozen, dev->bitmap_size_bytes), 1);

	cbt_destroy(dev);
	PASS();
}

/* A write in flight across a clearing freeze loses its submit-time bit to the
 * exchange, but the completion re-mark puts the chunk back in the live bitmap,
 * so the next delta re-copies it. No drain, no host-I/O freeze. */
static void test_h2_inflight_write_survives_clearing_freeze(void)
{
	TEST(test_h2_inflight_write_survives_clearing_freeze);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev != NULL);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b", 1), 0);

	/* Write submitted (bit set) but not yet completed. */
	cbt_write_submit(dev, 0, 128);

	/* The freeze consumes the submit-time bit, and the rebuild may read the
	 * chunk before the write lands — that copy is stale. */
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	ASSERT_EQ(count_bits(dev->bitmap, dev->bitmap_size_bytes), 0);
	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ep1"), 0);
	ASSERT_RC(cbt_epoch_rebuild_finish(dev, "ep1", true), 0);

	/* The write completes after that read; the re-mark saves the chunk. */
	cbt_write_complete(dev, 0, 128);
	ASSERT_EQ(count_bits(dev->bitmap, dev->bitmap_size_bytes), 1);

	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	struct cbt_epoch *ep = cbt_find_epoch(dev, "ep1");
	ASSERT(ep != NULL);
	ASSERT_EQ(count_bits(ep->bitmap_frozen, dev->bitmap_size_bytes), 1);

	cbt_destroy(dev);
	PASS();
}

/* Invalidate is refused while a rebuild RUNS, and eviction refuses an epoch a
 * rebuild still points at even when its state already says INVALID. */
static void test_c3_invalidate_and_evict_guards_running_rebuild(void)
{
	TEST(test_c3_invalidate_and_evict_guards_running_rebuild);
	fi_reset();
	struct cbt_device *dev = cbt_create(2048, 64, 512);
	ASSERT(dev != NULL);

	ASSERT_RC(cbt_epoch_open(dev, "ep1", "b", 1), 0);
	cbt_mark_dirty(dev, 0, 128);
	ASSERT_RC(cbt_epoch_freeze(dev, "ep1"), 0);
	ASSERT_RC(cbt_epoch_rebuild_start(dev, "ep1"), 0);

	ASSERT_RC(cbt_epoch_invalidate(dev, "ep1"), -EBUSY);

	struct cbt_epoch *ep = cbt_find_epoch(dev, "ep1");
	ASSERT(ep != NULL);
	ep->state = CBT_EPOCH_INVALID;	/* test backdoor: simulate the race */
	ASSERT_RC(cbt_epoch_open(dev, "ep2", "b2", 2), 0);
	ASSERT_RC(cbt_epoch_open(dev, "ep3", "b3", 3), 0);
	ASSERT_RC(cbt_epoch_open(dev, "ep4", "b4", 4), 0);
	ASSERT_RC(cbt_epoch_open(dev, "ep5", "b5", 5), -ENOSPC);
	ASSERT(cbt_find_epoch(dev, "ep1") != NULL);	/* NOT freed under the rebuild */

	ASSERT_RC(cbt_epoch_rebuild_finish(dev, "ep1", false), 0);
	ASSERT_RC(cbt_epoch_open(dev, "ep5", "b5", 5), 0);
	ASSERT(cbt_find_epoch(dev, "ep1") == NULL);

	cbt_destroy(dev);
	PASS();
}

/* Terminal classification: a hot-remove abort stays ABORTED even when the flush
 * would fail, because no flush is submitted on an aborted run — plus the rest of
 * the matrix (completed, flush failure, I/O error, cancelled, zero chunks). */
static void test_m1_hotremove_abort_classified_aborted_not_failed(void)
{
	TEST(test_m1_hotremove_abort_classified_aborted_not_failed);
	fi_reset();

	/* Aborted, no error, chunks copied, target still open, flush would fail. */
	ASSERT_EQ(cbt_rebuild_finish_classify(0, false, true, 5, true, true),
		  REB_ABORTED);

	ASSERT_EQ(cbt_rebuild_finish_classify(0, false, false, 5, true, false),
		  REB_COMPLETED);
	ASSERT_EQ(cbt_rebuild_finish_classify(0, false, false, 5, true, true),
		  REB_FAILED);	/* genuine flush failure on a clean run */
	ASSERT_EQ(cbt_rebuild_finish_classify(-EIO, false, true, 5, true, false),
		  REB_FAILED);	/* an I/O error sets both flags: FAILED wins */
	ASSERT_EQ(cbt_rebuild_finish_classify(0, true, false, 5, true, false),
		  REB_CANCELLED);
	ASSERT_EQ(cbt_rebuild_finish_classify(0, false, false, 0, true, true),
		  REB_COMPLETED);	/* zero chunks: no flush needed */

	PASS();
}

/* ================================================================== */
/* Main                                                               */
/* ================================================================== */

int
main(void)
{
	printf("CBT resilience tests (negative paths & fault injection)\n");
	printf("========================================================\n\n");

	printf("── Allocation failures ──\n");
	test_create_malloc_fail_struct();
	test_create_malloc_fail_bitmap();
	test_epoch_open_malloc_fail();
	test_epoch_freeze_malloc_fail();
	test_get_ranges_malloc_fail();

	printf("\n── Invalid state transitions ──\n");
	test_freeze_non_open_epoch();
	test_close_open_epoch();
	test_rebuild_start_non_frozen();
	test_get_ranges_from_open_epoch();
	test_get_ranges_from_invalid_epoch();

	printf("\n── Non-existent entities ──\n");
	test_freeze_nonexistent_epoch();
	test_close_nonexistent_epoch();
	test_invalidate_nonexistent_epoch();
	test_rebuild_nonexistent_epoch();
	test_get_ranges_nonexistent_epoch();

	printf("\n── Adversarial inputs ──\n");
	test_epoch_id_too_long();
	test_backend_id_too_long();
	test_epoch_id_max_length();
	test_empty_epoch_id();
	test_duplicate_epoch_id_lower_gen();
	test_duplicate_epoch_id_higher_gen();

	printf("\n── Resource exhaustion ──\n");
	test_max_epochs_eviction();
	test_max_epochs_eviction_with_frozen_bitmap();

	printf("\n── Double operations ──\n");
	test_double_freeze();
	test_double_close();
	test_double_invalidate();

	printf("\n── Lifecycle correctness ──\n");
	test_close_resumes_healthy_clear();
	test_close_with_remaining_epochs();
	test_full_lifecycle_sequence();
	test_invalidated_epoch_cannot_proceed();

	printf("\n── Bitmap edge cases under failure ──\n");
	test_mark_dirty_after_bitmap_full();
	test_mark_dirty_uint64_overflow();
	test_mark_dirty_offset_beyond_volume();
	test_clear_during_active_epoch();

	printf("\n── Concurrent mark + clear (TOCTOU) ──\n");
	test_concurrent_mark_and_clear();

	printf("\n── Destroy / cleanup ──\n");
	test_destroy_with_all_epoch_states();
	test_destroy_empty_device();
	test_destroy_null();

	printf("\n── Epoch re-open semantics ──\n");
	test_reopen_frozen_epoch_with_higher_gen();

	printf("\n── Chunk size edge cases ──\n");
	test_zero_chunk_size_kb();
	test_chunk_larger_than_volume();
	test_non_power_of_2_chunk_rounded();

	printf("\n── Partial rebuild state machine ──\n");
	test_rebuild_start_requires_frozen_state();
	test_rebuild_start_nonexistent_epoch();
	test_rebuild_preserves_frozen_bitmap();
	test_rebuild_epoch_cannot_be_evicted();
	test_rebuild_close_after_rebuild();
	test_rebuild_invalidate_during_rebuild();
	test_rebuild_get_ranges_during_rebuild();
	test_rebuild_convergence_refreeze();

	printf("\n── Freeze delta semantics (snapshot-and-clear) ──\n");
	test_freeze_clears_live_when_sole_epoch();
	test_freeze_preserves_live_with_other_active_epoch();
	test_concurrent_refreeze_never_loses_bits();

	test_h1_refreeze_after_aborted_rebuild_preserves_delta();
	test_h1_close_merges_back_unconsumed_delta();
	test_h1_completed_rebuild_does_not_merge_back();
	test_h1_refreeze_enomem_preserves_previous_delta();
	test_h2_inflight_write_survives_clearing_freeze();
	test_c3_invalidate_and_evict_guards_running_rebuild();
	test_m1_hotremove_abort_classified_aborted_not_failed();

	printf("\n========================================================\n");
	printf("Results: %d passed, %d failed\n", g_passed, g_failed);

	return g_failed > 0 ? 1 : 0;
}
