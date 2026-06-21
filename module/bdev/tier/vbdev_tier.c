/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Evariops. All rights reserved.
 *
 *   bdev_tier — composite tier-mapped vbdev (SPEC-73A/D, M1). See vbdev_tier.h.
 *
 *   Design notes:
 *     - One vbdev per node. Bands appended fast->slow; band == one base bdev.
 *     - The low LBA range [0, md_num_blocks) is MIRRORED (RAID1) across two
 *       bands (md_mirror_a / md_mirror_b) so blobstore metadata (the L2P)
 *       survives a single disk loss (D1). The rest is a pure CONCAT.
 *     - Per-band failure isolation (C-FAIL-1): an I/O addressed to a DEGRADED
 *       band's range completes -EIO; the vbdev never reports a global failure.
 *     - The band table is NOT persisted on disk: the CSI control-plane is the
 *       source of truth (CRD) and deterministically replays create + add_band
 *       (+ retire_band) on agent startup, reproducing the identical layout.
 */

#include "vbdev_tier.h"

#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/likely.h"

static int vbdev_tier_init(void);
static void vbdev_tier_finish(void);
static int vbdev_tier_get_ctx_size(void);
static int vbdev_tier_config_json(struct spdk_json_write_ctx *w);

static struct spdk_bdev_module tier_if = {
	.name		= "tier",
	.module_init	= vbdev_tier_init,
	.module_fini	= vbdev_tier_finish,
	.config_json	= vbdev_tier_config_json,
	.get_ctx_size	= vbdev_tier_get_ctx_size,
};
SPDK_BDEV_MODULE_REGISTER(tier, &tier_if)

/* All composites on this node. */
static TAILQ_HEAD(, vbdev_tier) g_tier_nodes = TAILQ_HEAD_INITIALIZER(g_tier_nodes);

/* Per-IO context. For a mirrored md write we fan out to 2 bands and complete the
 * original only when both legs are done (remaining counter + worst status). */
struct tier_bdev_io {
	struct spdk_io_channel	*ch;
	int			remaining;	/* outstanding legs (1 for concat, 2 for md mirror write) */
	enum spdk_bdev_io_status status;	/* worst-of across legs */
	struct spdk_bdev_io_wait_entry bdev_io_wait;
};

static void vbdev_tier_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io);
static int vbdev_tier_destruct(void *ctx);
static void tier_sb_persist_cb(void *cb_arg, int rc);

/* --------------------------------------------------------------------------
 * Band table lookups
 * -------------------------------------------------------------------------- */

struct vbdev_tier *
vbdev_tier_get_by_name(const char *name)
{
	struct vbdev_tier *t;

	TAILQ_FOREACH(t, &g_tier_nodes, link) {
		if (t->bdev.name != NULL && strcmp(t->bdev.name, name) == 0) {
			return t;
		}
	}
	return NULL;
}

struct tier_band *
vbdev_tier_band_by_id(struct vbdev_tier *t, uint32_t band_id)
{
	struct tier_band *b;

	TAILQ_FOREACH(b, &t->bands, link) {
		if (b->band_id == band_id) {
			return b;
		}
	}
	return NULL;
}

struct tier_band *
vbdev_tier_band_of_lba(struct vbdev_tier *t, uint64_t lba, uint64_t *band_offset)
{
	struct tier_band *b;

	TAILQ_FOREACH(b, &t->bands, link) {
		if (b->state == TIER_BAND_RETIRED) {
			continue;
		}
		if (lba >= b->lba_start && lba < b->lba_start + b->num_blocks) {
			if (band_offset) {
				*band_offset = lba - b->lba_start;
			}
			return b;
		}
	}
	return NULL;
}

/* --------------------------------------------------------------------------
 * I/O completion
 * -------------------------------------------------------------------------- */

static void
_tier_leg_complete(struct spdk_bdev_io *leg_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	struct tier_bdev_io *io_ctx = (struct tier_bdev_io *)orig_io->driver_ctx;

	if (!success) {
		io_ctx->status = SPDK_BDEV_IO_STATUS_FAILED;
	}
	spdk_bdev_free_io(leg_io);

	if (--io_ctx->remaining == 0) {
		spdk_bdev_io_complete(orig_io, io_ctx->status);
	}
}

