/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Evariops. All rights reserved.
 *
 *   bdev_tier — composite vbdev. See vbdev_tier.h.
 *
 *   Design notes:
 *     - One vbdev per node. Bands appended fast->slow; band == one base bdev.
 *     - The low LBA range [0, md_num_blocks) is MIRRORED (RAID1) across two
 *       bands (md_mirror_a / md_mirror_b) so blobstore metadata survives a
 *       single disk loss. The rest is a pure CONCAT.
 *     - Failure is isolated per band: an I/O addressed to a DEGRADED band's
 *       range completes -EIO; the vbdev never reports a global failure.
 *     - The band table IS persisted on disk: one superblock copy per band,
 *       highest seq wins (vbdev_tier_sb.c). The SB is authoritative for
 *       geometry; the CSI control-plane replays create + assemble_band from
 *       the highest-seq SB on agent startup.
 */

#include "vbdev_tier.h"

#include "spdk/rpc.h"
#include "spdk/env.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/util.h"
#include "spdk/likely.h"
#include "spdk/crc32.h"
#include "spdk/uuid.h"

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
 * original only when the LAST leg is done — never while a submitted leg is in
 * flight, since the leg callback holds cb_arg == orig_io. */
struct tier_bdev_io {
	struct spdk_io_channel	*ch;
	int			remaining;	/* outstanding legs (1 for concat, 2 for md mirror write) */
	uint8_t			good_legs;	/* legs completed successfully (md mirror fan-out) */
	bool			md_retry_done;	/* at most one mirror-failover retry per md read */
	bool			submit_failed;	/* a leg SUBMISSION failed (no degrade; orig fails) */
	enum spdk_bdev_io_status status;	/* worst-of across legs */
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
 * Fill-accounting provider (registered by the lvol layer; see vbdev_tier.h)
 * -------------------------------------------------------------------------- */

void
vbdev_tier_set_usage_provider(const char *tier_name, vbdev_tier_usage_fn fn, void *ctx)
{
	struct vbdev_tier *t = vbdev_tier_get_by_name(tier_name);

	if (t == NULL) {
		SPDK_WARNLOG("tier '%s': usage provider registered for an unknown composite\n",
			     tier_name);
		return;
	}
	t->usage_fn = fn;
	t->usage_ctx = ctx;
}

void
vbdev_tier_clear_usage_provider(const char *tier_name)
{
	struct vbdev_tier *t = vbdev_tier_get_by_name(tier_name);

	if (t == NULL) {
		return;		/* composite already torn down — nothing to clear */
	}
	t->usage_fn = NULL;
	t->usage_ctx = NULL;
}

/* --------------------------------------------------------------------------
 * I/O completion
 * -------------------------------------------------------------------------- */

static int tier_submit_leg(struct vbdev_tier *t, struct tier_io_channel *tch,
			   struct spdk_bdev_io *bdev_io, struct tier_band *band,
			   uint64_t base_phys);

/* Map a base bdev back to its band (leg completions only carry the base bdev_io). */
static struct tier_band *
tier_band_by_base_bdev(struct vbdev_tier *t, struct spdk_bdev *base)
{
	struct tier_band *b;

	TAILQ_FOREACH(b, &t->bands, link) {
		if (b->desc != NULL && spdk_bdev_desc_get_bdev(b->desc) == base) {
			return b;
		}
	}
	return NULL;
}

/* The md-mirror leg that is NOT `b` (NULL if unmirrored / not found). */
static struct tier_band *
tier_md_other_leg(struct vbdev_tier *t, struct tier_band *b)
{
	uint32_t other = (b != NULL && b->band_id == t->md_mirror_a) ? t->md_mirror_b
			 : t->md_mirror_a;

	if (other == UINT32_MAX) {
		return NULL;
	}
	return vbdev_tier_band_by_id(t, other);
}

static void tier_band_drain_release(struct tier_band *band);

/* Account a leg completion against this reactor's in-flight counter, and perform
 * the channel put a band drain deferred to the LAST completion. Must run AFTER
 * spdk_bdev_free_io(leg_io): the io returns to the base channel's cache, which
 * must still exist. Runs on the submitting reactor, so inflight[] needs no atomics. */
static void
tier_leg_channel_release(struct vbdev_tier *t, struct tier_io_channel *tch,
			 struct spdk_bdev *leg_bdev)
{
	struct tier_band *b = tier_band_by_base_bdev(t, leg_bdev);

