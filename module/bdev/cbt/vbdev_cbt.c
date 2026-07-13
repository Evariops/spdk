/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Evariops.
 *   All rights reserved.
 */

/*
 * SPDK CBT (Change Block Tracking) vbdev module.
 *
 * A passthrough bdev that maintains a cumulative dirty bitmap for every
 * write/unmap/write_zeroes that flows through it.  The bitmap is used to
 * drive incremental (partial) RAID rebuilds after backend outages.
 *
 * Bitmap lifecycle is RESET-DRIVEN (D3): the bitmap only shrinks when the
 * orchestrator explicitly calls bdev_cbt_reset after certifying all backends
 * are healthy and in-sync. There is NO automatic healthy-clear poller — an
 * in-target timer cannot know distributed backend health, and a wrong clear
 * silently destroys the delta needed for partial rebuild.
 *
 * Design reference: SPEC-52 §2.
 */

#include "spdk/stdinc.h"

#include "vbdev_cbt_internal.h"
#include "vbdev_cbt_query.h"	/* Evariops 0014.6: raid-facing epoch facts */
/* Evariops 0014.5: the rebuild outcome registry lives in the raid module (the
 * nexus process hosts both); patch 0015 materializes this header at image
 * build. Linked via --whole-archive, so the cross-module symbol resolves. */
#include "../raid/bdev_raid_outcomes.h"
#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/endian.h"
#include "spdk/string.h"
#include "spdk/thread.h"
#include "spdk/util.h"
#include "spdk/bdev_module.h"
#include "spdk/log.h"
#include "spdk/uuid.h"

/* ================================================================== */
/* Forward declarations                                               */
/* ================================================================== */

static int  vbdev_cbt_init(void);
static void vbdev_cbt_finish(void);
static int  vbdev_cbt_get_ctx_size(void);
static void vbdev_cbt_examine(struct spdk_bdev *bdev);
static int  vbdev_cbt_config_json(struct spdk_json_write_ctx *w);
static uint64_t cbt_count_dirty_bits(const uint8_t *bitmap, uint64_t size_bytes);

/* ================================================================== */
/* Module registration                                                */
/* ================================================================== */

static struct spdk_bdev_module cbt_if = {
	.name           = "cbt",
	.module_init    = vbdev_cbt_init,
	.get_ctx_size   = vbdev_cbt_get_ctx_size,
	.examine_config = vbdev_cbt_examine,
	.module_fini    = vbdev_cbt_finish,
	.config_json    = vbdev_cbt_config_json,
};

SPDK_BDEV_MODULE_REGISTER(cbt, &cbt_if)

/* ================================================================== */
/* Internal structures                                                */
/* ================================================================== */

/* Deferred-create entry: remembered until the base bdev appears. */
struct cbt_bdev_name {
	char                        *vbdev_name;
	char                        *bdev_name;
	uint32_t                     chunk_size_kb;
	TAILQ_ENTRY(cbt_bdev_name)  link;
};
static TAILQ_HEAD(, cbt_bdev_name) g_bdev_names =
	TAILQ_HEAD_INITIALIZER(g_bdev_names);

static TAILQ_HEAD(, vbdev_cbt) g_cbt_nodes =
	TAILQ_HEAD_INITIALIZER(g_cbt_nodes);

/* Per-IO context (embedded in spdk_bdev_io->driver_ctx). */
struct cbt_bdev_io {
	struct spdk_io_channel         *ch;
	struct spdk_bdev_io_wait_entry  bdev_io_wait;
};

/* ================================================================== */
/* Helpers                                                            */
/* ================================================================== */

/* Lazy popcount — compute dirty_chunks on demand (called from poller/RPCs,
 * not from the IO hot path). Uses 64-bit popcount for speed on large bitmaps.
 */
uint64_t
cbt_popcount_bitmap(const struct vbdev_cbt *cbt)
{
	const uint8_t *src = cbt->bitmap;
	uint64_t n = cbt->bitmap_size_bytes / 8;
	uint64_t tail = cbt->bitmap_size_bytes % 8;
	uint64_t count = 0;

	for (uint64_t i = 0; i < n; i++) {
		uint64_t word;
		memcpy(&word, src + i * 8, sizeof(word));
		count += (uint64_t)__builtin_popcountll(word);
	}
	/* Handle tail bytes. */
	if (tail > 0) {
		const uint8_t *rest = src + n * 8;
		for (uint64_t i = 0; i < tail; i++) {
			count += (uint64_t)__builtin_popcount(rest[i]);
		}
	}
	return count;
}

struct vbdev_cbt *
cbt_find_by_name(const char *name)
{
	struct vbdev_cbt *node;

	TAILQ_FOREACH(node, &g_cbt_nodes, link) {
		if (strcmp(spdk_bdev_get_name(&node->cbt_bdev), name) == 0) {
			return node;
		}
	}
	return NULL;
}

static struct cbt_epoch *
cbt_find_epoch(struct vbdev_cbt *cbt, const char *epoch_id)
{
	struct cbt_epoch *ep;

	TAILQ_FOREACH(ep, &cbt->epochs, link) {
		if (strcmp(ep->epoch_id, epoch_id) == 0) {
			return ep;
		}
	}
	return NULL;
}

static bool
cbt_any_epoch_open(struct vbdev_cbt *cbt)
{
	struct cbt_epoch *ep;

	TAILQ_FOREACH(ep, &cbt->epochs, link) {
		if (ep->state == CBT_EPOCH_OPEN || ep->state == CBT_EPOCH_FROZEN ||
		    ep->state == CBT_EPOCH_REBUILDING) {
			return true;
		}
	}
	return false;
}

/* Epochs share the single live bitmap: snapshot-and-clear at freeze is only
 * safe when no OTHER epoch still needs the accumulated view. */
static bool
cbt_has_other_active_epoch(struct vbdev_cbt *cbt, struct cbt_epoch *self)
{
	struct cbt_epoch *ep;

	TAILQ_FOREACH(ep, &cbt->epochs, link) {
		if (ep == self) {
			continue;
		}
		if (ep->state == CBT_EPOCH_OPEN || ep->state == CBT_EPOCH_FROZEN ||
		    ep->state == CBT_EPOCH_REBUILDING) {
			return true;
		}
	}
	return false;
}

/* H1: OR an unconsumed frozen delta back into the live bitmap before its
 * buffer is discarded (re-freeze, close, evict). Bits exchanged out of the
 * live bitmap at freeze exist ONLY in bitmap_frozen until a rebuild COMPLETES;
 * freeing that buffer without this merge-back permanently loses the chunks —
 * a later "successful" delta rebuild then promotes a silently divergent
 * member under skip_rebuild.
 *
 * Pessimistic by design: chunks the aborted rebuild DID copy are re-marked
 * too and get re-copied on the next iteration — wasted bandwidth, never lost
 * data. Lock-free: per-byte atomic OR, same discipline as the IO-thread
 * markers; caller is the app thread and all call sites are guarded against a
 * concurrently RUNNING rebuild, so bitmap_frozen has no concurrent reader. */
static void
cbt_epoch_restore_unconsumed_delta(struct vbdev_cbt *cbt, struct cbt_epoch *ep)
{
	uint64_t restored = 0;

	if (!ep->frozen_live_consumed || ep->bitmap_frozen == NULL) {
		return;
	}
	for (uint64_t i = 0; i < cbt->bitmap_size_bytes; i++) {
		uint8_t b = ep->bitmap_frozen[i];
		if (b != 0) {
			__atomic_fetch_or(&cbt->bitmap[i], b, __ATOMIC_RELAXED);
			restored += (uint64_t)__builtin_popcount(b);
		}
	}
	ep->frozen_live_consumed = false;
	SPDK_NOTICELOG("CBT: epoch '%s' unconsumed delta merged back into live bitmap "
		       "(%lu chunks)\n", ep->epoch_id, (unsigned long)restored);
}

/* ================================================================== */
/* Bitmap operations (hot path — may run on any reactor thread)       */
/* Uses atomic OR so concurrent IO threads cannot lose bits.          */
/*                                                                    */
/* Performance design:                                                */
/*   - chunk_shift replaces division (chunk_size guaranteed P2)       */
/*   - No atomic counter increment per chunk (dirty_chunks is         */
/*     recomputed lazily via popcount when needed by RPCs/poller)     */
/*   - total_writes_tracked uses relaxed add (stats only)            */
/* ================================================================== */

/* Core bit-setter, shared by the submit-time mark and the completion-time
 * re-mark (H2). No statistics side effect. */
static inline void
cbt_mark_dirty_bits(struct vbdev_cbt *cbt, uint64_t offset_blocks, uint64_t num_blocks)
{
	uint64_t chunk_start, chunk_end;

	/* Reject zero-length (would underflow chunk_end). */
	if (num_blocks == 0 || cbt->bitmap_size_bits == 0) {
		return;
	}

	chunk_start = offset_blocks >> cbt->chunk_shift;
	chunk_end   = (offset_blocks + num_blocks - 1) >> cbt->chunk_shift;

	/* Clamp to bitmap bounds. */
	if (chunk_end >= cbt->bitmap_size_bits) {
		chunk_end = cbt->bitmap_size_bits - 1;
	}

	/* Fast path: set full bytes for large ranges. */
	uint64_t byte_start = chunk_start >> 3;
	uint64_t byte_end   = chunk_end >> 3;

	if (byte_start == byte_end) {
		/* All bits in a single byte. */
		uint8_t mask = 0;
		for (uint64_t i = chunk_start; i <= chunk_end; i++) {
			mask |= (uint8_t)(1u << (i & 7));
		}
		__atomic_fetch_or(&cbt->bitmap[byte_start], mask, __ATOMIC_RELAXED);
	} else {
		/* First partial byte. */
		uint8_t first_mask = (uint8_t)(0xFF << (chunk_start & 7));
		__atomic_fetch_or(&cbt->bitmap[byte_start], first_mask, __ATOMIC_RELAXED);

		/* Full bytes in between — use memset for large spans. */
		uint64_t full_start = byte_start + 1;
		uint64_t full_end   = byte_end;  /* exclusive */
		if (full_end > full_start) {
			/* For full bytes, 0xFF is idempotent with OR, so direct set is safe.
			 * Concurrent atomic ORs on the same byte can only add bits. */
			memset(&cbt->bitmap[full_start], 0xFF, full_end - full_start);
		}

		/* Last partial byte. */
		uint8_t last_mask = (uint8_t)(0xFF >> (7 - (chunk_end & 7)));
		__atomic_fetch_or(&cbt->bitmap[byte_end], last_mask, __ATOMIC_RELAXED);
	}
}

static inline void
cbt_mark_dirty(struct vbdev_cbt *cbt, uint64_t offset_blocks, uint64_t num_blocks)
{
	cbt_mark_dirty_bits(cbt, offset_blocks, num_blocks);
	__atomic_fetch_add(&cbt->total_writes_tracked, 1, __ATOMIC_RELAXED);
}

/* D3: the healthy-clear poller was removed — it required an explicit
 * bdev_cbt_set_backends_healthy() signal that no caller ever sent (dead code),
 * so the bitmap only tended towards 100% dirty. Clearing is reset-driven:
 * the orchestrator calls bdev_cbt_reset (refused while any epoch is active). */

/* Forward declaration (rebuild registry, defined below) — used by the epoch
 * state guards CBT-1/CBT-2/c5. */
struct cbt_rebuild_ctx;
static struct cbt_rebuild_ctx *cbt_rebuild_find_active_for_epoch(const struct vbdev_cbt *cbt,
								 const char *epoch_id);

/* ================================================================== */
/* IO forwarding (passthrough + tracking)                             */
/* ================================================================== */

static void vbdev_cbt_submit_request(struct spdk_io_channel *ch,
				     struct spdk_bdev_io *bdev_io);

static void
cbt_init_ext_io_opts(struct spdk_bdev_io *bdev_io, struct spdk_bdev_ext_io_opts *opts)
{
	memset(opts, 0, sizeof(*opts));
	opts->size = sizeof(*opts);
	opts->memory_domain = bdev_io->u.bdev.memory_domain;
	opts->memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;
	opts->metadata = bdev_io->u.bdev.md_buf;
	opts->dif_check_flags_exclude_mask = ~bdev_io->u.bdev.dif_check_flags;
}

