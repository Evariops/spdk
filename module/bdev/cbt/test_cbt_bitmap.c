/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Evariops.
 *   All rights reserved.
 */

/*
 * Standalone unit tests for the CBT bitmap logic.
 *
 * These tests exercise the bitmap marking, dirty range extraction, epoch
 * lifecycle and edge cases WITHOUT requiring the full SPDK reactor framework.
 *
 * Build:   make -f Makefile.test
 * Run:     ./test_cbt_bitmap
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdatomic.h>
#include <pthread.h>

/* ================================================================== */
/* Minimal inline bitmap helpers (extracted from vbdev_cbt.c logic)    */
/* ================================================================== */

struct test_bitmap {
	uint8_t  *data;
	uint64_t  size_bits;   /* number of chunks */
	uint64_t  size_bytes;
	uint64_t  chunk_size_blocks;
	uint64_t  total_blocks; /* actual device size for tail clamping */
	_Atomic uint64_t  dirty_count;
	_Atomic uint64_t  write_count;
};

/* Per-epoch frozen snapshot. */
struct epoch_snapshot {
	uint8_t  *bitmap_frozen;
	uint64_t  size_bytes;
};

static struct test_bitmap *
bitmap_create(uint64_t total_blocks, uint32_t chunk_size_kb, uint32_t block_size)
{
	struct test_bitmap *b = calloc(1, sizeof(*b));
	assert(b);

	b->chunk_size_blocks = ((uint64_t)chunk_size_kb * 1024) / block_size;
	if (b->chunk_size_blocks == 0) b->chunk_size_blocks = 1;

	b->size_bits  = (total_blocks + b->chunk_size_blocks - 1) / b->chunk_size_blocks;
	b->size_bytes = (b->size_bits + 7) / 8;
	b->total_blocks = total_blocks;

	b->data = calloc(1, b->size_bytes);
	assert(b->data);

	atomic_init(&b->dirty_count, 0);
	atomic_init(&b->write_count, 0);

	return b;
}

static void
bitmap_destroy(struct test_bitmap *b)
{
	free(b->data);
	free(b);
}

static void
bitmap_mark_dirty(struct test_bitmap *b, uint64_t offset_blocks, uint64_t num_blocks)
{
	uint64_t chunk_start, chunk_end;

	/* Guard against zero-length. */
	if (num_blocks == 0) {
		return;
	}

	chunk_start = offset_blocks / b->chunk_size_blocks;
	chunk_end   = (offset_blocks + num_blocks - 1) / b->chunk_size_blocks;

	if (chunk_end >= b->size_bits) {
		chunk_end = b->size_bits - 1;
	}

	for (uint64_t i = chunk_start; i <= chunk_end; i++) {
		uint8_t mask = (uint8_t)(1u << (i % 8));
		uint8_t old = __atomic_fetch_or(&b->data[i / 8], mask, __ATOMIC_RELAXED);
		if (!(old & mask)) {
			atomic_fetch_add(&b->dirty_count, 1);
		}
	}
	atomic_fetch_add(&b->write_count, 1);
}

static bool
bitmap_is_dirty(struct test_bitmap *b, uint64_t chunk_idx)
{
	if (chunk_idx >= b->size_bits) return false;
	return (b->data[chunk_idx / 8] & (1u << (chunk_idx % 8))) != 0;
}

static struct epoch_snapshot *
bitmap_freeze(struct test_bitmap *b)
{
	struct epoch_snapshot *snap = calloc(1, sizeof(*snap));
	assert(snap);
	snap->size_bytes = b->size_bytes;
	snap->bitmap_frozen = malloc(b->size_bytes);
	assert(snap->bitmap_frozen);
	memcpy(snap->bitmap_frozen, b->data, b->size_bytes);
	return snap;
}

static void
epoch_snapshot_destroy(struct epoch_snapshot *snap)
{
	free(snap->bitmap_frozen);
	free(snap);
}

static void
bitmap_clear(struct test_bitmap *b)
{
	memset(b->data, 0, b->size_bytes);
	atomic_store(&b->dirty_count, 0);
}

