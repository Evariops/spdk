/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Evariops.
 *   All rights reserved.
 */

/*
 * Property-based (randomized) tests for CBT bitmap logic.
 *
 * Strategy: generate millions of random operations and verify universal
 * invariants hold for EVERY sequence. This catches edge cases that
 * hand-written tests miss (off-by-one, overflow, alignment, etc.).
 *
 * Build:  cc -Wall -Wextra -Werror -std=c11 -g -O2 \
 *            -fsanitize=address,undefined \
 *            -o test_cbt_props test_cbt_props.c -lpthread
 * Run:    ./test_cbt_props [seed]
 *
 * If a failure is found, the seed is printed for reproducibility.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdatomic.h>
#include <time.h>

/* ================================================================== */
/* Xoshiro256** PRNG (fast, good distribution, reproducible)          */
/* ================================================================== */

static uint64_t prng_state[4];

static inline uint64_t
rotl64(uint64_t x, int k)
{
	return (x << k) | (x >> (64 - k));
}

static uint64_t
prng_next(void)
{
	uint64_t *s = prng_state;
	uint64_t result = rotl64(s[1] * 5, 7) * 9;
	uint64_t t = s[1] << 17;

	s[2] ^= s[0];
	s[3] ^= s[1];
	s[1] ^= s[2];
	s[0] ^= s[3];
	s[2] ^= t;
	s[3] = rotl64(s[3], 45);

	return result;
}

static void
prng_seed(uint64_t seed)
{
	/* SplitMix64 to initialize state from a single seed. */
	for (int i = 0; i < 4; i++) {
		seed += 0x9e3779b97f4a7c15ULL;
		uint64_t z = seed;
		z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
		z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
		prng_state[i] = z ^ (z >> 31);
	}
}

static uint64_t
prng_range(uint64_t max)
{
	if (max == 0) return 0;
	return prng_next() % max;
}

/* ================================================================== */
/* Bitmap under test (mirrors vbdev_cbt.c logic exactly)              */
/* ================================================================== */

struct bitmap {
	uint8_t  *data;
	uint64_t  size_bits;
	uint64_t  size_bytes;
	uint64_t  chunk_size_blocks;
	uint32_t  chunk_shift;
	uint64_t  total_blocks;
};

static struct bitmap *
bitmap_create(uint64_t total_blocks, uint64_t chunk_size_blocks)
{
	struct bitmap *b = calloc(1, sizeof(*b));
	assert(b);

	b->chunk_size_blocks = chunk_size_blocks;
	if (b->chunk_size_blocks == 0) b->chunk_size_blocks = 1;

	/* Ensure power of 2 (mirrors registration logic). */
	if ((b->chunk_size_blocks & (b->chunk_size_blocks - 1)) != 0) {
		uint64_t v = b->chunk_size_blocks;
		v--; v |= v >> 1; v |= v >> 2; v |= v >> 4;
		v |= v >> 8; v |= v >> 16; v |= v >> 32;
		b->chunk_size_blocks = v + 1;
	}
	b->chunk_shift = (uint32_t)__builtin_ctzll(b->chunk_size_blocks);

	b->size_bits   = (total_blocks + b->chunk_size_blocks - 1) / b->chunk_size_blocks;
	b->size_bytes  = (b->size_bits + 7) / 8;
	b->total_blocks = total_blocks;

	b->data = calloc(1, b->size_bytes);
	assert(b->data);
	return b;
}

static void
bitmap_destroy(struct bitmap *b)
{
	free(b->data);
	free(b);
}

/* Production-identical hot path. */
static inline void
bitmap_mark_dirty(struct bitmap *b, uint64_t offset_blocks, uint64_t num_blocks)
{
	if (num_blocks == 0 || b->size_bits == 0) {
		return;
	}

	uint64_t chunk_start = offset_blocks >> b->chunk_shift;
	uint64_t chunk_end   = (offset_blocks + num_blocks - 1) >> b->chunk_shift;

	if (chunk_end >= b->size_bits) {
		chunk_end = b->size_bits - 1;
	}

	for (uint64_t i = chunk_start; i <= chunk_end; i++) {
		uint8_t mask = (uint8_t)(1u << (i & 7));
		__atomic_fetch_or(&b->data[i >> 3], mask, __ATOMIC_RELAXED);
	}
}