static void
_cbt_complete_io(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;

	/* H2: RE-mark the dirty bit at completion, BEFORE acking the host.
	 *
	 * The submit-time mark alone is not enough under snapshot-and-clear
	 * freeze: a freeze can exchange-and-consume the bit while this write is
	 * still in flight to the base. The rebuild may then read the chunk
	 * before the write lands, and the bit exists in no bitmap afterwards —
	 * the chunk silently diverges forever. Re-marking here closes that
	 * window without any drain or freeze of host I/O:
	 *   - write landed before the rebuild read  → the consumed bit is fine,
	 *     the rebuild copies the new data;
	 *   - write lands after                     → this re-mark puts the bit
	 *     in the (new) live bitmap → captured by the next delta.
	 * Re-mark also on FAILURE: a failed write may have partially reached
	 * media. The submit-time mark stays for crash conservatism.
	 * Cost: one relaxed atomic OR on a byte whose cacheline the submit-time
	 * mark touched moments ago. */
	switch (orig_io->type) {
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_COPY:
		cbt_mark_dirty_bits(SPDK_CONTAINEROF(orig_io->bdev, struct vbdev_cbt, cbt_bdev),
				    orig_io->u.bdev.offset_blocks,
				    orig_io->u.bdev.num_blocks);
		break;
	default:
		break;
	}

	spdk_bdev_io_complete_base_io_status(orig_io, bdev_io);
	spdk_bdev_free_io(bdev_io);
}

static void
cbt_resubmit_io(void *arg)
{
	struct spdk_bdev_io *bdev_io = arg;
	struct cbt_bdev_io  *io_ctx = (struct cbt_bdev_io *)bdev_io->driver_ctx;

	vbdev_cbt_submit_request(io_ctx->ch, bdev_io);
}

static void
cbt_queue_io(struct spdk_bdev_io *bdev_io)
{
	struct cbt_bdev_io   *io_ctx = (struct cbt_bdev_io *)bdev_io->driver_ctx;
	struct cbt_io_channel *cbt_ch = spdk_io_channel_get_ctx(io_ctx->ch);
	int rc;

	io_ctx->bdev_io_wait.bdev   = bdev_io->bdev;
	io_ctx->bdev_io_wait.cb_fn  = cbt_resubmit_io;
	io_ctx->bdev_io_wait.cb_arg = bdev_io;

	rc = spdk_bdev_queue_io_wait(bdev_io->bdev, cbt_ch->base_ch,
				     &io_ctx->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("CBT: queue io failed rc=%d\n", rc);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
cbt_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io,
		    bool success)
{
	struct vbdev_cbt     *cbt_node = SPDK_CONTAINEROF(bdev_io->bdev,
							  struct vbdev_cbt, cbt_bdev);
	struct cbt_io_channel *cbt_ch  = spdk_io_channel_get_ctx(ch);
	struct cbt_bdev_io    *io_ctx  = (struct cbt_bdev_io *)bdev_io->driver_ctx;
	struct spdk_bdev_ext_io_opts io_opts;
	int rc;

	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	cbt_init_ext_io_opts(bdev_io, &io_opts);
	rc = spdk_bdev_readv_blocks_ext(cbt_node->base_desc, cbt_ch->base_ch,
				    bdev_io->u.bdev.iovs, bdev_io->u.bdev.iovcnt,
				    bdev_io->u.bdev.offset_blocks,
				    bdev_io->u.bdev.num_blocks,
				    _cbt_complete_io, bdev_io, &io_opts);
	if (rc == -ENOMEM) {
		io_ctx->ch = ch;
		cbt_queue_io(bdev_io);
	} else if (rc != 0) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
vbdev_cbt_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_cbt      *cbt_node = SPDK_CONTAINEROF(bdev_io->bdev,
							   struct vbdev_cbt, cbt_bdev);
	struct cbt_io_channel *cbt_ch   = spdk_io_channel_get_ctx(ch);
	struct cbt_bdev_io    *io_ctx   = (struct cbt_bdev_io *)bdev_io->driver_ctx;
	int rc = 0;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, cbt_read_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		break;

	case SPDK_BDEV_IO_TYPE_WRITE:
		cbt_mark_dirty(cbt_node, bdev_io->u.bdev.offset_blocks,
			       bdev_io->u.bdev.num_blocks);
		{
			struct spdk_bdev_ext_io_opts io_opts;
			cbt_init_ext_io_opts(bdev_io, &io_opts);
			rc = spdk_bdev_writev_blocks_ext(cbt_node->base_desc, cbt_ch->base_ch,
						     bdev_io->u.bdev.iovs,
						     bdev_io->u.bdev.iovcnt,
						     bdev_io->u.bdev.offset_blocks,
						     bdev_io->u.bdev.num_blocks,
						     _cbt_complete_io, bdev_io, &io_opts);
		}
		break;

	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		cbt_mark_dirty(cbt_node, bdev_io->u.bdev.offset_blocks,
			       bdev_io->u.bdev.num_blocks);
		rc = spdk_bdev_write_zeroes_blocks(cbt_node->base_desc, cbt_ch->base_ch,
						   bdev_io->u.bdev.offset_blocks,
						   bdev_io->u.bdev.num_blocks,
						   _cbt_complete_io, bdev_io);
		break;

	case SPDK_BDEV_IO_TYPE_UNMAP:
		cbt_mark_dirty(cbt_node, bdev_io->u.bdev.offset_blocks,
			       bdev_io->u.bdev.num_blocks);
		rc = spdk_bdev_unmap_blocks(cbt_node->base_desc, cbt_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _cbt_complete_io, bdev_io);
		break;

	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush_blocks(cbt_node->base_desc, cbt_ch->base_ch,
					    bdev_io->u.bdev.offset_blocks,
					    bdev_io->u.bdev.num_blocks,
					    _cbt_complete_io, bdev_io);
		break;

	case SPDK_BDEV_IO_TYPE_RESET:
		rc = spdk_bdev_reset(cbt_node->base_desc, cbt_ch->base_ch,
				     _cbt_complete_io, bdev_io);
		break;

	case SPDK_BDEV_IO_TYPE_ABORT:
		rc = spdk_bdev_abort(cbt_node->base_desc, cbt_ch->base_ch,
				     bdev_io->u.abort.bio_to_abort,
				     _cbt_complete_io, bdev_io);
		break;

	case SPDK_BDEV_IO_TYPE_COPY:
		/* Copy implies destination is modified. */
		cbt_mark_dirty(cbt_node, bdev_io->u.bdev.offset_blocks,
			       bdev_io->u.bdev.num_blocks);
		rc = spdk_bdev_copy_blocks(cbt_node->base_desc, cbt_ch->base_ch,
					   bdev_io->u.bdev.offset_blocks,
					   bdev_io->u.bdev.copy.src_offset_blocks,
					   bdev_io->u.bdev.num_blocks,
					   _cbt_complete_io, bdev_io);
		break;

	default:
		/* Forward unknown IO types without tracking (zcopy, compare,
		 * zone ops, seek, etc.) — they don't modify data.
		 */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (rc != 0) {
		if (rc == -ENOMEM) {
			io_ctx->ch = ch;
			cbt_queue_io(bdev_io);
		} else {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		}
	}
}

/* ================================================================== */
/* bdev function table                                                */
/* ================================================================== */

static bool
vbdev_cbt_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct vbdev_cbt *cbt_node = ctx;

	/* Only advertise IO types we explicitly handle in submit_request. */
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_FLUSH:
	case SPDK_BDEV_IO_TYPE_RESET:
	case SPDK_BDEV_IO_TYPE_ABORT:
	case SPDK_BDEV_IO_TYPE_COPY:
		return spdk_bdev_io_type_supported(cbt_node->base_bdev, io_type);
	default:
		return false;
	}
}

static struct spdk_io_channel *
vbdev_cbt_get_io_channel(void *ctx)
{
	struct vbdev_cbt *cbt_node = ctx;

	return spdk_get_io_channel(cbt_node);
}

/* 0014.6 (cbt part): stable state names for get_bdevs consumers. */
static const char *
cbt_epoch_state_name(enum cbt_epoch_state state)
{
	switch (state) {
	case CBT_EPOCH_OPEN:        return "open";
	case CBT_EPOCH_FROZEN:      return "frozen";
	case CBT_EPOCH_REBUILDING:  return "rebuilding";
	case CBT_EPOCH_COMPLETED:   return "completed";
	case CBT_EPOCH_INVALID:     return "invalid";
	default:                    return "unknown";
	}
}

/* 0014.10 (RPC-CONTRACT §12): epoch-at-ejection — see vbdev_cbt_query.h.
 * Refuses (-EEXIST) when an OPEN epoch already bounds the round: the CP's own
 * round (or a previous auto-open) suffices, and an implicit takeover would
 * steal the nonce from under the controller. FROZEN/REBUILDING epochs do not
 * block: those are prior rounds being digested, while the live bitmap keeps
 * tracking — the new epoch bounds the NEW ejection. */
int
vbdev_cbt_auto_epoch_open(const char *bdev_name, const char *stale_backend_id)
{
	struct vbdev_cbt *cbt;
	struct cbt_epoch *ep;
	char epoch_id[CBT_EPOCH_ID_MAX];
	char nonce[CBT_NONCE_MAX];
	uint64_t max_gen = 0;
	uint64_t ticks = spdk_get_ticks();

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	cbt = cbt_find_by_name(bdev_name);
	if (cbt == NULL) {
		return -ENODEV;
	}

	TAILQ_FOREACH(ep, &cbt->epochs, link) {
		if (ep->state == CBT_EPOCH_OPEN) {
			return -EEXIST;
		}
		if (ep->generation > max_gen) {
			max_gen = ep->generation;
		}
	}

	snprintf(epoch_id, sizeof(epoch_id), "auto-%016" PRIx64, ticks);
	snprintf(nonce, sizeof(nonce), "auto%08x", (uint32_t)ticks);

	SPDK_NOTICELOG("CBT: auto epoch '%s' (nonce %s) on '%s' — member '%s' ejected\n",
		       epoch_id, nonce, bdev_name, stale_backend_id);

	return bdev_cbt_epoch_open(bdev_name, epoch_id, stale_backend_id,
				   max_gen + 1, nonce);
}

/* 0014.6 (RPC-CONTRACT §3): cross-module query — the raid module publishes
 * these facts per member in ITS get_bdevs output (see vbdev_cbt_query.h).
 * Epochs are appended at open, so the last list entry is the most recently
 * opened one; closed epochs leave the list, so everything here is live. */
int
vbdev_cbt_query_latest_epoch(const char *bdev_name, struct vbdev_cbt_epoch_facts *out)
{
	struct vbdev_cbt *cbt;
	struct cbt_epoch *ep, *latest = NULL;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	cbt = cbt_find_by_name(bdev_name);
	if (cbt == NULL) {
		return -ENODEV;
	}

	TAILQ_FOREACH(ep, &cbt->epochs, link) {
		latest = ep;
	}
	if (latest == NULL) {
		return -ENOENT;
	}

	snprintf(out->nonce, sizeof(out->nonce), "%s", latest->nonce);
	out->state = cbt_epoch_state_name(latest->state);
	out->truncated = latest->truncated;

	return 0;
}

static int
vbdev_cbt_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct vbdev_cbt *cbt_node = ctx;
	struct cbt_epoch *ep;

	spdk_json_write_name(w, "cbt");
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name",
				     spdk_bdev_get_name(&cbt_node->cbt_bdev));
	spdk_json_write_named_string(w, "base_bdev_name",
				     spdk_bdev_get_name(cbt_node->base_bdev));
	spdk_json_write_named_uint32(w, "chunk_size_kb", cbt_node->chunk_size_kb);
	spdk_json_write_named_uint64(w, "dirty_chunks", cbt_popcount_bitmap(cbt_node));
	spdk_json_write_named_uint64(w, "total_chunks", cbt_node->bitmap_size_bits);

	/* 0014.1/0014.2/0014.6 (RPC-CONTRACT §3/§5): the observation source — the
	 * control-plane's EpochObservation is built from get_bdevs, never from a
	 * dedicated poll. Per epoch: {epoch_id, nonce, state, generation, truncated}. */
	spdk_json_write_named_array_begin(w, "epochs");
	TAILQ_FOREACH(ep, &cbt_node->epochs, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "epoch_id", ep->epoch_id);
		spdk_json_write_named_string(w, "nonce", ep->nonce);
		spdk_json_write_named_string(w, "state", cbt_epoch_state_name(ep->state));
		spdk_json_write_named_uint64(w, "generation", ep->generation);
		spdk_json_write_named_bool(w, "truncated", ep->truncated);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	spdk_json_write_object_end(w);

	return 0;
}

static void
vbdev_cbt_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/* No per-bdev config needed. */
}

static int
vbdev_cbt_get_memory_domains(void *ctx, struct spdk_memory_domain **domains,
			     int array_size)
{
	struct vbdev_cbt *cbt_node = ctx;

	return spdk_bdev_get_memory_domains(cbt_node->base_bdev, domains, array_size);
}

static void
_cbt_device_unregister_cb(void *io_device)
{
	struct vbdev_cbt *cbt_node = io_device;

	free(cbt_node->bitmap);
	free(cbt_node->cbt_bdev.name);

	/* Free any remaining epochs and their frozen bitmaps. */
	struct cbt_epoch *ep;
	while ((ep = TAILQ_FIRST(&cbt_node->epochs)) != NULL) {
		TAILQ_REMOVE(&cbt_node->epochs, ep, link);
		free(ep->bitmap_frozen);
		free(ep);
	}

	free(cbt_node);
}

