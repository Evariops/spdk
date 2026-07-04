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
#include "spdk/assert.h"
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

/* ---- On-disk superblock v2 (INV-T1: at native SPDK level, à la bdev_raid_sb) ----
 * See docs/FORMAT-tier-superblock.md for the authoritative layout description.
 *
 * One copy is written into a RESERVED region at the start of EACH base bdev. Each
 * copy self-describes the WHOLE composite. There is NO examine path: assembly is
 * driven by the CSI agent (SPEC-73 A2), which reads every disk's SB via
 * bdev_tier_read_sb, picks the highest-seq copy, and replays
 * create + assemble_band at the stored geometry. Swap/replacement detection
 * (live wwn vs the slot's stored wwn) is done by the CSI during that replay;
 * in-module the only wwn guard is the duplicate-wwn rejection at add/assemble.
 * The reserved region is per-disk (NOT inside the mirrored md range).
 *
 * v2 (clean break — v1 disks are NOT readable; the project pre-dates any public
 * deployment, redeploys are wipe+reinstall by design, so no migration path):
 *  - F-5: the 256 KiB reserve holds TWO 128 KiB SLOTS (A at 0, B at 128 KiB).
 *    Generation seq N is written to slot N%2, so a torn write destroys at most
 *    one slot; readers validate both and take the highest-seq valid one.
 *  - F-3: cluster_blocks widened to u64.
 *  - F-2: generation_uuid (fencing: identifies the composite INSTANCE — a
 *    re-created composite mints a new uuid, so stale disks from a previous
 *    life cannot be cross-assembled), created_epoch_sec (informative wall
 *    clock), plus 96 B of header reserve and 32 B per band descriptor.
 * Format: little-endian only (F-4); layout locked by the static asserts below (F-1). */
#define TIER_SB_MAGIC		0x5449455253423032ULL	/* "TIERSB02" */
#define TIER_SB_VERSION		2u
#define TIER_SB_RESERVE_BYTES	(256 * 1024)		/* reserved per base bdev for the sb */
#define TIER_SB_SLOT_BYTES	(128 * 1024)		/* F-5: two A/B slots inside the reserve */
#define TIER_SB_GEN_UUID_LEN	16

/* On-disk band descriptor (192 B, stable layout). */
struct tier_sb_band {
	uint32_t	band_id;
	uint32_t	tier;		/* enum tier_class */
	uint32_t	state;		/* enum tier_band_state */
	uint32_t	reserved0;
	uint64_t	lba_start;	/* position in the composite address space */
	uint64_t	num_blocks;
	char		wwn[TIER_WWN_LEN];
	char		serial[TIER_SERIAL_LEN];
	uint8_t		reserved[32];	/* F-2 */
};

/* On-disk superblock (identical content on every band; 256 B header + bands). */
struct tier_superblock {
	uint64_t	magic;
	uint32_t	version;
	uint32_t	crc;		/* CRC32c over the whole struct with crc field = 0 */
	uint64_t	seq;		/* monotone; on conflict, highest seq wins */
	uint64_t	created_epoch_sec;	/* wall clock at serialization (informative) */
	uint8_t		generation_uuid[TIER_SB_GEN_UUID_LEN];	/* composite instance (fencing, F-2) */
	char		composite_name[TIER_BDEV_NAME_LEN];
	uint64_t	md_num_blocks;	/* size of the mirrored md region (composite blocks) */
	uint64_t	cluster_blocks;	/* blobstore cluster size in blocks (grain, F1; u64 since v2) */
	uint32_t	md_mirror_a;	/* band slot ids holding the md RAID1 pair */
	uint32_t	md_mirror_b;
	uint32_t	num_bands;
	uint32_t	this_band_id;	/* which band slot this copy physically sits on */
	uint32_t	blocklen;	/* common block size */
	uint32_t	reserved0;
	uint8_t		reserved[104];	/* F-2 (pads the header to exactly 256 B) */
	struct tier_sb_band bands[TIER_MAX_BANDS];
};

