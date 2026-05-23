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
 * Design reference: SPEC-52 §2.
 */

#include "spdk/stdinc.h"

#include "vbdev_cbt_internal.h"
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

static inline void
cbt_mark_dirty(struct vbdev_cbt *cbt, uint64_t offset_blocks, uint64_t num_blocks)
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

	__atomic_fetch_add(&cbt->total_writes_tracked, 1, __ATOMIC_RELAXED);
}

/* ================================================================== */
/* Healthy-clear poller                                               */
/* ================================================================== */

static int
cbt_healthy_clear_poller_fn(void *ctx)
{
	struct vbdev_cbt *cbt = ctx;

	/* If any epoch is open/frozen/rebuilding, do NOT clear. */
	if (cbt->healthy_clear_suspended || cbt_any_epoch_open(cbt)) {
		return SPDK_POLLER_IDLE;
	}

	/* Only clear if the orchestrator has explicitly confirmed all
	 * backends are healthy and in-sync.
	 */
	if (!cbt->backends_healthy) {
		return SPDK_POLLER_IDLE;
	}

	/* Clear the bitmap — all backends are confirmed healthy.
	 * This runs on the owner thread (same as epoch ops). IO threads
	 * may concurrently set bits via atomic OR; a few bits set between
	 * our check and the memset are acceptable (they will be re-set on
	 * the next write and caught by the next clear cycle).
	 */
	if (cbt_popcount_bitmap(cbt) > 0) {
		memset(cbt->bitmap, 0, cbt->bitmap_size_bytes);
	}

	return SPDK_POLLER_IDLE;
}

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

static int
vbdev_cbt_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct vbdev_cbt *cbt_node = ctx;

	spdk_json_write_name(w, "cbt");
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name",
				     spdk_bdev_get_name(&cbt_node->cbt_bdev));
	spdk_json_write_named_string(w, "base_bdev_name",
				     spdk_bdev_get_name(cbt_node->base_bdev));
	spdk_json_write_named_uint32(w, "chunk_size_kb", cbt_node->chunk_size_kb);
	spdk_json_write_named_uint64(w, "dirty_chunks", cbt_popcount_bitmap(cbt_node));
	spdk_json_write_named_uint64(w, "total_chunks", cbt_node->bitmap_size_bits);
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

	if (cbt_node->healthy_poller) {
		spdk_poller_unregister(&cbt_node->healthy_poller);
	}

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
	if (type == SPDK_BDEV_EVENT_REMOVE) {
		struct vbdev_cbt *node, *tmp;

		TAILQ_FOREACH_SAFE(node, &g_cbt_nodes, link, tmp) {
			if (bdev == node->base_bdev) {
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

		/* ── Register healthy-clear poller (always-on) ── */
		cbt_node->healthy_poller = SPDK_POLLER_REGISTER(
			cbt_healthy_clear_poller_fn, cbt_node,
			CBT_HEALTHY_CLEAR_INTERVAL_US);

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
		    const char *stale_backend_id, uint64_t generation)
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

	/* Check if epoch already exists. */
	ep = cbt_find_epoch(cbt, epoch_id);
	if (ep) {
		if (generation > ep->generation) {
			/* Replace with higher generation. */
			ep->generation = generation;
			snprintf(ep->stale_backend_id, sizeof(ep->stale_backend_id),
				 "%s", stale_backend_id);
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
		/* Safe to evict: COMPLETED or INVALID. */
		SPDK_WARNLOG("CBT: max epochs reached, evicting '%s'\n",
			     oldest->epoch_id);
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
	ep->generation = generation;
	ep->state = CBT_EPOCH_OPEN;

	TAILQ_INSERT_TAIL(&cbt->epochs, ep, link);
	cbt->epoch_count++;

	/* Suspend healthy-clear while any epoch is open. */
	cbt->healthy_clear_suspended = true;

	SPDK_NOTICELOG("CBT: epoch_open '%s' for stale backend '%s' gen=%lu\n",
		       epoch_id, stale_backend_id, (unsigned long)generation);
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

	/* Free previous frozen bitmap if re-freezing. */
	free(ep->bitmap_frozen);

	/* Allocate and snapshot the current bitmap into this epoch.
	 *
	 * Thread safety: IO threads write individual bits with atomic OR.
	 * We read the bitmap here on the app thread. On x86/arm64, each byte
	 * read is atomic, so we never see a torn byte. The semantic guarantee
	 * is: any write that completed its IO callback before this function
	 * was called is guaranteed to be in the snapshot. Writes that are
	 * in-flight may or may not be captured. This is acceptable because
	 * the orchestrator must ensure all writes are drained before calling
	 * freeze if it needs a precise point-in-time snapshot.
	 *
	 * An atomic fence ensures visibility of all relaxed stores before
	 * we read.
	 */
	ep->bitmap_frozen = malloc(cbt->bitmap_size_bytes);
	if (!ep->bitmap_frozen) {
		return -ENOMEM;
	}
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	memcpy(ep->bitmap_frozen, cbt->bitmap, cbt->bitmap_size_bytes);
	ep->state = CBT_EPOCH_FROZEN;

	SPDK_NOTICELOG("CBT: epoch_freeze '%s' (dirty=%lu/%lu)\n",
		       epoch_id,
		       (unsigned long)cbt_popcount_bitmap(cbt),
		       (unsigned long)cbt->bitmap_size_bits);
	return 0;
}

int
bdev_cbt_epoch_close(const char *cbt_name, const char *epoch_id)
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

	ep->state = CBT_EPOCH_COMPLETED;
	TAILQ_REMOVE(&cbt->epochs, ep, link);
	cbt->epoch_count--;
	free(ep->bitmap_frozen);
	free(ep);

	/* Resume healthy-clear if no more active epochs. */
	if (!cbt_any_epoch_open(cbt)) {
		cbt->healthy_clear_suspended = false;
	}

	SPDK_NOTICELOG("CBT: epoch_close '%s'\n", epoch_id);
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
	bool      in_use;
};

struct cbt_rebuild_ctx {
	struct vbdev_cbt         *cbt;
	struct cbt_epoch         *epoch;
	struct spdk_bdev_desc    *src_desc;
	struct spdk_bdev_desc    *dst_desc;
	struct spdk_io_channel   *src_ch;
	struct spdk_io_channel   *dst_ch;

	/* Bitmap to walk (frozen bitmap or override ranges) */
	const uint8_t            *bitmap;
	struct cbt_rebuild_range *override_ranges;
	uint32_t                  num_ranges;
	uint32_t                  current_range_idx;

	/* Bitmap scan position */
	uint64_t                  current_bit;

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
	int                       error;
};

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
	/* If the bdev is removed mid-rebuild, the IO callbacks will
	 * return failure and the rebuild will abort gracefully.
	 */
}

/* Find the next dirty bit starting from ctx->current_bit.
 * Returns false if no more dirty bits. */
static bool
cbt_rebuild_find_next_dirty(struct cbt_rebuild_ctx *ctx, uint64_t *out_bit)
{
	const uint8_t *bmap = ctx->bitmap;
	uint64_t total = ctx->cbt->bitmap_size_bits;

	while (ctx->current_bit < total) {
		uint64_t byte_idx = ctx->current_bit >> 3;
		uint8_t byte_val = bmap[byte_idx];

		if (byte_val == 0) {
			/* Skip entire byte — no dirty bits. */
			ctx->current_bit = (byte_idx + 1) << 3;
			continue;
		}

		uint8_t bit_pos = ctx->current_bit & 7;
		if (byte_val & (1u << bit_pos)) {
			*out_bit = ctx->current_bit;
			ctx->current_bit++;
			return true;
		}
		ctx->current_bit++;
	}
	return false;
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
		ctx->chunks_copied++;
		ctx->bytes_copied += slot->chunk_length_blocks *
				     ctx->cbt->cbt_bdev.blocklen;
		ctx->bytes_this_window += slot->chunk_length_blocks *
					  ctx->cbt->cbt_bdev.blocklen;
	}

	if (slot) {
		slot->in_use = false;
	}

	if (ctx->aborted && ctx->outstanding_ios == 0) {
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

	/* Write the data to the target bdev. */
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
			cbt_rebuild_submit_next(ctx);
		}
	}

	return ctx->throttled ? SPDK_POLLER_BUSY : SPDK_POLLER_IDLE;
}