static void
_cbt_base_bdev_close(void *ctx)
{
	spdk_bdev_close((struct spdk_bdev_desc *)ctx);
}

static int
vbdev_cbt_destruct(void *ctx)
{
	struct vbdev_cbt *cbt_node = ctx;

	TAILQ_REMOVE(&g_cbt_nodes, cbt_node, link);

	spdk_bdev_module_release_bdev(cbt_node->base_bdev);

	if (cbt_node->thread && cbt_node->thread != spdk_get_thread()) {
		spdk_thread_send_msg(cbt_node->thread, _cbt_base_bdev_close,
				     cbt_node->base_desc);
	} else {
		spdk_bdev_close(cbt_node->base_desc);
	}

	spdk_io_device_unregister(cbt_node, _cbt_device_unregister_cb);
	return 0;
}

static const struct spdk_bdev_fn_table vbdev_cbt_fn_table = {
	.destruct           = vbdev_cbt_destruct,
	.submit_request     = vbdev_cbt_submit_request,
	.io_type_supported  = vbdev_cbt_io_type_supported,
	.get_io_channel     = vbdev_cbt_get_io_channel,
	.dump_info_json     = vbdev_cbt_dump_info_json,
	.write_config_json  = vbdev_cbt_write_config_json,
	.get_memory_domains = vbdev_cbt_get_memory_domains,
};

/* ================================================================== */
/* Channel create / destroy                                           */
/* ================================================================== */

static int
cbt_bdev_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct cbt_io_channel *cbt_ch = ctx_buf;
	struct vbdev_cbt      *cbt_node = io_device;

	cbt_ch->base_ch = spdk_bdev_get_io_channel(cbt_node->base_desc);
	if (!cbt_ch->base_ch) {
		SPDK_ERRLOG("CBT: failed to get base IO channel\n");
		return -ENOMEM;
	}
	return 0;
}

static void
cbt_bdev_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct cbt_io_channel *cbt_ch = ctx_buf;

	spdk_put_io_channel(cbt_ch->base_ch);
}

/* ================================================================== */
/* Hot-remove callback                                                */
/* ================================================================== */

static void
vbdev_cbt_base_bdev_event_cb(enum spdk_bdev_event_type type,
			     struct spdk_bdev *bdev, void *event_ctx)
{
	if (type == SPDK_BDEV_EVENT_RESIZE) {
		/* 0014.2 (RPC-CONTRACT §5, T-D6): the live bitmap was sized at create —
		 * a grown base leaves the growth zone tracked by NOTHING. Every live
		 * epoch becomes a lie: mark it truncated so the control-plane routes to
		 * a FULL rebuild (D14) instead of trusting a partial delta. */
		struct vbdev_cbt *rnode;
		struct cbt_epoch *rep;

		TAILQ_FOREACH(rnode, &g_cbt_nodes, link) {
			if (bdev != rnode->base_bdev) {
				continue;
			}
			TAILQ_FOREACH(rep, &rnode->epochs, link) {
				if (!rep->truncated) {
					rep->truncated = true;
					SPDK_WARNLOG("CBT: base bdev '%s' resized — epoch "
						     "'%s' marked truncated (delta no longer "
						     "covers the device)\n",
						     spdk_bdev_get_name(bdev), rep->epoch_id);
				}
			}
		}
		return;
	}

	if (type == SPDK_BDEV_EVENT_REMOVE) {
		struct vbdev_cbt *node, *tmp;
		struct cbt_bdev_name *name, *ntmp;

		TAILQ_FOREACH_SAFE(node, &g_cbt_nodes, link, tmp) {
			if (bdev == node->base_bdev) {
				/* c4: drop the deferred-create entry too — otherwise a
				 * reappearing base bdev silently recreates the cbt vbdev
				 * with a VIRGIN bitmap that masquerades as continuous
				 * tracking history. Recreation must be an explicit
				 * bdev_cbt_create from the orchestrator. */
				TAILQ_FOREACH_SAFE(name, &g_bdev_names, link, ntmp) {
					if (strcmp(name->vbdev_name,
						   spdk_bdev_get_name(&node->cbt_bdev)) == 0) {
						TAILQ_REMOVE(&g_bdev_names, name, link);
						free(name->bdev_name);
						free(name->vbdev_name);
						free(name);
					}
				}
				spdk_bdev_unregister(&node->cbt_bdev, NULL, NULL);
			}
		}
	}
}

/* ================================================================== */
/* Registration (internal — called from examine or RPC)               */
/* ================================================================== */

static int
vbdev_cbt_register(const char *bdev_name)
{
	struct cbt_bdev_name *name;
	struct vbdev_cbt     *cbt_node;
	struct spdk_bdev     *bdev;
	int rc = 0;

	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(name->bdev_name, bdev_name) != 0) {
			continue;
		}

		cbt_node = calloc(1, sizeof(*cbt_node));
		if (!cbt_node) {
			return -ENOMEM;
		}

		TAILQ_INIT(&cbt_node->epochs);

		cbt_node->cbt_bdev.name = strdup(name->vbdev_name);
		if (!cbt_node->cbt_bdev.name) {
			free(cbt_node);
			return -ENOMEM;
		}
		cbt_node->cbt_bdev.product_name = "cbt";

		rc = spdk_bdev_open_ext(bdev_name, true,
					vbdev_cbt_base_bdev_event_cb,
					NULL, &cbt_node->base_desc);
		if (rc) {
			if (rc != -ENODEV) {
				SPDK_ERRLOG("CBT: cannot open bdev %s\n", bdev_name);
			}
			free(cbt_node->cbt_bdev.name);
			free(cbt_node);
			return rc;
		}

		bdev = spdk_bdev_desc_get_bdev(cbt_node->base_desc);
		cbt_node->base_bdev = bdev;

		/* ── Compute bitmap dimensions ── */
		cbt_node->chunk_size_kb     = name->chunk_size_kb;
		cbt_node->chunk_size_blocks = ((uint64_t)name->chunk_size_kb * 1024) /
					      bdev->blocklen;
		if (cbt_node->chunk_size_blocks == 0) {
			cbt_node->chunk_size_blocks = 1;
		}

		/* Ensure chunk_size_blocks is a power of 2 for fast-path shift. */
		if ((cbt_node->chunk_size_blocks & (cbt_node->chunk_size_blocks - 1)) != 0) {
			/* Round up to next power of 2. */
			uint64_t v = cbt_node->chunk_size_blocks;
			v--;
			v |= v >> 1; v |= v >> 2; v |= v >> 4;
			v |= v >> 8; v |= v >> 16; v |= v >> 32;
			cbt_node->chunk_size_blocks = v + 1;
		}
		cbt_node->chunk_shift = (uint32_t)__builtin_ctzll(cbt_node->chunk_size_blocks);

		cbt_node->bitmap_size_bits  = (bdev->blockcnt + cbt_node->chunk_size_blocks - 1) /
					      cbt_node->chunk_size_blocks;
		cbt_node->bitmap_size_bytes = (cbt_node->bitmap_size_bits + 7) / 8;

		cbt_node->total_blocks = bdev->blockcnt;

		cbt_node->bitmap = calloc(1, cbt_node->bitmap_size_bytes);
		if (!cbt_node->bitmap) {
			SPDK_ERRLOG("CBT: bitmap allocation failed (%lu bytes)\n",
				    (unsigned long)cbt_node->bitmap_size_bytes);
			spdk_bdev_close(cbt_node->base_desc);
			free(cbt_node->cbt_bdev.name);
			free(cbt_node);
			return -ENOMEM;
		}

		/* ── Copy geometry from base bdev ── */
		cbt_node->cbt_bdev.write_cache        = bdev->write_cache;
		cbt_node->cbt_bdev.required_alignment  = bdev->required_alignment;
		cbt_node->cbt_bdev.optimal_io_boundary = bdev->optimal_io_boundary;
		cbt_node->cbt_bdev.blocklen            = bdev->blocklen;
		cbt_node->cbt_bdev.blockcnt            = bdev->blockcnt;
		cbt_node->cbt_bdev.md_interleave       = bdev->md_interleave;
		cbt_node->cbt_bdev.md_len              = bdev->md_len;
		cbt_node->cbt_bdev.dif_type            = bdev->dif_type;
		cbt_node->cbt_bdev.dif_is_head_of_md   = bdev->dif_is_head_of_md;
		cbt_node->cbt_bdev.dif_check_flags     = bdev->dif_check_flags;
		cbt_node->cbt_bdev.dif_pi_format       = bdev->dif_pi_format;
		cbt_node->cbt_bdev.numa                = bdev->numa;

		/* ── Generate stable UUID from base bdev UUID ── */
		{
			static const char cbt_ns_uuid_str[] = "a1b2c3d4-e5f6-4a7b-8c9d-0e1f2a3b4c5d";
			struct spdk_uuid ns_uuid;
			spdk_uuid_parse(&ns_uuid, cbt_ns_uuid_str);
			spdk_uuid_generate_sha1(&cbt_node->cbt_bdev.uuid, &ns_uuid,
						(const char *)&bdev->uuid,
						sizeof(struct spdk_uuid));
		}

		cbt_node->cbt_bdev.ctxt      = cbt_node;
		cbt_node->cbt_bdev.fn_table  = &vbdev_cbt_fn_table;
		cbt_node->cbt_bdev.module    = &cbt_if;

		TAILQ_INSERT_TAIL(&g_cbt_nodes, cbt_node, link);

		spdk_io_device_register(cbt_node,
					cbt_bdev_ch_create_cb,
					cbt_bdev_ch_destroy_cb,
					sizeof(struct cbt_io_channel),
					name->vbdev_name);

		cbt_node->thread = spdk_get_thread();

		rc = spdk_bdev_module_claim_bdev(bdev, cbt_node->base_desc,
						 cbt_node->cbt_bdev.module);
		if (rc) {
			SPDK_ERRLOG("CBT: cannot claim bdev %s\n", bdev_name);
			spdk_bdev_close(cbt_node->base_desc);
			TAILQ_REMOVE(&g_cbt_nodes, cbt_node, link);
			spdk_io_device_unregister(cbt_node, NULL);
			free(cbt_node->bitmap);
			free(cbt_node->cbt_bdev.name);
			free(cbt_node);
			return rc;
		}

		rc = spdk_bdev_register(&cbt_node->cbt_bdev);
		if (rc) {
			SPDK_ERRLOG("CBT: cannot register bdev %s\n", name->vbdev_name);
			spdk_bdev_module_release_bdev(bdev);
			spdk_bdev_close(cbt_node->base_desc);
			TAILQ_REMOVE(&g_cbt_nodes, cbt_node, link);
			spdk_io_device_unregister(cbt_node, NULL);
			free(cbt_node->bitmap);
			free(cbt_node->cbt_bdev.name);
			free(cbt_node);
			return rc;
		}

		SPDK_NOTICELOG("CBT: created vbdev '%s' over '%s' "
			       "(chunk=%u KB, bitmap=%lu bytes, %lu chunks)\n",
			       name->vbdev_name, bdev_name,
			       cbt_node->chunk_size_kb,
			       (unsigned long)cbt_node->bitmap_size_bytes,
			       (unsigned long)cbt_node->bitmap_size_bits);
	}

	return rc;
}

/* ================================================================== */
/* Public API: create / delete                                        */
/* ================================================================== */

static int
cbt_insert_name(const char *bdev_name, const char *vbdev_name, uint32_t chunk_size_kb)
{
	struct cbt_bdev_name *name;

	TAILQ_FOREACH(name, &g_bdev_names, link) {
		if (strcmp(vbdev_name, name->vbdev_name) == 0) {
			SPDK_ERRLOG("CBT: vbdev %s already exists\n", vbdev_name);
			return -EEXIST;
		}
	}

	name = calloc(1, sizeof(*name));
	if (!name) {
		return -ENOMEM;
	}

	name->bdev_name = strdup(bdev_name);
	name->vbdev_name = strdup(vbdev_name);
	if (!name->bdev_name || !name->vbdev_name) {
		free(name->bdev_name);
		free(name->vbdev_name);
		free(name);
		return -ENOMEM;
	}
	name->chunk_size_kb = chunk_size_kb ? chunk_size_kb : CBT_CHUNK_SIZE_DEFAULT_KB;

	TAILQ_INSERT_TAIL(&g_bdev_names, name, link);
	return 0;
}

