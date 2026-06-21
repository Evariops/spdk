/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Evariops. All rights reserved.
 *
 *   bdev_tier — composite tier-mapped vbdev (SPEC-73A/D, M1).
 *
 *   One vbdev_tier per node aggregates ALL local disks into a single linear
 *   address space, split into BANDS (one band == one physical base bdev),
 *   ordered fast -> slow [Ultra | Premium | Standard | Economy].
 *
 *   Layout (SPEC-73A §4, §5B, D1):
 *     LBA 0 .......................................... LBA (size-1)
 *     [ metadata region (md)        | data region (concat of bands)      ]
 *     [ MIRRORED RAID1 on 2 bands   | band0 | band1 | band2 | ...        ]
 *
 *   - The blobstore consumes the whole thing via spdk_bdev_create_bs_dev().
 *     Its super-block + masks + extent-pages (the L2P) live at LOW LBA, so they
 *     fall in the MIRRORED md region => a single disk loss is non-fatal (D1).
 *   - The data region is a pure CONCAT (sum of band sizes), routed by address
 *     arithmetic (no L2P table => no double indirection). lowest-first
 *     allocation therefore yields write-to-fast for free (SPEC-73A §3).
 *   - Per-band failure isolation (C-FAIL-1): a degraded band returns an I/O
 *     ERROR on its own LBA range only; the vbdev NEVER reports is_degraded
 *     globally (that would force a whole-chunk rebuild — B3 / §10A).
 */

#ifndef SPDK_VBDEV_TIER_H
#define SPDK_VBDEV_TIER_H