	if (b == NULL || b->band_id >= TIER_MAX_BANDS) {
		return;
	}
	assert(tch->inflight[b->band_id] > 0);
	if (tch->inflight[b->band_id] == 0 || --tch->inflight[b->band_id] > 0) {
		return;
	}
	if (tch->drain_deferred[b->band_id]) {
		tch->drain_deferred[b->band_id] = false;
		if (tch->base_ch[b->band_id] != NULL) {
			spdk_put_io_channel(tch->base_ch[b->band_id]);
			tch->base_ch[b->band_id] = NULL;
		}
		tier_band_drain_release(b);
	}
}

/* The DEGRADED persist mutates composite-global SB state owned by t->thread,
 * while a leg completion runs on the submitting reactor — so the persist is
 * funneled through spdk_thread_send_msg. The message re-resolves the composite
 * through g_tier_nodes, never through captured pointers, so a composite torn
 * down while the message was in flight is simply skipped. */
struct tier_degrade_msg {
	struct vbdev_tier	*t;
	uint32_t		band_id;
};

static bool
tier_node_is_live(const struct vbdev_tier *t)
{
	struct vbdev_tier *n;

	TAILQ_FOREACH(n, &g_tier_nodes, link) {
		if (n == t) {
			return true;
		}
	}
	return false;
}

static void
tier_degrade_persist_msg(void *arg)
{
	struct tier_degrade_msg *m = arg;
	struct vbdev_tier *t = m->t;

	if (tier_node_is_live(t) && t->registered) {
		tier_sb_write_all(t, tier_sb_persist_cb, NULL);
	}
	free(m);
}

/* A WRITE/UNMAP/WRITE_ZEROES/FLUSH to a mirrored-md leg that does NOT land while
 * the sibling leg DID leaves the two metadata copies divergent and both ACTIVE:
 * resync_md only targets DEGRADED legs, so it would never repair them, and a
 * reboot could prefer the stale copy. Degrade the failing leg and persist.
 * Idempotent. May run on any reactor; the persist is funneled to t->thread. */
static void
tier_degrade_md_leg(struct vbdev_tier *t, struct tier_band *leg)
{
	if (leg == NULL || leg->state != TIER_BAND_ACTIVE) {
		return;
	}
	SPDK_ERRLOG("tier '%s': md write failed on band %u — degrading leg\n",
		    t->bdev.name, leg->band_id);
	leg->state = TIER_BAND_DEGRADED;
	if (!t->registered) {
		return;
	}
	if (spdk_get_thread() == t->thread) {
		tier_sb_write_all(t, tier_sb_persist_cb, NULL);
		return;
	}
	struct tier_degrade_msg *m = calloc(1, sizeof(*m));
	if (m == NULL) {
		/* The RAM state is DEGRADED (reads/fan-outs already avoid the leg);
		 * the next SB persist from any path records it durably. */
		SPDK_ERRLOG("tier '%s': cannot queue DEGRADED persist for band %u (out "
			    "of memory) — will persist with the next SB write\n",
			    t->bdev.name, leg->band_id);
		return;
	}
	m->t = t;
	m->band_id = leg->band_id;
	spdk_thread_send_msg(t->thread, tier_degrade_persist_msg, m);
}

/* True iff `band` is an md-mirror leg and the base-physical offset `base_phys`
 * lands inside the mirrored md region [sb_blocks, sb_blocks+md). Classify a
 * completing leg from its own offset, NOT from the whole orig_io range, which
 * mis-classifies a whole-device op. */
static inline bool
tier_leg_is_md(const struct vbdev_tier *t, const struct tier_band *band, uint64_t base_phys)
{
	return band != NULL &&
	       (band->band_id == t->md_mirror_a || band->band_id == t->md_mirror_b) &&
	       base_phys >= t->sb_blocks && base_phys < t->sb_blocks + t->md_num_blocks;
}

static void
_tier_leg_complete(struct spdk_bdev_io *leg_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	struct tier_bdev_io *io_ctx = (struct tier_bdev_io *)orig_io->driver_ctx;
	struct vbdev_tier *t = SPDK_CONTAINEROF(orig_io->bdev, struct vbdev_tier, bdev);
	struct tier_io_channel *leg_tch = spdk_io_channel_get_ctx(io_ctx->ch);
	struct spdk_bdev *leg_bdev = leg_io->bdev;	/* for accounting after free_io */
	bool md_range = vbdev_tier_is_md_range(t, orig_io->u.bdev.offset_blocks,
					       orig_io->u.bdev.num_blocks);

	if (success) {
		io_ctx->good_legs++;
	} else {
		struct tier_band *fb = tier_band_by_base_bdev(t, leg_io->bdev);
		/* Classify THIS leg from its own base offset, not orig_io's range. */
		bool leg_md = tier_leg_is_md(t, fb, leg_io->u.bdev.offset_blocks);

		if (leg_md && orig_io->type == SPDK_BDEV_IO_TYPE_READ && !io_ctx->md_retry_done) {
			/* Async media error on one md leg — the mirror holds a healthy
			 * copy; fail over instead of failing the read. */
			struct tier_band *alt = tier_md_other_leg(t, fb);

			if (alt != NULL && alt != fb && alt->state == TIER_BAND_ACTIVE &&
			    alt->desc != NULL) {
				struct tier_io_channel *tch = spdk_io_channel_get_ctx(io_ctx->ch);

				io_ctx->md_retry_done = true;
				spdk_bdev_free_io(leg_io);
				tier_leg_channel_release(t, leg_tch, leg_bdev);
				if (tier_submit_leg(t, tch, orig_io, alt,
						    t->sb_blocks + orig_io->u.bdev.offset_blocks) == 0) {
					return;	/* retry in flight; completion re-enters here */
				}
				/* The retry submission failed and leg_io is ALREADY freed.
				 * Complete the (single-leg) md read as failed here and return —
				 * falling through would free leg_io a second time. */
				io_ctx->status = SPDK_BDEV_IO_STATUS_FAILED;
				if (--io_ctx->remaining == 0) {
					spdk_bdev_io_complete(orig_io, io_ctx->status);
				}
				return;
			}
		}
		if (leg_md && orig_io->type != SPDK_BDEV_IO_TYPE_READ) {
			/* A failed md-mirror WRITE/mgmt leg diverges — degrade + persist. */
			tier_degrade_md_leg(t, fb);
		}
		io_ctx->status = SPDK_BDEV_IO_STATUS_FAILED;
	}
	spdk_bdev_free_io(leg_io);
	tier_leg_channel_release(t, leg_tch, leg_bdev);

	if (--io_ctx->remaining == 0) {
		/* An md-mirror WRITE/mgmt op succeeds when at least one leg persisted it
		 * (the failed leg was degraded above — raid1 semantics). A leg whose
		 * SUBMISSION failed got no data and no degrade, so the orig must fail. */
		if (md_range && orig_io->type != SPDK_BDEV_IO_TYPE_READ &&
		    io_ctx->good_legs > 0 && !io_ctx->submit_failed) {
			spdk_bdev_io_complete(orig_io, SPDK_BDEV_IO_STATUS_SUCCESS);
			return;
		}
		spdk_bdev_io_complete(orig_io, io_ctx->status);
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

/* Submit one leg of the original I/O (any supported type) to a band at the given
 * base-physical offset. */
static int
tier_submit_leg(struct vbdev_tier *t, struct tier_io_channel *tch, struct spdk_bdev_io *bdev_io,
		struct tier_band *band, uint64_t base_phys)
{
	struct spdk_io_channel *base_ch = tch->base_ch[band->band_id];
	struct spdk_bdev_ext_io_opts io_opts;
	int rc;

	if (spdk_unlikely(band->state != TIER_BAND_ACTIVE || band->desc == NULL ||
			  base_ch == NULL)) {
		return -EIO;	/* per-band isolation: caller fails THIS io, not the chunk */
	}

	switch (bdev_io->type) {
	case SPDK_BDEV_IO_TYPE_READ:
		tier_init_ext_io_opts(bdev_io, &io_opts);
		rc = spdk_bdev_readv_blocks_ext(band->desc, base_ch, bdev_io->u.bdev.iovs,
						bdev_io->u.bdev.iovcnt, base_phys,
						bdev_io->u.bdev.num_blocks, _tier_leg_complete,
						bdev_io, &io_opts);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		tier_init_ext_io_opts(bdev_io, &io_opts);
		rc = spdk_bdev_writev_blocks_ext(band->desc, base_ch, bdev_io->u.bdev.iovs,
						 bdev_io->u.bdev.iovcnt, base_phys,
						 bdev_io->u.bdev.num_blocks, _tier_leg_complete,
						 bdev_io, &io_opts);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		rc = spdk_bdev_write_zeroes_blocks(band->desc, base_ch, base_phys,
						   bdev_io->u.bdev.num_blocks,
						   _tier_leg_complete, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		rc = spdk_bdev_unmap_blocks(band->desc, base_ch, base_phys,
					    bdev_io->u.bdev.num_blocks,
					    _tier_leg_complete, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_FLUSH:
		rc = spdk_bdev_flush_blocks(band->desc, base_ch, base_phys,
					    bdev_io->u.bdev.num_blocks,
					    _tier_leg_complete, bdev_io);
		break;
	default:
		return -EINVAL;
	}
	if (rc == 0) {
		/* Gate this reactor's channel put on the leg's completion. */
		tch->inflight[band->band_id]++;
	}
	return rc;
}

/* Fan an md-region WRITE / WRITE_ZEROES / UNMAP / FLUSH out to every ACTIVE md
 * leg. Once one leg is submitted, the orig_io completes ONLY from
 * _tier_leg_complete — a later submission failure just drops that leg's count.
 * Returns 0 if at least one leg is in flight, -errno if none was submitted. */
static int
tier_route_md_fanout(struct vbdev_tier *t, struct tier_io_channel *tch,
		     struct spdk_bdev_io *bdev_io)
{
	struct tier_bdev_io *io_ctx = (struct tier_bdev_io *)bdev_io->driver_ctx;
	struct tier_band *legs[2];
	struct tier_band *failed[2];
	struct tier_band *a = vbdev_tier_band_by_id(t, t->md_mirror_a);
	struct tier_band *b = vbdev_tier_band_by_id(t, t->md_mirror_b);
	int nlegs = 0, submitted = 0, nfailed = 0, i, rc = -EIO;

	if (a != NULL && a->state == TIER_BAND_ACTIVE) {
		legs[nlegs++] = a;
	}
	if (b != NULL && b->state == TIER_BAND_ACTIVE) {
		legs[nlegs++] = b;
	}
	if (nlegs == 0) {
		return -EIO;
	}
	io_ctx->remaining = nlegs;
	for (i = 0; i < nlegs; i++) {
		rc = tier_submit_leg(t, tch, bdev_io, legs[i],
				     t->sb_blocks + bdev_io->u.bdev.offset_blocks);
		if (rc != 0) {
			io_ctx->status = SPDK_BDEV_IO_STATUS_FAILED;
			io_ctx->submit_failed = true;
			io_ctx->remaining--;
			failed[nfailed++] = legs[i];
		} else {
			submitted++;
		}
	}
	if (submitted == 0) {
		return rc;	/* nothing in flight; caller completes/requeues, no leg wrote */
	}
	/* A sibling leg is in flight and WILL write the new md data, so any md leg that
	 * failed to submit now diverges from it — degrade it. Otherwise both legs stay
	 * ACTIVE, resync_md (DEGRADED-only) never repairs the silent divergence, and a
	 * reboot could prefer the stale copy. */
	for (i = 0; i < nfailed; i++) {
		tier_degrade_md_leg(t, failed[i]);
	}
	return 0;
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

	/* Metadata region: RAID1-mirrored across two bands when the composite has ≥2 disks;
	 * single-copy on the one band when the node has a single disk (band_b == NULL). The md
	 * maps to base-physical [sb_blocks, sb_blocks+md) on each md band. */
	if (vbdev_tier_is_md_range(t, offset, num)) {
		if (!is_write) {
			band = vbdev_tier_band_by_id(t, t->md_mirror_a);
			band_b = vbdev_tier_band_by_id(t, t->md_mirror_b);	/* may be NULL: single-band composite */
			if (band == NULL) {
				return -EIO;
			}
			/* Prefer an ACTIVE leg; with a mirror, fall back to the secondary.
			 * (An ASYNC media error on the chosen leg is retried on the mirror
			 * by _tier_leg_complete.) */
			struct tier_band *src = band;
			if (band->state != TIER_BAND_ACTIVE && band_b != NULL && band_b->state == TIER_BAND_ACTIVE) {
				src = band_b;
			}
			io_ctx->remaining = 1;
			rc = tier_submit_leg(t, tch, bdev_io, src, t->sb_blocks + offset);
			if (rc != 0 && src == band && band_b != NULL && band_b->state == TIER_BAND_ACTIVE) {
				rc = tier_submit_leg(t, tch, bdev_io, band_b, t->sb_blocks + offset);
			}
			return rc;
		}
		/* Write: fan out to every ACTIVE md leg. */
		return tier_route_md_fanout(t, tch, bdev_io);
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
	return tier_submit_leg(t, tch, bdev_io, band, band->phys_offset + band_off);
}

/* Submit a FLUSH/UNMAP/WRITE_ZEROES to one band over an explicit sub-range. */
static int
tier_submit_mgmt_range(struct tier_band *band, struct spdk_io_channel *base_ch,
		       enum spdk_bdev_io_type type, uint64_t base_phys, uint64_t num,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	switch (type) {
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		return spdk_bdev_write_zeroes_blocks(band->desc, base_ch, base_phys, num, cb, cb_arg);
	case SPDK_BDEV_IO_TYPE_UNMAP:
		return spdk_bdev_unmap_blocks(band->desc, base_ch, base_phys, num, cb, cb_arg);
	case SPDK_BDEV_IO_TYPE_FLUSH:
		return spdk_bdev_flush_blocks(band->desc, base_ch, base_phys, num, cb, cb_arg);
	default:
		return -EINVAL;
	}
}

/* A RETIRED band keeps its composite LBA range as an unreclaimable HOLE
 * (vbdev_tier_band_of_lba skips RETIRED). For a FLUSH/UNMAP/WRITE_ZEROES a hole is
 * a durable NO-OP, so the mgmt router skips it instead of failing the whole op.
 * Returns the end LBA of the retired band covering `lba` so the caller can advance
 * past the hole, or 0 if `lba` is not inside one (a genuine gap is still an error). */
static uint64_t
tier_retired_hole_end(struct vbdev_tier *t, uint64_t lba)
{
	struct tier_band *b;

	TAILQ_FOREACH(b, &t->bands, link) {
		if (b->state == TIER_BAND_RETIRED &&
		    lba >= b->lba_start && lba < b->lba_start + b->num_blocks) {
			return b->lba_start + b->num_blocks;
		}
	}
	return 0;
}

/* Route a FLUSH/UNMAP/WRITE_ZEROES over ANY composite range, splitting it into one
 * leg per covered region: the mirrored md portion fans out to both md legs (a
 * single-leg unmap would desync the metadata copies), and each data band the range
 * crosses gets its own leg. This is what makes a whole-device FLUSH work, since it
 * spans md + every band. Returns 0 if any leg is in flight (or the op no-opped over
 * a hole and was completed here), -errno otherwise. */
static int
tier_route_mgmt(struct vbdev_tier *t, struct tier_io_channel *tch, struct spdk_bdev_io *bdev_io)
{
	struct tier_bdev_io *io_ctx = (struct tier_bdev_io *)bdev_io->driver_ctx;
	uint64_t offset = bdev_io->u.bdev.offset_blocks;
	uint64_t end = offset + bdev_io->u.bdev.num_blocks;
	struct tier_mgmt_seg {
		struct tier_band	*band;
		uint64_t		base_phys;
		uint64_t		num;
		bool			is_md;	/* md-region leg (degrade sibling on divergence) */
	} segs[2 + TIER_MAX_BANDS];
	struct tier_band *md_failed[2];
	int nseg = 0, submitted = 0, md_submitted = 0, md_nfailed = 0, i, rc = -EIO;
	uint64_t pos;

	/* md-covered portion [offset, min(end, md)) — mirrored across the active md legs. */
	if (offset < t->md_num_blocks) {
		uint64_t md_num = spdk_min(end, t->md_num_blocks) - offset;
		struct tier_band *legs[2];
		int n = 0, k;

		legs[0] = vbdev_tier_band_by_id(t, t->md_mirror_a);
		legs[1] = vbdev_tier_band_by_id(t, t->md_mirror_b);
		for (k = 0; k < 2; k++) {
			if (legs[k] != NULL && legs[k]->state == TIER_BAND_ACTIVE) {
				segs[nseg].band = legs[k];
				segs[nseg].base_phys = t->sb_blocks + offset;
				segs[nseg].num = md_num;
				segs[nseg].is_md = true;
				nseg++;
				n++;
			}
		}
		if (n == 0) {
			return -EIO;	/* md region has no active leg */
		}
	}

	/* data portion [max(offset, md), end) — one leg per owning band segment. A
	 * RETIRED band's range is a hole: skip it (mgmt no-op) instead of failing, so a
	 * whole-device FLUSH/UNMAP/WRITE_ZEROES survives a retired band. */
	pos = spdk_max(offset, t->md_num_blocks);
	while (pos < end) {
		uint64_t band_off, seg_num;
		struct tier_band *band = vbdev_tier_band_of_lba(t, pos, &band_off);

		if (band == NULL) {
			uint64_t hole_end = tier_retired_hole_end(t, pos);

			if (hole_end > pos) {
				pos = spdk_min(hole_end, end);	/* skip the retired hole */
				continue;
			}
			return -EIO;	/* a genuinely unmapped gap is still an error */
		}
		if (nseg >= (int)SPDK_COUNTOF(segs)) {
			return -EIO;	/* too many segments */
		}
		seg_num = spdk_min(end - pos, band->num_blocks - band_off);
		segs[nseg].band = band;
		segs[nseg].base_phys = band->phys_offset + band_off;
		segs[nseg].num = seg_num;
		segs[nseg].is_md = false;
		nseg++;
		pos += seg_num;
	}

	if (nseg == 0) {
		/* The whole range fell inside retired hole(s) — a mgmt op over a hole is a
		 * durable no-op. Complete SUCCESS here (returning 0; the single caller does
		 * nothing more on rc == 0). */
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
		return 0;
	}
	int flush_skipped = 0;

	io_ctx->remaining = nseg;
	for (i = 0; i < nseg; i++) {
		struct tier_band *band = segs[i].band;
		struct spdk_io_channel *base_ch = band->band_id < TIER_MAX_BANDS ?
						  tch->base_ch[band->band_id] : NULL;

		if (band->state != TIER_BAND_ACTIVE || band->desc == NULL || base_ch == NULL) {
			/* A FLUSH over a DEGRADED *data* band is a no-op, not a failure:
			 * the range is already unreachable (reads and writes return -EIO)
			 * and no flush can make an unreachable range durable. Failing the
			 * segment would break every whole-device flush barrier, which
			 * defeats the per-band isolation design. Mutations
			 * (UNMAP/WRITE_ZEROES) and md legs keep failing honestly. */
			if (bdev_io->type == SPDK_BDEV_IO_TYPE_FLUSH && !segs[i].is_md) {
				SPDK_WARNLOG("tier '%s': flush skips degraded band %u "
					     "(range already unreachable)\n",
					     t->bdev.name, band->band_id);
				flush_skipped++;
				io_ctx->remaining--;
				continue;
			}
			rc = -EIO;	/* per-band isolation: this leg fails, op fails */
		} else {
			rc = tier_submit_mgmt_range(band, base_ch, bdev_io->type,
						    segs[i].base_phys, segs[i].num,
						    _tier_leg_complete, bdev_io);
		}
		if (rc != 0) {
			io_ctx->status = SPDK_BDEV_IO_STATUS_FAILED;
			io_ctx->submit_failed = true;
			io_ctx->remaining--;
			if (segs[i].is_md && md_nfailed < (int)SPDK_COUNTOF(md_failed)) {
				md_failed[md_nfailed++] = band;
			}
		} else {
			/* Gate this reactor's channel put on the leg's completion. */
			tch->inflight[band->band_id]++;
			submitted++;
			if (segs[i].is_md) {
				md_submitted++;
			}
		}
	}
	if (submitted == 0) {
		if (flush_skipped > 0 && !io_ctx->submit_failed) {
			/* Every segment was a degraded-band flush no-op. */
			spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_SUCCESS);
			return 0;
		}
		return rc;	/* nothing in flight; caller completes the orig_io */
	}
	/* If at least one md leg took the md-region write, any md leg whose md-region
	 * seg failed to submit now diverges — degrade it. If NO md leg took it, the two
	 * legs are still identical, so leave them ACTIVE. */
	if (md_submitted > 0) {
		for (i = 0; i < md_nfailed; i++) {
			tier_degrade_md_leg(t, md_failed[i]);
		}
	}
	return 0;
}

/* RESET fan-out: reset every distinct active base disk and complete the composite
 * reset only when all legs finish. Uses a dedicated completion (not
 * _tier_leg_complete) because a reset carries no LBA range — is_md_range() would
 * misclassify it and wrongly degrade an md leg on failure. */
static void
_tier_reset_leg_complete(struct spdk_bdev_io *leg_io, bool success, void *cb_arg)
{
	struct spdk_bdev_io *orig_io = cb_arg;
	struct tier_bdev_io *io_ctx = (struct tier_bdev_io *)orig_io->driver_ctx;
	struct vbdev_tier *t = SPDK_CONTAINEROF(orig_io->bdev, struct vbdev_tier, bdev);
	struct spdk_bdev *leg_bdev = leg_io->bdev;

	if (!success) {
		io_ctx->status = SPDK_BDEV_IO_STATUS_FAILED;
	}
	spdk_bdev_free_io(leg_io);
	tier_leg_channel_release(t, spdk_io_channel_get_ctx(io_ctx->ch), leg_bdev);
	if (--io_ctx->remaining == 0) {
		spdk_bdev_io_complete(orig_io, io_ctx->status);
	}
}

static int
tier_route_reset(struct vbdev_tier *t, struct tier_io_channel *tch, struct spdk_bdev_io *bdev_io)
{
	struct tier_bdev_io *io_ctx = (struct tier_bdev_io *)bdev_io->driver_ctx;
	struct tier_band *b;
	int nlegs = 0, submitted = 0, rc = -EIO;

	TAILQ_FOREACH(b, &t->bands, link) {
		if (b->state == TIER_BAND_ACTIVE && b->desc != NULL &&
		    b->band_id < TIER_MAX_BANDS && tch->base_ch[b->band_id] != NULL) {
			nlegs++;
		}
	}
	if (nlegs == 0) {
		return -EIO;
	}
	io_ctx->remaining = nlegs;
	TAILQ_FOREACH(b, &t->bands, link) {
		struct spdk_io_channel *base_ch;

		if (b->state != TIER_BAND_ACTIVE || b->desc == NULL ||
		    b->band_id >= TIER_MAX_BANDS || tch->base_ch[b->band_id] == NULL) {
			continue;
		}
		base_ch = tch->base_ch[b->band_id];
		rc = spdk_bdev_reset(b->desc, base_ch, _tier_reset_leg_complete, bdev_io);
		if (rc != 0) {
			io_ctx->status = SPDK_BDEV_IO_STATUS_FAILED;
			io_ctx->remaining--;
		} else {
			/* Gate this reactor's channel put on the leg's completion. */
			tch->inflight[b->band_id]++;
			submitted++;
		}
	}
	if (submitted == 0) {
		return rc;
	}
	return 0;
}

static void
tier_read_get_buf_cb(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io, bool success)
{
	struct vbdev_tier *t = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_tier, bdev);
	struct tier_io_channel *tch = spdk_io_channel_get_ctx(ch);
	int rc;

	if (!success) {
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	rc = tier_route_rw(t, tch, bdev_io, false);
	if (rc != 0) {
		/* -ENOMEM completes NOMEM so the bdev core requeues and retries the whole
		 * submit; this covers md reads too, whose range is not owned by any band. */
		spdk_bdev_io_complete(bdev_io, rc == -ENOMEM ? SPDK_BDEV_IO_STATUS_NOMEM :
				      SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static void
vbdev_tier_submit_request(struct spdk_io_channel *ch, struct spdk_bdev_io *bdev_io)
{
	struct vbdev_tier *t = SPDK_CONTAINEROF(bdev_io->bdev, struct vbdev_tier, bdev);
	struct tier_io_channel *tch = spdk_io_channel_get_ctx(ch);
	struct tier_bdev_io *io_ctx = (struct tier_bdev_io *)bdev_io->driver_ctx;
	int rc = 0;

	io_ctx->ch = ch;
	io_ctx->status = SPDK_BDEV_IO_STATUS_SUCCESS;
	io_ctx->good_legs = 0;
	io_ctx->md_retry_done = false;
	io_ctx->submit_failed = false;

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
		/* A mgmt op is a mutation/barrier — route it to EVERY covered leg (both md
		 * legs for the mirrored portion, one per data band). This is the only
		 * correct handling for a range spanning the md/data boundary or multiple
		 * bands, including the whole-device flush an NVMe Flush issues. */
		rc = tier_route_mgmt(t, tch, bdev_io);
		break;
	case SPDK_BDEV_IO_TYPE_RESET:
		rc = tier_route_reset(t, tch, bdev_io);
		break;
	default:
		SPDK_ERRLOG("tier: unsupported I/O type %d\n", bdev_io->type);
		spdk_bdev_io_complete(bdev_io, SPDK_BDEV_IO_STATUS_FAILED);
		return;
	}

	if (rc != 0) {
		/* Propagate -ENOMEM as NOMEM (bdev-core retry); everything else fails. */
		spdk_bdev_io_complete(bdev_io, rc == -ENOMEM ? SPDK_BDEV_IO_STATUS_NOMEM :
				      SPDK_BDEV_IO_STATUS_FAILED);
	}
}

static bool
vbdev_tier_io_type_supported(void *ctx, enum spdk_bdev_io_type io_type)
{
	struct vbdev_tier *t = (struct vbdev_tier *)ctx;
	struct tier_band *b;

	switch (io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
	case SPDK_BDEV_IO_TYPE_WRITE:
	case SPDK_BDEV_IO_TYPE_RESET:
		return true;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
	case SPDK_BDEV_IO_TYPE_UNMAP:
	case SPDK_BDEV_IO_TYPE_FLUSH:
		/* Every leg is submitted to a base band, so the composite can only
		 * honor these if EVERY active band's base bdev does — advertising a
		 * type a band cannot execute makes the leg complete FAILED (a base
		 * lacking UNMAP would fail every discard the consumer was told worked). */
		TAILQ_FOREACH(b, &t->bands, link) {
			if (b->state == TIER_BAND_RETIRED || b->desc == NULL) {
				continue;
			}
			if (!spdk_bdev_io_type_supported(spdk_bdev_desc_get_bdev(b->desc),
							 io_type)) {
				return false;
			}
		}
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

	memset(tch, 0, sizeof(*tch));
	TAILQ_FOREACH(b, &t->bands, link) {
		if (b->state != TIER_BAND_RETIRED && b->desc != NULL &&
		    b->band_id < TIER_MAX_BANDS) {
			tch->base_ch[b->band_id] = spdk_bdev_get_io_channel(b->desc);
			if (tch->base_ch[b->band_id] == NULL) {
				/* An ACTIVE band with a NULL base channel would return -EIO for
				 * every I/O on this reactor (an unexplained per-core failure).
				 * Fail channel creation instead, releasing the channels already
				 * opened for this tch. */
				uint32_t j;

				SPDK_ERRLOG("tier '%s': cannot open base channel for band %u\n",
					    t->bdev.name, b->band_id);
				for (j = 0; j < TIER_MAX_BANDS; j++) {
					if (tch->base_ch[j] != NULL) {
						spdk_put_io_channel(tch->base_ch[j]);
						tch->base_ch[j] = NULL;
					}
				}
				return -ENOMEM;
			}
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

/* --------------------------------------------------------------------------
 * Band drain + close: remove the band's per-reactor base channels, THEN close its
 * desc. Closing without the drain violates the SPDK ownership contract (channels
 * outlive the desc) and, on hot-remove, leaves the base bdev's unregister pending
 * forever.
 * -------------------------------------------------------------------------- */

struct tier_band_drain_ctx {
	struct vbdev_tier	*t;
	struct tier_band	*band;
};

/* Final step of a band drain — runs on the app thread once every reactor has put
 * its base channel (drain_refs == 0). The desc close itself is still gated on
 * desc_pins: a relocate/resync engine submitting on this desc defers it to the
 * engine's terminal (tier_band_desc_unpin). */
static void
tier_band_try_close(struct tier_band *band)
{
	assert(spdk_get_thread() == band->t->thread);
	if (band->desc_pins > 0) {
		band->close_deferred = true;
		return;
	}
	if (band->desc != NULL) {
		spdk_bdev_module_release_bdev(spdk_bdev_desc_get_bdev(band->desc));
		spdk_bdev_close(band->desc);
		band->desc = NULL;
	}
	band->draining = false;
	if (band->drain_cb) {
		void (*cb)(void *, int) = band->drain_cb;
		void *cb_arg = band->drain_cb_arg;

		band->drain_cb = NULL;
		band->drain_cb_arg = NULL;
		cb(cb_arg, 0);
	}
}

static void
tier_band_try_close_msg(void *arg)
{
	tier_band_try_close(arg);
}

/* Release one drain reference (a reactor that finished its deferred put, or the
 * drain fan-out itself). The LAST release performs the close — on the app thread,
 * because spdk_bdev_close must run on the opening thread. */
static void
tier_band_drain_release(struct tier_band *band)
{
	if (__atomic_sub_fetch(&band->drain_refs, 1, __ATOMIC_ACQ_REL) != 0) {
		return;
	}
	if (spdk_get_thread() == band->t->thread) {
		tier_band_try_close(band);
	} else {
		spdk_thread_send_msg(band->t->thread, tier_band_try_close_msg, band);
	}
}

static void
tier_band_drain_ch_iter(struct spdk_io_channel_iter *i)
{
	struct tier_band_drain_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct tier_io_channel *tch = spdk_io_channel_get_ctx(_ch);
	uint32_t id = ctx->band->band_id;

	if (id < TIER_MAX_BANDS && tch->base_ch[id] != NULL) {
		if (tch->inflight[id] > 0) {
			/* Legs in flight on this reactor — putting the channel now would
			 * destroy it with io_outstanding > 0. Defer the put to the last
			 * leg completion. */
			__atomic_fetch_add(&ctx->band->drain_refs, 1, __ATOMIC_RELAXED);
			tch->drain_deferred[id] = true;
		} else {
			spdk_put_io_channel(tch->base_ch[id]);
			tch->base_ch[id] = NULL;
		}
	}
	spdk_for_each_channel_continue(i, 0);
}

static void
tier_band_drain_done(struct spdk_io_channel_iter *i, int status)
{
	struct tier_band_drain_ctx *ctx = spdk_io_channel_iter_get_ctx(i);
	struct tier_band *band = ctx->band;

	(void)status;
	free(ctx);
	/* Release the fan-out's own reference; if no reactor deferred, this is
	 * the last one and the close runs now (we are on the app thread). */
	tier_band_drain_release(band);
}

static int
tier_band_drain_and_close(struct vbdev_tier *t, struct tier_band *band,
			  void (*cb)(void *cb_arg, int rc), void *cb_arg)
{
	struct tier_band_drain_ctx *ctx;

	if (band->draining) {
		/* One drain at a time per band; initiations are app-thread-only. The
		 * in-flight drain finishes the job, but a second explicit-cb caller must
		 * not lose its completion, so reject it (retire retries idempotently). */
		return cb != NULL ? -EALREADY : 0;
	}
	if (!t->registered) {
		/* No io_device / channels yet: close directly (respecting pins). */
		band->draining = true;
		band->drain_cb = cb;
		band->drain_cb_arg = cb_arg;
		tier_band_try_close(band);
		return 0;
	}
	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}
	ctx->t = t;
	ctx->band = band;
	band->draining = true;
	band->drain_cb = cb;
	band->drain_cb_arg = cb_arg;
	__atomic_store_n(&band->drain_refs, 1, __ATOMIC_RELAXED);
	spdk_for_each_channel(t, tier_band_drain_ch_iter, ctx, tier_band_drain_done);
	return 0;
}

/* Bracket an engine's (relocate / md-resync) use of a band's desc. App thread
 * only. A drain that lands while pinned closes the desc at unpin. */
static void
tier_band_desc_pin(struct tier_band *band)
{
	band->desc_pins++;
}

static void
tier_band_desc_unpin(struct tier_band *band)
{
	assert(band->desc_pins > 0);
	if (--band->desc_pins == 0 && band->close_deferred) {
		band->close_deferred = false;
		tier_band_try_close(band);
	}
}

/* Base bdev hot-remove: degrade the band (per-band isolation — do NOT tear down
 * the composite), PERSIST the degradation, and honor the SPDK REMOVE contract
 * (drain channels + close desc). */
static void
tier_base_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *event_ctx)
{
	struct tier_band *band = event_ctx;
	struct vbdev_tier *t = band->t;

	if (type != SPDK_BDEV_EVENT_REMOVE) {
		return;
	}
	SPDK_WARNLOG("tier: base bdev '%s' removed, degrading band %u\n",
		     bdev->name, band->band_id);
	band->state = TIER_BAND_DEGRADED;
	/* Persist DEGRADED to the surviving bands (the fan-out excludes this one) —
	 * otherwise a reboot reassembles the band ACTIVE and md reads can prefer its
	 * STALE metadata copy, which is silent corruption. */
	if (t->registered) {
		tier_sb_write_all(t, tier_sb_persist_cb, NULL);
	}
	/* Close the desc (after the channel drain) or the removed base bdev's
	 * unregister pends forever. An in-flight SB fan-out may hold a write on THIS
	 * band's desc, so defer the drain+close to vbdev_tier_sb_fanout_idle rather
	 * than break the channel-before-desc ownership contract. */
	if (t->sb_write_inflight || t->sb_write_queued) {
		band->close_pending = true;
	} else if (tier_band_drain_and_close(t, band, NULL, NULL) != 0) {
		SPDK_ERRLOG("tier: cannot drain band %u after hot-remove (out of memory); "
			    "desc left open\n", band->band_id);
	}
}

/* --------------------------------------------------------------------------
 * Lifecycle: create / add_band / retire_band
 * -------------------------------------------------------------------------- */

struct vbdev_tier *
vbdev_tier_create(const char *name, uint64_t md_num_blocks, uint64_t cluster_blocks)
{
	struct vbdev_tier *t;

	t = calloc(1, sizeof(*t));
	if (t == NULL) {
		return NULL;
	}
	TAILQ_INIT(&t->bands);
	TAILQ_INIT(&t->sb_pending_cbs);
	t->next_band_id = 0;
	/* Composite-global SB state is owned by this (the app/RPC) thread. */
	t->thread = spdk_get_thread();
	/* The md region is the FIRST boundary the blobstore crosses; round it UP to the cluster
	 * grain so the md/data boundary is cluster-aligned. Band boundaries are aligned in add_band. */
	t->cluster_blocks = cluster_blocks;
	t->md_num_blocks = tier_align_up(t, md_num_blocks);
	t->md_mirror_a = UINT32_MAX;
	t->md_mirror_b = UINT32_MAX;
	t->blocklen = 0;
	t->total_num_blocks = 0;
	/* Mint the composite INSTANCE uuid — stored in every SB copy. A re-created
	 * composite gets a fresh uuid, so disks left over from a previous life can
	 * never be cross-assembled with the new instance. */
	{
		struct spdk_uuid u;

		SPDK_STATIC_ASSERT(sizeof(u) == TIER_SB_GEN_UUID_LEN, "uuid size");
		spdk_uuid_generate(&u);
		memcpy(t->gen_uuid, &u, sizeof(t->gen_uuid));
	}

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
		    const char *wwn, const char *serial, const uint8_t *part_uuid,
		    uint64_t part_start_lba, uint64_t part_size_blocks, uint32_t *out_band_id)
{
	struct tier_band *band, *existing;
	struct spdk_bdev *base_bdev;
	uint64_t usable_blocks, lba_start, num_blocks, phys_offset;
	int rc;

	/* Geometry is frozen at register (blockcnt, per-reactor base channels): a band
	 * added afterwards would not be addressable yet would be persisted to the SB,
	 * so a reboot would reassemble a geometry the running composite never served.
	 * The CSI contract adds all bands before register; enforce it. */
	if (t->registered) {
		SPDK_ERRLOG("tier '%s': add_band after register is not allowed\n", t->bdev.name);
		return -EBUSY;
	}
	/* After assembly the md legs are either both reoccupied at their stored slots
	 * or their disks are ABSENT. The auto-layout below would promote this NEW
	 * device into the missing slot — a blank ACTIVE mirror leg with no resync —
	 * and the leg-A branch would additionally reset total_num_blocks, laying this
	 * band over the assembled bands' address space. A post-assembly append may
	 * only be a plain concat, so an incomplete md mirror refuses the add. */
	if (t->assembled &&
	    (t->md_mirror_a == UINT32_MAX || t->md_mirror_b == UINT32_MAX)) {
		SPDK_ERRLOG("tier '%s': refusing add_band after assembly with incomplete md "
			    "mirror (a=%u b=%u): the composite is degraded, a new device must "
			    "never be promoted to an md leg\n",
			    t->bdev.name, t->md_mirror_a, t->md_mirror_b);
		return -EPROTO;
	}
	/* Bound the slot BEFORE it is assigned: band_id indexes the fixed-size
	 * base_ch[TIER_MAX_BANDS] and is stored in a 64-slot superblock. An
	 * out-of-range id would read past base_ch[]. */
	if (t->next_band_id >= TIER_MAX_BANDS) {
		SPDK_ERRLOG("tier '%s': cannot add band, max %d reached\n", t->bdev.name,
			    TIER_MAX_BANDS);
		return -ENOSPC;
	}

	/* Reject a disk identity already present in this composite: a duplicate wwn means
	 * the same physical disk was enumerated into two bands, which would silently
	 * double-count capacity and corrupt the concat geometry. */
	if (wwn != NULL && wwn[0] != '\0') {
		TAILQ_FOREACH(existing, &t->bands, link) {
			if (strncmp(existing->wwn, wwn, sizeof(existing->wwn)) == 0) {
				SPDK_ERRLOG("tier: band wwn '%s' already present (duplicate disk '%s')\n",
					    wwn, base_bdev_name);
				return -EEXIST;
			}
		}
	}

	band = calloc(1, sizeof(*band));
	if (band == NULL) {
		return -ENOMEM;
	}
	band->t = t;

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
		/* The superblock is written as whole TIER_SB_SLOT_BYTES slots, so the slot
		 * must be an integral number of blocks. A blocklen that does not divide it
		 * makes the FIRST SB write fail -EINVAL at register, leaving a registered
		 * composite no reassembly can find. Reject the incompatible base here. */
		if (TIER_SB_SLOT_BYTES % t->blocklen != 0) {
			SPDK_ERRLOG("tier: band '%s' blocklen %u cannot host the superblock slot "
				    "(%d not a multiple)\n", base_bdev_name, t->blocklen,
				    TIER_SB_SLOT_BYTES);
			spdk_bdev_close(band->desc);
			free(band);
			return -EINVAL;
		}
	} else if (base_bdev->blocklen != t->blocklen) {
		SPDK_ERRLOG("tier: band '%s' blocklen %u != composite %u\n",
			    base_bdev_name, base_bdev->blocklen, t->blocklen);
		spdk_bdev_close(band->desc);
		free(band);
		return -EINVAL;
	}
	/* Reserve a superblock region at the start of EACH base bdev. */
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
	if (part_uuid) {
		memcpy(band->part_uuid, part_uuid, sizeof(band->part_uuid));
	}
	band->part_start_lba = part_start_lba;
	band->part_size_blocks = part_size_blocks;

	/* Geometry. Each disk reserves [0, sb_blocks) for the superblock; usable =
	 * blockcnt - sb_blocks. The first two bands additionally host the mirrored md
	 * region [0, md_num_blocks) of the composite at base-physical
	 * [sb_blocks, sb_blocks+md_num_blocks); their DATA contribution follows.
	 * Computed into locals and committed to `t` only after every check, so a
	 * refused band leaves the composite untouched. */
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
		lba_start = t->md_num_blocks;			/* this band's data tail follows md (aligned) */
		/* Round the data contribution DOWN to the cluster grain (the trailing remainder is an
		 * unusable hole) so the NEXT band starts cluster-aligned and no cluster straddles. */
		num_blocks = tier_align_down(t, usable_blocks - t->md_num_blocks);
		phys_offset = t->sb_blocks + t->md_num_blocks;
	} else if (t->md_mirror_b == UINT32_MAX) {
		/* Band B: hosts the md MIRROR (not re-counted) + a data tail. */
		if (usable_blocks <= t->md_num_blocks) {
			SPDK_ERRLOG("tier: band '%s' too small for md mirror\n", base_bdev_name);
			spdk_bdev_module_release_bdev(base_bdev);
			spdk_bdev_close(band->desc);
			free(band);
			return -ENOSPC;
		}
		lba_start = t->total_num_blocks;		/* aligned (prior boundaries aligned) */
		num_blocks = tier_align_down(t, usable_blocks - t->md_num_blocks);
		phys_offset = t->sb_blocks + t->md_num_blocks;
	} else {
		/* Plain concat band. */
		lba_start = t->total_num_blocks;		/* aligned */
		num_blocks = tier_align_down(t, usable_blocks);
		phys_offset = t->sb_blocks;
	}

	if (num_blocks == 0) {
		SPDK_ERRLOG("tier: band '%s' has no cluster-aligned capacity (cluster_blocks=%" PRIu64 ")\n",
			    base_bdev_name, t->cluster_blocks);
		spdk_bdev_module_release_bdev(base_bdev);
		spdk_bdev_close(band->desc);
		free(band);
		return -ENOSPC;
	}

	/* Same guard as assemble_band: no two bands may overlap in the composite
	 * address space. The auto-layout above cannot produce one on its own, but
	 * placement must never rest solely on total_num_blocks being the high-water
	 * mark of the bands actually present. */
	TAILQ_FOREACH(existing, &t->bands, link) {
		if (lba_start < existing->lba_start + existing->num_blocks &&
		    existing->lba_start < lba_start + num_blocks) {
			SPDK_ERRLOG("tier: add band '%s' [%" PRIu64 ", +%" PRIu64
				    ") overlaps band %u\n", base_bdev_name, lba_start,
				    num_blocks, existing->band_id);
			spdk_bdev_module_release_bdev(base_bdev);
			spdk_bdev_close(band->desc);
			free(band);
			return -EEXIST;
		}
	}

	/* Commit — nothing below can fail. */
	if (t->md_mirror_a == UINT32_MAX) {
		t->md_mirror_a = band->band_id;
		t->total_num_blocks = t->md_num_blocks;		/* md region occupies [0, md), cluster-aligned;
								 * fresh provisioning only (assembled refuses above) */
	} else if (t->md_mirror_b == UINT32_MAX) {
		t->md_mirror_b = band->band_id;
	}
	band->lba_start = lba_start;
	band->num_blocks = num_blocks;
	band->phys_offset = phys_offset;
	t->total_num_blocks += num_blocks;

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

/* Place a band at an EXPLICIT stored geometry (from the on-disk superblock) instead of the
 * add_band auto-layout. The CSI agent uses this to reassemble a composite IDENTICALLY across
 * reboots (stable slot to lba_start, regardless of disk enumeration order) and to detect a
 * swapped disk, whose live wwn will not match the slot's stored wwn. */
int
vbdev_tier_assemble_band(struct vbdev_tier *t, const char *base_bdev_name, uint32_t band_id,
			 enum tier_class tier, const char *wwn, const char *serial,
			 const uint8_t *part_uuid, uint64_t part_start_lba,
			 uint64_t part_size_blocks, uint64_t lba_start, uint64_t num_blocks,
			 enum tier_band_state state, bool is_md)
{
	static const uint8_t zero_uuid[TIER_PART_UUID_LEN];
	struct tier_band *band, *existing;
	struct spdk_bdev *base_bdev;
	uint64_t phys_offset;
	int rc;

	/* The band table is FROZEN at register (blockcnt and per-reactor base channels
	 * are fixed there). This RPC is SPDK_RPC_RUNTIME, so without the guard it could
	 * splice into t->bands while reactors walk it lock-free, leaving a torn `next`
	 * pointer. Assembly is always create -> assemble -> register; enforce it. */
	if (t->registered) {
		SPDK_ERRLOG("tier '%s': assemble_band after register is not allowed\n", t->bdev.name);
		return -EBUSY;
	}
	/* The RPC decodes band_id/state as raw u32 — bound BOTH before they index
	 * base_ch[] or route I/O (an out-of-enum state would route like ACTIVE). */
	if (band_id >= TIER_MAX_BANDS) {
		SPDK_ERRLOG("tier: assemble band_id %u out of range (max %d)\n",
			    band_id, TIER_MAX_BANDS - 1);
		return -EINVAL;
	}
	if (state > TIER_BAND_RETIRED) {
		SPDK_ERRLOG("tier: assemble band %u invalid state %d\n", band_id, state);
		return -EINVAL;
	}
	if (num_blocks == 0) {
		return -EINVAL;
	}
	/* Reject ranges whose composite end wraps u64 — otherwise the overlap and
	 * capacity checks below (lba_start + num_blocks, phys_offset + num_blocks)
	 * wrap to a small value and an oversized band slips past every guard. */
	if (lba_start > UINT64_MAX - num_blocks) {
		SPDK_ERRLOG("tier: assemble band %u range [%" PRIu64 ", +%" PRIu64 ") overflows\n",
			    band_id, lba_start, num_blocks);
		return -EINVAL;
	}
	/* The data region starts after the mirrored md region; a band placed inside
	 * [0, md) would shadow the mirrored metadata range. */
	if (lba_start < t->md_num_blocks) {
		SPDK_ERRLOG("tier: assemble band %u lba_start %" PRIu64 " inside md region\n",
			    band_id, lba_start);
		return -EINVAL;
	}
	if (is_md && t->md_num_blocks == 0) {
		SPDK_ERRLOG("tier: assemble band %u is_md on a composite without md region\n", band_id);
		return -EINVAL;
	}
	if (is_md && t->md_mirror_a != UINT32_MAX && t->md_mirror_b != UINT32_MAX) {
		SPDK_ERRLOG("tier: assemble band %u — both md mirror slots already assigned\n", band_id);
		return -EINVAL;
	}
	if (vbdev_tier_band_by_id(t, band_id) != NULL) {
		return -EEXIST;
	}
	/* No two bands may overlap in the composite address space; a retired slot keeps
	 * its range as an unreclaimable hole, so it counts too. */
	TAILQ_FOREACH(existing, &t->bands, link) {
		if (lba_start < existing->lba_start + existing->num_blocks &&
		    existing->lba_start < lba_start + num_blocks) {
			SPDK_ERRLOG("tier: assemble band %u [%" PRIu64 ", +%" PRIu64
				    ") overlaps band %u\n", band_id, lba_start, num_blocks,
				    existing->band_id);
			return -EEXIST;
		}
		/* Same duplicate-disk guard as add_band. */
		if (wwn != NULL && wwn[0] != '\0' &&
		    strncmp(existing->wwn, wwn, sizeof(existing->wwn)) == 0) {
			SPDK_ERRLOG("tier: assemble band wwn '%s' already present\n", wwn);
			return -EEXIST;
		}
		/* Mirror of the duplicate-wwn rule for the unit identity: one partition
		 * enumerated into two bands is the same double-count corruption. An
		 * all-zero part_uuid carries no identity and is never compared. */
		if (part_uuid != NULL &&
		    memcmp(part_uuid, zero_uuid, TIER_PART_UUID_LEN) != 0 &&
		    memcmp(existing->part_uuid, part_uuid, TIER_PART_UUID_LEN) == 0) {
			char uuid_hex[2 * TIER_PART_UUID_LEN + 1];

			SPDK_ERRLOG("tier: assemble band %u part_uuid '%s' already present "
				    "on band %u\n", band_id,
				    tier_part_uuid_hex(part_uuid, uuid_hex), existing->band_id);
			return -EEXIST;
		}
	}
	band = calloc(1, sizeof(*band));
	if (band == NULL) {
		return -ENOMEM;
	}
	band->t = t;
	rc = spdk_bdev_open_ext(base_bdev_name, true, tier_base_event_cb, band, &band->desc);
	if (rc != 0) {
		SPDK_ERRLOG("tier: assemble cannot open '%s' rc=%d\n", base_bdev_name, rc);
		free(band);
		return rc;
	}
	base_bdev = spdk_bdev_desc_get_bdev(band->desc);
	if (t->blocklen == 0) {
		t->blocklen = base_bdev->blocklen;
		/* Reject a blocklen that cannot host the fixed superblock slot (see
		 * vbdev_tier_add_band) — else the first SB persist fails and the composite
		 * is unrecoverable. */
		if (TIER_SB_SLOT_BYTES % t->blocklen != 0) {
			SPDK_ERRLOG("tier: assemble band '%s' blocklen %u cannot host the superblock "
				    "slot (%d not a multiple)\n", base_bdev_name, t->blocklen,
				    TIER_SB_SLOT_BYTES);
			spdk_bdev_close(band->desc);
			free(band);
			return -EINVAL;
		}
	} else if (base_bdev->blocklen != t->blocklen) {
		SPDK_ERRLOG("tier: assemble band '%s' blocklen %u != composite %u\n",
			    base_bdev_name, base_bdev->blocklen, t->blocklen);
		spdk_bdev_close(band->desc);
		free(band);
		return -EINVAL;
	}
	if (t->sb_blocks == 0) {
		t->sb_blocks = spdk_divide_round_up(TIER_SB_RESERVE_BYTES, t->blocklen);
	}
	/* The stored geometry must FIT the real disk (the register guard only checks
	 * alignment) — otherwise the band's tail returns -EIO at runtime. */
	phys_offset = is_md ? (t->sb_blocks + t->md_num_blocks) : t->sb_blocks;
	if (phys_offset >= base_bdev->blockcnt ||
	    num_blocks > base_bdev->blockcnt - phys_offset) {
		SPDK_ERRLOG("tier: assemble band %u geometry (phys_off=%" PRIu64 " num=%" PRIu64
			    ") exceeds disk '%s' capacity %" PRIu64 "\n", band_id, phys_offset,
			    num_blocks, base_bdev_name, base_bdev->blockcnt);
		spdk_bdev_close(band->desc);
		free(band);
		return -ENOSPC;
	}
	rc = spdk_bdev_module_claim_bdev(base_bdev, band->desc, &tier_if);
	if (rc != 0) {
		SPDK_ERRLOG("tier: assemble cannot claim '%s' rc=%d\n", base_bdev_name, rc);
		spdk_bdev_close(band->desc);
		free(band);
		return rc;
	}
	band->band_id = band_id;
	band->tier = tier;
	band->state = state;
	snprintf(band->base_bdev_name, sizeof(band->base_bdev_name), "%s", base_bdev_name);
	if (wwn) {
		snprintf(band->wwn, sizeof(band->wwn), "%s", wwn);
	}
	if (serial) {
		snprintf(band->serial, sizeof(band->serial), "%s", serial);
	}
	if (part_uuid) {
		memcpy(band->part_uuid, part_uuid, sizeof(band->part_uuid));
	}
	band->part_start_lba = part_start_lba;
	band->part_size_blocks = part_size_blocks;
	band->lba_start = lba_start;
	band->num_blocks = num_blocks;
	/* md-hosting bands carry the mirrored md region at base-physical [sb_blocks, sb_blocks+md); their
	 * data tail starts after it. Plain bands start at sb_blocks. (Matches vbdev_tier_add_band.) */
	band->phys_offset = is_md ? (t->sb_blocks + t->md_num_blocks) : t->sb_blocks;
	if (is_md) {
		if (t->md_mirror_a == UINT32_MAX) {
			t->md_mirror_a = band_id;
		} else if (t->md_mirror_b == UINT32_MAX) {
			t->md_mirror_b = band_id;
		}
	}
	if (band_id >= t->next_band_id) {
		t->next_band_id = band_id + 1;
	}
	if (lba_start + num_blocks > t->total_num_blocks) {
		t->total_num_blocks = lba_start + num_blocks;
	}
	TAILQ_INSERT_TAIL(&t->bands, band, link);
	t->num_bands++;
	t->assembled = true;	/* from now on add_band may only append a plain concat band */
	SPDK_NOTICELOG("tier '%s': assembled band %u ('%s', tier=%d, state=%d) lba_start=%" PRIu64
		       " num_blocks=%" PRIu64 " is_md=%d\n", t->bdev.name, band_id, base_bdev_name,
		       tier, state, lba_start, num_blocks, is_md);
	return 0;
}

struct tier_retire_ctx {
	struct vbdev_tier	*t;
	struct tier_band	*band;
	void			(*cb)(void *cb_arg, int rc);
	void			*cb_arg;
	int			persist_rc;
};

static void
tier_retire_drained(void *cb_arg, int rc)
{
	struct tier_retire_ctx *ctx = cb_arg;

	(void)rc;
	SPDK_NOTICELOG("tier '%s': retired band %u (persist rc=%d)\n",
		       ctx->t->bdev.name, ctx->band->band_id, ctx->persist_rc);
	if (ctx->cb) {
		ctx->cb(ctx->cb_arg, ctx->persist_rc);
	}
	free(ctx);
}

static void
tier_retire_persisted(void *cb_arg, int rc)
{
	struct tier_retire_ctx *ctx = cb_arg;

	ctx->persist_rc = rc;
	/* Drain the band's per-reactor channels BEFORE closing its desc. On a persist
	 * failure we still drain+close, but report the error so the caller retries the
	 * idempotent retire until the SB is durable. */
	if (tier_band_drain_and_close(ctx->t, ctx->band, tier_retire_drained, ctx) != 0) {
		if (ctx->cb) {
			ctx->cb(ctx->cb_arg, rc != 0 ? rc : -ENOMEM);
		}
		free(ctx);
	}
}

int
vbdev_tier_retire_band(struct vbdev_tier *t, uint32_t band_id,
		       void (*cb)(void *cb_arg, int rc), void *cb_arg)
{
	struct tier_band *band = vbdev_tier_band_by_id(t, band_id);
	struct tier_retire_ctx *ctx;

	if (band == NULL) {
		return -ENODEV;
	}
	/* An md-mirror band holds one of the two metadata copies; retiring it would
	 * destroy the blobstore-metadata redundancy with no rebuild path. */
	if (band_id == t->md_mirror_a || band_id == t->md_mirror_b) {
		SPDK_ERRLOG("tier '%s': refusing to retire md-mirror band %u\n",
			    t->bdev.name, band_id);
		return -EBUSY;
	}
	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}
	ctx->t = t;
	ctx->band = band;
	ctx->cb = cb;
	ctx->cb_arg = cb_arg;

	/* The caller guarantees the band was evacuated (clusters relocated) before
	 * retiring. The slot and its LBA range stay as an unreclaimable hole. Re-running
	 * the flow on an already-RETIRED band is the idempotent retry path: it
	 * re-persists (in case the first persist failed) and re-closes. */
	band->state = TIER_BAND_RETIRED;
	/* Persist to the SURVIVING bands BEFORE closing the retired one's desc, so the
	 * seq bump durably records the retirement, then drain+close, then complete: the
	 * caller acks only a durable retirement. */
	if (t->registered) {
		if (tier_sb_write_all(t, tier_retire_persisted, ctx) != 0) {
			free(ctx);
			return -ENOMEM;
		}
	} else {
		tier_retire_persisted(ctx, 0);
	}
	return 0;
}

int
vbdev_tier_delete(struct vbdev_tier *t)
{
	/* An in-flight async op — SB fan-out, relocate copy, md resync, register-time
	 * seq rehydrate — holds this composite's base-band descriptors and pointers into
	 * `t`, so unregistering now would free them under the op. Defer the teardown; a
	 * queued follow-up fan-out counts too, since it will hold descriptors. */
	if (t->sb_write_inflight || t->sb_write_queued || t->async_inflight > 0) {
		t->delete_pending = true;
		return 0;
	}
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

/* Run a teardown deferred behind async work, but only once EVERYTHING that pins
 * `t` has drained (no SB fan-out in flight or queued, no relocate/resync/
 * rehydrate). Returns true if it ran the teardown — the caller must not touch `t`. */
static bool
tier_run_deferred_delete(struct vbdev_tier *t)
{
	if (t->delete_pending && t->async_inflight == 0 &&
	    !t->sb_write_inflight && !t->sb_write_queued) {
		t->delete_pending = false;
		vbdev_tier_delete(t);
		return true;
	}
	return false;
}

/* Bracket a composite async op (relocate / resync / register seq-rehydrate) whose
 * context holds pointers into `t`. Call _begin before launching the async chain and
 * _end at its terminal completion. _end may run a delete deferred behind the op —
 * after it returns, the caller must not touch `t`. */
static void
tier_async_op_begin(struct vbdev_tier *t)
{
	t->async_inflight++;
}

static void
tier_async_op_end(struct vbdev_tier *t)
{
	if (t->async_inflight > 0) {
		t->async_inflight--;
	}
	tier_run_deferred_delete(t);
}

/* Run any teardown deferred behind an SB fan-out, now that it has drained. Called
 * from tier_sb_fanout_complete. Returns true if a deferred delete consumed the
 * composite (caller must not touch `t`). */
bool
vbdev_tier_sb_fanout_idle(struct vbdev_tier *t)
{
	struct tier_band *b, *tmp;

	/* A queued follow-up fan-out still OWNS callbacks coalesced behind this one, and
	 * some of them release an async_inflight ref at their terminal. Dropping the
	 * follow-up would strand those callbacks, leak the ref, and the deferred delete
	 * would NEVER run. Return false and let the caller run the follow-up; tear down
	 * here only when no follow-up remains. */
	if (t->delete_pending) {
		if (t->sb_write_queued) {
			return false;	/* caller runs the queued follow-up (serves the cbs) */
		}
		tier_run_deferred_delete(t);	/* runs iff async_inflight == 0 */
		return true;
	}
	/* A base hot-remove that landed during the fan-out deferred the degraded band's
	 * channel-drain+close. The fan-out has drained, so the desc no longer has an
	 * in-flight SB write — close it now. The band is DEGRADED, hence excluded from
	 * any follow-up fan-out. */
	TAILQ_FOREACH_SAFE(b, &t->bands, link, tmp) {
		if (!b->close_pending) {
			continue;
		}
		b->close_pending = false;
		if (tier_band_drain_and_close(t, b, NULL, NULL) != 0) {
			SPDK_ERRLOG("tier: deferred drain of band %u failed (out of memory); "
				    "desc left open\n", b->band_id);
		}
	}
	return false;
}

/* Fire-and-forget superblock persistence completion (logs failures). */
static void
tier_sb_persist_cb(void *cb_arg, int rc)
{
	if (rc != 0) {
		SPDK_ERRLOG("tier: superblock persist failed rc=%d\n", rc);
	}
}

/* register() re-reads every band's superblock and restarts above the highest seq
 * found. The CSI replays create/assemble/register with t->seq = 0, so without this
 * the first persist writes seq 1, which a pre-restart SB at a high seq out-votes
 * FOREVER (highest seq wins) — the composite then reassembles to the STALE geometry,
 * silently undoing every retire and relocate persisted at the high seq. Best-effort:
 * if no read can be launched, persist at the current seq. */
struct tier_register_seed_ctx {
	struct vbdev_tier	*t;
	int			remaining;
	uint64_t		max_seq;
};

static void
tier_register_seed_finish(struct tier_register_seed_ctx *ctx)
{
	struct vbdev_tier *t = ctx->t;

	if (ctx->max_seq > t->seq) {
		t->seq = ctx->max_seq;	/* the persist below reserves seq max+1 (monotone) */
	}
	free(ctx);
	/* Launch the persist BEFORE releasing the rehydrate ref: tier_sb_write_all sets
	 * sb_write_inflight synchronously, so a delete deferred behind the rehydrate is
	 * then held by the fan-out instead of freeing `t` out from under us here. */
	if (tier_sb_write_all(t, tier_sb_persist_cb, NULL) != 0) {
		SPDK_ERRLOG("tier '%s': initial superblock persist could not be launched\n",
			    t->bdev.name);
	}
	tier_async_op_end(t);
}

static void
tier_register_seed_read_cb(void *cb_arg, const struct tier_superblock *sb, int rc)
{
	struct tier_register_seed_ctx *ctx = cb_arg;

	if (rc == 0 && sb != NULL && sb->seq > ctx->max_seq) {
		ctx->max_seq = sb->seq;
	}
	if (--ctx->remaining == 0) {
		tier_register_seed_finish(ctx);
	}
}

static void
tier_register_seed_seq_and_persist(struct vbdev_tier *t)
{
	struct tier_register_seed_ctx *ctx;
	struct tier_band *b;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		/* Fallback: persist at the current seq (may lose cross-restart monotonicity). */
		if (tier_sb_write_all(t, tier_sb_persist_cb, NULL) != 0) {
			SPDK_ERRLOG("tier '%s': initial superblock persist could not be launched\n",
				    t->bdev.name);
		}
		return;
	}
	ctx->t = t;
	ctx->remaining = 1;	/* hold a ref while launching the per-band reads */
	tier_async_op_begin(t);	/* defer any delete until the rehydrate + persist drains */
	TAILQ_FOREACH(b, &t->bands, link) {
		if (b->desc == NULL) {
			continue;	/* DEGRADED/absent leg — nothing to read */
		}
		ctx->remaining++;
		if (tier_sb_read_desc(b->desc, t->blocklen, tier_register_seed_read_cb, ctx) != 0) {
			ctx->remaining--;	/* this read did not launch */
		}
	}
	if (--ctx->remaining == 0) {
		tier_register_seed_finish(ctx);
	}
}

/* Register the composite bdev once its bands are configured (called by RPC). */
int
vbdev_tier_register(struct vbdev_tier *t)
{
	int rc;

	/* A re-register would fail spdk_bdev_register with -EEXIST, and the error path
	 * would then spdk_io_device_unregister() the io_device of the LIVE bdev.
	 * Refuse up front. */
	if (t->registered) {
		return -EEXIST;
	}
	if (t->num_bands == 0 || t->blocklen == 0) {
		return -EINVAL;
	}

	/* Every band/region boundary MUST be cluster-aligned, else a blobstore cluster can
	 * straddle a boundary and its I/O fails -EIO. Refuse to register otherwise: turn a
	 * latent corruption into an explicit provisioning error. */
	if (t->cluster_blocks > 1) {
		struct tier_band *vb;
		if (t->md_num_blocks % t->cluster_blocks != 0) {
			SPDK_ERRLOG("tier '%s': md_num_blocks %" PRIu64 " not aligned to cluster %" PRIu64 "\n",
				    t->bdev.name, t->md_num_blocks, t->cluster_blocks);
			return -EINVAL;
		}
		TAILQ_FOREACH(vb, &t->bands, link) {
			if (vb->state == TIER_BAND_RETIRED) {
				continue;
			}
			if (vb->lba_start % t->cluster_blocks != 0 || vb->num_blocks % t->cluster_blocks != 0) {
				SPDK_ERRLOG("tier '%s': band %u geometry (lba_start=%" PRIu64 " num_blocks=%"
					    PRIu64 ") not cluster-aligned (%" PRIu64 ")\n", t->bdev.name,
					    vb->band_id, vb->lba_start, vb->num_blocks, t->cluster_blocks);
				return -EINVAL;
			}
		}
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

	/* Rehydrate t->seq from the on-disk SBs, THEN persist the superblock to every
	 * band. This makes the generation monotone across a restart. Async and
	 * best-effort; failures are logged. */
	tier_register_seed_seq_and_persist(t);
	return 0;
}

/* --------------------------------------------------------------------------
 * relocate / quiesce co-design — only the registering module may quiesce.
 * -------------------------------------------------------------------------- */

/* Direct base-bdev copy between bands, bypassing the composite and its quiesce. When
 * verify is on, the destination is read back and CRC32c-compared with the source, so a
 * silent media/write corruption is detected at relocate time rather than later. */
struct tier_copy_ctx {
	struct vbdev_tier	*t;		/* for the async-op lifecycle ref */
	struct tier_band	*src_band;
	struct tier_band	*dst_band;
	struct spdk_io_channel	*src_ch;
	struct spdk_io_channel	*dst_ch;
	void			*buf;		/* source data (also the write buffer) */
	void			*vbuf;		/* read-back verify buffer */
	uint64_t		dst_phys;
	uint64_t		num_blocks;
	uint32_t		blocklen;
	uint32_t		src_crc;	/* CRC32c of buf, computed after the source read */
	bool			verify;		/* run the read-back + CRC check (per disk class) */
	tier_relocate_cb	cb_fn;
	void			*cb_arg;
};

static void
tier_copy_finish(struct tier_copy_ctx *c, int rc)
{
	struct vbdev_tier *t = c->t;

	if (c->src_ch) {
		spdk_put_io_channel(c->src_ch);
	}
	if (c->dst_ch) {
		spdk_put_io_channel(c->dst_ch);
	}
	/* Release the desc pins AFTER the channels — a hot-remove close deferred
	 * behind this engine may run at the unpin. */
	tier_band_desc_unpin(c->src_band);
	tier_band_desc_unpin(c->dst_band);
	if (c->buf) {
		spdk_dma_free(c->buf);
	}
	if (c->vbuf) {
		spdk_dma_free(c->vbuf);
	}
	c->cb_fn(c->cb_arg, rc);
	free(c);
	/* Release the composite async ref LAST — this may run a bdev_tier_delete that
	 * was deferred behind this relocate, so `t` must not be touched after. */
	tier_async_op_end(t);
}

/* A band's desc is NULLed by tier_band_drain_and_close on hot-remove. The desc pins
 * keep it open under the engine, so this re-check is the early abort on a
 * hot-removed or degraded destination, not a NULL-deref guard. */
static inline bool
tier_copy_dst_alive(const struct tier_copy_ctx *c)
{
	return c->dst_band->desc != NULL && c->dst_band->state == TIER_BAND_ACTIVE;
}

/* Write-then-flush durability step. A base WITHOUT FLUSH support has no volatile
 * write cache to drain — the completed write IS durable, so short-circuit to the
 * callback instead of submitting a flush that spdk_bdev_flush_blocks fails
 * synchronously. The callback must accept bdev_io == NULL on that short-circuit. */
static void
tier_flush_or_durable(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		      uint64_t offset_blocks, uint64_t num_blocks,
		      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	int rc;

	if (!spdk_bdev_io_type_supported(spdk_bdev_desc_get_bdev(desc),
					 SPDK_BDEV_IO_TYPE_FLUSH)) {
		cb(NULL, true, cb_arg);
		return;
	}
	rc = spdk_bdev_flush_blocks(desc, ch, offset_blocks, num_blocks, cb, cb_arg);
	if (rc != 0) {
		cb(NULL, false, cb_arg);
	}
}

/* Destination read-back complete — CRC32c-compare with the source. */
static void
tier_copy_verify_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct tier_copy_ctx *c = cb_arg;
	uint32_t dst_crc;

	spdk_bdev_free_io(bdev_io);
	if (!success) {
		tier_copy_finish(c, -EIO);
		return;
	}
	dst_crc = spdk_crc32c_update(c->vbuf, c->num_blocks * (uint64_t)c->blocklen, ~0u);
	if (dst_crc != c->src_crc) {
		SPDK_ERRLOG("tier: relocate CRC mismatch (src=%08x dst=%08x) — write corruption\n",
			    c->src_crc, dst_crc);
		tier_copy_finish(c, -EIO);
		return;
	}
	tier_copy_finish(c, 0);
}

/* Destination flushed — durability is now satisfied on EVERY path. If verify is off
 * the relocate is done; otherwise read the destination back from MEDIA (the flush
 * guaranteed it landed, not just the write cache) and CRC-compare with the source. */
static void
tier_copy_flush_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct tier_copy_ctx *c = cb_arg;
	int rc;

	if (bdev_io != NULL) {	/* NULL on the no-FLUSH-support short-circuit */
		spdk_bdev_free_io(bdev_io);
	}
	if (!success) {
		tier_copy_finish(c, -EIO);
		return;
	}
	/* The read-back and verify are optional per disk class — on media the
	 * control-plane trusts, skip the extra read; the durability flush already ran.
	 * The blob freeze already makes the move correct, so verify only detects a
	 * SILENT media/write corruption at relocate time. */
	if (!c->verify) {
		tier_copy_finish(c, 0);
		return;
	}
	c->vbuf = spdk_dma_malloc(c->num_blocks * (uint64_t)c->blocklen, c->blocklen, NULL);
	if (c->vbuf == NULL) {
		tier_copy_finish(c, -ENOMEM);
		return;
	}
	if (!tier_copy_dst_alive(c)) {
		tier_copy_finish(c, -EIO);	/* destination hot-removed mid-copy */
		return;
	}
	rc = spdk_bdev_read_blocks(c->dst_band->desc, c->dst_ch, c->vbuf, c->dst_phys,
				   c->num_blocks, tier_copy_verify_done, c);
	if (rc != 0) {
		tier_copy_finish(c, rc);
	}
}

static void
tier_copy_write_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct tier_copy_ctx *c = cb_arg;

	spdk_bdev_free_io(bdev_io);
	if (!success) {
		tier_copy_finish(c, -EIO);
		return;
	}
	if (!tier_copy_dst_alive(c)) {
		tier_copy_finish(c, -EIO);	/* destination hot-removed mid-copy */
		return;
	}
	/* FLUSH the destination on EVERY path, not only under `verify`: the caller swaps
	 * the L2P and frees the SOURCE cluster the moment this relocate ACKs, so data
	 * still sitting in the destination's volatile write cache is LOST on a power cut.
	 * Flush-before-commit is unconditional; the read-back and CRC stay opt-in. */
	tier_flush_or_durable(c->dst_band->desc, c->dst_ch, c->dst_phys, c->num_blocks,
			      tier_copy_flush_done, c);
}

static void
tier_copy_read_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct tier_copy_ctx *c = cb_arg;
	int rc;

	spdk_bdev_free_io(bdev_io);
	if (!success) {
		tier_copy_finish(c, -EIO);
		return;
	}
	/* Snapshot the source CRC now: the caller holds an lvol-bdev quiesce that
	 * DRAINED in-flight host writes before this copy started, so the source is
	 * stable. A blob freeze alone would not drain them. */
	if (c->verify) {
		c->src_crc = spdk_crc32c_update(c->buf, c->num_blocks * (uint64_t)c->blocklen, ~0u);
	}
	if (!tier_copy_dst_alive(c)) {
		tier_copy_finish(c, -EIO);	/* destination hot-removed mid-copy */
		return;
	}
	rc = spdk_bdev_write_blocks(c->dst_band->desc, c->dst_ch, c->buf, c->dst_phys,
				    c->num_blocks, tier_copy_write_done, c);
	if (rc != 0) {
		tier_copy_finish(c, rc);
	}
}

/* The caller must run this ENTIRE copy under an LVOL-BDEV quiesce (spdk_bdev_quiesce:
 * drains outstanding host I/O, then holds new I/O ABOVE the blob-to-LBA translation)
 * plus an inner blob-level freeze — NOT a composite-level quiesce, which holds host
 * writes BELOW the translation so a held write replays to the OLD lba after the L2P
 * swap and lands on a freed cluster. Both halves matter: a freeze alone gates only NEW
 * submissions, while a write already in flight could land after this copy read it. The
 * window stalls the blob for drain+read+flush+readback+commit. This copy path reads the
 * base bdevs DIRECTLY, so it is not itself held by the quiesce or the freeze. */
int
vbdev_tier_relocate_copy(struct vbdev_tier *t, uint64_t src_lba, uint64_t dst_lba,
			 uint64_t num_blocks, bool verify, tier_relocate_cb cb_fn, void *cb_arg)
{
	struct tier_copy_ctx *c;
	struct tier_band *sb, *db;
	uint64_t src_off, dst_off;
	int rc;

	sb = vbdev_tier_band_of_lba(t, src_lba, &src_off);
	db = vbdev_tier_band_of_lba(t, dst_lba, &dst_off);
	if (sb == NULL || db == NULL || sb->state != TIER_BAND_ACTIVE ||
	    db->state != TIER_BAND_ACTIVE || sb->desc == NULL || db->desc == NULL) {
		return -EIO;
	}
	/* The copy must stay inside both bands: a straddling range would read or write a
	 * NEIGHBOUR band's blocks through the wrong phys mapping. */
	if (src_off + num_blocks > sb->num_blocks || dst_off + num_blocks > db->num_blocks) {
		SPDK_ERRLOG("tier: relocate copy range straddles a band boundary\n");
		return -EINVAL;
	}

	c = calloc(1, sizeof(*c));
	if (c == NULL) {
		return -ENOMEM;
	}
	c->t = t;
	tier_async_op_begin(t);	/* defer any bdev_tier_delete until this copy drains */
	c->src_band = sb;
	c->dst_band = db;
	/* Pin both descs — a hot-remove drain landing mid-copy defers the close to
	 * tier_copy_finish's unpin instead of closing under our I/O. */
	tier_band_desc_pin(sb);
	tier_band_desc_pin(db);
	c->num_blocks = num_blocks;
	c->blocklen = t->blocklen;
	c->dst_phys = db->phys_offset + dst_off;
	c->verify = verify;
	c->cb_fn = cb_fn;
	c->cb_arg = cb_arg;
	c->buf = spdk_dma_malloc(num_blocks * (uint64_t)t->blocklen, t->blocklen, NULL);
	c->src_ch = spdk_bdev_get_io_channel(sb->desc);
	c->dst_ch = spdk_bdev_get_io_channel(db->desc);
	if (c->buf == NULL || c->src_ch == NULL || c->dst_ch == NULL) {
		tier_copy_finish(c, -ENOMEM);
		return 0;
	}

	rc = spdk_bdev_read_blocks(sb->desc, c->src_ch, c->buf, sb->phys_offset + src_off,
				   num_blocks, tier_copy_read_done, c);
	if (rc != 0) {
		tier_copy_finish(c, rc);
	}
	return 0;
}

/* --------------------------------------------------------------------------
 * md-mirror resync — rebuild a replacement md leg from the healthy one.
 *
 * The target band is typically a replacement disk assembled DEGRADED into an md
 * slot. The copy runs under a QUIESCE of the composite md range: unlike the
 * relocate path, the md region is IDENTITY-mapped (no L2P swap), so held writes
 * replay to the same LBA — and they replay AFTER the target is activated, reaching
 * both legs. The direct base-bdev copy below is not held by that quiesce.
 * Stall bound: one full md-region copy (size the md region accordingly).
 * -------------------------------------------------------------------------- */

struct tier_md_resync_ctx {
	struct vbdev_tier	*t;
	struct tier_band	*src;
	struct tier_band	*dst;
	struct spdk_io_channel	*src_ch;
	struct spdk_io_channel	*dst_ch;
	void			*buf;
	uint64_t		chunk_blocks;
	uint64_t		off;		/* md-region blocks copied so far */
	uint64_t		io_blocks;	/* size of the in-flight chunk */
	int			rc;
	bool			quiesced;
	void			(*cb)(void *cb_arg, int rc);
	void			*cb_arg;
};

static void tier_md_resync_next(struct tier_md_resync_ctx *c);

/* The healthy source leg must stay ACTIVE; the target is DEGRADED by design and is
 * activated at the end, so only its desc matters. The desc pins keep both open under
 * the engine, so this is the early abort when a leg is hot-removed mid-resync. */
static inline bool
tier_md_resync_legs_alive(const struct tier_md_resync_ctx *c)
{
	return c->src->desc != NULL && c->src->state == TIER_BAND_ACTIVE &&
	       c->dst->desc != NULL && c->dst->state != TIER_BAND_RETIRED;
}

static void
tier_md_resync_unquiesced(void *cb_arg, int status)
{
	struct tier_md_resync_ctx *c = cb_arg;
	struct vbdev_tier *t = c->t;

	(void)status;	/* best-effort; the resync outcome is c->rc */
	if (c->src_ch) {
		spdk_put_io_channel(c->src_ch);
	}
	if (c->dst_ch) {
		spdk_put_io_channel(c->dst_ch);
	}
	/* Release the desc pins AFTER the channels (a deferred close may run). */
	tier_band_desc_unpin(c->src);
	tier_band_desc_unpin(c->dst);
	if (c->buf) {
		spdk_dma_free(c->buf);
	}
	c->cb(c->cb_arg, c->rc);
	free(c);
	/* Release the composite async ref LAST — it may run a deferred bdev_tier_delete,
	 * so do not touch `t` afterward. */
	tier_async_op_end(t);
}

static void
tier_md_resync_finish(struct tier_md_resync_ctx *c, int rc)
{
	c->rc = rc;
	if (c->quiesced) {
		if (spdk_bdev_unquiesce_range(&c->t->bdev, &tier_if, 0, c->t->md_num_blocks,
					      tier_md_resync_unquiesced, c) == 0) {
			return;
		}
		SPDK_ERRLOG("tier '%s': md resync unquiesce dispatch failed\n", c->t->bdev.name);
	}
	tier_md_resync_unquiesced(c, 0);
}

static void
tier_md_resync_persisted(void *cb_arg, int rc)
{
	struct tier_md_resync_ctx *c = cb_arg;

	if (rc != 0) {
		/* Not durable: revert so the CSI retries (the copy itself is redoable). */
		c->dst->state = TIER_BAND_DEGRADED;
	}
	tier_md_resync_finish(c, rc);
}

/* Open the resynced leg's base channel on every EXISTING tier io_channel: they were
 * created before this band was assembled, so base_ch[id] is NULL there and
 * activating without this would fail every md write leg to it. A per-reactor open
 * failure is PROPAGATED — never activate a leg only some reactors can reach. */
static void
tier_md_resync_ch_open_iter(struct spdk_io_channel_iter *i)
{
	struct tier_md_resync_ctx *c = spdk_io_channel_iter_get_ctx(i);
	struct spdk_io_channel *_ch = spdk_io_channel_iter_get_channel(i);
	struct tier_io_channel *tch = spdk_io_channel_get_ctx(_ch);
	uint32_t id = c->dst->band_id;
	int rc = 0;

	if (tch->base_ch[id] == NULL && c->dst->desc != NULL) {
		tch->base_ch[id] = spdk_bdev_get_io_channel(c->dst->desc);
		if (tch->base_ch[id] == NULL) {
			rc = -ENOMEM;
		}
	}
	spdk_for_each_channel_continue(i, rc);
}

static void
tier_md_resync_ch_open_done(struct spdk_io_channel_iter *i, int status)
{
	struct tier_md_resync_ctx *c = spdk_io_channel_iter_get_ctx(i);

	if (status != 0) {
		tier_md_resync_finish(c, status);
		return;
	}
	/* Copy durable + channels reachable: activate the leg, persist, unquiesce. */
	c->dst->state = TIER_BAND_ACTIVE;
	if (tier_sb_write_all(c->t, tier_md_resync_persisted, c) != 0) {
		c->dst->state = TIER_BAND_DEGRADED;
		tier_md_resync_finish(c, -ENOMEM);
	}
}

static void
tier_md_resync_flush_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct tier_md_resync_ctx *c = cb_arg;

	if (bdev_io != NULL) {	/* NULL on the no-FLUSH-support short-circuit */
		spdk_bdev_free_io(bdev_io);
	}
	if (!success) {
		tier_md_resync_finish(c, -EIO);
		return;
	}
	spdk_for_each_channel(c->t, tier_md_resync_ch_open_iter, c,
			      tier_md_resync_ch_open_done);
}

static void
tier_md_resync_write_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct tier_md_resync_ctx *c = cb_arg;

	spdk_bdev_free_io(bdev_io);
	if (!success) {
		tier_md_resync_finish(c, -EIO);
		return;
	}
	c->off += c->io_blocks;
	tier_md_resync_next(c);
}

static void
tier_md_resync_read_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct tier_md_resync_ctx *c = cb_arg;
	int rc;

	spdk_bdev_free_io(bdev_io);
	if (!success) {
		tier_md_resync_finish(c, -EIO);
		return;
	}
	if (!tier_md_resync_legs_alive(c)) {
		tier_md_resync_finish(c, -ENODEV);	/* leg hot-removed mid-resync */
		return;
	}
	rc = spdk_bdev_write_blocks(c->dst->desc, c->dst_ch, c->buf,
				    c->t->sb_blocks + c->off, c->io_blocks,
				    tier_md_resync_write_done, c);
	if (rc != 0) {
		tier_md_resync_finish(c, rc);
	}
}

static void
tier_md_resync_next(struct tier_md_resync_ctx *c)
{
	int rc;

	if (!tier_md_resync_legs_alive(c)) {
		tier_md_resync_finish(c, -ENODEV);	/* leg hot-removed mid-resync */
		return;
	}
	if (c->off >= c->t->md_num_blocks) {
		/* Bases without FLUSH support short-circuit to the callback. */
		tier_flush_or_durable(c->dst->desc, c->dst_ch, c->t->sb_blocks,
				      c->t->md_num_blocks, tier_md_resync_flush_done, c);
		return;
	}
	c->io_blocks = spdk_min(c->chunk_blocks, c->t->md_num_blocks - c->off);
	rc = spdk_bdev_read_blocks(c->src->desc, c->src_ch, c->buf,
				   c->t->sb_blocks + c->off, c->io_blocks,
				   tier_md_resync_read_done, c);
	if (rc != 0) {
		tier_md_resync_finish(c, rc);
	}
}

static void
tier_md_resync_quiesced(void *cb_arg, int status)
{
	struct tier_md_resync_ctx *c = cb_arg;

	if (status != 0) {
		tier_md_resync_finish(c, status);
		return;
	}
	c->quiesced = true;
	tier_md_resync_next(c);
}

int
vbdev_tier_resync_md(struct vbdev_tier *t, uint32_t target_band_id,
		     void (*cb)(void *cb_arg, int rc), void *cb_arg)
{
	struct tier_md_resync_ctx *c;
	struct tier_band *dst = vbdev_tier_band_by_id(t, target_band_id);
	struct tier_band *src;
	uint64_t chunk_blocks;
	int rc;

	if (!t->registered || t->md_num_blocks == 0) {
		return -EINVAL;
	}
	if (dst == NULL || dst->desc == NULL) {
		return -ENODEV;
	}
	if (target_band_id != t->md_mirror_a && target_band_id != t->md_mirror_b) {
		return -EINVAL;	/* only md legs carry the mirrored region */
	}
	if (dst->state != TIER_BAND_DEGRADED) {
		return -EINVAL;	/* resync only rebuilds a degraded/replacement leg */
	}
	src = tier_md_other_leg(t, dst);
	if (src == NULL || src->state != TIER_BAND_ACTIVE || src->desc == NULL) {
		return -EIO;	/* no healthy leg to copy from */
	}

	c = calloc(1, sizeof(*c));
	if (c == NULL) {
		return -ENOMEM;
	}
	chunk_blocks = spdk_max(1, (1024u * 1024u) / t->blocklen);	/* 1 MiB chunks */
	c->t = t;
	tier_async_op_begin(t);	/* defer any bdev_tier_delete until this resync drains */
	c->src = src;
	c->dst = dst;
	/* Pin both descs — a hot-remove drain landing mid-resync defers the close to
	 * the engine's terminal unpin instead of closing under our I/O. */
	tier_band_desc_pin(src);
	tier_band_desc_pin(dst);
	c->chunk_blocks = chunk_blocks;
	c->cb = cb;
	c->cb_arg = cb_arg;
	c->buf = spdk_dma_malloc(chunk_blocks * (uint64_t)t->blocklen, t->blocklen, NULL);
	c->src_ch = spdk_bdev_get_io_channel(src->desc);
	c->dst_ch = spdk_bdev_get_io_channel(dst->desc);
	if (c->buf == NULL || c->src_ch == NULL || c->dst_ch == NULL) {
		tier_md_resync_finish(c, -ENOMEM);
		return 0;
	}
	rc = spdk_bdev_quiesce_range(&t->bdev, &tier_if, 0, t->md_num_blocks,
				     tier_md_resync_quiesced, c);
	if (rc != 0) {
		tier_md_resync_finish(c, rc);
	}
	return 0;
}

/* --------------------------------------------------------------------------
 * module init / finish
 * -------------------------------------------------------------------------- */

/* Per-process boot id, minted once at module init. The CSI compares it across polls
 * to detect a target restart, which invalidates ALL volatile state (standing pauses,
 * in-flight relocations, cbt epochs). Exposed by evariops_get_capabilities. */
char g_tier_boot_id[SPDK_UUID_STRING_LEN];

static int
vbdev_tier_init(void)
{
	struct spdk_uuid u;

	spdk_uuid_generate(&u);
	spdk_uuid_fmt_lower(g_tier_boot_id, sizeof(g_tier_boot_id), &u);
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