int
bdev_cbt_create_disk(const char *base_bdev_name, const char *cbt_name,
		     uint32_t chunk_size_kb)
{
	struct cbt_bdev_name *entry;
	int rc;

	rc = cbt_insert_name(base_bdev_name, cbt_name, chunk_size_kb);
	if (rc) {
		return rc;
	}

	rc = vbdev_cbt_register(base_bdev_name);
	if (rc == -ENODEV) {
		SPDK_NOTICELOG("CBT: vbdev creation deferred for base bdev '%s'\n",
			       base_bdev_name);
		rc = 0;
	} else if (rc != 0) {
		/* Registration failed — rollback the name entry. */
		TAILQ_FOREACH(entry, &g_bdev_names, link) {
			if (strcmp(entry->vbdev_name, cbt_name) == 0) {
				TAILQ_REMOVE(&g_bdev_names, entry, link);
				free(entry->bdev_name);
				free(entry->vbdev_name);
				free(entry);
				break;
			}
		}
	}
	return rc;
}

void
bdev_cbt_delete_disk(const char *cbt_name,
		     spdk_bdev_unregister_cb cb_fn, void *cb_arg)
{
	struct cbt_bdev_name *name;
	int rc;

	assert(cb_fn != NULL);

	rc = spdk_bdev_unregister_by_name(cbt_name, &cbt_if, cb_fn, cb_arg);
	if (rc == 0) {
		TAILQ_FOREACH(name, &g_bdev_names, link) {
			if (strcmp(name->vbdev_name, cbt_name) == 0) {
				TAILQ_REMOVE(&g_bdev_names, name, link);
				free(name->bdev_name);
				free(name->vbdev_name);
				free(name);
				break;
			}
		}
	} else if (rc == -ENODEV) {
		/* The bdev doesn't exist (deferred create never completed).
		 * Clean up the deferred entry from g_bdev_names.
		 */
		TAILQ_FOREACH(name, &g_bdev_names, link) {
			if (strcmp(name->vbdev_name, cbt_name) == 0) {
				TAILQ_REMOVE(&g_bdev_names, name, link);
				free(name->bdev_name);
				free(name->vbdev_name);
				free(name);
				cb_fn(cb_arg, 0);
				return;
			}
		}
		cb_fn(cb_arg, rc);
	} else {
		cb_fn(cb_arg, rc);
	}
}

/* ================================================================== */
/* Public API: epoch operations                                       */
/* ================================================================== */

int
bdev_cbt_epoch_open(const char *cbt_name, const char *epoch_id,
		    const char *stale_backend_id, uint64_t generation,
		    const char *nonce)
{
	struct vbdev_cbt *cbt;
	struct cbt_epoch *ep;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	cbt = cbt_find_by_name(cbt_name);
	if (!cbt) {
		return -ENODEV;
	}

	/* Validate epoch_id length. */
	if (strlen(epoch_id) >= CBT_EPOCH_ID_MAX) {
		return -ENAMETOOLONG;
	}
	if (strlen(stale_backend_id) >= CBT_BACKEND_ID_MAX) {
		return -ENAMETOOLONG;
	}
	/* 0014.1: the nonce is opaque and optional (pre-0014 callers pass NULL). */
	if (nonce != NULL && strlen(nonce) >= CBT_NONCE_MAX) {
		return -ENAMETOOLONG;
	}

	/* Check if epoch already exists. */
	ep = cbt_find_epoch(cbt, epoch_id);
	if (ep) {
		if (generation > ep->generation) {
			/* c5 (requalified): a higher-generation takeover must NOT rip the
			 * epoch away from a rebuild that is scanning/writing its frozen
			 * bitmap — that corrupts the state machine and opens the CBT-1 UAF.
			 * Gate on an ACTUALLY-RUNNING rebuild, not on ep->state: a COMPLETED
			 * rebuild deliberately leaves the epoch in REBUILDING (finalize keeps
			 * it there), so keying on the state would refuse every later takeover
			 * forever (cancel_rebuild returns -EINVAL — no running rebuild — so
			 * the flow would deadlock). */
			if (cbt_rebuild_find_active_for_epoch(cbt, epoch_id) != NULL) {
				SPDK_ERRLOG("CBT: epoch_open gen=%lu refused: epoch '%s' has an "
					    "active rebuild\n", (unsigned long)generation, epoch_id);
				return -EBUSY;
			}
			/* Replace with higher generation. 0014.1: the takeover carries the
			 * NEW round's nonce; 0014.2: `truncated` deliberately survives —
			 * the bitmap still does not cover a growth zone from a resize. */
			ep->generation = generation;
			snprintf(ep->stale_backend_id, sizeof(ep->stale_backend_id),
				 "%s", stale_backend_id);
			snprintf(ep->nonce, sizeof(ep->nonce), "%s", nonce ? nonce : "");
			ep->state = CBT_EPOCH_OPEN;
			return 0;
		}
		return -EEXIST;
	}

	if (cbt->epoch_count >= CBT_MAX_EPOCHS) {
		/* Evict the oldest epoch only if it is safe to do so.
		 * Never evict an epoch that is actively being used for rebuild.
		 */
		struct cbt_epoch *oldest = TAILQ_FIRST(&cbt->epochs);
		if (!oldest || oldest->state == CBT_EPOCH_FROZEN ||
		    oldest->state == CBT_EPOCH_REBUILDING ||
		    oldest->state == CBT_EPOCH_OPEN) {
			SPDK_ERRLOG("CBT: max epochs reached, cannot evict "
				    "active epoch '%s' (state=%d)\n",
				    oldest ? oldest->epoch_id : "?",
				    oldest ? (int)oldest->state : -1);
			return -ENOSPC;
		}
		/* C3 (defense-in-depth): with the epoch_invalidate rebuild guard an
		 * INVALID epoch cannot have a RUNNING rebuild (rebuild_start requires
		 * FROZEN/REBUILDING), but never free an epoch a rebuild ctx still
		 * points at — that is a UAF read (ctx->bitmap) AND write (finalize). */
		if (cbt_rebuild_find_active_for_epoch(cbt, oldest->epoch_id) != NULL) {
			SPDK_ERRLOG("CBT: max epochs reached, epoch '%s' has an active "
				    "rebuild — refusing eviction\n", oldest->epoch_id);
			return -ENOSPC;
		}
		/* Safe to evict: COMPLETED or INVALID. */
		SPDK_WARNLOG("CBT: max epochs reached, evicting '%s'\n",
			     oldest->epoch_id);
		/* H1: an INVALID epoch may still hold an unconsumed exchanged delta. */
		cbt_epoch_restore_unconsumed_delta(cbt, oldest);
		TAILQ_REMOVE(&cbt->epochs, oldest, link);
		cbt->epoch_count--;
		free(oldest->bitmap_frozen);
		free(oldest);
	}

	ep = calloc(1, sizeof(*ep));
	if (!ep) {
		return -ENOMEM;
	}

	snprintf(ep->epoch_id, sizeof(ep->epoch_id), "%s", epoch_id);
	snprintf(ep->stale_backend_id, sizeof(ep->stale_backend_id),
		 "%s", stale_backend_id);
	snprintf(ep->nonce, sizeof(ep->nonce), "%s", nonce ? nonce : "");
	ep->generation = generation;
	ep->state = CBT_EPOCH_OPEN;

	TAILQ_INSERT_TAIL(&cbt->epochs, ep, link);
	cbt->epoch_count++;

	SPDK_NOTICELOG("CBT: epoch_open '%s' nonce='%s' for stale backend '%s' gen=%lu\n",
		       epoch_id, ep->nonce, stale_backend_id, (unsigned long)generation);
	return 0;
}

int
bdev_cbt_epoch_freeze(const char *cbt_name, const char *epoch_id)
{
	struct vbdev_cbt *cbt;
	struct cbt_epoch *ep;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	cbt = cbt_find_by_name(cbt_name);
	if (!cbt) {
		return -ENODEV;
	}

	ep = cbt_find_epoch(cbt, epoch_id);
	if (!ep) {
		return -ENOENT;
	}
	if (ep->state != CBT_EPOCH_OPEN && ep->state != CBT_EPOCH_FROZEN &&
	    ep->state != CBT_EPOCH_REBUILDING) {
		return -EINVAL;
	}
	/* CBT-1: a RUNNING rebuild holds ctx->bitmap = ep->bitmap_frozen and scans it
	 * asynchronously — freeing/reallocating it here is a use-after-free read.
	 * Same guard start_rebuild already applies, made symmetric. */
	if (cbt_rebuild_find_active_for_epoch(cbt, epoch_id) != NULL) {
		SPDK_ERRLOG("CBT: epoch_freeze '%s' refused: rebuild in progress\n", epoch_id);
		return -EBUSY;
	}

	/* H1: allocate the new snapshot BEFORE touching the old one — an ENOMEM
	 * must leave the epoch exactly as it was (the old code freed the only
	 * copy of the previous delta first, so a failed malloc bricked the epoch
	 * AND lost the un-copied chunks). */
	uint8_t *new_frozen = malloc(cbt->bitmap_size_bytes);
	if (!new_frozen) {
		return -ENOMEM;
	}

	/* H1: re-freeze after a FAILED/ABORTED rebuild — the previous delta was
	 * exchanged out of the live bitmap and its un-copied chunks exist only in
	 * the old bitmap_frozen. Merge it back first: the exchange below then
	 * re-captures (old unconsumed delta ∪ writes since last freeze), which is
	 * exactly the correct retry set. */
	if (ep->bitmap_frozen != NULL) {
		cbt_epoch_restore_unconsumed_delta(cbt, ep);
		free(ep->bitmap_frozen);
	}
	ep->bitmap_frozen = new_frozen;

	/* Snapshot the current bitmap into this epoch.
	 *
	 * Thread safety: IO threads write individual bits with atomic OR.
	 * We read the bitmap here on the app thread. On x86/arm64, each byte
	 * read is atomic, so we never see a torn byte. Any write that completed
	 * its IO callback before this function was called is guaranteed to be
	 * in the snapshot. A write still in flight may have its submit-time bit
	 * consumed by the exchange below, but that is harmless (H2): the
	 * completion path RE-marks the bit before acking the host, so the chunk
	 * either was copied with the new data (write landed before the rebuild
	 * read) or lands in the next delta (bit re-set in the live bitmap). No
	 * host-I/O drain is required around freeze. */
	__atomic_thread_fence(__ATOMIC_ACQUIRE);

	if (!cbt_has_other_active_epoch(cbt, ep)) {
		/* Snapshot-AND-CLEAR (atomic per-byte exchange): each freeze captures the
		 * DELTA since the previous freeze, so iterative partial rebuilds converge
		 * geometrically once copy_rate > dirty_rate. The previous accumulate-only
		 * semantics recopied the union of everything dirtied since epoch open on
		 * every iteration — residual was monotonically non-decreasing and the
		 * convergence loop could not terminate by design.
		 *
		 * Exchange (not memcpy+memset): a bit OR'd by an IO thread between the
		 * copy and the clear would be LOST — a missed chunk under skip_rebuild
		 * is silent data divergence. The exchange makes each concurrent OR land
		 * either before (captured in this snapshot) or after (tracked for the
		 * next delta). */
		for (uint64_t i = 0; i < cbt->bitmap_size_bytes; i++) {
			ep->bitmap_frozen[i] =
				__atomic_exchange_n(&cbt->bitmap[i], 0, __ATOMIC_ACQ_REL);
		}
		/* H1: these bits now exist ONLY here until a rebuild COMPLETES. */
		ep->frozen_live_consumed = true;
	} else {
		/* Another epoch still consumes the accumulated live view — snapshot only.
		 * Convergence is degraded (residual includes prior deltas) but correct. */
		memcpy(ep->bitmap_frozen, cbt->bitmap, cbt->bitmap_size_bytes);
		ep->frozen_live_consumed = false;	/* live bitmap untouched */
		SPDK_NOTICELOG("CBT: epoch_freeze '%s' without clear (other active epochs)\n",
			       epoch_id);
	}
	ep->state = CBT_EPOCH_FROZEN;

	SPDK_NOTICELOG("CBT: epoch_freeze '%s' (dirty=%lu/%lu, live_after=%lu)\n",
		       epoch_id,
		       (unsigned long)cbt_count_dirty_bits(ep->bitmap_frozen,
				       cbt->bitmap_size_bytes),
		       (unsigned long)cbt->bitmap_size_bits,
		       (unsigned long)cbt_popcount_bitmap(cbt));
	return 0;
}