static void
cbt_rebuild_submit_next(struct cbt_rebuild_ctx *ctx)
{
	struct cbt_rebuild_io_slot *slot;
	uint64_t offset_blocks, length_blocks;
	int rc;

	while (!ctx->aborted && ctx->outstanding_ios < ctx->max_outstanding) {
		/* Bandwidth throttle check. */
		if (ctx->max_bytes_per_sec > 0 &&
		    ctx->bytes_this_window >= ctx->max_bytes_per_sec) {
			ctx->throttled = true;
			break;
		}

		slot = cbt_rebuild_get_free_slot(ctx);
		if (!slot) {
			break;
		}

		/* Get next chunk to copy. */
		if (ctx->override_ranges) {
			if (!cbt_rebuild_find_next_range(ctx, &offset_blocks, &length_blocks)) {
				goto done_scanning;
			}
		} else {
			uint64_t bit;
			if (!cbt_rebuild_find_next_dirty(ctx, &bit)) {
				goto done_scanning;
			}
			offset_blocks = bit * ctx->cbt->chunk_size_blocks;
			length_blocks = ctx->cbt->chunk_size_blocks;
			/* Clamp to device size. */
			if (offset_blocks + length_blocks > ctx->cbt->total_blocks) {
				length_blocks = ctx->cbt->total_blocks - offset_blocks;
			}
		}

		slot->in_use = true;
		slot->chunk_offset_blocks = offset_blocks;
		slot->chunk_length_blocks = length_blocks;
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

static void
cbt_rebuild_finish(struct cbt_rebuild_ctx *ctx)
{
	struct cbt_rebuild_result result = {0};
	uint64_t elapsed_tsc = spdk_get_ticks() - ctx->start_tsc;

	result.chunks_copied = ctx->chunks_copied;
	result.bytes_copied = ctx->bytes_copied;
	result.duration_ms = elapsed_tsc * 1000 / cbt_get_tsc_hz();
	result.error = ctx->error;
	result.completed = (ctx->error == 0);

	/* Compute residual dirty ratio: re-snapshot the live bitmap and count
	 * new dirty bits that arrived during the copy.
	 */
	if (result.completed && ctx->cbt->bitmap_size_bits > 0) {
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		/* Re-freeze the epoch's bitmap_frozen with current live bitmap. */
		memcpy(ctx->epoch->bitmap_frozen, ctx->cbt->bitmap,
		       ctx->cbt->bitmap_size_bytes);
		/* Count dirty bits in the new snapshot. */
		uint64_t new_dirty = 0;
		const uint8_t *bmap = ctx->epoch->bitmap_frozen;
		uint64_t n = ctx->cbt->bitmap_size_bytes / 8;
		uint64_t tail = ctx->cbt->bitmap_size_bytes % 8;
		for (uint64_t i = 0; i < n; i++) {
			uint64_t word;
			memcpy(&word, bmap + i * 8, sizeof(word));
			new_dirty += (uint64_t)__builtin_popcountll(word);
		}
		for (uint64_t i = 0; i < tail; i++) {
			new_dirty += (uint64_t)__builtin_popcount(bmap[n * 8 + i]);
		}
		result.residual_dirty_ratio = (double)new_dirty /
					      (double)ctx->cbt->bitmap_size_bits;
	}

	/* Transition epoch to REBUILDING state (keep it there). */
	if (ctx->epoch->state == CBT_EPOCH_FROZEN) {
		ctx->epoch->state = CBT_EPOCH_REBUILDING;
	}

	/* Cleanup. */
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
	for (int i = 0; i < ctx->num_slots; i++) {
		if (ctx->slots[i].buf) {
			spdk_dma_free(ctx->slots[i].buf);
		}
	}
	free(ctx->slots);
	free(ctx->override_ranges);

	/* Invoke completion callback. */
	ctx->cb_fn(ctx->cb_arg, &result);
	free(ctx);
}

int
bdev_cbt_partial_rebuild(const char *cbt_name, const char *epoch_id,
			 const char *target_bdev_name,
			 uint64_t max_bw_mb_sec, uint32_t queue_depth,
			 const struct cbt_rebuild_range *override_ranges,
			 uint32_t num_ranges,
			 cbt_rebuild_done_cb cb_fn, void *cb_arg)
{
	struct vbdev_cbt *cbt;
	struct cbt_epoch *ep;
	struct cbt_rebuild_ctx *ctx;
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
	if (ep->state != CBT_EPOCH_FROZEN) {
		return -EINVAL;
	}
	if (!ep->bitmap_frozen) {
		return -EINVAL;
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

	/* Open source bdev (the CBT bdev itself — reads go to base/RAID). */
	rc = spdk_bdev_open_ext(spdk_bdev_get_name(&cbt->cbt_bdev), false,
				cbt_rebuild_base_event_cb, NULL, &ctx->src_desc);
	if (rc != 0) {
		free(ctx->override_ranges);
		free(ctx);
		return rc;
	}

	/* Open target bdev. */
	rc = spdk_bdev_open_ext(target_bdev_name, true,
				cbt_rebuild_base_event_cb, NULL, &ctx->dst_desc);
	if (rc != 0) {
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
		spdk_bdev_close(ctx->src_desc);
		spdk_bdev_close(ctx->dst_desc);
		free(ctx->override_ranges);
		free(ctx);
		return -ENOMEM;
	}

	/* Allocate DMA buffer slots. */
	chunk_bytes = (uint64_t)cbt->chunk_size_blocks * cbt->cbt_bdev.blocklen;
	ctx->slots = calloc((size_t)ctx->num_slots, sizeof(*ctx->slots));
	if (!ctx->slots) {
		spdk_put_io_channel(ctx->src_ch);
		spdk_put_io_channel(ctx->dst_ch);
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

	SPDK_NOTICELOG("CBT: partial_rebuild started for '%s' epoch '%s' → '%s' "
		       "(qd=%d, bw_limit=%lu MB/s)\n",
		       cbt_name, epoch_id, target_bdev_name,
		       ctx->max_outstanding,
		       (unsigned long)max_bw_mb_sec);

	/* Kick off the first batch. */
	cbt_rebuild_submit_next(ctx);
	return 0;
}

/* ================================================================== */
/* Legacy aliases                                                     */
/* ================================================================== */

int
bdev_cbt_start_tracking(const char *cbt_name)
{
	return bdev_cbt_epoch_open(cbt_name, "__legacy__", "__legacy__", 0);
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

void
bdev_cbt_set_backends_healthy(const char *cbt_name, bool healthy)
{
	struct vbdev_cbt *cbt = cbt_find_by_name(cbt_name);

	if (!cbt) {
		return;
	}

	cbt->backends_healthy = healthy;
	SPDK_NOTICELOG("CBT: '%s' backends_healthy=%d\n", cbt_name, (int)healthy);
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
