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
tier_sb_serialize(struct vbdev_tier *t, struct tier_band *self, struct tier_superblock *sb)
{
	struct tier_band *b;
	uint32_t i = 0;

	memset(sb, 0, sizeof(*sb));
	sb->magic = TIER_SB_MAGIC;
	sb->version = TIER_SB_VERSION;
	sb->seq = t->seq;
	snprintf(sb->composite_name, sizeof(sb->composite_name), "%s", t->bdev.name);
	sb->md_num_blocks = t->md_num_blocks;
	sb->md_mirror_a = t->md_mirror_a;
	sb->md_mirror_b = t->md_mirror_b;
	sb->num_bands = t->num_bands;
	sb->this_band_id = self ? self->band_id : UINT32_MAX;
	sb->blocklen = t->blocklen;

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

	if (sb->magic != TIER_SB_MAGIC || sb->version != TIER_SB_VERSION) {
		return false;
	}
	tmp = *sb;
	tmp.crc = 0;
	crc = spdk_crc32c_update(&tmp, sizeof(tmp), ~0u);
	return crc == sb->crc;
}

/* ---- async write to all bands ---------------------------------------------- */

struct tier_sb_write_ctx {
	struct vbdev_tier	*t;
	int			remaining;
	int			status;
	void			(*cb)(void *cb_arg, int rc);
	void			*cb_arg;
};

struct tier_sb_band_write {
	struct tier_sb_write_ctx *parent;
	struct spdk_io_channel	*ch;
	void			*buf;
};

static void
tier_sb_write_band_done(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct tier_sb_band_write *bw = cb_arg;
	struct tier_sb_write_ctx *ctx = bw->parent;

	spdk_bdev_free_io(bdev_io);
	spdk_put_io_channel(bw->ch);
	spdk_dma_free(bw->buf);
	free(bw);

	if (!success) {
		ctx->status = -EIO;
	}
	if (--ctx->remaining == 0) {
		if (ctx->cb) {
			ctx->cb(ctx->cb_arg, ctx->status);
		}
		free(ctx);
	}
}

int
tier_sb_write_all(struct vbdev_tier *t, void (*cb)(void *cb_arg, int rc), void *cb_arg)
{
	struct tier_sb_write_ctx *ctx;
	struct tier_band *b;
	size_t bufsz = (size_t)t->sb_blocks * t->blocklen;
	int launched = 0;

	if (t->sb_blocks == 0 || bufsz < sizeof(struct tier_superblock)) {
		return -EINVAL;
	}

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL) {
		return -ENOMEM;
	}
	ctx->t = t;
	ctx->status = 0;
	ctx->cb = cb;
	ctx->cb_arg = cb_arg;
	ctx->remaining = 1;	/* hold a ref while we launch, released at the end */

	t->seq++;

	TAILQ_FOREACH(b, &t->bands, link) {
		struct tier_sb_band_write *bw;
		int rc;

		if (b->state == TIER_BAND_RETIRED || b->desc == NULL) {
			continue;
		}
		bw = calloc(1, sizeof(*bw));
		if (bw == NULL) {
			ctx->status = -ENOMEM;
			continue;
		}
		bw->parent = ctx;
		bw->buf = spdk_dma_zmalloc(bufsz, t->blocklen, NULL);
		if (bw->buf == NULL) {
			ctx->status = -ENOMEM;
			free(bw);
			continue;
		}
		tier_sb_serialize(t, b, (struct tier_superblock *)bw->buf);
		bw->ch = spdk_bdev_get_io_channel(b->desc);
		if (bw->ch == NULL) {
			ctx->status = -ENOMEM;
			spdk_dma_free(bw->buf);
			free(bw);
			continue;
		}
		ctx->remaining++;
		rc = spdk_bdev_write_blocks(b->desc, bw->ch, bw->buf, 0, t->sb_blocks,
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

	/* Release the holding ref; if no band write was launched, complete now. */
	if (--ctx->remaining == 0) {
		int status = ctx->status;
		if (ctx->cb) {
			ctx->cb(ctx->cb_arg, status);
		}
		free(ctx);
	}
	(void)launched;
	return 0;
}

/* ---- async read from one base bdev desc ------------------------------------ */

struct tier_sb_read_ctx {
	struct spdk_bdev_desc	*desc;
	struct spdk_io_channel	*ch;
	void			*buf;
	uint32_t		sb_blocks;
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
		sb = (const struct tier_superblock *)rc_ctx->buf;
		if (!tier_sb_valid(sb)) {
			sb = NULL;
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