int
bdev_cbt_epoch_close(const char *cbt_name, const char *epoch_id,
		     enum cbt_epoch_close_mode mode,
		     const char *rebuild_token)
{
	struct vbdev_cbt *cbt;
	struct cbt_epoch *ep;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	cbt = cbt_find_by_name(cbt_name);
	if (!cbt) {
		return -ENODEV;
	}

	ep = cbt_find_epoch(cbt, epoch_id);
	if (!ep) {
		return -ENOENT;
	}

	/* Only FROZEN, REBUILDING, or COMPLETED epochs can be closed. */
	if (ep->state == CBT_EPOCH_OPEN) {
		return -EINVAL;
	}
	/* CBT-2: a RUNNING rebuild writes ctx->epoch->bitmap_frozen and
	 * ctx->epoch->state at completion — freeing the epoch under it is a
	 * use-after-free WRITE. Cancel the rebuild first. */
	if (cbt_rebuild_find_active_for_epoch(cbt, epoch_id) != NULL) {
		SPDK_ERRLOG("CBT: epoch_close '%s' refused: rebuild in progress\n", epoch_id);
		return -EBUSY;
	}

	/* 0014.3 (RPC-CONTRACT §4): CONSUMED deliberately discards the frozen delta —
	 * the caller certifies it was copied by a verified-successful rebuild. The
	 * certification is the outcome-registry token (0014.5): it must name a LOCAL
	 * registry entry in state succeeded+verified — `verified` is set by the
	 * integrated verify phase (0014.8, K sampled windows against the arbiter
	 * leg). Without that proof: -EPERM, never a silent downgrade to PRESERVE. */
	if (mode == CBT_EPOCH_CLOSE_CONSUMED) {
		if (rebuild_token == NULL || rebuild_token[0] == '\0') {
			SPDK_ERRLOG("CBT: epoch_close '%s' mode=consumed refused: no "
				    "rebuild token (-EPERM)\n", epoch_id);
			return -EPERM;
		}
		if (!raid_rebuild_outcome_is_succeeded_verified(rebuild_token)) {
			SPDK_ERRLOG("CBT: epoch_close '%s' mode=consumed refused: token "
				    "'%s' is not a succeeded+verified rebuild outcome "
				    "(-EPERM)\n", epoch_id, rebuild_token);
			return -EPERM;
		}
		SPDK_NOTICELOG("CBT: epoch_close '%s' CONSUMED under token '%s' — "
			       "frozen delta discarded by certification\n",
			       epoch_id, rebuild_token);
	} else {
		/* H1: closing an epoch must not lose dirty history. If the frozen delta
		 * was exchanged out of the live bitmap and never proven copied (rebuild
		 * aborted/failed/never run), merge it back before discarding. */
		cbt_epoch_restore_unconsumed_delta(cbt, ep);
	}

	ep->state = CBT_EPOCH_COMPLETED;
	TAILQ_REMOVE(&cbt->epochs, ep, link);
	cbt->epoch_count--;
	free(ep->bitmap_frozen);
	free(ep);

	SPDK_NOTICELOG("CBT: epoch_close '%s' (%s)\n", epoch_id,
		       mode == CBT_EPOCH_CLOSE_CONSUMED ? "consumed" : "preserve");
	return 0;
}

int
bdev_cbt_epoch_invalidate(const char *cbt_name, const char *epoch_id)
{
	struct vbdev_cbt *cbt;
	struct cbt_epoch *ep;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	cbt = cbt_find_by_name(cbt_name);
	if (!cbt) {
		return -ENODEV;
	}

	ep = cbt_find_epoch(cbt, epoch_id);
	if (!ep) {
		return -ENOENT;
	}

	/* C3: same guard as freeze (CBT-1) and close (CBT-2). Invalidating a
	 * REBUILDING epoch makes it evictable by epoch_open's max-epochs path,
	 * which would free ep->bitmap_frozen and ep under the RUNNING rebuild —
	 * UAF read in the chunk scanner, UAF write in finalize. */
	if (cbt_rebuild_find_active_for_epoch(cbt, epoch_id) != NULL) {
		SPDK_ERRLOG("CBT: epoch_invalidate '%s' refused: rebuild in progress "
			    "(cancel it first)\n", epoch_id);
		return -EBUSY;
	}

	ep->state = CBT_EPOCH_INVALID;
	SPDK_WARNLOG("CBT: epoch_invalidate '%s' → full rebuild required\n", epoch_id);
	return 0;
}

int
bdev_cbt_epoch_rebuild_start(const char *cbt_name, const char *epoch_id)
{
	struct vbdev_cbt *cbt;
	struct cbt_epoch *ep;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	cbt = cbt_find_by_name(cbt_name);
	if (!cbt) {
		return -ENODEV;
	}

	ep = cbt_find_epoch(cbt, epoch_id);
	if (!ep) {
		return -ENOENT;
	}

	if (ep->state != CBT_EPOCH_FROZEN) {
		return -EINVAL;
	}

	ep->state = CBT_EPOCH_REBUILDING;
	SPDK_NOTICELOG("CBT: epoch_rebuild_start '%s'\n", epoch_id);
	return 0;
}