static void
vbdev_tier_resubmit_io(void *arg)
{
	struct spdk_bdev_io *bdev_io = (struct spdk_bdev_io *)arg;
	struct tier_bdev_io *io_ctx = (struct tier_bdev_io *)bdev_io->driver_ctx;

	vbdev_tier_submit_request(io_ctx->ch, bdev_io);
}

static void
vbdev_tier_queue_io(struct spdk_bdev_io *bdev_io, struct tier_band *band,
		    struct spdk_io_channel *base_ch)
{
	struct tier_bdev_io *io_ctx = (struct tier_bdev_io *)bdev_io->driver_ctx;
	int rc;

	io_ctx->bdev_io_wait.bdev = spdk_bdev_desc_get_bdev(band->desc);
	io_ctx->bdev_io_wait.cb_fn = vbdev_tier_resubmit_io;
	io_ctx->bdev_io_wait.cb_arg = bdev_io;

	rc = spdk_bdev_queue_io_wait(spdk_bdev_desc_get_bdev(band->desc), base_ch,
				     &io_ctx->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("tier: queue_io_wait failed rc=%d\n", rc);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
tier_init_ext_io_opts(struct spdk_bdev_io *bdev_io, struct spdk_bdev_ext_io_opts *opts)
{
	memset(opts, 0, sizeof(*opts));
	opts->size = sizeof(*opts);
	opts->memory_domain = bdev_io->u.bdev.memory_domain;
	opts->memory_domain_ctx = bdev_io->u.bdev.memory_domain_ctx;
	opts->metadata = bdev_io->u.bdev.md_buf;
	opts->dif_check_flags_exclude_mask = ~bdev_io->u.bdev.dif_check_flags;
}

/* --------------------------------------------------------------------------
 * I/O submission — translate composite LBA -> band(s)
 * -------------------------------------------------------------------------- */

/* Submit one read/write leg to a band at the given band-relative offset. */
static int
tier_submit_leg(struct vbdev_tier *t, struct tier_io_channel *tch, struct spdk_bdev_io *bdev_io,
		struct tier_band *band, uint64_t base_phys, bool is_write)
{
	struct spdk_io_channel *base_ch = tch->base_ch[band->band_id];
	struct spdk_bdev_ext_io_opts io_opts;

	if (spdk_unlikely(band->state == TIER_BAND_DEGRADED || band->desc == NULL ||
			  base_ch == NULL)) {
		return -EIO;	/* per-band isolation: caller fails THIS io, not the chunk */
	}

	tier_init_ext_io_opts(bdev_io, &io_opts);

	if (is_write) {
		return spdk_bdev_writev_blocks_ext(band->desc, base_ch, bdev_io->u.bdev.iovs,
						   bdev_io->u.bdev.iovcnt, base_phys,
						   bdev_io->u.bdev.num_blocks, _tier_leg_complete,
						   bdev_io, &io_opts);
	}
	return spdk_bdev_readv_blocks_ext(band->desc, base_ch, bdev_io->u.bdev.iovs,
					  bdev_io->u.bdev.iovcnt, base_phys,
					  bdev_io->u.bdev.num_blocks, _tier_leg_complete,
					  bdev_io, &io_opts);
}

/* Route a read or write to the band(s) owning [offset, offset+num). Handles the
 * mirrored md region (write -> 2 legs, read -> primary leg) and the concat data
 * region (1 leg). Returns 0 on success (legs submitted), -errno otherwise. */
static int
tier_route_rw(struct vbdev_tier *t, struct tier_io_channel *tch, struct spdk_bdev_io *bdev_io,
	      bool is_write)
{
	struct tier_bdev_io *io_ctx = (struct tier_bdev_io *)bdev_io->driver_ctx;
	uint64_t offset = bdev_io->u.bdev.offset_blocks;
	uint64_t num = bdev_io->u.bdev.num_blocks;
	struct tier_band *band, *band_b;
	uint64_t band_off;
	int rc;

	io_ctx->status = SPDK_BDEV_IO_STATUS_SUCCESS;

	/* Mirrored metadata region: same physical band-relative offset on both legs. */
	if (vbdev_tier_is_md_range(t, offset, num)) {
		band = vbdev_tier_band_by_id(t, t->md_mirror_a);
		band_b = vbdev_tier_band_by_id(t, t->md_mirror_b);
		if (band == NULL || band_b == NULL) {
			return -EIO;
		}
		/* The md region maps to band-relative offset == composite offset for
		 * both mirror bands (they reserve [0, md_num_blocks) at their start). */
		if (!is_write) {
			/* Read from primary; fall back to secondary if primary degraded.
			 * md maps to base-physical [sb_blocks, sb_blocks+md) on both mirror bands. */
			struct tier_band *src = (band->state == TIER_BAND_ACTIVE) ? band : band_b;
			io_ctx->remaining = 1;
			rc = tier_submit_leg(t, tch, bdev_io, src, t->sb_blocks + offset, false);
			if (rc != 0 && src == band && band_b->state == TIER_BAND_ACTIVE) {
				rc = tier_submit_leg(t, tch, bdev_io, band_b, t->sb_blocks + offset, false);
			}
			return rc;
		}
		/* Write: fan out to both legs that are still active. */
		io_ctx->remaining = 0;
		if (band->state == TIER_BAND_ACTIVE) {
			io_ctx->remaining++;
		}
		if (band_b->state == TIER_BAND_ACTIVE) {
			io_ctx->remaining++;
		}
		if (io_ctx->remaining == 0) {
			return -EIO;
		}
		if (band->state == TIER_BAND_ACTIVE) {
			rc = tier_submit_leg(t, tch, bdev_io, band, t->sb_blocks + offset, true);
			if (rc != 0) {
				return rc;
			}
		}
		if (band_b->state == TIER_BAND_ACTIVE) {
			rc = tier_submit_leg(t, tch, bdev_io, band_b, t->sb_blocks + offset, true);
			if (rc != 0) {
				return rc;
			}
		}
		return 0;
	}

	/* Data region: single band. Reject a straddle of band boundary (defensive;
	 * blobstore cluster I/O is band-aligned). */
	band = vbdev_tier_band_of_lba(t, offset, &band_off);
	if (band == NULL) {
		return -EIO;
	}
	if (offset + num > band->lba_start + band->num_blocks) {
		SPDK_ERRLOG("tier: I/O straddles band boundary (off=%" PRIu64 " num=%" PRIu64 ")\n",
			    offset, num);
		return -EINVAL;
	}
	io_ctx->remaining = 1;
	return tier_submit_leg(t, tch, bdev_io, band, band->phys_offset + band_off, is_write);
}

static void
tier_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	struct vbdev_tier *t = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_tier, bdev);
	struct tier_io_channel *tch = spdk_io_channel_get_ctx(ch);
	struct tier_bdev_io *io_ctx = (struct tier_bdev_io *)bdev_io->driver_ctx;
	int rc;

	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	rc = tier_route_rw(t, tch, bdev_io, false);
	if (rc != 0) {
		if (rc == -ENOMEM) {
			io_ctx->ch = ch;
			/* requeue on the owning band */
			uint64_t off;
			struct tier_band *b = vbdev_tier_band_of_lba(t, bdev_io->u.bdev.offset_blocks, &off);
			if (b) {
				vbdev_tier_queue_io(bdev_io, b, tch->base_ch[b->band_id]);
				return;
			}
		}
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
vbdev_tier_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_tier *t = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_tier, bdev);
	struct tier_io_channel *tch = spdk_io_channel_get_ctx(ch);
	struct tier_bdev_io *io_ctx = (struct tier_bdev_io *)bdev_io->driver_ctx;
	uint64_t band_off;
	struct tier_band *band;
	int rc = 0;