/* F-1: lock the on-disk ABI — any layout drift is a compile error. */
SPDK_STATIC_ASSERT(sizeof(struct tier_sb_band) == 192, "tier_sb_band on-disk ABI changed");
SPDK_STATIC_ASSERT(offsetof(struct tier_superblock, bands) == 256,
		   "tier_superblock header on-disk ABI changed");
SPDK_STATIC_ASSERT(sizeof(struct tier_superblock) == 12544, "tier_superblock on-disk ABI changed");

/* F-5: generation seq N lives in slot N%2 — alternating slots survive torn writes. */
static inline uint32_t
tier_sb_slot_for_seq(uint64_t seq)
{
	return (uint32_t)(seq & 1);
}

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

	/* Open handle to the underlying disk (NULL while retired or hot-removed). */
	struct spdk_bdev_desc	*desc;

	/* Back-pointer to the composite (needed by the hot-remove event callback,
	 * which only receives the band as event_ctx). */
	struct vbdev_tier	*t;

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
	uint8_t			gen_uuid[TIER_SB_GEN_UUID_LEN];	/* composite instance uuid (minted at
							 * create, stored in every SB copy — F-2 fencing) */
	uint64_t		cluster_blocks;	/* blobstore cluster size in blocks; ALL band/md boundaries are
						 * aligned to this so no cluster ever straddles a band/region
						 * boundary (F1) — a straddling cluster would fail I/O (-EIO). */
	uint64_t		seq;		/* current superblock generation (monotone). M5(a): RESERVED
						 * (incremented) at write_all entry, so no two write_all
						 * generations can ever share a seq — gaps are harmless
						 * ("highest seq wins"), duplicates are fatal. */
	uint64_t		total_num_blocks;	/* md region + Σ data bands (excludes per-disk sb reserve) */
	bool			registered;

	/* M5(a): serialize tier_sb_write_all — one fan-out in flight at a time;
	 * concurrent requests queue their callbacks and are coalesced into ONE
	 * follow-up fan-out that persists the latest state. */
	bool			sb_write_inflight;
	bool			sb_write_queued;
	TAILQ_HEAD(, tier_sb_pending_cb) sb_pending_cbs;

	TAILQ_ENTRY(vbdev_tier)	link;
};