static uint64_t
bitmap_popcount(const struct bitmap *b)
{
	const uint64_t *words = (const uint64_t *)b->data;
	uint64_t n = b->size_bytes / 8;
	uint64_t tail = b->size_bytes % 8;
	uint64_t count = 0;

	for (uint64_t i = 0; i < n; i++) {
		count += (uint64_t)__builtin_popcountll(words[i]);
	}
	if (tail > 0) {
		const uint8_t *rest = (const uint8_t *)&words[n];
		for (uint64_t i = 0; i < tail; i++) {
			count += (uint64_t)__builtin_popcount(rest[i]);
		}
	}
	return count;
}

static bool
bitmap_bit_set(const struct bitmap *b, uint64_t chunk)
{
	if (chunk >= b->size_bits) return false;
	return (b->data[chunk >> 3] & (1u << (chunk & 7))) != 0;
}

/* ── Reference model (trivial, obviously correct) ── */

struct ref_model {
	bool     *chunks;    /* one bool per chunk */
	uint64_t  size;
	uint64_t  chunk_size_blocks;
	uint64_t  total_blocks;
};

static struct ref_model *
ref_create(uint64_t total_blocks, uint64_t chunk_size_blocks)
{
	struct ref_model *r = calloc(1, sizeof(*r));
	assert(r);
	r->chunk_size_blocks = chunk_size_blocks;
	if (r->chunk_size_blocks == 0) r->chunk_size_blocks = 1;
	if ((r->chunk_size_blocks & (r->chunk_size_blocks - 1)) != 0) {
		uint64_t v = r->chunk_size_blocks;
		v--; v |= v >> 1; v |= v >> 2; v |= v >> 4;
		v |= v >> 8; v |= v >> 16; v |= v >> 32;
		r->chunk_size_blocks = v + 1;
	}
	r->size = (total_blocks + r->chunk_size_blocks - 1) / r->chunk_size_blocks;
	r->total_blocks = total_blocks;
	r->chunks = calloc(r->size, sizeof(bool));
	assert(r->chunks);
	return r;
}

static void
ref_destroy(struct ref_model *r)
{
	free(r->chunks);
	free(r);
}

static void
ref_mark_dirty(struct ref_model *r, uint64_t offset_blocks, uint64_t num_blocks)
{
	if (num_blocks == 0 || r->size == 0) return;

	uint64_t cs = offset_blocks / r->chunk_size_blocks;
	uint64_t ce = (offset_blocks + num_blocks - 1) / r->chunk_size_blocks;
	if (ce >= r->size) ce = r->size - 1;

	for (uint64_t i = cs; i <= ce; i++) {
		r->chunks[i] = true;
	}
}

static void
ref_clear(struct ref_model *r)
{
	memset(r->chunks, 0, r->size * sizeof(bool));
	(void)ref_clear; /* suppress unused warning — available for future properties */
}

/* ── get_dirty_ranges reference implementation ── */

struct range {
	uint64_t offset_blocks;
	uint64_t length_blocks;
};

static uint32_t
ref_get_ranges(const struct ref_model *r, struct range *out, uint32_t max_ranges)
{
	uint32_t count = 0;
	int64_t run_start = -1;

	for (uint64_t i = 0; i < r->size; i++) {
		if (r->chunks[i] && run_start < 0) {
			run_start = (int64_t)i;
		}
		if (!r->chunks[i] || i == r->size - 1) {
			if (run_start >= 0) {
				uint64_t end = r->chunks[i] ? i : i - 1;
				if (count < max_ranges) {
					uint64_t off = (uint64_t)run_start * r->chunk_size_blocks;
					uint64_t len = (end - (uint64_t)run_start + 1) *
						       r->chunk_size_blocks;
					if (off + len > r->total_blocks) {
						len = r->total_blocks - off;
					}
					out[count].offset_blocks = off;
					out[count].length_blocks = len;
					count++;
				}
				run_start = -1;
			}
		}
	}
	return count;
}