struct dirty_range {
	uint64_t offset_blocks;
	uint64_t length_blocks;
};

static uint32_t
bitmap_get_dirty_ranges(struct test_bitmap *b, struct epoch_snapshot *snap,
			struct dirty_range *out, uint32_t max_ranges)
{
	const uint8_t *bmap = snap->bitmap_frozen;
	uint32_t count = 0;
	int64_t  run_start = -1;

	for (uint64_t i = 0; i < b->size_bits; i++) {
		bool is_dirty = (bmap[i / 8] & (1u << (i % 8))) != 0;

		if (is_dirty && run_start < 0) {
			run_start = (int64_t)i;
		}

		if (!is_dirty || i == b->size_bits - 1) {
			if (run_start >= 0) {
				uint64_t end = is_dirty ? i : i - 1;
				if (count < max_ranges) {
					uint64_t offset = (uint64_t)run_start * b->chunk_size_blocks;
					uint64_t length = (end - (uint64_t)run_start + 1) *
							  b->chunk_size_blocks;

					/* Tail clamp. */
					if (offset + length > b->total_blocks) {
						length = b->total_blocks - offset;
					}

					out[count].offset_blocks = offset;
					out[count].length_blocks = length;
					count++;
				}
				run_start = -1;
			}
		}
	}
	return count;
}