	io_ctx->ch = ch;

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		spdk_bdev_io_get_buf(bdev_io, tier_read_get_buf_cb,
				     bdev_io->u.bdev.num_blocks * bdev_io->bdev->blocklen);
		return;
	case SPDK_BDEV_IO_TYPE_WRITE:
		rc = tier_route_rw(t, tch, bdev_io, true);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		/* Management ops on the data region: route to the owning band. md region
		 * management ops would need the mirror fan-out; blobstore issues these on
		 * data clusters, so route single-band (md uses write/read). */
		band = vbdev_tier_band_of_lba(t, bdev_io->u.bdev.offset_blocks, &band_off);
		if (band == NULL || band->state != TIER_BAND_ACTIVE || band->desc == NULL ||
		    tch->base_ch[band->band_id] == NULL) {
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
			return;
		}
		io_ctx->remaining = 1;
		io_ctx->status = SPDK_BDEV_IO_STATUS_SUCCESS;
		if (bdev_io->type == SPDK_BDEV_IO_TYPE_WRITE_ZEROES) {
			rc = spdk_bdev_write_zeroes_blocks(band->desc, tch->base_ch[band->band_id],
							   band->phys_offset + band_off, bdev_io->u.bdev.num_blocks,
							   _tier_leg_complete, bdev_io);
		} else if (bdev_io->type == SPDK_BDEV_IO_TYPE_UNMAP) {
			rc = spdk_bdev_unmap_blocks(band->desc, tch->base_ch[band->band_id],
						    band->phys_offset + band_off, bdev_io->u.bdev.num_blocks,
						    _tier_leg_complete, bdev_io);
		} else {
			rc = spdk_bdev_flush_blocks(band->desc, tch->base_ch[band->band_id],
						    band->phys_offset + band_off, bdev_io->u.bdev.num_blocks,
						    _tier_leg_complete, bdev_io);
		}
		break;
	default:
		SPDK_ERRLOG("tier: unsupported I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (rc != 0) {
		spdk_bdev_io_complete(bdev_io,
				      rc == -EIO ? SPDK_BDEV_IO_STATUS_FAILED : SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
vbdev_tier_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		return true;
	default:
		return false;
	}
}

/* --------------------------------------------------------------------------
 * Channels
 * -------------------------------------------------------------------------- */

static int
tier_ch_create_cb(void *io_device, void *ctx_buf)
{
	struct tier_io_channel *tch = ctx_buf;
	struct vbdev_tier *t = io_device;
	struct tier_band *b;

	memset(tch->base_ch, 0, sizeof(tch->base_ch));
	TAILQ_FOREACH(b, &t->bands, link) {
		if (b->state != TIER_BAND_RETIRED && b->desc != NULL &&
		    b->band_id < TIER_MAX_BANDS) {
			tch->base_ch[b->band_id] = spdk_bdev_get_io_channel(b->desc);
		}
	}
	return 0;
}

static void
tier_ch_destroy_cb(void *io_device, void *ctx_buf)
{
	struct tier_io_channel *tch = ctx_buf;
	int i;

	for (i = 0; i < TIER_MAX_BANDS; i++) {
		if (tch->base_ch[i] != NULL) {
			spdk_put_io_channel(tch->base_ch[i]);
			tch->base_ch[i] = NULL;
		}
	}
}

static struct spdk_io_channel *
vbdev_tier_get_io_channel(void *ctx)
{
	struct vbdev_tier *t = (struct vbdev_tier *)ctx;

	return spdk_get_io_channel(t);
}

/* --------------------------------------------------------------------------
 * dump / config json
 * -------------------------------------------------------------------------- */

static int
vbdev_tier_dump_info_json(void *ctx, struct spdk_json_write_ctx *w)
{
	struct vbdev_tier *t = (struct vbdev_tier *)ctx;
	struct tier_band *b;

	spdk_json_write_named_object_begin(w, "tier");
	spdk_json_write_named_string(w, "name", spdk_bdev_get_name(&t->bdev));
	spdk_json_write_named_uint64(w, "md_num_blocks", t->md_num_blocks);
	spdk_json_write_named_uint32(w, "md_mirror_a", t->md_mirror_a);
	spdk_json_write_named_uint32(w, "md_mirror_b", t->md_mirror_b);
	spdk_json_write_named_array_begin(w, "bands");
	TAILQ_FOREACH(b, &t->bands, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_uint32(w, "band_id", b->band_id);
		spdk_json_write_named_string(w, "base_bdev_name", b->base_bdev_name);
		spdk_json_write_named_uint32(w, "tier", b->tier);
		spdk_json_write_named_uint32(w, "state", b->state);
		spdk_json_write_named_uint64(w, "lba_start", b->lba_start);
		spdk_json_write_named_uint64(w, "num_blocks", b->num_blocks);
		spdk_json_write_named_string(w, "wwn", b->wwn);
		spdk_json_write_named_string(w, "serial", b->serial);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);
	return 0;
}

static void
vbdev_tier_write_config_json(struct spdk_bdev *bdev, struct spdk_json_write_ctx *w)
{
	/* The CSI control-plane replays create/add_band from CRD; no per-bdev config emitted. */
}

static const struct spdk_bdev_fn_table vbdev_tier_fn_table = {
	.destruct		= vbdev_tier_destruct,
	.submit_request		= vbdev_tier_submit_request,
	.io_type_supported	= vbdev_tier_io_type_supported,
	.get_io_channel		= vbdev_tier_get_io_channel,
	.dump_info_json		= vbdev_tier_dump_info_json,
	.write_config_json	= vbdev_tier_write_config_json,
};

/* --------------------------------------------------------------------------
 * destruct
 * -------------------------------------------------------------------------- */

static void
_tier_device_unregister_cb(void *io_device)
{
	struct vbdev_tier *t = io_device;
	struct tier_band *b;

	while ((b = TAILQ_FIRST(&t->bands))) {
		TAILQ_REMOVE(&t->bands, b, link);
		if (b->desc != NULL) {
			spdk_bdev_module_release_bdev(spdk_bdev_desc_get_bdev(b->desc));
			spdk_bdev_close(b->desc);
		}
		free(b);
	}
	free(t->bdev.name);
	free(t);
}

static int
vbdev_tier_destruct(void *ctx)
{
	struct vbdev_tier *t = (struct vbdev_tier *)ctx;

	TAILQ_REMOVE(&g_tier_nodes, t, link);
	spdk_io_device_unregister(t, _tier_device_unregister_cb);
	return 0;
}

/* Base bdev hot-remove: mark the band degraded (per-band isolation), do NOT tear
 * down the composite. The CSI brain reacts via tier events + rebuild-by-range. */
static void
tier_base_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	struct tier_band *band = event_ctx;