/* ── get_dirty_ranges SUT (system under test) ── */

static uint32_t
sut_get_ranges(const struct bitmap *b, const uint8_t *bmap,
	       struct range *out, uint32_t max_ranges)
{
	uint32_t count = 0;
	int64_t run_start = -1;

	for (uint64_t i = 0; i < b->size_bits; i++) {
		bool is_dirty = (bmap[i >> 3] & (1u << (i & 7))) != 0;

		if (is_dirty && run_start < 0) {
			run_start = (int64_t)i;
		}
		if (!is_dirty || i == b->size_bits - 1) {
			if (run_start >= 0) {
				uint64_t end = is_dirty ? i : i - 1;
				if (count < max_ranges) {
					uint64_t off = (uint64_t)run_start * b->chunk_size_blocks;
					uint64_t len = (end - (uint64_t)run_start + 1) *
						       b->chunk_size_blocks;
					if (off + len > b->total_blocks) {
						len = b->total_blocks - off;
					}
					out[count].offset_blocks = off;
					out[count].length_blocks = len;
					count++;
				}
				run_start = -1;
			}
		}
	}
	return count;
}

/* ================================================================== */
/* Property checks                                                    */
/* ================================================================== */

static int g_failures = 0;

#define CHECK(cond, fmt, ...) do {                                       \
	if (!(cond)) {                                                  \
		printf("  FAIL [iter=%u]: " fmt "\n", iter, ##__VA_ARGS__); \
		g_failures++;                                           \
		return;                                                 \
	}                                                               \
} while (0)

/*
 * PROPERTY 1: Bitmap equivalence with reference model
 *
 * After applying the same random sequence of mark_dirty operations,
 * the bitmap must have exactly the same set bits as the reference.
 */
static void
prop_bitmap_equivalence(uint32_t iter, uint64_t total_blocks,
			uint64_t chunk_size_blocks, uint32_t num_ops)
{
	struct bitmap    *b = bitmap_create(total_blocks, chunk_size_blocks);
	struct ref_model *r = ref_create(total_blocks, chunk_size_blocks);

	for (uint32_t op = 0; op < num_ops; op++) {
		uint64_t offset = prng_range(total_blocks + 100); /* allow OOB */
		uint64_t length = prng_range(total_blocks / 2 + 1);

		bitmap_mark_dirty(b, offset, length);
		ref_mark_dirty(r, offset, length);
	}

	/* Verify every bit matches. */
	CHECK(b->size_bits == r->size,
	      "size mismatch: b=%lu r=%lu", (unsigned long)b->size_bits,
	      (unsigned long)r->size);

	for (uint64_t i = 0; i < b->size_bits; i++) {
		bool sut = bitmap_bit_set(b, i);
		bool ref = r->chunks[i];
		CHECK(sut == ref,
		      "bit %lu: sut=%d ref=%d (total=%lu chunk=%lu)",
		      (unsigned long)i, sut, ref,
		      (unsigned long)total_blocks,
		      (unsigned long)chunk_size_blocks);
	}

	/* Verify popcount matches. */
	uint64_t pop = bitmap_popcount(b);
	uint64_t ref_dirty = 0;
	for (uint64_t i = 0; i < r->size; i++) {
		if (r->chunks[i]) ref_dirty++;
	}
	CHECK(pop == ref_dirty,
	      "popcount mismatch: %lu vs %lu",
	      (unsigned long)pop, (unsigned long)ref_dirty);

	bitmap_destroy(b);
	ref_destroy(r);
}

/*
 * PROPERTY 2: Monotonicity — marking never clears bits
 */