/* Queued completion callback for a serialized tier_sb_write_all request. */
struct tier_sb_pending_cb {
	void	(*cb)(void *cb_arg, int rc);
	void	*cb_arg;
	TAILQ_ENTRY(tier_sb_pending_cb) link;
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

/* Round an LBA/length down/up to the composite cluster grain (F1: keep every band/region
 * boundary cluster-aligned so no blobstore cluster straddles a boundary). cluster_blocks==0
 * (legacy) ⇒ no alignment. */
static inline uint64_t
tier_align_down(const struct vbdev_tier *t, uint64_t v)
{
	return (t->cluster_blocks > 1) ? (v - (v % t->cluster_blocks)) : v;
}
static inline uint64_t
tier_align_up(const struct vbdev_tier *t, uint64_t v)
{
	return (t->cluster_blocks > 1) ? tier_align_down(t, v + t->cluster_blocks - 1) : v;
}

/* Lifecycle (RPC-driven, SPEC-73A §9.1 / C-MUT-2). */
struct vbdev_tier *vbdev_tier_create(const char *name, uint64_t md_num_blocks, uint64_t cluster_blocks);
struct vbdev_tier *vbdev_tier_get_by_name(const char *name);
int vbdev_tier_add_band(struct vbdev_tier *t, const char *base_bdev_name,
			enum tier_class tier, const char *wwn, const char *serial,
			uint32_t *out_band_id);
/* SPEC-73 A2: place a band at explicit stored geometry (superblock-authoritative reassembly). */
int vbdev_tier_assemble_band(struct vbdev_tier *t, const char *base_bdev_name, uint32_t band_id,
			     enum tier_class tier, const char *wwn, const char *serial,
			     uint64_t lba_start, uint64_t num_blocks, enum tier_band_state state, bool is_md);
/* MJ6: async — cb fires AFTER the retirement is persisted to the surviving bands'
 * superblocks (rc != 0 ⇒ NOT durable, caller must retry; in-memory state is
 * already RETIRED). T-7: retiring an md-mirror band is refused (-EBUSY). */
int vbdev_tier_retire_band(struct vbdev_tier *t, uint32_t band_id,
			   void (*cb)(void *cb_arg, int rc), void *cb_arg);
/* C3: resync the mirrored md region onto a DEGRADED md leg (typically a
 * replacement disk assembled DEGRADED into an md slot), then activate it and
 * persist. Runs under a quiesce of the composite md range (identity-mapped, so
 * held writes replay correctly to BOTH legs after activation). Async; cb gets
 * rc != 0 on failure (leg left DEGRADED — retry). */
int vbdev_tier_resync_md(struct vbdev_tier *t, uint32_t target_band_id,
			 void (*cb)(void *cb_arg, int rc), void *cb_arg);
/* Register the composite bdev once its bands are configured. */
int vbdev_tier_register(struct vbdev_tier *t);
/* Tear down + unregister (cleanup). */
int vbdev_tier_delete(struct vbdev_tier *t);

/* Superblock (vbdev_tier_sb.c) — native-level persistence (INV-T1). */
void tier_sb_serialize(struct vbdev_tier *t, struct tier_band *self, uint64_t seq,
		       uint64_t created_epoch_sec, struct tier_superblock *sb);
bool tier_sb_valid(const struct tier_superblock *sb);	/* magic + crc check (LE-only, F-4) */
/* F-5: pick the best (valid, highest-seq) slot inside a full reserve buffer
 * (slot A at 0, slot B at TIER_SB_SLOT_BYTES). NULL if neither is valid.
 * Pure — unit-tested host-side. */
const struct tier_superblock *tier_sb_select(const void *reserve_buf, size_t reserve_len);
/* Async-write the (serialized) superblock to every ACTIVE band (M5(b): DEGRADED
 * bands are excluded — their stale copy is out-voted by seq at reassembly), then
 * FLUSH the written slot (F-6). Generation seq N goes to slot N%2 (F-5), so a
 * torn write can only destroy one slot. Serialized (M5(a)): a call while a
 * fan-out is in flight queues cb and coalesces into one follow-up fan-out of the
 * LATEST state. cb fires once, rc != 0 if any band failed. cb may be NULL. */
int tier_sb_write_all(struct vbdev_tier *t, void (*cb)(void *cb_arg, int rc), void *cb_arg);
/* Async-read BOTH superblock slots from a base bdev desc; cb receives the best
 * valid slot per tier_sb_select (NULL + rc on failure). The sb pointer is only
 * valid for the duration of the callback. */
int tier_sb_read_desc(struct spdk_bdev_desc *desc, uint32_t blocklen,
		      void (*cb)(void *cb_arg, const struct tier_superblock *sb, int rc), void *cb_arg);

/* M2b: copy num_blocks from src composite-LBA to dst composite-LBA by resolving
 * each to its band + physical offset and doing a direct base-bdev read+write
 * (bypasses the composite, so it is NOT held by the caller's blob-level freeze).
 * C1: the caller must hold spdk_blob_freeze_io on the owning blob for the whole
 * copy+commit — a composite-level quiesce is NOT a valid barrier here (it holds
 * writes below the blob→LBA translation and replays them to the OLD lba). */
typedef void (*tier_relocate_cb)(void *cb_arg, int status);
/* verify (PF4): run the C5 read-back + CRC32c check after the write (detects a
 * silent media/write corruption at relocate time). Optional per disk class. */
int vbdev_tier_relocate_copy(struct vbdev_tier *t, uint64_t src_lba, uint64_t dst_lba,
			     uint64_t num_blocks, bool verify, tier_relocate_cb cb_fn, void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_VBDEV_TIER_H */