	if (type == SPDK_BDEV_EVENT_REMOVE) {
		SPDK_WARNLOG("tier: base bdev '%s' removed, marking band %u DEGRADED\n",
			     bdev->name, band->band_id);
		band->state = TIER_BAND_DEGRADED;
	}
}

/* --------------------------------------------------------------------------
 * Lifecycle: create / add_band / retire_band
 * -------------------------------------------------------------------------- */

struct vbdev_tier *
vbdev_tier_create(const char *name, uint64_t md_num_blocks)
{
	struct vbdev_tier *t;

	t = calloc(1, sizeof(*t));
	if (t == NULL) {
		return NULL;
	}
	TAILQ_INIT(&t->bands);
	t->next_band_id = 0;
	t->md_num_blocks = md_num_blocks;
	t->md_mirror_a = UINT32_MAX;
	t->md_mirror_b = UINT32_MAX;
	t->blocklen = 0;
	t->total_num_blocks = 0;

	t->bdev.name = strdup(name);
	if (t->bdev.name == NULL) {
		free(t);
		return NULL;
	}
	t->bdev.product_name = "tier";
	t->bdev.write_cache = 0;
	t->bdev.ctxt = t;
	t->bdev.fn_table = &vbdev_tier_fn_table;
	t->bdev.module = &tier_if;

	TAILQ_INSERT_TAIL(&g_tier_nodes, t, link);
	return t;
}