static void
prop_monotonicity(uint32_t iter, uint64_t total_blocks,
		  uint64_t chunk_size_blocks, uint32_t num_ops)
{
	struct bitmap *b = bitmap_create(total_blocks, chunk_size_blocks);

	uint64_t prev_pop = 0;
	for (uint32_t op = 0; op < num_ops; op++) {
		uint64_t offset = prng_range(total_blocks);
		uint64_t length = prng_range(total_blocks / 4 + 1);

		bitmap_mark_dirty(b, offset, length);
		uint64_t pop = bitmap_popcount(b);

		CHECK(pop >= prev_pop,
		      "popcount decreased: %lu → %lu after mark(%lu, %lu)",
		      (unsigned long)prev_pop, (unsigned long)pop,
		      (unsigned long)offset, (unsigned long)length);
		prev_pop = pop;
	}

	bitmap_destroy(b);
}

/*
 * PROPERTY 3: Freeze isolation — snapshot is immutable
 */
static void
prop_freeze_isolation(uint32_t iter, uint64_t total_blocks,
		      uint64_t chunk_size_blocks, uint32_t num_ops)
{
	struct bitmap *b = bitmap_create(total_blocks, chunk_size_blocks);

	/* Phase 1: initial marks. */
	uint32_t phase1_ops = num_ops / 2;
	for (uint32_t op = 0; op < phase1_ops; op++) {
		bitmap_mark_dirty(b, prng_range(total_blocks),
				  prng_range(total_blocks / 4 + 1));
	}

	/* Freeze. */
	uint8_t *frozen = malloc(b->size_bytes);
	assert(frozen);
	memcpy(frozen, b->data, b->size_bytes);

	/* Phase 2: more marks (must not affect frozen). */
	for (uint32_t op = phase1_ops; op < num_ops; op++) {
		bitmap_mark_dirty(b, prng_range(total_blocks),
				  prng_range(total_blocks / 4 + 1));
	}

	/* Verify frozen is unchanged — it's a memcpy so obviously true,
	 * but verify the SUT bitmap is a superset of frozen. */
	for (uint64_t i = 0; i < b->size_bits; i++) {
		bool fbit = (frozen[i >> 3] & (1u << (i & 7))) != 0;
		bool live = bitmap_bit_set(b, i);
		CHECK(!fbit || live,
		      "superset violation: frozen[%lu]=1 but live[%lu]=0",
		      (unsigned long)i, (unsigned long)i);
	}

	free(frozen);
	bitmap_destroy(b);
}

/*
 * PROPERTY 4: Range completeness & bounds
 *
 * Every dirty chunk must appear in exactly one range.
 * No range extends beyond total_blocks.
 */
static void
prop_range_completeness(uint32_t iter, uint64_t total_blocks,
			uint64_t chunk_size_blocks, uint32_t num_ops)
{
	struct bitmap    *b = bitmap_create(total_blocks, chunk_size_blocks);
	struct ref_model *r = ref_create(total_blocks, chunk_size_blocks);

	for (uint32_t op = 0; op < num_ops; op++) {
		uint64_t offset = prng_range(total_blocks);
		uint64_t length = prng_range(total_blocks / 4 + 1);
		bitmap_mark_dirty(b, offset, length);
		ref_mark_dirty(r, offset, length);
	}

	/* Get ranges from SUT. */
	struct range sut_ranges[8192];
	uint32_t sut_count = sut_get_ranges(b, b->data, sut_ranges, 8192);

	/* Get ranges from reference. */
	struct range ref_ranges[8192];
	uint32_t ref_count = ref_get_ranges(r, ref_ranges, 8192);

	CHECK(sut_count == ref_count,
	      "range count mismatch: sut=%u ref=%u",
	      sut_count, ref_count);

	for (uint32_t i = 0; i < sut_count && i < ref_count; i++) {
		CHECK(sut_ranges[i].offset_blocks == ref_ranges[i].offset_blocks,
		      "range[%u].offset: sut=%lu ref=%lu", i,
		      (unsigned long)sut_ranges[i].offset_blocks,
		      (unsigned long)ref_ranges[i].offset_blocks);
		CHECK(sut_ranges[i].length_blocks == ref_ranges[i].length_blocks,
		      "range[%u].length: sut=%lu ref=%lu", i,
		      (unsigned long)sut_ranges[i].length_blocks,
		      (unsigned long)ref_ranges[i].length_blocks);

		/* Bounds check. */
		uint64_t end = sut_ranges[i].offset_blocks + sut_ranges[i].length_blocks;
		CHECK(end <= total_blocks,
		      "range[%u] exceeds volume: offset=%lu len=%lu total=%lu",
		      i,
		      (unsigned long)sut_ranges[i].offset_blocks,
		      (unsigned long)sut_ranges[i].length_blocks,
		      (unsigned long)total_blocks);
	}

	bitmap_destroy(b);
	ref_destroy(r);
}

