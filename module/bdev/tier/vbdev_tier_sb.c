/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Evariops. All rights reserved.
 *
 *   bdev_tier on-disk superblock (SPEC-73A INV-T1, à la bdev_raid_sb).
 *   One copy per band, in the reserved region [0, sb_blocks) of each base bdev
 *   (inside the RAID1-mirrored md range). Self-describes the whole composite so
 *   any present band can drive self-assembly + wwn validation.
 */

#include "vbdev_tier.h"

#include "spdk/env.h"
#include "spdk/crc32.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/util.h"

/* ---- serialize / validate -------------------------------------------------- */

void
tier_sb_serialize(struct vbdev_tier *t, struct tier_band *self, uint64_t seq,
		  uint64_t created_epoch_sec, struct tier_superblock *sb)
{
	struct tier_band *b;
	uint32_t i = 0;

	memset(sb, 0, sizeof(*sb));
	sb->magic = TIER_SB_MAGIC;
	sb->version = TIER_SB_VERSION;
	sb->seq = seq;		/* M5(a): generation RESERVED at write_all entry (t->seq already bumped) */
	sb->created_epoch_sec = created_epoch_sec;
	memcpy(sb->generation_uuid, t->gen_uuid, sizeof(sb->generation_uuid));
	snprintf(sb->composite_name, sizeof(sb->composite_name), "%s", t->bdev.name);
	sb->md_num_blocks = t->md_num_blocks;
	sb->md_mirror_a = t->md_mirror_a;
	sb->md_mirror_b = t->md_mirror_b;
	sb->num_bands = t->num_bands;
	sb->this_band_id = self ? self->band_id : UINT32_MAX;
	sb->blocklen = t->blocklen;
	sb->cluster_blocks = t->cluster_blocks;	/* F1 grain; u64 on disk since v2 (F-3) */

	TAILQ_FOREACH(b, &t->bands, link) {
		if (i >= TIER_MAX_BANDS) {
			break;
		}
		sb->bands[i].band_id = b->band_id;
		sb->bands[i].tier = b->tier;
		sb->bands[i].state = b->state;
		sb->bands[i].lba_start = b->lba_start;
		sb->bands[i].num_blocks = b->num_blocks;
		snprintf(sb->bands[i].wwn, sizeof(sb->bands[i].wwn), "%s", b->wwn);
		snprintf(sb->bands[i].serial, sizeof(sb->bands[i].serial), "%s", b->serial);
		i++;
	}

	sb->crc = 0;
	sb->crc = spdk_crc32c_update(sb, sizeof(*sb), ~0u);
}

bool
tier_sb_valid(const struct tier_superblock *sb)
{
	struct tier_superblock tmp;
	uint32_t crc;

	/* F-4: the format is declared little-endian only (amd64/arm64 targets). A
	 * byte-swapped magic means the SB was written by a big-endian host — refuse
	 * loudly instead of failing the CRC silently. */
	if (sb->magic == __builtin_bswap64(TIER_SB_MAGIC)) {
		SPDK_ERRLOG("tier sb: byte-swapped magic (big-endian writer?) — refusing\n");
		return false;
	}
	if (sb->magic != TIER_SB_MAGIC || sb->version != TIER_SB_VERSION) {
		return false;
	}
	tmp = *sb;
	tmp.crc = 0;
	crc = spdk_crc32c_update(&tmp, sizeof(tmp), ~0u);
	return crc == sb->crc;
}

/* F-5: pick the best (valid, highest-seq) of the two slots in a reserve buffer. */
const struct tier_superblock *
tier_sb_select(const void *reserve_buf, size_t reserve_len)
{
	const struct tier_superblock *a, *b;
	bool a_ok, b_ok;

	if (reserve_buf == NULL || reserve_len < TIER_SB_RESERVE_BYTES) {
		return NULL;
	}
	a = (const struct tier_superblock *)reserve_buf;
	b = (const struct tier_superblock *)((const uint8_t *)reserve_buf + TIER_SB_SLOT_BYTES);
	a_ok = tier_sb_valid(a);
	b_ok = tier_sb_valid(b);
	if (a_ok && b_ok) {
		return (b->seq > a->seq) ? b : a;
	}
	if (a_ok) {
		return a;
	}
	if (b_ok) {
		return b;
	}
	return NULL;
}

