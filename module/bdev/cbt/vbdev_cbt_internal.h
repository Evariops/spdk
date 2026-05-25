/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Evariops.
 *   All rights reserved.
 */

#ifndef SPDK_VBDEV_CBT_INTERNAL_H
#define SPDK_VBDEV_CBT_INTERNAL_H

#include "vbdev_cbt.h"
#include "spdk/thread.h"

/* ── Internal structures exposed only to vbdev_cbt_rpc.c ───────────── */

struct cbt_io_channel {
	struct spdk_io_channel *base_ch;
};

struct vbdev_cbt {
	struct spdk_bdev            *base_bdev;
	struct spdk_bdev_desc       *base_desc;
	struct spdk_bdev             cbt_bdev;

	/* ── Cumulative bitmap (always-on) ── */
	uint8_t                     *bitmap;
	uint64_t                     bitmap_size_bits;
	uint64_t                     bitmap_size_bytes;
	uint64_t                     chunk_size_blocks;
	uint32_t                     chunk_size_kb;
	uint32_t                     chunk_shift;    /* log2(chunk_size_blocks) for fast path */
	uint64_t                     total_blocks;

	/* ── Healthy-clear poller ── */
	struct spdk_poller          *healthy_poller;
	bool                         healthy_clear_suspended;
	bool                         backends_healthy;     /* explicit health signal from orchestrator */
	bool                         dirty_history_valid;  /* false after restart until first epoch completes */

	/* ── Epoch management ── */
	TAILQ_HEAD(, cbt_epoch)      epochs;
	uint64_t                     epoch_count;

	/* ── Statistics ── */
	uint64_t                     total_writes_tracked;

	TAILQ_ENTRY(vbdev_cbt)       link;
	struct spdk_thread          *thread;
};

/* Find a CBT vbdev by its registered name. */
struct vbdev_cbt *cbt_find_by_name(const char *name);

/* Compute dirty chunk count via popcount (not on hot path). */
uint64_t cbt_popcount_bitmap(const struct vbdev_cbt *cbt);

#endif /* SPDK_VBDEV_CBT_INTERNAL_H */