int
bdev_cbt_epoch_get_dirty_ranges(const char *cbt_name, const char *epoch_id,
				uint32_t max_ranges,
				struct cbt_dirty_range **out_ranges,
				uint32_t *out_count,
				uint64_t *out_dirty_chunks,
				uint64_t *out_total_chunks,
				uint32_t *out_chunk_size_kb,
				bool *out_truncated)
{
	struct vbdev_cbt *cbt;
	struct cbt_epoch *ep;
	const uint8_t    *bmap;
	uint64_t          i;
	uint32_t          count = 0;
	uint64_t          dirty = 0;
	bool              truncated = false;
	struct cbt_dirty_range *ranges;
	uint32_t          cap;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	cbt = cbt_find_by_name(cbt_name);
	if (!cbt) {
		return -ENODEV;
	}

	ep = cbt_find_epoch(cbt, epoch_id);
	if (!ep) {
		return -ENOENT;
	}
	if (ep->state != CBT_EPOCH_FROZEN && ep->state != CBT_EPOCH_REBUILDING) {
		return -EINVAL;
	}
	if (!ep->bitmap_frozen) {
		return -EINVAL;
	}

	bmap = ep->bitmap_frozen;
	cap  = max_ranges ? max_ranges : 4096;
	if (cap > CBT_MAX_RANGES_LIMIT) {
		cap = CBT_MAX_RANGES_LIMIT;
	}
	ranges = calloc(cap, sizeof(*ranges));
	if (!ranges) {
		return -ENOMEM;
	}

	/* Walk the frozen bitmap and coalesce contiguous dirty chunks. */
	int64_t run_start = -1;

	for (i = 0; i < cbt->bitmap_size_bits; i++) {
		bool is_dirty = (bmap[i / 8] & (1u << (i % 8))) != 0;

		if (is_dirty) {
			dirty++;
			if (run_start < 0) {
				run_start = (int64_t)i;
			}
		}

		if (!is_dirty || i == cbt->bitmap_size_bits - 1) {
			if (run_start >= 0) {
				uint64_t end = is_dirty ? i : i - 1;

				if (count < cap) {
					uint64_t offset = (uint64_t)run_start * cbt->chunk_size_blocks;
					uint64_t length = (end - (uint64_t)run_start + 1) *
							   cbt->chunk_size_blocks;

					/* Clamp the tail range to actual device size. */
					if (offset + length > cbt->total_blocks) {
						length = cbt->total_blocks - offset;
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

	*out_ranges       = ranges;
	*out_count        = count;
	*out_dirty_chunks = dirty;
	*out_total_chunks = cbt->bitmap_size_bits;
	*out_chunk_size_kb = cbt->chunk_size_kb;
	*out_truncated    = truncated;

	return 0;
}

/* ================================================================== */
/* Partial rebuild — async copy of dirty chunks                       */
/* ================================================================== */

struct cbt_rebuild_io_slot {
	void     *buf;
	uint64_t  chunk_offset_blocks;
	uint64_t  chunk_length_blocks;
	uint64_t  read_start_tsc;
	uint64_t  write_start_tsc;
	bool      in_use;
};

struct cbt_rebuild_ctx {
	struct vbdev_cbt         *cbt;
	struct cbt_epoch         *epoch;
	struct spdk_bdev_desc    *src_desc;
	struct spdk_bdev_desc    *dst_desc;
	/* Lifetime pin (UAF guard): `cbt` and `epoch`/`bitmap` are BARE pointers into
	 * the cbt vbdev, but the rebuild reads its geometry/bitmap on every chunk. A
	 * base hot-remove unregisters+frees the cbt vbdev; when the read source is the
	 * cbt bdev itself (default) src_desc already pins it, but with a source_bdev_name
	 * OVERRIDE nothing did, so the free raced the rebuild. This extra descriptor on
	 * the cbt bdev makes SPDK defer the cbt destruct until the rebuild's cleanup
	 * closes it. NULL when src_desc already covers the cbt bdev. */
	struct spdk_bdev_desc    *cbt_pin_desc;
	struct spdk_io_channel   *src_ch;
	struct spdk_io_channel   *dst_ch;

	/* Bitmap to walk (frozen bitmap or override ranges) */
	const uint8_t            *bitmap;
	struct cbt_rebuild_range *override_ranges;
	uint32_t                  num_ranges;
	uint32_t                  current_range_idx;

	/* Bitmap scan position */
	uint64_t                  current_bit;
	uint64_t                  total_dirty_chunks; /* precomputed for progress */

	/* Progress */
	uint64_t                  chunks_copied;
	uint64_t                  bytes_copied;
	uint64_t                  start_tsc;
	int                       outstanding_ios;
	int                       max_outstanding;

	/* Bandwidth throttle */
	uint64_t                  max_bytes_per_sec;
	uint64_t                  bytes_this_window;
	uint64_t                  window_start_tsc;
	struct spdk_poller       *throttle_poller;
	bool                      throttled;

	/* DMA buffer slots */
	struct cbt_rebuild_io_slot *slots;
	int                       num_slots;

	/* Completion */
	cbt_rebuild_done_cb       cb_fn;
	void                     *cb_arg;
	bool                      aborted;
	bool                      cancelled;  /* graceful cancel (drain, don't abort) */
	int                       error;

	/* ── Async model (Phase 2) ── */
	char                      rebuild_id[CBT_REBUILD_ID_MAX];
	enum cbt_rebuild_state    state;
	uint64_t                  completion_tsc;  /* TSC when finished (for GC) */
	struct cbt_rebuild_result final_result;    /* stored for get_status queries */
	TAILQ_ENTRY(cbt_rebuild_ctx) registry_link;

	/* ── Instrumentation counters ── */
	uint64_t                  submit_calls;
	uint64_t                  submit_no_slot;
	uint64_t                  submit_throttled;
	uint64_t                  total_read_tsc;
	uint64_t                  total_write_tsc;
	uint64_t                  max_read_tsc;
	uint64_t                  max_write_tsc;
	uint64_t                  qd_sum;
	uint64_t                  qd_samples;
	uint64_t                  ios_coalesced;
};

/* ── Global rebuild registry ── */
static TAILQ_HEAD(cbt_rebuild_registry_head, cbt_rebuild_ctx) g_rebuild_registry =
	TAILQ_HEAD_INITIALIZER(g_rebuild_registry);
static uint64_t g_rebuild_id_counter = 0;
static struct spdk_poller *g_rebuild_gc_poller = NULL;

static struct cbt_rebuild_ctx *
cbt_rebuild_find_by_id(const char *rebuild_id)
{
	struct cbt_rebuild_ctx *ctx;
	TAILQ_FOREACH(ctx, &g_rebuild_registry, registry_link) {
		if (strcmp(ctx->rebuild_id, rebuild_id) == 0) {
			return ctx;
		}
	}
	return NULL;
}

/* New: discriminate by cbt node too — two cbt vbdevs may legitimately use the
 * same epoch_id (the old name-only match made them block each other). */
static struct cbt_rebuild_ctx *
cbt_rebuild_find_active_for_epoch(const struct vbdev_cbt *cbt, const char *epoch_id)
{
	struct cbt_rebuild_ctx *ctx;
	TAILQ_FOREACH(ctx, &g_rebuild_registry, registry_link) {
		if (ctx->state == CBT_REBUILD_RUNNING && ctx->cbt == cbt &&
		    strcmp(ctx->epoch->epoch_id, epoch_id) == 0) {
			return ctx;
		}
	}
	return NULL;
}

static void
cbt_rebuild_registry_cleanup(struct cbt_rebuild_ctx *ctx)
{
	if (ctx->throttle_poller) {
		spdk_poller_unregister(&ctx->throttle_poller);
	}
	if (ctx->src_ch) {
		spdk_put_io_channel(ctx->src_ch);
	}
	if (ctx->dst_ch) {
		spdk_put_io_channel(ctx->dst_ch);
	}
	if (ctx->src_desc) {
		spdk_bdev_close(ctx->src_desc);
	}
	if (ctx->dst_desc) {
		spdk_bdev_close(ctx->dst_desc);
	}
	if (ctx->cbt_pin_desc) {
		/* Releasing the lifetime pin: lets a cbt destruct deferred by a hot-remove
		 * finally proceed (see cbt_pin_desc). */
		spdk_bdev_close(ctx->cbt_pin_desc);
		ctx->cbt_pin_desc = NULL;
	}
	for (int i = 0; i < ctx->num_slots; i++) {
		if (ctx->slots[i].buf) {
			spdk_dma_free(ctx->slots[i].buf);
		}
	}
	free(ctx->slots);
	ctx->slots = NULL;
	free(ctx->override_ranges);
	ctx->override_ranges = NULL;
}

static int
cbt_rebuild_gc_poller_fn(void *arg)
{
	struct cbt_rebuild_ctx *ctx, *tmp;
	uint64_t now = spdk_get_ticks();
	uint64_t gc_tsc = (uint64_t)CBT_REBUILD_GC_DELAY_US *
			  spdk_get_ticks_hz() / 1000000;
	bool any_removed = false;

	TAILQ_FOREACH_SAFE(ctx, &g_rebuild_registry, registry_link, tmp) {
		if (ctx->state == CBT_REBUILD_RUNNING) {
			continue;
		}
		if (now - ctx->completion_tsc > gc_tsc) {
			TAILQ_REMOVE(&g_rebuild_registry, ctx, registry_link);
			free(ctx);
			any_removed = true;
		}
	}

	(void)any_removed;
	return SPDK_POLLER_IDLE;
}

static uint64_t
cbt_count_dirty_bits(const uint8_t *bitmap, uint64_t size_bytes)
{
	uint64_t count = 0;
	uint64_t n = size_bytes / 8;
	uint64_t tail = size_bytes % 8;
	for (uint64_t i = 0; i < n; i++) {
		uint64_t word;
		memcpy(&word, bitmap + i * 8, sizeof(word));
		count += (uint64_t)__builtin_popcountll(word);
	}
	for (uint64_t i = 0; i < tail; i++) {
		count += (uint64_t)__builtin_popcount(bitmap[n * 8 + i]);
	}
	return count;
}

static void cbt_rebuild_submit_next(struct cbt_rebuild_ctx *ctx);
static void cbt_rebuild_finish(struct cbt_rebuild_ctx *ctx);

static uint64_t
cbt_get_tsc_hz(void)
{
	return spdk_get_ticks_hz();
}

static struct cbt_rebuild_io_slot *
cbt_rebuild_get_free_slot(struct cbt_rebuild_ctx *ctx)
{
	for (int i = 0; i < ctx->num_slots; i++) {
		if (!ctx->slots[i].in_use) {
			return &ctx->slots[i];
		}
	}
	return NULL;
}

static void
cbt_rebuild_base_event_cb(enum spdk_bdev_event_type type,
			  struct spdk_bdev *bdev, void *event_ctx)
{
	struct cbt_rebuild_ctx *ctx = event_ctx;

	/* A REMOVE of the source, target, OR the pinned cbt bdev aborts the rebuild.
	 * Marking aborted stops it at the next chunk boundary; the in-flight I/O
	 * drains and the terminal path closes every descriptor (including the cbt
	 * lifetime pin) — only then does a hot-remove-driven cbt destruct proceed.
	 * When the read source is the cbt bdev itself and no I/O is in flight the old
	 * "let the I/O fail" heuristic could stall, so set the flag explicitly. */
	if (type == SPDK_BDEV_EVENT_REMOVE && ctx != NULL &&
	    ctx->state == CBT_REBUILD_RUNNING) {
		SPDK_WARNLOG("CBT rebuild: bdev '%s' removed — aborting\n",
			     spdk_bdev_get_name(bdev));
		ctx->aborted = true;
	}
}

/* Maximum number of chunks to coalesce into a single I/O.
 * 16 chunks × 64KB = 1 MiB max I/O size — sweet spot for NVMe-oF TCP.
 */
#define CBT_REBUILD_MAX_COALESCE_CHUNKS  16

/* Find the next contiguous run of dirty bits starting from ctx->current_bit.
 * Returns the starting bit and the run length (in chunks), up to max_chunks.
 * Returns false if no more dirty bits. */
static bool
cbt_rebuild_find_next_dirty_run(struct cbt_rebuild_ctx *ctx,
				uint64_t *out_start_bit, uint64_t *out_run_len)
{
	const uint8_t *bmap = ctx->bitmap;
	uint64_t total = ctx->cbt->bitmap_size_bits;
	uint64_t start;
	uint64_t run;

	/* Find first dirty bit. */
	while (ctx->current_bit < total) {
		uint64_t byte_idx = ctx->current_bit >> 3;
		uint8_t byte_val = bmap[byte_idx];

		if (byte_val == 0) {
			ctx->current_bit = (byte_idx + 1) << 3;
			continue;
		}

		uint8_t bit_pos = ctx->current_bit & 7;
		if (byte_val & (1u << bit_pos)) {
			goto found_start;
		}
		ctx->current_bit++;
	}
	return false;

found_start:
	start = ctx->current_bit;
	run = 1;
	ctx->current_bit++;

	/* Extend the run while consecutive bits are dirty, up to max coalesce. */
	while (run < CBT_REBUILD_MAX_COALESCE_CHUNKS && ctx->current_bit < total) {
		uint64_t byte_idx = ctx->current_bit >> 3;
		uint8_t bit_pos = ctx->current_bit & 7;
		if (!(bmap[byte_idx] & (1u << bit_pos))) {
			break;
		}
		run++;
		ctx->current_bit++;
	}

	*out_start_bit = start;
	*out_run_len = run;
	return true;
}

/* Find the next range to copy when using override_ranges. */
static bool
cbt_rebuild_find_next_range(struct cbt_rebuild_ctx *ctx,
			    uint64_t *out_offset, uint64_t *out_length)
{
	if (ctx->current_range_idx >= ctx->num_ranges) {
		return false;
	}
	*out_offset = ctx->override_ranges[ctx->current_range_idx].offset_blocks;
	*out_length = ctx->override_ranges[ctx->current_range_idx].length_blocks;
	ctx->current_range_idx++;
	return true;
}

static void
cbt_rebuild_write_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct cbt_rebuild_ctx *ctx = cb_arg;
	struct cbt_rebuild_io_slot *slot = NULL;

	/* Find which slot this write belongs to by matching the buffer. */
	for (int i = 0; i < ctx->num_slots; i++) {
		if (ctx->slots[i].in_use && ctx->slots[i].buf == bdev_io->iov.iov_base) {
			slot = &ctx->slots[i];
			break;
		}
	}

	spdk_bdev_free_io(bdev_io);
	ctx->outstanding_ios--;

	if (!success) {
		ctx->error = -EIO;
		ctx->aborted = true;
	} else if (slot) {
		/* Instrumentation: write latency */
		uint64_t write_tsc = spdk_get_ticks() - slot->write_start_tsc;
		ctx->total_write_tsc += write_tsc;
		if (write_tsc > ctx->max_write_tsc) {
			ctx->max_write_tsc = write_tsc;
		}

		ctx->chunks_copied++;
		ctx->bytes_copied += slot->chunk_length_blocks *
				     ctx->cbt->cbt_bdev.blocklen;
		ctx->bytes_this_window += slot->chunk_length_blocks *
					  ctx->cbt->cbt_bdev.blocklen;
	}

	if (slot) {
		slot->in_use = false;
	}

	if ((ctx->aborted || ctx->cancelled) && ctx->outstanding_ios == 0) {
		cbt_rebuild_finish(ctx);
		return;
	}

	cbt_rebuild_submit_next(ctx);
}

static void
cbt_rebuild_read_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct cbt_rebuild_ctx *ctx = cb_arg;
	struct cbt_rebuild_io_slot *slot = NULL;
	int rc;

	/* Find the slot. */
	for (int i = 0; i < ctx->num_slots; i++) {
		if (ctx->slots[i].in_use && ctx->slots[i].buf == bdev_io->iov.iov_base) {
			slot = &ctx->slots[i];
			break;
		}
	}

	spdk_bdev_free_io(bdev_io);

	if (!success || !slot) {
		ctx->outstanding_ios--;
		if (slot) {
			slot->in_use = false;
		}
		ctx->error = -EIO;
		ctx->aborted = true;
		if (ctx->outstanding_ios == 0) {
			cbt_rebuild_finish(ctx);
		}
		return;
	}

	/* Instrumentation: read latency */
	uint64_t read_tsc = spdk_get_ticks() - slot->read_start_tsc;
	ctx->total_read_tsc += read_tsc;
	if (read_tsc > ctx->max_read_tsc) {
		ctx->max_read_tsc = read_tsc;
	}

	/* Write the data to the target bdev. */
	slot->write_start_tsc = spdk_get_ticks();
	rc = spdk_bdev_write(ctx->dst_desc, ctx->dst_ch,
			     slot->buf,
			     slot->chunk_offset_blocks * ctx->cbt->cbt_bdev.blocklen,
			     slot->chunk_length_blocks * ctx->cbt->cbt_bdev.blocklen,
			     cbt_rebuild_write_cb, ctx);
	if (rc != 0) {
		slot->in_use = false;
		ctx->outstanding_ios--;
		ctx->error = rc;
		ctx->aborted = true;
		if (ctx->outstanding_ios == 0) {
			cbt_rebuild_finish(ctx);
		}
	}
}

static int
cbt_rebuild_throttle_poller_fn(void *arg)
{
	struct cbt_rebuild_ctx *ctx = arg;
	uint64_t now = spdk_get_ticks();
	uint64_t elapsed_us = (now - ctx->window_start_tsc) * 1000000 / cbt_get_tsc_hz();

	/* Reset window every second. */
	if (elapsed_us >= 1000000) {
		ctx->bytes_this_window = 0;
		ctx->window_start_tsc = now;
		if (ctx->throttled) {
			ctx->throttled = false;
			/* R10: on the legacy path cbt_rebuild_submit_next may drain the last
			 * outstanding I/O and free(ctx) (submit_next → finish → finalize →
			 * TAILQ_REMOVE + free). Do NOT dereference ctx afterward. We un-throttled
			 * and drove work, so report BUSY without touching ctx; finalize already
			 * unregistered this poller, so the return value is only ever consumed
			 * while ctx is still alive. */
			cbt_rebuild_submit_next(ctx);
			return SPDK_POLLER_BUSY;
		}
	}

	/* Not reached via submit_next → ctx is alive here. */
	return ctx->throttled ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static void
cbt_rebuild_submit_next(struct cbt_rebuild_ctx *ctx)
{
	struct cbt_rebuild_io_slot *slot;
	uint64_t offset_blocks, length_blocks;
	int rc;

	ctx->submit_calls++;
	ctx->qd_sum += (uint64_t)ctx->outstanding_ios;
	ctx->qd_samples++;

	while (!ctx->aborted && !ctx->cancelled &&
	       ctx->outstanding_ios < ctx->max_outstanding) {
		/* Bandwidth throttle check. */
		if (ctx->max_bytes_per_sec > 0 &&
		    ctx->bytes_this_window >= ctx->max_bytes_per_sec) {
			ctx->throttled = true;
			ctx->submit_throttled++;
			break;
		}

		slot = cbt_rebuild_get_free_slot(ctx);
		if (!slot) {
			ctx->submit_no_slot++;
			break;
		}

		/* Get next chunk(s) to copy. */
		if (ctx->override_ranges) {
			if (!cbt_rebuild_find_next_range(ctx, &offset_blocks, &length_blocks)) {
				goto done_scanning;
			}
		} else {
			uint64_t start_bit, run_len;
			if (!cbt_rebuild_find_next_dirty_run(ctx, &start_bit, &run_len)) {
				goto done_scanning;
			}
			offset_blocks = start_bit * ctx->cbt->chunk_size_blocks;
			length_blocks = run_len * ctx->cbt->chunk_size_blocks;
			/* Clamp to device size. */
			if (offset_blocks + length_blocks > ctx->cbt->total_blocks) {
				length_blocks = ctx->cbt->total_blocks - offset_blocks;
			}
			if (run_len > 1) {
				ctx->ios_coalesced += run_len - 1;
			}
		}

		slot->in_use = true;
		slot->chunk_offset_blocks = offset_blocks;
		slot->chunk_length_blocks = length_blocks;
		slot->read_start_tsc = spdk_get_ticks();
		ctx->outstanding_ios++;

		rc = spdk_bdev_read(ctx->src_desc, ctx->src_ch,
				    slot->buf,
				    offset_blocks * ctx->cbt->cbt_bdev.blocklen,
				    length_blocks * ctx->cbt->cbt_bdev.blocklen,
				    cbt_rebuild_read_cb, ctx);
		if (rc != 0) {
			slot->in_use = false;
			ctx->outstanding_ios--;
			ctx->error = rc;
			ctx->aborted = true;
			break;
		}
	}

	/* If no outstanding IOs and we're done or aborted, finish. */
	if (ctx->outstanding_ios == 0) {
		cbt_rebuild_finish(ctx);
	}
	return;

done_scanning:
	/* No more dirty bits/ranges. Wait for outstanding IOs to drain. */
	if (ctx->outstanding_ios == 0) {
		cbt_rebuild_finish(ctx);
	}
}

static void cbt_rebuild_finalize(struct cbt_rebuild_ctx *ctx);

static void
cbt_rebuild_flush_cb(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct cbt_rebuild_ctx *ctx = cb_arg;

	spdk_bdev_free_io(bdev_io);
	if (!success) {
		SPDK_ERRLOG("CBT rebuild: target flush failed — member NOT durably synced\n");
		ctx->error = -EIO;
		ctx->aborted = true;
	}
	cbt_rebuild_finalize(ctx);
}

/* CBT-4: all chunk writes landed — FLUSH the target before declaring the rebuild
 * COMPLETED. Without it the copied chunks may sit in a volatile write cache; a
 * power cut then leaves a lacunary member that the control-plane believes synced. */
static void
cbt_rebuild_finish(struct cbt_rebuild_ctx *ctx)
{
	/* M1: !aborted — a hot-remove abort must reach finalize with error == 0
	 * so R1 classifies it ABORTED (resumable), not FAILED. Flushing the
	 * (possibly removed) target here would fail, overwrite the
	 * classification, and durability of a partial copy is moot anyway: the
	 * resume re-copies from the merged-back delta (H1). */
	if (ctx->error == 0 && !ctx->cancelled && !ctx->aborted &&
	    ctx->chunks_copied > 0 &&
	    ctx->dst_desc != NULL && ctx->dst_ch != NULL) {
		struct spdk_bdev *dst = spdk_bdev_desc_get_bdev(ctx->dst_desc);

		if (spdk_bdev_io_type_supported(dst, SPDK_BDEV_IO_TYPE_FLUSH)) {
			int rc = spdk_bdev_flush_blocks(ctx->dst_desc, ctx->dst_ch, 0,
							spdk_bdev_get_num_blocks(dst),
							cbt_rebuild_flush_cb, ctx);
			if (rc == 0) {
				return;	/* finalize from the flush callback */
			}
			SPDK_ERRLOG("CBT rebuild: target flush submit failed rc=%d\n", rc);
			ctx->error = rc;
			ctx->aborted = true;
		}
		/* No FLUSH support ⇒ no volatile cache to drain — fall through. */
	}
	cbt_rebuild_finalize(ctx);
}

static void
cbt_rebuild_finalize(struct cbt_rebuild_ctx *ctx)
{
	struct cbt_rebuild_result result = {0};
	uint64_t elapsed_tsc = spdk_get_ticks() - ctx->start_tsc;
	uint64_t hz = cbt_get_tsc_hz();
	uint64_t avg_read_us, avg_write_us, max_read_us, max_write_us, avg_qd;
	uint64_t new_dirty;

	result.chunks_copied = ctx->chunks_copied;
	result.bytes_copied = ctx->bytes_copied;
	result.duration_ms = elapsed_tsc * 1000 / hz;
	result.error = ctx->error;
	/* R1: an ABORTED rebuild (hot-remove mid-flight) is neither a success nor a
	 * plain I/O error. It must NOT report completed, or the control-plane marks the
	 * member fully synced after a mid-rebuild abort → silent under-replication. */
	result.completed = (ctx->error == 0 && !ctx->cancelled && !ctx->aborted);

	/* ── Instrumentation report ── */
	avg_read_us = ctx->chunks_copied ?
		(ctx->total_read_tsc * 1000000 / hz) / ctx->chunks_copied : 0;
	avg_write_us = ctx->chunks_copied ?
		(ctx->total_write_tsc * 1000000 / hz) / ctx->chunks_copied : 0;
	max_read_us = ctx->max_read_tsc * 1000000 / hz;
	max_write_us = ctx->max_write_tsc * 1000000 / hz;
	avg_qd = ctx->qd_samples ? ctx->qd_sum / ctx->qd_samples : 0;

	SPDK_NOTICELOG("CBT rebuild stats: duration=%lums chunks=%lu bytes=%lu MiB\n",
		       (unsigned long)result.duration_ms,
		       (unsigned long)result.chunks_copied,
		       (unsigned long)(result.bytes_copied >> 20));
	SPDK_NOTICELOG("  read_latency:  avg=%luus max=%luus\n",
		       (unsigned long)avg_read_us, (unsigned long)max_read_us);
	SPDK_NOTICELOG("  write_latency: avg=%luus max=%luus\n",
		       (unsigned long)avg_write_us, (unsigned long)max_write_us);
	SPDK_NOTICELOG("  pipeline: avg_qd=%lu submit_calls=%lu no_slot=%lu throttled=%lu\n",
		       (unsigned long)avg_qd,
		       (unsigned long)ctx->submit_calls,
		       (unsigned long)ctx->submit_no_slot,
		       (unsigned long)ctx->submit_throttled);
	SPDK_NOTICELOG("  ios_coalesced=%lu throughput=%lu MiB/s\n",
		       (unsigned long)ctx->ios_coalesced,
		       result.duration_ms ?
		       (unsigned long)(result.bytes_copied / 1024 / 1024 * 1000 /
				       result.duration_ms) : 0);

	/* Compute residual dirty ratio on successful completion. */
	if (result.completed && ctx->cbt->bitmap_size_bits > 0) {
		/* H1: every chunk of the exchanged delta is now proven copied — the
		 * frozen buffer no longer holds the only copy of anything, and the
		 * memcpy below repurposes it as a residual snapshot of the live
		 * bitmap. It must NOT be merged back on a later discard. */
		ctx->epoch->frozen_live_consumed = false;
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		memcpy(ctx->epoch->bitmap_frozen, ctx->cbt->bitmap,
		       ctx->cbt->bitmap_size_bytes);
		new_dirty = cbt_count_dirty_bits(ctx->epoch->bitmap_frozen,
						 ctx->cbt->bitmap_size_bytes);
		result.residual_dirty_ratio = (double)new_dirty /
					      (double)ctx->cbt->bitmap_size_bits;
	}

	/* Transition epoch to REBUILDING state (keep it there). */
	if (ctx->epoch->state == CBT_EPOCH_FROZEN) {
		ctx->epoch->state = CBT_EPOCH_REBUILDING;
	}

	/* Cleanup I/O resources. */
	cbt_rebuild_registry_cleanup(ctx);

	/* ── Finalize state for the registry ──
	 * R1: order matters. An I/O failure sets BOTH error and aborted (read/write_cb),
	 * so check error before aborted → it maps to FAILED. A hot-remove abort sets
	 * ONLY aborted (error == 0) → ABORTED. Neither is COMPLETED. */
	if (ctx->cancelled) {
		ctx->state = CBT_REBUILD_CANCELLED;
	} else if (ctx->error != 0) {
		ctx->state = CBT_REBUILD_FAILED;
	} else if (ctx->aborted) {
		ctx->state = CBT_REBUILD_ABORTED;
	} else {
		ctx->state = CBT_REBUILD_COMPLETED;
	}
	ctx->final_result = result;
	ctx->completion_tsc = spdk_get_ticks();

	/* Invoke completion callback (legacy fire-and-wait path). */
	if (ctx->cb_fn) {
		ctx->cb_fn(ctx->cb_arg, &result);
	}

	/* If this is the legacy path (has cb_fn, no rebuild_id), remove from
	 * registry and free immediately. Async entries stay for get_status.
	 */
	if (ctx->rebuild_id[0] == '\0') {
		TAILQ_REMOVE(&g_rebuild_registry, ctx, registry_link);
		free(ctx);
	}
	/* else: ctx stays in g_rebuild_registry for get_status queries.
	 * GC poller will free it after CBT_REBUILD_GC_DELAY_US.
	 */
}

/* Shared engine start for both the legacy (deferred-response) and async
 * (rebuild_id) paths. rebuild_id, when non-NULL, is stamped on the context
 * BEFORE the first I/O can complete — a zero-dirty bitmap finishes
 * SYNCHRONOUSLY, and the old tag-after-return dance freed the context before
 * the id was set (get_rebuild_status then returned -ENOENT forever). */
static int
cbt_rebuild_start(const char *cbt_name, const char *epoch_id,
		  const char *target_bdev_name,
		  const char *source_bdev_name,
		  uint64_t max_bw_mb_sec, uint32_t queue_depth,
		  const struct cbt_rebuild_range *override_ranges,
		  uint32_t num_ranges, const char *rebuild_id,
		  cbt_rebuild_done_cb cb_fn, void *cb_arg)
{
	struct vbdev_cbt *cbt;
	struct cbt_epoch *ep;
	struct cbt_rebuild_ctx *ctx;
	const char *src_name;
	uint64_t chunk_bytes;
	int rc;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	cbt = cbt_find_by_name(cbt_name);
	if (!cbt) {
		return -ENODEV;
	}

	ep = cbt_find_epoch(cbt, epoch_id);
	if (!ep) {
		return -ENOENT;
	}
	if (ep->state != CBT_EPOCH_FROZEN && ep->state != CBT_EPOCH_REBUILDING) {
		return -EINVAL;
	}
	if (!ep->bitmap_frozen) {
		return -EINVAL;
	}
	/* New: the legacy RPC path had NO anti-double-rebuild guard — two concurrent
	 * rebuilds would share ep->bitmap_frozen. One rebuild per (cbt, epoch). */
	if (cbt_rebuild_find_active_for_epoch(cbt, epoch_id) != NULL) {
		return -EBUSY;
	}

	/* Validate queue_depth. */
	if (queue_depth == 0) {
		queue_depth = CBT_REBUILD_DEFAULT_QD;
	}
	if (queue_depth > CBT_REBUILD_MAX_QD) {
		queue_depth = CBT_REBUILD_MAX_QD;
	}

	/* Allocate rebuild context. */
	ctx = calloc(1, sizeof(*ctx));
	if (!ctx) {
		return -ENOMEM;
	}

	ctx->cbt = cbt;
	ctx->epoch = ep;
	ctx->max_outstanding = (int)queue_depth;
	ctx->num_slots = (int)queue_depth;
	ctx->max_bytes_per_sec = max_bw_mb_sec * 1024 * 1024;
	ctx->cb_fn = cb_fn;
	ctx->cb_arg = cb_arg;
	ctx->start_tsc = spdk_get_ticks();
	ctx->window_start_tsc = ctx->start_tsc;
	ctx->bitmap = ep->bitmap_frozen;
	if (rebuild_id != NULL) {
		/* Stamp the id NOW (see function comment): a synchronous finish must
		 * already see it so the ctx survives in the registry for get_status. */
		snprintf(ctx->rebuild_id, sizeof(ctx->rebuild_id), "%s", rebuild_id);
	}

	/* Copy override ranges if provided. */
	if (override_ranges && num_ranges > 0) {
		ctx->override_ranges = calloc(num_ranges, sizeof(*ctx->override_ranges));
		if (!ctx->override_ranges) {
			free(ctx);
			return -ENOMEM;
		}
		memcpy(ctx->override_ranges, override_ranges,
		       num_ranges * sizeof(*ctx->override_ranges));
		ctx->num_ranges = num_ranges;
	}

	/* Open source bdev. Use source_bdev_name override if provided,
	 * otherwise default to the CBT bdev itself (reads go to base/RAID).
	 */
	src_name = source_bdev_name ? source_bdev_name :
		   spdk_bdev_get_name(&cbt->cbt_bdev);
	rc = spdk_bdev_open_ext(src_name, false,
				cbt_rebuild_base_event_cb, ctx, &ctx->src_desc);
	if (rc != 0) {
		free(ctx->override_ranges);
		free(ctx);
		return rc;
	}

	/* UAF guard: pin the cbt vbdev's lifetime for the whole rebuild. `ctx->cbt`
	 * / `ctx->epoch` / `ctx->bitmap` are bare pointers into it, dereferenced on
	 * every chunk; a base hot-remove would otherwise free the cbt vbdev underneath
	 * a running rebuild. When the read source IS the cbt bdev, src_desc already
	 * pins it — only a source override needs a dedicated pin descriptor. */
	if (strcmp(src_name, spdk_bdev_get_name(&cbt->cbt_bdev)) != 0) {
		rc = spdk_bdev_open_ext(spdk_bdev_get_name(&cbt->cbt_bdev), false,
					cbt_rebuild_base_event_cb, ctx, &ctx->cbt_pin_desc);
		if (rc != 0) {
			spdk_bdev_close(ctx->src_desc);
			free(ctx->override_ranges);
			free(ctx);
			return rc;
		}
	}

	/* Open target bdev. */
	rc = spdk_bdev_open_ext(target_bdev_name, true,
				cbt_rebuild_base_event_cb, ctx, &ctx->dst_desc);
	if (rc != 0) {
		if (ctx->cbt_pin_desc) spdk_bdev_close(ctx->cbt_pin_desc);
		spdk_bdev_close(ctx->src_desc);
		free(ctx->override_ranges);
		free(ctx);
		return rc;
	}

	/* Get IO channels. */
	ctx->src_ch = spdk_bdev_get_io_channel(ctx->src_desc);
	ctx->dst_ch = spdk_bdev_get_io_channel(ctx->dst_desc);
	if (!ctx->src_ch || !ctx->dst_ch) {
		if (ctx->src_ch) spdk_put_io_channel(ctx->src_ch);
		if (ctx->dst_ch) spdk_put_io_channel(ctx->dst_ch);
		if (ctx->cbt_pin_desc) spdk_bdev_close(ctx->cbt_pin_desc);
		spdk_bdev_close(ctx->src_desc);
		spdk_bdev_close(ctx->dst_desc);
		free(ctx->override_ranges);
		free(ctx);
		return -ENOMEM;
	}

	/* Allocate DMA buffer slots.
	 * Each slot must hold up to CBT_REBUILD_MAX_COALESCE_CHUNKS chunks
	 * to support I/O coalescing (e.g., 16 × 64KB = 1 MiB per slot).
	 */
	chunk_bytes = (uint64_t)cbt->chunk_size_blocks * cbt->cbt_bdev.blocklen *
		      CBT_REBUILD_MAX_COALESCE_CHUNKS;
	ctx->slots = calloc((size_t)ctx->num_slots, sizeof(*ctx->slots));
	if (!ctx->slots) {
		spdk_put_io_channel(ctx->src_ch);
		spdk_put_io_channel(ctx->dst_ch);
		if (ctx->cbt_pin_desc) spdk_bdev_close(ctx->cbt_pin_desc);
		spdk_bdev_close(ctx->src_desc);
		spdk_bdev_close(ctx->dst_desc);
		free(ctx->override_ranges);
		free(ctx);
		return -ENOMEM;
	}

	for (int i = 0; i < ctx->num_slots; i++) {
		ctx->slots[i].buf = spdk_dma_malloc(chunk_bytes, 4096, NULL);
		if (!ctx->slots[i].buf) {
			/* Free already allocated. */
			for (int j = 0; j < i; j++) {
				spdk_dma_free(ctx->slots[j].buf);
			}
			free(ctx->slots);
			spdk_put_io_channel(ctx->src_ch);
			spdk_put_io_channel(ctx->dst_ch);
			if (ctx->cbt_pin_desc) spdk_bdev_close(ctx->cbt_pin_desc);
			spdk_bdev_close(ctx->src_desc);
			spdk_bdev_close(ctx->dst_desc);
			free(ctx->override_ranges);
			free(ctx);
			return -ENOMEM;
		}
	}

	/* Start bandwidth throttle poller if needed. */
	if (ctx->max_bytes_per_sec > 0) {
		ctx->throttle_poller = SPDK_POLLER_REGISTER(
			cbt_rebuild_throttle_poller_fn, ctx, 100000); /* 100ms */
	}

	/* Precompute total dirty chunks for progress reporting. */
	ctx->total_dirty_chunks = cbt_count_dirty_bits(ep->bitmap_frozen,
						       cbt->bitmap_size_bytes);

	/* Register in the rebuild registry. */
	ctx->state = CBT_REBUILD_RUNNING;
	TAILQ_INSERT_TAIL(&g_rebuild_registry, ctx, registry_link);

	/* Start GC poller on first use. */
	if (!g_rebuild_gc_poller) {
		g_rebuild_gc_poller = SPDK_POLLER_REGISTER(
			cbt_rebuild_gc_poller_fn, NULL, 10000000); /* 10s */
	}

	SPDK_NOTICELOG("CBT: rebuild started for '%s' epoch '%s' → '%s' "
		       "(id=%s, qd=%d, bw_limit=%lu MB/s, coalesce=%d chunks/io)\n",
		       cbt_name, epoch_id, target_bdev_name,
		       rebuild_id != NULL ? rebuild_id : "-",
		       ctx->max_outstanding,
		       (unsigned long)max_bw_mb_sec,
		       CBT_REBUILD_MAX_COALESCE_CHUNKS);

	/* Kick off the first batch. */
	cbt_rebuild_submit_next(ctx);
	return 0;
}

int
bdev_cbt_partial_rebuild(const char *cbt_name, const char *epoch_id,
			 const char *target_bdev_name,
			 const char *source_bdev_name,
			 uint64_t max_bw_mb_sec, uint32_t queue_depth,
			 const struct cbt_rebuild_range *override_ranges,
			 uint32_t num_ranges,
			 cbt_rebuild_done_cb cb_fn, void *cb_arg)
{
	return cbt_rebuild_start(cbt_name, epoch_id, target_bdev_name, source_bdev_name,
				 max_bw_mb_sec, queue_depth, override_ranges, num_ranges,
				 NULL, cb_fn, cb_arg);
}

/* ================================================================== */
/* Async rebuild API (Phase 2)                                        */
/* ================================================================== */

int
bdev_cbt_start_rebuild(const char *cbt_name, const char *epoch_id,
		       const char *target_bdev_name,
		       const char *source_bdev_name,
		       uint64_t max_bw_mb_sec, uint32_t queue_depth,
		       char *out_rebuild_id)
{
	uint64_t id;
	int rc;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	/* Generate the rebuild_id FIRST: cbt_rebuild_start stamps it on the context
	 * before any I/O, so even a synchronous finish (zero dirty chunks) leaves a
	 * queryable registry entry (the old post-hoc tagging raced exactly that). */
	id = ++g_rebuild_id_counter;
	snprintf(out_rebuild_id, CBT_REBUILD_ID_MAX, "rebuild-%lu", (unsigned long)id);

	rc = cbt_rebuild_start(cbt_name, epoch_id, target_bdev_name,
			       source_bdev_name, max_bw_mb_sec,
			       queue_depth, NULL, 0, out_rebuild_id, NULL, NULL);
	if (rc != 0) {
		/* No rebuild was created — clear the pre-stamped id so a caller that
		 * ignores rc cannot query/persist a phantom "rebuild-N". */
		out_rebuild_id[0] = '\0';
		return rc;
	}
	return 0;
}

int
bdev_cbt_get_rebuild_status(const char *rebuild_id,
			    struct cbt_rebuild_status *out_status)
{
	struct cbt_rebuild_ctx *ctx;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	ctx = cbt_rebuild_find_by_id(rebuild_id);
	if (!ctx) {
		return -ENOENT;
	}

	out_status->state = ctx->state;
	out_status->chunks_copied = ctx->chunks_copied;
	out_status->total_chunks = ctx->total_dirty_chunks;
	out_status->bytes_copied = ctx->bytes_copied;

	if (ctx->state == CBT_REBUILD_RUNNING) {
		uint64_t elapsed_tsc = spdk_get_ticks() - ctx->start_tsc;
		out_status->duration_ms = elapsed_tsc * 1000 / cbt_get_tsc_hz();
	} else {
		/* Freeze duration at completion time. */
		out_status->duration_ms = ctx->final_result.duration_ms;
	}

	if (ctx->state != CBT_REBUILD_RUNNING) {
		out_status->residual_dirty_ratio = ctx->final_result.residual_dirty_ratio;
	} else {
		out_status->residual_dirty_ratio = 0.0;
	}
	out_status->error = ctx->error;

	return 0;
}

int
bdev_cbt_update_rebuild_options(const char *rebuild_id,
				uint64_t max_bw_mb_sec,
				uint32_t queue_depth)
{
	struct cbt_rebuild_ctx *ctx;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	ctx = cbt_rebuild_find_by_id(rebuild_id);
	if (!ctx) {
		return -ENOENT;
	}
	if (ctx->state != CBT_REBUILD_RUNNING) {
		return -EINVAL;
	}

	/* Update bandwidth limit — takes effect at next window reset.
	 * 0 = no change (field omitted in JSON). To set unlimited, use a
	 * very large value. Non-zero = limit in MB/s.
	 */
	if (max_bw_mb_sec > 0) {
		ctx->max_bytes_per_sec = max_bw_mb_sec * 1024 * 1024;
	}

	/* Update queue depth — takes effect immediately in submit_next.
	 * 0 = no change (field omitted in JSON).
	 */
	if (queue_depth > 0 && queue_depth <= CBT_REBUILD_MAX_QD) {
		ctx->max_outstanding = (int)queue_depth;
	}

	SPDK_NOTICELOG("CBT: rebuild '%s' options updated: bw=%lu MB/s qd=%d\n",
		       rebuild_id, (unsigned long)max_bw_mb_sec,
		       ctx->max_outstanding);
	return 0;
}

int
bdev_cbt_cancel_rebuild(const char *rebuild_id, uint64_t *out_chunks_copied)
{
	struct cbt_rebuild_ctx *ctx;

	assert(spdk_get_thread() == spdk_thread_get_app_thread());

	ctx = cbt_rebuild_find_by_id(rebuild_id);
	if (!ctx) {
		return -ENOENT;
	}
	if (ctx->state != CBT_REBUILD_RUNNING) {
		return -EINVAL;
	}

	/* Set cancelled flag — submit_next will stop issuing new I/Os.
	 * In-flight I/Os will complete normally, then cbt_rebuild_finish
	 * will be called from write_cb when outstanding_ios reaches 0.
	 */
	ctx->cancelled = true;
	*out_chunks_copied = ctx->chunks_copied;

	SPDK_NOTICELOG("CBT: rebuild '%s' cancelled (chunks_copied=%lu)\n",
		       rebuild_id, (unsigned long)ctx->chunks_copied);
	return 0;
}

/* ================================================================== */
/* Legacy aliases                                                     */
/* ================================================================== */

int
bdev_cbt_start_tracking(const char *cbt_name)
{
	return bdev_cbt_epoch_open(cbt_name, "__legacy__", "__legacy__", 0, NULL);
}

int
bdev_cbt_stop_tracking(const char *cbt_name)
{
	return bdev_cbt_epoch_freeze(cbt_name, "__legacy__");
}

int
bdev_cbt_get_dirty_ranges(const char *cbt_name, uint32_t max_ranges,
			  struct cbt_dirty_range **out_ranges,
			  uint32_t *out_count,
			  uint64_t *out_dirty_chunks,
			  uint64_t *out_total_chunks,
			  uint32_t *out_chunk_size_kb,
			  bool *out_truncated)
{
	return bdev_cbt_epoch_get_dirty_ranges(cbt_name, "__legacy__", max_ranges,
					       out_ranges, out_count,
					       out_dirty_chunks, out_total_chunks,
					       out_chunk_size_kb, out_truncated);
}

int
bdev_cbt_reset(const char *cbt_name)
{
	struct vbdev_cbt *cbt = cbt_find_by_name(cbt_name);

	if (!cbt) {
		return -ENODEV;
	}

	/* Refuse reset while any epoch is active — it would destroy
	 * the delta needed for partial rebuild.
	 */
	if (cbt_any_epoch_open(cbt)) {
		SPDK_ERRLOG("CBT: cannot reset '%s' — active epochs exist\n", cbt_name);
		return -EBUSY;
	}

	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	memset(cbt->bitmap, 0, cbt->bitmap_size_bytes);
	return 0;
}

/* ================================================================== */
/* Module lifecycle                                                   */
/* ================================================================== */

static int
vbdev_cbt_init(void)
{
	return 0;
}

static void
vbdev_cbt_finish(void)
{
	struct cbt_bdev_name *name;

	while ((name = TAILQ_FIRST(&g_bdev_names))) {
		TAILQ_REMOVE(&g_bdev_names, name, link);
		free(name->bdev_name);
		free(name->vbdev_name);
		free(name);
	}
}

static int
vbdev_cbt_get_ctx_size(void)
{
	return sizeof(struct cbt_bdev_io);
}

static int
vbdev_cbt_config_json(struct spdk_json_write_ctx *w)
{
	struct vbdev_cbt *node;

	TAILQ_FOREACH(node, &g_cbt_nodes, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "method", "bdev_cbt_create");
		spdk_json_write_named_object_begin(w, "params");
		spdk_json_write_named_string(w, "base_bdev_name",
					     spdk_bdev_get_name(node->base_bdev));
		spdk_json_write_named_string(w, "name",
					     spdk_bdev_get_name(&node->cbt_bdev));
		spdk_json_write_named_uint32(w, "chunk_size_kb", node->chunk_size_kb);
		spdk_json_write_object_end(w);
		spdk_json_write_object_end(w);
	}
	return 0;
}

static void
vbdev_cbt_examine(struct spdk_bdev *bdev)
{
	vbdev_cbt_register(bdev->name);
	spdk_bdev_module_examine_done(&cbt_if);
}

SPDK_LOG_REGISTER_COMPONENT(vbdev_cbt)