/* ---- async write to all bands ----------------------------------------------
 *
 * M5(a): serialized. One fan-out in flight per composite; requests arriving
 * meanwhile queue their callback on t->sb_pending_cbs and are coalesced into
 * ONE follow-up fan-out of the latest in-memory state. The generation is
 * RESERVED (t->seq++) at fan-out start: a partially-failed fan-out can never
 * share a seq with a later, different table ("highest seq wins" stays sound —
 * gaps are harmless, duplicates are fatal).
 * M5(b): only ACTIVE bands are written. The old `!= RETIRED` filter included
 * DEGRADED bands whose write always fails → status stuck at -EIO → a
 * retirement was never observed as persisted once any band degraded.
 * F-6: each copy is FLUSHed after the write — a "committed" seq sitting in a
 * volatile write cache is not a commit. */

struct tier_sb_write_ctx {
	struct vbdev_tier	*t;
	int			remaining;
	int			status;
	TAILQ_HEAD(, tier_sb_pending_cb) cbs;	/* callbacks served by THIS fan-out */
};

struct tier_sb_band_write {
	struct tier_sb_write_ctx *parent;
	struct spdk_bdev_desc	*desc;
	struct spdk_io_channel	*ch;
	void			*buf;
	uint64_t		slot_off_blocks;	/* F-5: this generation's slot */
	uint32_t		slot_blocks;
};

static void tier_sb_write_start(struct vbdev_tier *t);

static void
tier_sb_fanout_complete(struct tier_sb_write_ctx *ctx)
{
	struct vbdev_tier *t = ctx->t;
	struct tier_sb_pending_cb *p;
	int status = ctx->status;

	/* Run the callbacks queued behind this fan-out. A callback may request a
	 * teardown (bdev_tier_delete) — that is DEFERRED (delete_pending), never run
	 * synchronously here, so `t` is guaranteed to survive this loop. */
	while ((p = TAILQ_FIRST(&ctx->cbs)) != NULL) {
		TAILQ_REMOVE(&ctx->cbs, p, link);
		p->cb(p->cb_arg, status);
		free(p);
	}
	free(ctx);
	t->sb_write_inflight = false;

	/* T-4b: honor teardown deferred behind this fan-out (a pending delete, or a
	 * hot-removed band's drain+close). If a deferred delete consumed the
	 * composite, `t` is gone/unregistering — do not touch it. */
	if (vbdev_tier_sb_fanout_idle(t)) {
		return;
	}
	if (t->sb_write_queued) {
		t->sb_write_queued = false;
		tier_sb_write_start(t);
	}
}

static void
tier_sb_band_io_done(struct tier_sb_band_write *bw, bool success)
{
	struct tier_sb_write_ctx *ctx = bw->parent;

	spdk_put_io_channel(bw->ch);
	spdk_dma_free(bw->buf);
	free(bw);
	if (!success) {
		ctx->status = -EIO;
	}
	if (--ctx->remaining == 0) {
		tier_sb_fanout_complete(ctx);
	}
}

static void
tier_sb_flush_band_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	spdk_bdev_free_io(bdev_io);
	tier_sb_band_io_done(cb_arg, success);
}

static void
tier_sb_write_band_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct tier_sb_band_write *bw = cb_arg;
	int rc;

	spdk_bdev_free_io(bdev_io);
	if (!success) {
		tier_sb_band_io_done(bw, false);
		return;
	}
	/* F-6: flush before calling this copy durable. Same contract as the CBT
	 * rebuild path: a base bdev without FLUSH support (bdev_uring) has no
	 * volatile cache to drain — treat the completed write as durable instead
	 * of failing the persist. This EIO'd EVERY superblock persist on uring
	 * bases, so no tier SB was ever written on such nodes. */
	if (!spdk_bdev_io_type_supported(spdk_bdev_desc_get_bdev(bw->desc),
					 SPDK_BDEV_IO_TYPE_FLUSH)) {
		tier_sb_band_io_done(bw, true);
		return;
	}
	rc = spdk_bdev_flush_blocks(bw->desc, bw->ch, bw->slot_off_blocks, bw->slot_blocks,
				    tier_sb_flush_band_done, bw);
	if (rc != 0) {
		tier_sb_band_io_done(bw, false);
	}
}