int
vbdev_tier_add_band(struct vbdev_tier *t, const char *base_bdev_name, enum tier_class tier,
		    const char *wwn, const char *serial, uint32_t *out_band_id)
{
	struct tier_band *band;
	struct spdk_bdev *base_bdev;
	uint64_t usable_blocks;
	int rc;

	band = calloc(1, sizeof(*band));
	if (band == NULL) {
		return -ENOMEM;
	}

	rc = spdk_bdev_open_ext(base_bdev_name, true, tier_base_event_cb, band, &band->desc);
	if (rc != 0) {
		SPDK_ERRLOG("tier: cannot open base bdev '%s' rc=%d\n", base_bdev_name, rc);
		free(band);
		return rc;
	}
	base_bdev = spdk_bdev_desc_get_bdev(band->desc);

	/* All bands must share the block size (mixing 512/4096 corrupts geometry). */
	if (t->blocklen == 0) {
		t->blocklen = base_bdev->blocklen;
	} else if (base_bdev->blocklen != t->blocklen) {
		SPDK_ERRLOG("tier: band '%s' blocklen %u != composite %u\n",
			    base_bdev_name, base_bdev->blocklen, t->blocklen);
		spdk_bdev_close(band->desc);
		free(band);
		return -EINVAL;
	}
	/* Reserve a superblock region at the start of EACH base bdev (INV-T1). */
	if (t->sb_blocks == 0) {
		t->sb_blocks = spdk_divide_round_up(TIER_SB_RESERVE_BYTES, t->blocklen);
	}

	rc = spdk_bdev_module_claim_bdev(base_bdev, band->desc, &tier_if);
	if (rc != 0) {
		SPDK_ERRLOG("tier: cannot claim base bdev '%s' rc=%d\n", base_bdev_name, rc);
		spdk_bdev_close(band->desc);
		free(band);
		return rc;
	}

	band->band_id = t->next_band_id++;
	band->tier = tier;
	band->state = TIER_BAND_ACTIVE;
	snprintf(band->base_bdev_name, sizeof(band->base_bdev_name), "%s", base_bdev_name);
	if (wwn) {
		snprintf(band->wwn, sizeof(band->wwn), "%s", wwn);
	}
	if (serial) {
		snprintf(band->serial, sizeof(band->serial), "%s", serial);
	}

	/* Geometry. Each disk reserves [0, sb_blocks) for the superblock; usable =
	 * blockcnt - sb_blocks. The first two bands additionally host the mirrored md
	 * region [0, md_num_blocks) of the composite at base-physical
	 * [sb_blocks, sb_blocks+md_num_blocks); their DATA contribution follows. */
	if (base_bdev->blockcnt <= t->sb_blocks) {
		SPDK_ERRLOG("tier: band '%s' too small for superblock reserve\n", base_bdev_name);
		spdk_bdev_module_release_bdev(base_bdev);
		spdk_bdev_close(band->desc);
		free(band);
		return -ENOSPC;
	}
	usable_blocks = base_bdev->blockcnt - t->sb_blocks;

	if (t->md_mirror_a == UINT32_MAX) {
		/* Band A: hosts the md region (counted ONCE in the composite) + a data tail. */
		if (usable_blocks <= t->md_num_blocks) {
			SPDK_ERRLOG("tier: band '%s' too small for md region\n", base_bdev_name);
			spdk_bdev_module_release_bdev(base_bdev);
			spdk_bdev_close(band->desc);
			free(band);
			return -ENOSPC;
		}
		t->md_mirror_a = band->band_id;
		t->total_num_blocks = t->md_num_blocks;		/* md region occupies [0, md) */
		band->lba_start = t->md_num_blocks;		/* this band's data tail follows md */
		band->num_blocks = usable_blocks - t->md_num_blocks;
		band->phys_offset = t->sb_blocks + t->md_num_blocks;
		t->total_num_blocks += band->num_blocks;
	} else if (t->md_mirror_b == UINT32_MAX) {
		/* Band B: hosts the md MIRROR (not re-counted) + a data tail. */
		if (usable_blocks <= t->md_num_blocks) {
			SPDK_ERRLOG("tier: band '%s' too small for md mirror\n", base_bdev_name);
			spdk_bdev_module_release_bdev(base_bdev);
			spdk_bdev_close(band->desc);
			free(band);
			return -ENOSPC;
		}
		t->md_mirror_b = band->band_id;
		band->lba_start = t->total_num_blocks;
		band->num_blocks = usable_blocks - t->md_num_blocks;
		band->phys_offset = t->sb_blocks + t->md_num_blocks;
		t->total_num_blocks += band->num_blocks;
	} else {
		/* Plain concat band. */
		band->lba_start = t->total_num_blocks;
		band->num_blocks = usable_blocks;
		band->phys_offset = t->sb_blocks;
		t->total_num_blocks += band->num_blocks;
	}

	TAILQ_INSERT_TAIL(&t->bands, band, link);
	t->num_bands++;

	if (out_band_id) {
		*out_band_id = band->band_id;
	}
	SPDK_NOTICELOG("tier '%s': added band %u ('%s', tier=%d) lba_start=%" PRIu64
		       " num_blocks=%" PRIu64 "\n", t->bdev.name, band->band_id,
		       base_bdev_name, tier, band->lba_start, band->num_blocks);
	return 0;
}