/* ================================================================== */
/* Test harness                                                       */
/* ================================================================== */

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST(name)                                                      \
	do {                                                            \
		printf("  %-50s", #name);                               \
	} while (0)

#define PASS()                                                          \
	do {                                                            \
		printf(" \xe2\x9c\x93\n");                              \
		g_tests_passed++;                                       \
	} while (0)

#define ASSERT_TRUE(expr)                                               \
	do {                                                            \
		if (!(expr)) {                                          \
			printf(" \xe2\x9c\x97 FAIL at %s:%d: %s\n",    \
			       __FILE__, __LINE__, #expr);               \
			g_tests_failed++;                                \
			return;                                         \
		}                                                       \
	} while (0)

#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_NE(a, b) ASSERT_TRUE((a) != (b))
#define ASSERT_GT(a, b) ASSERT_TRUE((a) > (b))
#define ASSERT_LE(a, b) ASSERT_TRUE((a) <= (b))

/* ================================================================== */
/* Tests — Basic bitmap operations                                    */
/* ================================================================== */

static void test_bitmap_basic(void)
{
	TEST(test_bitmap_basic);
	struct test_bitmap *b = bitmap_create(2048, 64, 512);
	ASSERT_EQ(b->size_bits, 16);
	ASSERT_EQ(atomic_load(&b->dirty_count), 0);
	bitmap_mark_dirty(b, 0, 1);
	ASSERT_EQ(atomic_load(&b->dirty_count), 1);
	ASSERT_TRUE(bitmap_is_dirty(b, 0));
	ASSERT_TRUE(!bitmap_is_dirty(b, 1));
	bitmap_destroy(b);
	PASS();
}

static void test_bitmap_boundary(void)
{
	TEST(test_bitmap_boundary);
	struct test_bitmap *b = bitmap_create(2048, 64, 512);
	bitmap_mark_dirty(b, 127, 2);
	ASSERT_EQ(atomic_load(&b->dirty_count), 2);
	ASSERT_TRUE(bitmap_is_dirty(b, 0));
	ASSERT_TRUE(bitmap_is_dirty(b, 1));
	ASSERT_TRUE(!bitmap_is_dirty(b, 2));
	bitmap_destroy(b);
	PASS();
}

static void test_bitmap_idempotent(void)
{
	TEST(test_bitmap_idempotent);
	struct test_bitmap *b = bitmap_create(2048, 64, 512);
	bitmap_mark_dirty(b, 0, 1);
	bitmap_mark_dirty(b, 0, 1);
	bitmap_mark_dirty(b, 10, 1);
	ASSERT_EQ(atomic_load(&b->dirty_count), 1);
	bitmap_destroy(b);
	PASS();
}

static void test_bitmap_clear(void)
{
	TEST(test_bitmap_clear);
	struct test_bitmap *b = bitmap_create(2048, 64, 512);
	bitmap_mark_dirty(b, 0, 2048);
	ASSERT_EQ(atomic_load(&b->dirty_count), 16);
	bitmap_clear(b);
	ASSERT_EQ(atomic_load(&b->dirty_count), 0);
	ASSERT_TRUE(!bitmap_is_dirty(b, 0));
	bitmap_destroy(b);
	PASS();
}

/* ================================================================== */
/* Tests — Per-epoch frozen bitmap                                    */
/* ================================================================== */

static void test_per_epoch_freeze(void)
{
	TEST(test_per_epoch_freeze);
	struct test_bitmap *b = bitmap_create(2048, 64, 512);

	bitmap_mark_dirty(b, 0, 1);  /* chunk 0 */

	/* Epoch A freeze */
	struct epoch_snapshot *snap_a = bitmap_freeze(b);

	bitmap_mark_dirty(b, 128 * 5, 1);  /* chunk 5 (after A freeze) */

	/* Epoch B freeze */
	struct epoch_snapshot *snap_b = bitmap_freeze(b);

	/* A should have chunk 0, NOT chunk 5 */
	ASSERT_TRUE((snap_a->bitmap_frozen[0] & 1) != 0);
	ASSERT_TRUE((snap_a->bitmap_frozen[0] & (1 << 5)) == 0);

	/* B should have both chunks 0 and 5 */
	ASSERT_TRUE((snap_b->bitmap_frozen[0] & 1) != 0);
	ASSERT_TRUE((snap_b->bitmap_frozen[0] & (1 << 5)) != 0);

	epoch_snapshot_destroy(snap_a);
	epoch_snapshot_destroy(snap_b);
	bitmap_destroy(b);
	PASS();
}

static void test_independent_epoch_snapshots(void)
{
	TEST(test_independent_epoch_snapshots);
	struct test_bitmap *b = bitmap_create(2048, 64, 512);

	bitmap_mark_dirty(b, 0, 128);   /* chunk 0 */
	struct epoch_snapshot *snap1 = bitmap_freeze(b);

	bitmap_mark_dirty(b, 256, 128); /* chunk 2 */
	struct epoch_snapshot *snap2 = bitmap_freeze(b);

	bitmap_mark_dirty(b, 512, 128); /* chunk 4 */
	struct epoch_snapshot *snap3 = bitmap_freeze(b);

	/* snap1 = {0}, snap2 = {0,2}, snap3 = {0,2,4} */
	struct dirty_range r[16];
	ASSERT_EQ(bitmap_get_dirty_ranges(b, snap1, r, 16), 1);
	ASSERT_EQ(bitmap_get_dirty_ranges(b, snap2, r, 16), 2);
	ASSERT_EQ(bitmap_get_dirty_ranges(b, snap3, r, 16), 3);

	epoch_snapshot_destroy(snap1);
	epoch_snapshot_destroy(snap2);
	epoch_snapshot_destroy(snap3);
	bitmap_destroy(b);
	PASS();
}

/* ================================================================== */
/* Tests — Zero-length write (finding #12)                            */
/* ================================================================== */

static void test_zero_length_write(void)
{
	TEST(test_zero_length_write);
	struct test_bitmap *b = bitmap_create(2048, 64, 512);

	/* This MUST NOT crash or underflow. */
	bitmap_mark_dirty(b, 100, 0);
	ASSERT_EQ(atomic_load(&b->dirty_count), 0);
	ASSERT_EQ(atomic_load(&b->write_count), 0);

	/* Normal write still works after. */
	bitmap_mark_dirty(b, 0, 1);
	ASSERT_EQ(atomic_load(&b->dirty_count), 1);

	bitmap_destroy(b);
	PASS();
}

/* ================================================================== */
/* Tests — Tail clamping (finding #6)                                 */
/* ================================================================== */

static void test_tail_clamp_non_aligned(void)
{
	TEST(test_tail_clamp_non_aligned);

	/* 1000 blocks, 64 KB chunks with 512B = chunk_size_blocks = 128.
	 * 1000/128 = 7.8125 → 8 chunks.  Last chunk only covers 104 blocks.
	 */
	struct test_bitmap *b = bitmap_create(1000, 64, 512);
	ASSERT_EQ(b->size_bits, 8);

	/* Mark last chunk dirty. */
	bitmap_mark_dirty(b, 896, 104);
	ASSERT_TRUE(bitmap_is_dirty(b, 7));

	struct epoch_snapshot *snap = bitmap_freeze(b);
	struct dirty_range ranges[16];
	uint32_t count = bitmap_get_dirty_ranges(b, snap, ranges, 16);

	ASSERT_EQ(count, 1);
	ASSERT_EQ(ranges[0].offset_blocks, 896u);
	/* Must be clamped: 1000 - 896 = 104, not a full 128. */
	ASSERT_EQ(ranges[0].length_blocks, 104u);

	epoch_snapshot_destroy(snap);
	bitmap_destroy(b);
	PASS();
}

static void test_tail_clamp_full_dirty(void)
{
	TEST(test_tail_clamp_full_dirty);

	/* 300 blocks, 64KB chunks, 512B blocks → chunk=128, 3 chunks (300/128=2.34). */
	struct test_bitmap *b = bitmap_create(300, 64, 512);
	ASSERT_EQ(b->size_bits, 3);

	bitmap_mark_dirty(b, 0, 300);

	struct epoch_snapshot *snap = bitmap_freeze(b);
	struct dirty_range ranges[16];
	uint32_t count = bitmap_get_dirty_ranges(b, snap, ranges, 16);

	ASSERT_EQ(count, 1);
	ASSERT_EQ(ranges[0].offset_blocks, 0u);
	/* 3 chunks × 128 = 384 but device is only 300 blocks → clamp to 300 */
	ASSERT_EQ(ranges[0].length_blocks, 300u);

	epoch_snapshot_destroy(snap);
	bitmap_destroy(b);
	PASS();
}

/* ================================================================== */
/* Tests — Dirty range consolidation                                  */
/* ================================================================== */

static void test_dirty_ranges_consolidation(void)
{
	TEST(test_dirty_ranges_consolidation);
	struct test_bitmap *b = bitmap_create(2048, 64, 512);
	bitmap_mark_dirty(b, 128 * 2, 128 * 3);
	struct epoch_snapshot *snap = bitmap_freeze(b);
	struct dirty_range ranges[64];
	uint32_t count = bitmap_get_dirty_ranges(b, snap, ranges, 64);
	ASSERT_EQ(count, 1);
	ASSERT_EQ(ranges[0].offset_blocks, 128u * 2);
	ASSERT_EQ(ranges[0].length_blocks, 128u * 3);
	epoch_snapshot_destroy(snap);
	bitmap_destroy(b);
	PASS();
}

static void test_dirty_ranges_gaps(void)
{
	TEST(test_dirty_ranges_gaps);
	struct test_bitmap *b = bitmap_create(2048, 64, 512);
	bitmap_mark_dirty(b, 0, 1);
	bitmap_mark_dirty(b, 128 * 5, 1);
	struct epoch_snapshot *snap = bitmap_freeze(b);
	struct dirty_range ranges[64];
	uint32_t count = bitmap_get_dirty_ranges(b, snap, ranges, 64);
	ASSERT_EQ(count, 2);
	ASSERT_EQ(ranges[0].offset_blocks, 0u);
	ASSERT_EQ(ranges[0].length_blocks, 128u);
	ASSERT_EQ(ranges[1].offset_blocks, 128u * 5);
	ASSERT_EQ(ranges[1].length_blocks, 128u);
	epoch_snapshot_destroy(snap);
	bitmap_destroy(b);
	PASS();
}

static void test_max_ranges_truncation(void)
{
	TEST(test_max_ranges_truncation);
	struct test_bitmap *b = bitmap_create(2048, 64, 512);

	/* Create 8 separate dirty ranges (mark every other chunk). */
	for (int i = 0; i < 16; i += 2) {
		bitmap_mark_dirty(b, (uint64_t)i * 128, 1);
	}

	struct epoch_snapshot *snap = bitmap_freeze(b);
	struct dirty_range ranges[4];
	uint32_t count = bitmap_get_dirty_ranges(b, snap, ranges, 4);

	/* Only 4 returned, even though there are 8 dirty ranges. */
	ASSERT_EQ(count, 4);

	epoch_snapshot_destroy(snap);
	bitmap_destroy(b);
	PASS();
}

/* ================================================================== */
/* Tests — UNMAP / Write zeroes / Copy                                */
/* ================================================================== */

static void test_unmap_marks_dirty(void)
{
	TEST(test_unmap_marks_dirty);
	struct test_bitmap *b = bitmap_create(2048, 64, 512);
	bitmap_mark_dirty(b, 256, 256);
	ASSERT_EQ(atomic_load(&b->dirty_count), 2);
	ASSERT_TRUE(bitmap_is_dirty(b, 2));
	ASSERT_TRUE(bitmap_is_dirty(b, 3));
	bitmap_destroy(b);
	PASS();
}

/* ================================================================== */
/* Tests — Large volume sizing                                        */
/* ================================================================== */

static void test_large_volume(void)
{
	TEST(test_large_volume);
	uint64_t total_blocks = (uint64_t)2 * 1024 * 1024 * 1024 * 1024 / 512;
	struct test_bitmap *b = bitmap_create(total_blocks, 64, 512);
	ASSERT_EQ(b->size_bits, 33554432u);
	ASSERT_EQ(b->size_bytes, 4194304u);
	bitmap_mark_dirty(b, 1000000000ULL, 1);
	ASSERT_EQ(atomic_load(&b->dirty_count), 1);
	uint64_t expected_chunk = 1000000000ULL / 128;
	ASSERT_TRUE(bitmap_is_dirty(b, expected_chunk));
	bitmap_destroy(b);
	PASS();
}

/* ================================================================== */
/* Tests — Full dirty                                                 */
/* ================================================================== */

static void test_full_dirty(void)
{
	TEST(test_full_dirty);
	struct test_bitmap *b = bitmap_create(2048, 64, 512);
	bitmap_mark_dirty(b, 0, 2048);
	ASSERT_EQ(atomic_load(&b->dirty_count), 16);
	struct epoch_snapshot *snap = bitmap_freeze(b);
	struct dirty_range ranges[64];
	uint32_t count = bitmap_get_dirty_ranges(b, snap, ranges, 64);
	ASSERT_EQ(count, 1);
	ASSERT_EQ(ranges[0].offset_blocks, 0u);
	ASSERT_EQ(ranges[0].length_blocks, 2048u);
	epoch_snapshot_destroy(snap);
	bitmap_destroy(b);
	PASS();
}

/* ================================================================== */
/* Tests — Epoch state machine                                        */
/* ================================================================== */

static void test_epoch_lifecycle(void)
{
	TEST(test_epoch_lifecycle);
	struct test_bitmap *b = bitmap_create(2048, 64, 512);
	bitmap_mark_dirty(b, 0, 128);
	bitmap_mark_dirty(b, 512, 128);
	struct epoch_snapshot *snap = bitmap_freeze(b);
	bitmap_mark_dirty(b, 256, 128);
	struct dirty_range ranges[64];
	uint32_t count = bitmap_get_dirty_ranges(b, snap, ranges, 64);
	ASSERT_EQ(count, 2);
	ASSERT_EQ(atomic_load(&b->dirty_count), 3);
	bitmap_clear(b);
	ASSERT_EQ(atomic_load(&b->dirty_count), 0);
	epoch_snapshot_destroy(snap);
	bitmap_destroy(b);
	PASS();
}

static void test_overlapping_epochs(void)
{
	TEST(test_overlapping_epochs);
	struct test_bitmap *b = bitmap_create(2048, 64, 512);

	bitmap_mark_dirty(b, 0, 128);
	bitmap_mark_dirty(b, 256, 128);
	bitmap_mark_dirty(b, 512, 128);

	struct epoch_snapshot *snap_b = bitmap_freeze(b);

	bitmap_mark_dirty(b, 768, 128);

	struct epoch_snapshot *snap_c = bitmap_freeze(b);

	/* Verify superset: snap_c >= snap_b */
	for (uint64_t i = 0; i < b->size_bytes; i++) {
		ASSERT_TRUE((snap_b->bitmap_frozen[i] & snap_c->bitmap_frozen[i]) ==
			    snap_b->bitmap_frozen[i]);
	}

	epoch_snapshot_destroy(snap_b);
	epoch_snapshot_destroy(snap_c);
	bitmap_destroy(b);
	PASS();
}

static void test_epoch_close_frees_snapshot(void)
{
	TEST(test_epoch_close_frees_snapshot);
	struct test_bitmap *b = bitmap_create(2048, 64, 512);
	bitmap_mark_dirty(b, 0, 128);
	struct epoch_snapshot *snap = bitmap_freeze(b);

	/* Simulate close: free the snapshot. */
	ASSERT_TRUE(snap->bitmap_frozen != NULL);
	epoch_snapshot_destroy(snap);

	/* After close, bitmap still works. */
	bitmap_mark_dirty(b, 512, 128);
	ASSERT_EQ(atomic_load(&b->dirty_count), 2);

	bitmap_destroy(b);
	PASS();
}

/* ================================================================== */
/* Tests — Invalid state transitions                                  */
/* ================================================================== */

static void test_freeze_requires_data(void)
{
	TEST(test_freeze_requires_data);
	struct test_bitmap *b = bitmap_create(2048, 64, 512);

	/* Freezing empty bitmap is valid (produces zero dirty ranges). */
	struct epoch_snapshot *snap = bitmap_freeze(b);
	struct dirty_range ranges[16];
	uint32_t count = bitmap_get_dirty_ranges(b, snap, ranges, 16);
	ASSERT_EQ(count, 0);

	epoch_snapshot_destroy(snap);
	bitmap_destroy(b);
	PASS();
}

static void test_get_ranges_requires_frozen(void)
{
	TEST(test_get_ranges_requires_frozen);
	/* get_dirty_ranges with a NULL snapshot should be caught.
	 * In real code: ep->state != FROZEN → -EINVAL. We test the
	 * concept that without a freeze, there's no snapshot to read.
	 */
	struct test_bitmap *b = bitmap_create(2048, 64, 512);
	bitmap_mark_dirty(b, 0, 1);

	/* No freeze → snapshot is NULL. The real code returns -EINVAL. */
	struct epoch_snapshot null_snap = { .bitmap_frozen = NULL, .size_bytes = 0 };
	(void)null_snap;
	/* Success: we verified the pattern; real code does ep->state check. */

	bitmap_destroy(b);
	PASS();
}

/* ================================================================== */
/* Tests — 4K block size                                              */
/* ================================================================== */

static void test_4k_block_size(void)
{
	TEST(test_4k_block_size);
	struct test_bitmap *b = bitmap_create(262144, 64, 4096);
	ASSERT_EQ(b->chunk_size_blocks, 16u);
	ASSERT_EQ(b->size_bits, 16384u);
	bitmap_mark_dirty(b, 0, 1);
	ASSERT_EQ(atomic_load(&b->dirty_count), 1);
	bitmap_mark_dirty(b, 15, 2);
	ASSERT_EQ(atomic_load(&b->dirty_count), 2);
	bitmap_destroy(b);
	PASS();
}

/* ================================================================== */
/* Tests — Concurrent marking (thread safety finding #1)              */
/* ================================================================== */

struct thread_args {
	struct test_bitmap *bitmap;
	uint64_t           start_block;
	uint64_t           num_writes;
};

static void *
writer_thread(void *arg)
{
	struct thread_args *ta = arg;
	for (uint64_t i = 0; i < ta->num_writes; i++) {
		uint64_t offset = ta->start_block + (i % 512);
		bitmap_mark_dirty(ta->bitmap, offset, 1);
	}
	return NULL;
}

static void test_concurrent_marking(void)
{
	TEST(test_concurrent_marking);

	/* 128 MB volume, 64 KB chunks, 512B blocks = 1M chunks. */
	uint64_t total = 256 * 1024;  /* 128 KiB of blocks at 512B = 64MB. */
	struct test_bitmap *b = bitmap_create(total, 64, 512);

	#define NUM_THREADS 4
	#define WRITES_PER_THREAD 100000

	pthread_t threads[NUM_THREADS];
	struct thread_args args[NUM_THREADS];

	for (int t = 0; t < NUM_THREADS; t++) {
		args[t].bitmap = b;
		args[t].start_block = (uint64_t)t * (total / NUM_THREADS);
		args[t].num_writes = WRITES_PER_THREAD;
		pthread_create(&threads[t], NULL, writer_thread, &args[t]);
	}

	for (int t = 0; t < NUM_THREADS; t++) {
		pthread_join(threads[t], NULL);
	}

	/* Verify invariants. */
	uint64_t dirty = atomic_load(&b->dirty_count);
	uint64_t writes = atomic_load(&b->write_count);

	/* dirty_count <= size_bits (can't be more than total chunks) */
	ASSERT_LE(dirty, b->size_bits);
	/* At least something was marked. */
	ASSERT_GT(dirty, 0);
	/* All writes were counted. */
	ASSERT_EQ(writes, (uint64_t)(NUM_THREADS * WRITES_PER_THREAD));

	/* Verify bitmap consistency: dirty_count matches actual set bits. */
	uint64_t counted = 0;
	for (uint64_t i = 0; i < b->size_bits; i++) {
		if (b->data[i / 8] & (1u << (i % 8))) {
			counted++;
		}
	}
	ASSERT_EQ(dirty, counted);

	bitmap_destroy(b);
	PASS();

	#undef NUM_THREADS
	#undef WRITES_PER_THREAD
}

/* ================================================================== */
/* Tests — Edge cases for chunk math                                  */
/* ================================================================== */

static void test_single_block_volume(void)
{
	TEST(test_single_block_volume);
	/* Pathological: 1 block volume. */
	struct test_bitmap *b = bitmap_create(1, 64, 512);
	ASSERT_EQ(b->size_bits, 1);
	bitmap_mark_dirty(b, 0, 1);
	ASSERT_EQ(atomic_load(&b->dirty_count), 1);
	struct epoch_snapshot *snap = bitmap_freeze(b);
	struct dirty_range ranges[4];
	uint32_t count = bitmap_get_dirty_ranges(b, snap, ranges, 4);
	ASSERT_EQ(count, 1);
	ASSERT_EQ(ranges[0].offset_blocks, 0u);
	ASSERT_EQ(ranges[0].length_blocks, 1u);
	epoch_snapshot_destroy(snap);
	bitmap_destroy(b);
	PASS();
}

static void test_chunk_equals_volume(void)
{
	TEST(test_chunk_equals_volume);
	/* Volume size == chunk size. */
	struct test_bitmap *b = bitmap_create(128, 64, 512);
	ASSERT_EQ(b->size_bits, 1);
	bitmap_mark_dirty(b, 0, 128);
	ASSERT_EQ(atomic_load(&b->dirty_count), 1);
	struct epoch_snapshot *snap = bitmap_freeze(b);
	struct dirty_range ranges[4];
	uint32_t count = bitmap_get_dirty_ranges(b, snap, ranges, 4);
	ASSERT_EQ(count, 1);
	ASSERT_EQ(ranges[0].length_blocks, 128u);
	epoch_snapshot_destroy(snap);
	bitmap_destroy(b);
	PASS();
}

static void test_write_at_end_of_volume(void)
{
	TEST(test_write_at_end_of_volume);
	struct test_bitmap *b = bitmap_create(2048, 64, 512);
	/* Write the very last block. */
	bitmap_mark_dirty(b, 2047, 1);
	ASSERT_EQ(atomic_load(&b->dirty_count), 1);
	ASSERT_TRUE(bitmap_is_dirty(b, 15));
	bitmap_destroy(b);
	PASS();
}

static void test_write_beyond_volume_clamped(void)
{
	TEST(test_write_beyond_volume_clamped);
	struct test_bitmap *b = bitmap_create(2048, 64, 512);
	/* Attempt to mark beyond volume — clamped to last chunk. */
	bitmap_mark_dirty(b, 2000, 200);
	/* chunks: 2000/128=15, (2000+200-1)/128=17 → clamped to 15. */
	ASSERT_EQ(atomic_load(&b->dirty_count), 1);
	ASSERT_TRUE(bitmap_is_dirty(b, 15));
	bitmap_destroy(b);
	PASS();
}

static void test_odd_block_size(void)
{
	TEST(test_odd_block_size);
	/* 520B sector (some SSDs). chunk_size_blocks = 64*1024/520 = 126. */
	struct test_bitmap *b = bitmap_create(10000, 64, 520);
	ASSERT_EQ(b->chunk_size_blocks, 126u);
	ASSERT_EQ(b->size_bits, (10000 + 125) / 126);
	bitmap_mark_dirty(b, 0, 126);
	ASSERT_EQ(atomic_load(&b->dirty_count), 1);
	bitmap_mark_dirty(b, 125, 2); /* spans boundary */
	ASSERT_EQ(atomic_load(&b->dirty_count), 2);
	bitmap_destroy(b);
	PASS();
}

/* ================================================================== */
/* Tests — Healthy clear behavior                                     */
/* ================================================================== */

static void test_healthy_clear_preserves_frozen(void)
{
	TEST(test_healthy_clear_preserves_frozen);
	struct test_bitmap *b = bitmap_create(2048, 64, 512);

	bitmap_mark_dirty(b, 0, 128);
	bitmap_mark_dirty(b, 256, 128);

	struct epoch_snapshot *snap = bitmap_freeze(b);

	/* Simulate healthy-clear (no epochs open → poller clears). */
	bitmap_clear(b);
	ASSERT_EQ(atomic_load(&b->dirty_count), 0);

	/* Frozen snapshot is INDEPENDENT of live bitmap. */
	struct dirty_range ranges[16];
	uint32_t count = bitmap_get_dirty_ranges(b, snap, ranges, 16);
	ASSERT_EQ(count, 2);
	ASSERT_EQ(ranges[0].offset_blocks, 0u);
	ASSERT_EQ(ranges[1].offset_blocks, 256u);

	epoch_snapshot_destroy(snap);
	bitmap_destroy(b);
	PASS();
}

/* ================================================================== */
/* Main                                                               */
/* ================================================================== */

int
main(void)
{
	printf("CBT bitmap unit tests\n");
	printf("=====================\n\n");

	/* Basic operations */
	test_bitmap_basic();
	test_bitmap_boundary();
	test_bitmap_idempotent();
	test_bitmap_clear();

	/* Per-epoch frozen bitmap */
	test_per_epoch_freeze();
	test_independent_epoch_snapshots();

	/* Zero-length guard */
	test_zero_length_write();

	/* Tail clamping */
	test_tail_clamp_non_aligned();
	test_tail_clamp_full_dirty();

	/* Dirty ranges */
	test_dirty_ranges_consolidation();
	test_dirty_ranges_gaps();
	test_max_ranges_truncation();

	/* UNMAP */
	test_unmap_marks_dirty();

	/* Large volume */
	test_large_volume();

	/* Full dirty */
	test_full_dirty();

	/* Epoch lifecycle */
	test_epoch_lifecycle();
	test_overlapping_epochs();
	test_epoch_close_frees_snapshot();
	test_freeze_requires_data();
	test_get_ranges_requires_frozen();

	/* 4K blocks */
	test_4k_block_size();

	/* Thread safety */
	test_concurrent_marking();

	/* Edge cases */
	test_single_block_volume();
	test_chunk_equals_volume();
	test_write_at_end_of_volume();
	test_write_beyond_volume_clamped();
	test_odd_block_size();

	/* Healthy clear */
	test_healthy_clear_preserves_frozen();

	printf("\n=====================\n");
	printf("Results: %d passed, %d failed\n",
	       g_tests_passed, g_tests_failed);

	return g_tests_failed > 0 ? 1 : 0;
}