static void
tier_sb_write_start(struct vbdev_tier *t)
{
	struct tier_sb_write_ctx *ctx;
	struct tier_band *b;
	size_t bufsz = TIER_SB_SLOT_BYTES;	/* F-5: one slot per generation */
	uint32_t slot_blocks = TIER_SB_SLOT_BYTES / t->blocklen;
	uint64_t target_seq, slot_off_blocks, now_sec;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		struct tier_sb_pending_cb *p;

		t->sb_write_inflight = false;
		while ((p = TAILQ_FIRST(&t->sb_pending_cbs)) != NULL) {
			TAILQ_REMOVE(&t->sb_pending_cbs, p, link);
			p->cb(p->cb_arg, -ENOMEM);
			free(p);
		}
		/* M2: this IS a fan-out termination. A bdev_tier_delete deferred
		 * behind the queued follow-up (delete_pending), or a hot-removed
		 * band's close_pending, was waiting on vbdev_tier_sb_fanout_idle —
		 * returning without it stranded the composite (and its claimed base
		 * descs) forever when the drained callbacks held no async_inflight
		 * reference. Do not touch `t` if the teardown consumed it. */
		vbdev_tier_sb_fanout_idle(t);
		return;
	}
	t->sb_write_inflight = true;
	ctx->t = t;
	ctx->status = 0;
	TAILQ_INIT(&ctx->cbs);
	/* Take ownership of the queued callbacks (no TAILQ_CONCAT on glibc/SPDK). */
	{
		struct tier_sb_pending_cb *p;

		while ((p = TAILQ_FIRST(&t->sb_pending_cbs)) != NULL) {
			TAILQ_REMOVE(&t->sb_pending_cbs, p, link);
			TAILQ_INSERT_TAIL(&ctx->cbs, p, link);
		}
	}
	ctx->remaining = 1;	/* hold a ref while we launch, released at the end */

	/* M5(a): reserve the generation up front (see block comment above). */
	t->seq++;
	target_seq = t->seq;
	/* F-5: generation N lands in slot N%2 — a torn write only hurts one slot. */
	slot_off_blocks = (uint64_t)tier_sb_slot_for_seq(target_seq) * slot_blocks;
	now_sec = (uint64_t)time(NULL);

	int launched = 0;

	TAILQ_FOREACH(b, &t->bands, link) {
		struct tier_sb_band_write *bw;
		int rc;

		/* M5(b): ACTIVE bands only. */
		if (b->state != TIER_BAND_ACTIVE || b->desc == NULL) {
			continue;
		}
		bw = calloc(1, sizeof(*bw));
		if (bw == NULL) {
			ctx->status = -ENOMEM;
			continue;
		}
		bw->parent = ctx;
		bw->desc = b->desc;
		bw->slot_off_blocks = slot_off_blocks;
		bw->slot_blocks = slot_blocks;
		bw->buf = spdk_dma_zmalloc(bufsz, t->blocklen, NULL);
		if (bw->buf == NULL) {
			ctx->status = -ENOMEM;
			free(bw);
			continue;
		}
		tier_sb_serialize(t, b, target_seq, now_sec, (struct tier_superblock *)bw->buf);
		bw->ch = spdk_bdev_get_io_channel(b->desc);
		if (bw->ch == NULL) {
			ctx->status = -ENOMEM;
			spdk_dma_free(bw->buf);
			free(bw);
			continue;
		}
		ctx->remaining++;
		rc = spdk_bdev_write_blocks(b->desc, bw->ch, bw->buf, slot_off_blocks, slot_blocks,
					    tier_sb_write_band_done, bw);
		if (rc != 0) {
			ctx->remaining--;
			ctx->status = rc;
			spdk_put_io_channel(bw->ch);
			spdk_dma_free(bw->buf);
			free(bw);
			continue;
		}
		launched++;
	}

	/* M3: zero copies written must NOT complete rc == 0 — the callers'
	 * contract (rc != 0 ⇒ NOT durable, MJ6) treats 0 as "the state is on
	 * disk". With every band skipped (all DEGRADED/RETIRED/desc-less), a
	 * retire acked "durable" would silently resurrect after reboot from the
	 * old highest-seq superblocks. */
	if (launched == 0 && ctx->status == 0) {
		SPDK_ERRLOG("tier '%s': superblock persist wrote ZERO copies (no "
			    "ACTIVE band with an open desc) — reporting not durable\n",
			    t->bdev.name);
		ctx->status = -ENODEV;
	}

	/* Release the holding ref; if no band write was launched, complete now. */
	if (--ctx->remaining == 0) {
		tier_sb_fanout_complete(ctx);
	}
}