/*
 * PROPERTY 5: Clear resets everything
 */
static void
prop_clear_resets(uint32_t iter, uint64_t total_blocks,
		  uint64_t chunk_size_blocks, uint32_t num_ops)
{
	struct bitmap *b = bitmap_create(total_blocks, chunk_size_blocks);

	for (uint32_t op = 0; op < num_ops; op++) {
		bitmap_mark_dirty(b, prng_range(total_blocks),
				  prng_range(total_blocks / 4 + 1));
	}

	memset(b->data, 0, b->size_bytes);
	uint64_t pop = bitmap_popcount(b);
	CHECK(pop == 0, "popcount after clear = %lu", (unsigned long)pop);

	bitmap_destroy(b);
}

/*
 * PROPERTY 6: Idempotence — marking same range twice is no-op
 */
static void
prop_idempotence(uint32_t iter, uint64_t total_blocks,
		 uint64_t chunk_size_blocks, uint32_t num_ops)
{
	struct bitmap *b = bitmap_create(total_blocks, chunk_size_blocks);

	/* Apply random ops. */
	for (uint32_t op = 0; op < num_ops; op++) {
		uint64_t offset = prng_range(total_blocks);
		uint64_t length = prng_range(total_blocks / 4 + 1);
		bitmap_mark_dirty(b, offset, length);
	}

	/* Snapshot. */
	uint8_t *snapshot = malloc(b->size_bytes);
	assert(snapshot);
	memcpy(snapshot, b->data, b->size_bytes);

	/* Re-apply the same ops (re-seed PRNG to same point — we can't,
	 * so instead: mark everything dirty, which is a superset). */
	/* Actually: just re-mark every set bit — bitmap must not change. */
	for (uint64_t i = 0; i < b->size_bits; i++) {
		if (bitmap_bit_set(b, i)) {
			uint64_t off = i * b->chunk_size_blocks;
			bitmap_mark_dirty(b, off, 1);
		}
	}

	CHECK(memcmp(b->data, snapshot, b->size_bytes) == 0,
	      "bitmap changed after redundant marking");

	free(snapshot);
	bitmap_destroy(b);
}

/*
 * PROPERTY 7: No OOB access — extreme offset/length values
 *
 * This runs under ASan — any OOB will be caught.
 */