#include "spdk/stdinc.h"
#include "spdk/bdev.h"
#include "spdk/bdev_module.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Performance tier — totally ordered, fast -> slow.
 * Mirrors SpdkOperator.CustomResources.PerformanceTier (C#). The CSI brain
 * resolves a concrete band; SPDK only stores/reports the class. */
enum tier_class {
	TIER_ULTRA	= 0,
	TIER_PREMIUM	= 1,
	TIER_STANDARD	= 2,
	TIER_ECONOMY	= 3,
	TIER_CLASS_MAX
};

/* Lifecycle state of a band (SPEC-73A §5B.4). */
enum tier_band_state {
	TIER_BAND_ACTIVE	= 0,
	TIER_BAND_DEGRADED	= 1,	/* disk failed: I/O on its range returns -EIO (C-FAIL-1) */
	TIER_BAND_RETIRED	= 2,	/* evacuated + removed; slot kept, range unreclaimable */
};

#define TIER_WWN_LEN		64
#define TIER_SERIAL_LEN		64
#define TIER_BDEV_NAME_LEN	64
#define TIER_MAX_BANDS		64	/* per node; a node won't exceed this many disks */

/* ---- On-disk superblock (INV-T1: at native SPDK level, à la bdev_raid_sb) ----
 * One copy is written into a RESERVED region at the start of EACH base bdev. Each
 * copy self-describes the WHOLE composite, so any present band can drive
 * self-assembly via the examine path (no CSI needed to assemble). The reserved
 * region falls inside the mirrored md range, so it is itself RAID1-protected.
 * Disk identity (wwn) is validated at assembly to detect swap/replacement. */
#define TIER_SB_MAGIC		0x5449455253423031ULL	/* "TIERSB01" */
#define TIER_SB_VERSION		1u
#define TIER_SB_RESERVE_BYTES	(256 * 1024)		/* reserved per base bdev for the sb */

/* On-disk band descriptor (packed, stable layout). */
struct tier_sb_band {
	uint32_t	band_id;
	uint32_t	tier;		/* enum tier_class */
	uint32_t	state;		/* enum tier_band_state */
	uint32_t	reserved0;
	uint64_t	lba_start;	/* position in the composite address space */
	uint64_t	num_blocks;
	char		wwn[TIER_WWN_LEN];
	char		serial[TIER_SERIAL_LEN];
};

/* On-disk superblock (identical content on every band). */
struct tier_superblock {
	uint64_t	magic;
	uint32_t	version;
	uint32_t	crc;		/* CRC32c over the whole struct with crc field = 0 */
	uint64_t	seq;		/* monotone; on conflict, highest seq wins */
	char		composite_name[TIER_BDEV_NAME_LEN];
	uint64_t	md_num_blocks;	/* size of the mirrored md region (composite blocks) */
	uint32_t	md_mirror_a;	/* band slot ids holding the md RAID1 pair */
	uint32_t	md_mirror_b;
	uint32_t	num_bands;
	uint32_t	this_band_id;	/* which band slot this copy physically sits on */
	uint32_t	blocklen;	/* common block size */
	uint32_t	reserved1;
	struct tier_sb_band bands[TIER_MAX_BANDS];
};

/*
 * One band == one physical base bdev. bandId is a STABLE monotone slot,
 * never reused (a retired disk keeps its slot) — same model as a raid
 * base_bdev slot (SPEC-73A §5B.4).
 */
struct tier_band {
	uint32_t		band_id;
	enum tier_class		tier;
	enum tier_band_state	state;

	char			base_bdev_name[TIER_BDEV_NAME_LEN];
	char			wwn[TIER_WWN_LEN];	/* disk identity — detect a swapped disk in a slot */
	char			serial[TIER_SERIAL_LEN];

	/* Position in the composite linear address space (in blocks). */
	uint64_t		lba_start;	/* composite start of this band's contribution */
	uint64_t		num_blocks;	/* usable blocks contributed by this band */
	uint64_t		phys_offset;	/* base-bdev physical block where lba_start maps
						 * (>= sb_blocks; mirror band A adds md_num_blocks) */

	/* Open handle to the underlying disk (NULL while retired). */
	struct spdk_bdev_desc	*desc;

	TAILQ_ENTRY(tier_band)	link;
};

/*
 * The composite vbdev. One per node.
 */
struct vbdev_tier {
	struct spdk_bdev	bdev;		/* the bdev we register */

	TAILQ_HEAD(, tier_band)	bands;		/* ordered fast -> slow, by lba_start */
	uint32_t		num_bands;
	uint32_t		next_band_id;	/* monotone slot allocator */

	/* Mirrored metadata region [0, md_num_blocks) — RAID1 across two bands (D1).
	 * md_mirror_a / md_mirror_b are band_ids; both hold an identical copy of the
	 * low LBA range so blobstore metadata survives a single disk loss. */
	uint64_t		md_num_blocks;	/* size of the mirrored md region, in blocks */
	uint32_t		md_mirror_a;
	uint32_t		md_mirror_b;

	uint32_t		blocklen;	/* common block size of all bands (must match) */
	uint32_t		sb_blocks;	/* reserved superblock blocks at the start of EACH base bdev */
	uint64_t		seq;		/* current superblock generation (monotone) */
	uint64_t		total_num_blocks;	/* md region + Σ data bands (excludes per-disk sb reserve) */
	bool			registered;

	TAILQ_ENTRY(vbdev_tier)	link;
};

/*
 * Per-IO-channel context: one base channel per band (+ the md mirror channels).
 */
struct tier_io_channel {
	struct spdk_io_channel	*base_ch[TIER_MAX_BANDS];	/* indexed by band slot */
};

/* ---- Internal API (consumed by vbdev_tier.c, vbdev_tier_rpc.c, and the
 *      relocate/quiesce co-design in M2b) ------------------------------------ */

/* Resolve a composite LBA to the owning band + offset within that band.
 * Returns NULL if the LBA falls outside any active band's range. The md region
 * [0, md_num_blocks) is special-cased by the caller (mirrored). */
struct tier_band *vbdev_tier_band_of_lba(struct vbdev_tier *t, uint64_t lba,
					 uint64_t *band_offset);

/* Resolve by stable slot id. */
struct tier_band *vbdev_tier_band_by_id(struct vbdev_tier *t, uint32_t band_id);

/* True when [offset, offset+num_blocks) lies entirely in the mirrored md region. */
static inline bool
vbdev_tier_is_md_range(const struct vbdev_tier *t, uint64_t offset, uint64_t num_blocks)
{
	return offset < t->md_num_blocks &&
	       (offset + num_blocks) <= t->md_num_blocks;
}

/* Lifecycle (RPC-driven, SPEC-73A §9.1 / C-MUT-2). */
struct vbdev_tier *vbdev_tier_create(const char *name, uint64_t md_num_blocks);
struct vbdev_tier *vbdev_tier_get_by_name(const char *name);
int vbdev_tier_add_band(struct vbdev_tier *t, const char *base_bdev_name,
			enum tier_class tier, const char *wwn, const char *serial,
			uint32_t *out_band_id);
int vbdev_tier_retire_band(struct vbdev_tier *t, uint32_t band_id);
/* Register the composite bdev once its bands are configured. */
int vbdev_tier_register(struct vbdev_tier *t);
/* Tear down + unregister (cleanup). */
int vbdev_tier_delete(struct vbdev_tier *t);

/* Superblock (vbdev_tier_sb.c) — native-level persistence (INV-T1). */
void tier_sb_serialize(struct vbdev_tier *t, struct tier_band *self, struct tier_superblock *sb);
bool tier_sb_valid(const struct tier_superblock *sb);	/* magic + crc check */
/* Async-write the (serialized) superblock to EVERY active band. cb fires once,
 * with rc != 0 if any band failed. Increments t->seq. cb may be NULL (fire-and-forget). */
int tier_sb_write_all(struct vbdev_tier *t, void (*cb)(void *cb_arg, int rc), void *cb_arg);
/* Async-read the superblock from a base bdev desc into a freshly-allocated buffer;
 * cb receives the parsed sb (NULL + rc on failure) and owns freeing nothing (sb is on stack-copy). */
int tier_sb_read_desc(struct spdk_bdev_desc *desc, uint32_t blocklen,
		      void (*cb)(void *cb_arg, const struct tier_superblock *sb, int rc), void *cb_arg);

/* M2b co-design: quiesce a physical LBA range of THIS composite (only the
 * registering module may call spdk_bdev_quiesce_range — SPEC-73A §5.3). */
int vbdev_tier_relocate_quiesce(struct vbdev_tier *t, uint64_t lba, uint64_t num_blocks,
				spdk_bdev_quiesce_cb cb_fn, void *cb_arg);
int vbdev_tier_relocate_unquiesce(struct vbdev_tier *t, uint64_t lba, uint64_t num_blocks,
				  spdk_bdev_quiesce_cb cb_fn, void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_VBDEV_TIER_H */