int
vbdev_tier_retire_band(struct vbdev_tier *t, uint32_t band_id)
{
	struct tier_band *band = vbdev_tier_band_by_id(t, band_id);

	if (band == NULL) {
		return -ENODEV;
	}
	if (band->state == TIER_BAND_RETIRED) {
		return 0;	/* idempotent */
	}
	/* The CSI brain guarantees the band was evacuated (clusters relocated) before
	 * retiring. We keep the slot and its LBA range as an unreclaimable hole. */
	band->state = TIER_BAND_RETIRED;
	/* Persist the new band table to the SURVIVING bands BEFORE closing the retired
	 * one's desc (so the seq bump records the retirement). */
	if (t->registered) {
		tier_sb_write_all(t, tier_sb_persist_cb, NULL);
	}
	if (band->desc != NULL) {
		spdk_bdev_module_release_bdev(spdk_bdev_desc_get_bdev(band->desc));
		spdk_bdev_close(band->desc);
		band->desc = NULL;
	}
	SPDK_NOTICELOG("tier '%s': retired band %u\n", t->bdev.name, band_id);
	return 0;
}

int
vbdev_tier_delete(struct vbdev_tier *t)
{
	if (t->registered) {
		/* registered: unregister triggers destruct (frees bands + node) */
		spdk_bdev_unregister(&t->bdev, NULL, NULL);
	} else {
		/* never registered: free directly */
		TAILQ_REMOVE(&g_tier_nodes, t, link);
		_tier_device_unregister_cb(t);
	}
	return 0;
}