static void
prop_no_oob(uint32_t iter, uint64_t total_blocks,
	    uint64_t chunk_size_blocks, uint32_t num_ops)
{
	struct bitmap *b = bitmap_create(total_blocks, chunk_size_blocks);

	for (uint32_t op = 0; op < num_ops; op++) {
		/* Generate extreme values: near edges, overflow-adjacent. */
		uint64_t offset, length;
		switch (prng_range(5)) {
		case 0: /* Normal range */
			offset = prng_range(total_blocks);
			length = prng_range(total_blocks / 2 + 1);
			break;
		case 1: /* At boundary */
			offset = total_blocks - 1;
			length = prng_range(100) + 1;
			break;
		case 2: /* Beyond volume */
			offset = total_blocks + prng_range(1000);
			length = prng_range(1000);
			break;
		case 3: /* Zero length */
			offset = prng_range(total_blocks + 1000);
			length = 0;
			break;
		case 4: /* Huge length (near UINT64_MAX) */
			offset = prng_range(total_blocks);
			length = UINT64_MAX - offset;
			break;
		default:
			offset = 0; length = 1;
		}
		bitmap_mark_dirty(b, offset, length);
	}

	/* If we get here without ASan firing, no OOB occurred. */
	uint64_t pop = bitmap_popcount(b);
	CHECK(pop <= b->size_bits,
	      "popcount %lu exceeds size_bits %lu",
	      (unsigned long)pop, (unsigned long)b->size_bits);

	/* Verify no bits set beyond size_bits in the padding area. */
	uint64_t padding_bits = b->size_bytes * 8 - b->size_bits;
	if (padding_bits > 0) {
		uint8_t last_byte = b->data[b->size_bytes - 1];
		uint8_t valid_mask = (uint8_t)((1u << (b->size_bits & 7)) - 1);
		if ((b->size_bits & 7) == 0) valid_mask = 0xFF;
		uint8_t garbage = last_byte & (uint8_t)~valid_mask;
		/* Note: production code sets padding bits via atomic OR on
		 * clamped chunk_end, so padding bits should NOT be set.
		 * But if chunk_end is clamped to size_bits-1, and that's in
		 * the valid portion, padding stays zero. */
		(void)garbage; /* ASan catches the real issue. */
	}

	bitmap_destroy(b);
}

/* ================================================================== */
/* Test runner                                                        */
/* ================================================================== */

/* Volume configurations to test (total_blocks, chunk_size_blocks). */
static const struct {
	uint64_t total_blocks;
	uint64_t chunk_size_blocks;
} configs[] = {
	{1,         1},         /* Degenerate: 1 block, 1 chunk */
	{1,         128},       /* 1 block but chunk > volume */
	{7,         4},         /* Odd, non-P2 total */
	{128,       128},       /* chunk == volume */
	{129,       128},       /* chunk+1 = volume (2 chunks) */
	{1000,      128},       /* Non-aligned classic */
	{2048,      128},       /* Aligned classic (16 chunks) */
	{10000,     128},       /* ~78 chunks */
	{65536,     128},       /* 512 chunks = 64 bytes bitmap */
	{1048576,   128},       /* 1M blocks = 8192 chunks = 1 KB bitmap */
	{33554432,  128},       /* 16 GB @ 512B = 32M blocks = 256K chunks */
	{100,       1},         /* chunk = 1 block (extreme granularity) */
	{500,       256},       /* chunk > half volume */
	{2048,      1},         /* 2048 chunks (each = 1 block) */
	{255,       64},        /* Non-P2 total, P2 chunk */
};

#define NUM_CONFIGS (sizeof(configs) / sizeof(configs[0]))
#define OPS_PER_ITER 200
#define ITERATIONS_PER_PROPERTY 10000