int
tier_sb_write_all(struct vbdev_tier *t, void (*cb)(void *cb_arg, int rc), void *cb_arg)
{
	if (t->sb_blocks == 0 || t->blocklen == 0 ||
	    TIER_SB_SLOT_BYTES % t->blocklen != 0 ||
	    sizeof(struct tier_superblock) > TIER_SB_SLOT_BYTES) {
		return -EINVAL;
	}
	if (cb != NULL) {
		struct tier_sb_pending_cb *p = calloc(1, sizeof(*p));

		if (p == NULL) {
			return -ENOMEM;
		}
		p->cb = cb;
		p->cb_arg = cb_arg;
		TAILQ_INSERT_TAIL(&t->sb_pending_cbs, p, link);
	}
	if (t->sb_write_inflight) {
		/* M5(a): coalesce — one follow-up fan-out will persist the latest state. */
		t->sb_write_queued = true;
		return 0;
	}
	tier_sb_write_start(t);
	return 0;
}

/* ---- async read from one base bdev desc ------------------------------------ */

struct tier_sb_read_ctx {
	struct spdk_bdev_desc	*desc;
	struct spdk_io_channel	*ch;
	void			*buf;
	uint32_t		sb_blocks;
	uint32_t		blocklen;
	void			(*cb)(void *cb_arg, const struct tier_superblock *sb, int rc);
	void			*cb_arg;
};

static void
tier_sb_read_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct tier_sb_read_ctx *rc_ctx = cb_arg;
	const struct tier_superblock *sb = NULL;
	int rc = 0;

	spdk_bdev_free_io(bdev_io);

	if (!success) {
		rc = -EIO;
	} else {
		/* F-5: both slots were read — take the valid one with the highest seq. */
		sb = tier_sb_select(rc_ctx->buf, (size_t)rc_ctx->sb_blocks * rc_ctx->blocklen);
		if (sb == NULL) {
			rc = -EILSEQ;	/* no / invalid superblock on this disk */
		}
	}

	rc_ctx->cb(rc_ctx->cb_arg, sb, rc);

	spdk_put_io_channel(rc_ctx->ch);
	spdk_dma_free(rc_ctx->buf);
	free(rc_ctx);
}

int
tier_sb_read_desc(struct spdk_bdev_desc *desc, uint32_t blocklen,
		  void (*cb)(void *cb_arg, const struct tier_superblock *sb, int rc), void *cb_arg)
{
	struct tier_sb_read_ctx *ctx;
	uint32_t sb_blocks = spdk_divide_round_up(TIER_SB_RESERVE_BYTES, blocklen);
	size_t bufsz = (size_t)sb_blocks * blocklen;
	int rc;

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}
	ctx->desc = desc;
	ctx->sb_blocks = sb_blocks;
	ctx->blocklen = blocklen;
	ctx->cb = cb;
	ctx->cb_arg = cb_arg;
	ctx->buf = spdk_dma_zmalloc(bufsz, blocklen, NULL);
	if (ctx->buf == NULL) {
		free(ctx);
		return -ENOMEM;
	}
	ctx->ch = spdk_bdev_get_io_channel(desc);
	if (ctx->ch == NULL) {
		spdk_dma_free(ctx->buf);
		free(ctx);
		return -ENOMEM;
	}
	rc = spdk_bdev_read_blocks(desc, ctx->ch, ctx->buf, 0, sb_blocks, tier_sb_read_done, ctx);
	if (rc != 0) {
		spdk_put_io_channel(ctx->ch);
		spdk_dma_free(ctx->buf);
		free(ctx);
	}
	return rc;
}