/* Fire-and-forget superblock persistence completion (logs failures). */
static void
tier_sb_persist_cb(void *cb_arg, int rc)
{
	if (rc != 0) {
		SPDK_ERRLOG("tier: superblock persist failed rc=%d\n", rc);
	}
}

/* Register the composite bdev once its bands are configured (called by RPC). */
int
vbdev_tier_register(struct vbdev_tier *t)
{
	int rc;

	if (t->num_bands == 0 || t->blocklen == 0) {
		return -EINVAL;
	}
	t->bdev.blocklen = t->blocklen;
	t->bdev.blockcnt = t->total_num_blocks;

	spdk_io_device_register(t, tier_ch_create_cb, tier_ch_destroy_cb,
				sizeof(struct tier_io_channel), t->bdev.name);

	rc = spdk_bdev_register(&t->bdev);
	if (rc != 0) {
		SPDK_ERRLOG("tier: bdev_register('%s') failed rc=%d\n", t->bdev.name, rc);
		spdk_io_device_unregister(t, NULL);
		return rc;
	}
	t->registered = true;
	SPDK_NOTICELOG("tier '%s' registered: %u bands, %" PRIu64 " blocks of %u bytes (sb_blocks=%u)\n",
		       t->bdev.name, t->num_bands, t->bdev.blockcnt, t->bdev.blocklen, t->sb_blocks);

	/* Persist the superblock to every band (INV-T1). CRD is a backup source of
	 * truth; a failed sb write is logged. */
	tier_sb_write_all(t, tier_sb_persist_cb, NULL);
	return 0;
}

/* --------------------------------------------------------------------------
 * relocate-quiesce co-design (M2b) — only the registering module may quiesce.
 * -------------------------------------------------------------------------- */

int
vbdev_tier_relocate_quiesce(struct vbdev_tier *t, uint64_t lba, uint64_t num_blocks,
			    spdk_bdev_quiesce_cb cb_fn, void *cb_arg)
{
	return spdk_bdev_quiesce_range(&t->bdev, &tier_if, lba, num_blocks, cb_fn, cb_arg);
}

int
vbdev_tier_relocate_unquiesce(struct vbdev_tier *t, uint64_t lba, uint64_t num_blocks,
			      spdk_bdev_quiesce_cb cb_fn, void *cb_arg)
{
	return spdk_bdev_unquiesce_range(&t->bdev, &tier_if, lba, num_blocks, cb_fn, cb_arg);
}

/* --------------------------------------------------------------------------
 * module init / finish
 * -------------------------------------------------------------------------- */

static int
vbdev_tier_init(void)
{
	return 0;
}

static void
vbdev_tier_finish(void)
{
}

static int
vbdev_tier_get_ctx_size(void)
{
	return sizeof(struct tier_bdev_io);
}

static int
vbdev_tier_config_json(struct spdk_json_write_ctx *w)
{
	/* CSI replays create/add_band from CRD; nothing to emit. */
	return 0;
}

SPDK_LOG_REGISTER_COMPONENT(vbdev_tier)