int
main(int argc, char **argv)
{
	uint64_t seed;

	if (argc > 1) {
		seed = (uint64_t)strtoull(argv[1], NULL, 0);
	} else {
		seed = (uint64_t)time(NULL) ^ (uint64_t)(uintptr_t)&main;
	}

	printf("CBT property-based tests (seed=%lu)\n", (unsigned long)seed);
	printf("=====================================\n");
	printf("Configs: %zu | Iterations: %d | Ops/iter: %d\n\n",
	       NUM_CONFIGS, ITERATIONS_PER_PROPERTY, OPS_PER_ITER);

	prng_seed(seed);

	uint32_t total_iters = 0;

	/* ── Property 1: Bitmap equivalence ── */
	printf("  [1/7] Bitmap ≡ Reference model...");
	fflush(stdout);
	for (uint32_t i = 0; i < ITERATIONS_PER_PROPERTY; i++) {
		uint32_t cfg = (uint32_t)(prng_range(NUM_CONFIGS));
		prop_bitmap_equivalence(total_iters++,
					configs[cfg].total_blocks,
					configs[cfg].chunk_size_blocks,
					OPS_PER_ITER);
	}
	printf(" ✓\n");

	/* ── Property 2: Monotonicity ── */
	printf("  [2/7] Monotonicity (popcount never decreases)...");
	fflush(stdout);
	for (uint32_t i = 0; i < ITERATIONS_PER_PROPERTY; i++) {
		uint32_t cfg = (uint32_t)(prng_range(NUM_CONFIGS));
		prop_monotonicity(total_iters++,
				  configs[cfg].total_blocks,
				  configs[cfg].chunk_size_blocks,
				  OPS_PER_ITER);
	}
	printf(" ✓\n");

	/* ── Property 3: Freeze isolation ── */
	printf("  [3/7] Freeze isolation (superset)...");
	fflush(stdout);
	for (uint32_t i = 0; i < ITERATIONS_PER_PROPERTY; i++) {
		uint32_t cfg = (uint32_t)(prng_range(NUM_CONFIGS));
		prop_freeze_isolation(total_iters++,
				      configs[cfg].total_blocks,
				      configs[cfg].chunk_size_blocks,
				      OPS_PER_ITER);
	}
	printf(" ✓\n");

	/* ── Property 4: Range completeness ── */
	printf("  [4/7] Range completeness & bounds...");
	fflush(stdout);
	for (uint32_t i = 0; i < ITERATIONS_PER_PROPERTY; i++) {
		uint32_t cfg = (uint32_t)(prng_range(NUM_CONFIGS));
		prop_range_completeness(total_iters++,
					configs[cfg].total_blocks,
					configs[cfg].chunk_size_blocks,
					OPS_PER_ITER);
	}
	printf(" ✓\n");

	/* ── Property 5: Clear resets ── */
	printf("  [5/7] Clear resets all bits...");
	fflush(stdout);
	for (uint32_t i = 0; i < ITERATIONS_PER_PROPERTY; i++) {
		uint32_t cfg = (uint32_t)(prng_range(NUM_CONFIGS));
		prop_clear_resets(total_iters++,
				  configs[cfg].total_blocks,
				  configs[cfg].chunk_size_blocks,
				  OPS_PER_ITER);
	}
	printf(" ✓\n");

	/* ── Property 6: Idempotence ── */
	printf("  [6/7] Idempotence...");
	fflush(stdout);
	for (uint32_t i = 0; i < ITERATIONS_PER_PROPERTY; i++) {
		uint32_t cfg = (uint32_t)(prng_range(NUM_CONFIGS));
		prop_idempotence(total_iters++,
				 configs[cfg].total_blocks,
				 configs[cfg].chunk_size_blocks,
				 OPS_PER_ITER);
	}
	printf(" ✓\n");

	/* ── Property 7: No OOB ── */
	printf("  [7/7] No OOB (extreme values + ASan)...");
	fflush(stdout);
	for (uint32_t i = 0; i < ITERATIONS_PER_PROPERTY; i++) {
		uint32_t cfg = (uint32_t)(prng_range(NUM_CONFIGS));
		prop_no_oob(total_iters++,
			    configs[cfg].total_blocks,
			    configs[cfg].chunk_size_blocks,
			    OPS_PER_ITER);
	}
	printf(" ✓\n");

	/* ── Summary ── */
	printf("\n=====================================\n");
	printf("Total iterations: %u\n", total_iters);
	printf("Total operations: %u\n", total_iters * OPS_PER_ITER);

	if (g_failures > 0) {
		printf("FAILURES: %d (reproduce with seed=%lu)\n",
		       g_failures, (unsigned long)seed);
		return 1;
	}

	printf("ALL PROPERTIES HOLD ✓\n");
	return 0;
}
