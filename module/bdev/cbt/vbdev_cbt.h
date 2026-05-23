/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Evariops.
 *   All rights reserved.
 */

#ifndef SPDK_VBDEV_CBT_H
#define SPDK_VBDEV_CBT_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/bdev_module.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Compile-time defaults ─────────────────────────────────────────── */

#define CBT_CHUNK_SIZE_DEFAULT_KB       64
#define CBT_CHUNK_SIZE_MIN_KB           4
#define CBT_CHUNK_SIZE_MAX_KB           65536    /* 64 MB */
#define CBT_MAX_EPOCHS                  4
#define CBT_MAX_RANGES_LIMIT            65536
#define CBT_HEALTHY_CLEAR_INTERVAL_US   5000000  /* 5 s */
#define CBT_EPOCH_ID_MAX                64
#define CBT_BACKEND_ID_MAX              128

/* ── Epoch lifecycle ───────────────────────────────────────────────── */

enum cbt_epoch_state {
	CBT_EPOCH_OPEN        = 0,  /* backend stale, bitmap accumulates     */
	CBT_EPOCH_FROZEN      = 1,  /* bitmap_frozen captured for read       */
	CBT_EPOCH_REBUILDING  = 2,  /* partial rebuild in progress           */
	CBT_EPOCH_COMPLETED   = 3,  /* rebuild done, safe to discard         */
	CBT_EPOCH_INVALID     = 4,  /* bitmap lost, fallback full rebuild    */
};

struct cbt_epoch {
	char                    epoch_id[CBT_EPOCH_ID_MAX];
	char                    stale_backend_id[CBT_BACKEND_ID_MAX];
	uint64_t                generation;
	enum cbt_epoch_state    state;

	/* Per-epoch frozen bitmap (allocated on freeze, freed on close). */
	uint8_t                *bitmap_frozen;

	TAILQ_ENTRY(cbt_epoch)  link;
};

/* ── Dirty-range element returned by get_dirty_ranges ──────────────── */

struct cbt_dirty_range {
	uint64_t    offset_blocks;
	uint64_t    length_blocks;
};

/* ── Public API ────────────────────────────────────────────────────── */

/**
 * Create the CBT vbdev wrapper around an existing bdev.
 *
 * \param base_bdev_name  Name of the bdev to wrap (e.g. "nexus_vol123").
 * \param cbt_name        Name for the new CBT vbdev (e.g. "cbt_nexus_vol123").
 * \param chunk_size_kb   Tracking granularity in KiB (default 64).
 * \return 0 on success, negative errno on failure.
 */
int bdev_cbt_create_disk(const char *base_bdev_name, const char *cbt_name,
			 uint32_t chunk_size_kb);

/**
 * Delete the CBT vbdev, releasing the base bdev.
 *
 * \param cb_fn  Required callback. Must not be NULL.
 */
void bdev_cbt_delete_disk(const char *cbt_name,
			  spdk_bdev_unregister_cb cb_fn, void *cb_arg);

/* ── Epoch operations ──────────────────────────────────────────────── */

int bdev_cbt_epoch_open(const char *cbt_name, const char *epoch_id,
			const char *stale_backend_id, uint64_t generation);

int bdev_cbt_epoch_freeze(const char *cbt_name, const char *epoch_id);

int bdev_cbt_epoch_close(const char *cbt_name, const char *epoch_id);

int bdev_cbt_epoch_invalidate(const char *cbt_name, const char *epoch_id);

int bdev_cbt_epoch_rebuild_start(const char *cbt_name, const char *epoch_id);

/**
 * Return dirty ranges from the frozen bitmap of the given epoch.
 * Caller must free *out_ranges with free().
 * If the range list was truncated, *out_truncated is set to true.
 */
int bdev_cbt_epoch_get_dirty_ranges(const char *cbt_name, const char *epoch_id,
				    uint32_t max_ranges,
				    struct cbt_dirty_range **out_ranges,
				    uint32_t *out_count,
				    uint64_t *out_dirty_chunks,
				    uint64_t *out_total_chunks,
				    uint32_t *out_chunk_size_kb,
				    bool *out_truncated);

/* ── Legacy aliases (deprecated, will be removed in v2) ────────────── */

int bdev_cbt_start_tracking(const char *cbt_name);
int bdev_cbt_stop_tracking(const char *cbt_name);
int bdev_cbt_get_dirty_ranges(const char *cbt_name, uint32_t max_ranges,
			      struct cbt_dirty_range **out_ranges,
			      uint32_t *out_count,
			      uint64_t *out_dirty_chunks,
			      uint64_t *out_total_chunks,
			      uint32_t *out_chunk_size_kb,
			      bool *out_truncated);
int bdev_cbt_reset(const char *cbt_name);

/**
 * Notify CBT that all backends are confirmed healthy and in-sync.
 * Only after this call will the healthy-clear poller clear the bitmap.
 */
void bdev_cbt_set_backends_healthy(const char *cbt_name, bool healthy);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_VBDEV_CBT_H */
