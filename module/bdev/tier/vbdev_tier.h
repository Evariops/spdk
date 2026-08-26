/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Evariops. All rights reserved.
 *
 *   bdev_tier — composite vbdev aggregating a node's disks into one linear
 *   address space, split into BANDS (one band == one base bdev), fast -> slow.
 *
 *   Layout:
 *     [ metadata region (md)      | data region (concat of bands)      ]
 *     [ MIRRORED RAID1 on 2 bands | band0 | band1 | band2 | ...        ]
 *
 *   The blobstore keeps its superblock, masks and extent pages at LOW LBA, so
 *   they fall inside the mirrored md region and survive a single disk loss. The
 *   data region is a pure CONCAT routed by address arithmetic (no L2P table),
 *   so lowest-first allocation writes to the fastest band for free. Failure is
 *   isolated per band: a degraded band returns -EIO on its own LBA range only,
 *   and the composite never reports a global degradation.
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

/* Performance tier — totally ordered, fast -> slow. The CSI agent resolves a
 * concrete band; this module only stores and reports the class. */
enum tier_class {
	TIER_ULTRA	= 0,
	TIER_PREMIUM	= 1,
	TIER_STANDARD	= 2,
	TIER_ECONOMY	= 3,
	TIER_CLASS_MAX
};

/* Lifecycle state of a band. */
enum tier_band_state {
	TIER_BAND_ACTIVE	= 0,
	TIER_BAND_DEGRADED	= 1,	/* disk failed: I/O on its range returns -EIO */
	TIER_BAND_RETIRED	= 2,	/* evacuated + removed; slot kept, range unreclaimable */
};

#define TIER_WWN_LEN		64
#define TIER_SERIAL_LEN		64
#define TIER_BDEV_NAME_LEN	64
#define TIER_PART_UUID_LEN	16
#define TIER_MAX_BANDS		64	/* per node; a node won't exceed this many disks */

/* ---- On-disk superblock v2 --------------------------------------------------
 * One copy lives in a RESERVED region at the start of EACH base bdev (per-disk,
 * NOT inside the mirrored md range) and self-describes the WHOLE composite.
 * There is NO examine path: the CSI agent reads every disk's SB via
 * bdev_tier_read_sb, picks the highest-seq copy, and replays create +
 * assemble_band at the stored geometry. Swap detection (live wwn vs the slot's
 * stored wwn) belongs to that replay; in-module the only wwn guard is the
 * duplicate-wwn rejection at add/assemble.
 * The 256 KiB reserve holds TWO 128 KiB SLOTS (A at 0, B at 128 KiB): seq N is
 * written to slot N%2, so a torn write destroys at most one slot, and readers
 * validate both and take the highest-seq valid one.
 * Little-endian only; the static asserts below lock the layout. */
#define TIER_SB_MAGIC		0x5449455253423032ULL	/* "TIERSB02" */
#define TIER_SB_VERSION		2u
#define TIER_SB_RESERVE_BYTES	(256 * 1024)		/* reserved per base bdev for the sb */
#define TIER_SB_SLOT_BYTES	(128 * 1024)		/* two A/B slots inside the reserve */
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
	/* Unit identity, carved from the former reserved[32]. part_uuid is an OPAQUE
	 * 16-byte identity supplied by the control plane (a partition's PARTUUID);
	 * the module stores and reports it, never probes it. All-zero part_uuid =
	 * whole disk / no partition identity — exactly what every SB written before
	 * these fields existed contains, so old superblocks read back unchanged and
	 * no version bump is needed. part_start_lba/part_size_blocks record the
	 * unit's geometry on its PARENT device (0/0 for a whole disk), letting the
	 * control plane compare stored vs live geometry by equality at reassembly. */
	uint8_t		part_uuid[TIER_PART_UUID_LEN];
	uint64_t	part_start_lba;
	uint64_t	part_size_blocks;
};

/* On-disk superblock (identical content on every band; 256 B header + bands). */
struct tier_superblock {
	uint64_t	magic;
	uint32_t	version;
	uint32_t	crc;		/* CRC32c over the whole struct with crc field = 0 */
	uint64_t	seq;		/* monotone; on conflict, highest seq wins */
	uint64_t	created_epoch_sec;	/* wall clock at serialization (informative) */
	uint8_t		generation_uuid[TIER_SB_GEN_UUID_LEN];	/* composite instance; a re-created composite mints a new one,
							 * so disks from a previous life cannot be cross-assembled */
	char		composite_name[TIER_BDEV_NAME_LEN];
	uint64_t	md_num_blocks;	/* size of the mirrored md region (composite blocks) */
	uint64_t	cluster_blocks;	/* blobstore cluster size in blocks (alignment grain) */
	uint32_t	md_mirror_a;	/* band slot ids holding the md RAID1 pair */
	uint32_t	md_mirror_b;
	uint32_t	num_bands;
	uint32_t	this_band_id;	/* which band slot this copy physically sits on */
	uint32_t	blocklen;	/* common block size */
	uint32_t	reserved0;
	uint8_t		reserved[104];	/* pads the header to exactly 256 B */
	struct tier_sb_band bands[TIER_MAX_BANDS];
};

/* Lock the on-disk ABI — any layout drift is a compile error. */
SPDK_STATIC_ASSERT(sizeof(struct tier_sb_band) == 192, "tier_sb_band on-disk ABI changed");
SPDK_STATIC_ASSERT(offsetof(struct tier_sb_band, part_uuid) == 160,
		   "tier_sb_band part_uuid on-disk ABI changed");
SPDK_STATIC_ASSERT(offsetof(struct tier_sb_band, part_start_lba) == 176,
		   "tier_sb_band part_start_lba on-disk ABI changed");
SPDK_STATIC_ASSERT(offsetof(struct tier_sb_band, part_size_blocks) == 184,
		   "tier_sb_band part_size_blocks on-disk ABI changed");
SPDK_STATIC_ASSERT(offsetof(struct tier_superblock, bands) == 256,
		   "tier_superblock header on-disk ABI changed");
SPDK_STATIC_ASSERT(sizeof(struct tier_superblock) == 12544, "tier_superblock on-disk ABI changed");

/* Generation seq N lives in slot N%2 — alternating slots survive torn writes. */
static inline uint32_t
tier_sb_slot_for_seq(uint64_t seq)
{
	return (uint32_t)(seq & 1);
}

/* Render a part_uuid as 32 lowercase hex chars into out (>= 33 bytes). An
 * all-zero uuid renders as the EMPTY string — the wire convention for "no
 * partition identity" on every RPC that emits the field. Returns out. */
static inline const char *
tier_part_uuid_hex(const uint8_t uuid[TIER_PART_UUID_LEN], char out[2 * TIER_PART_UUID_LEN + 1])
{
	static const char hex[] = "0123456789abcdef";
	bool all_zero = true;
	int i;

	for (i = 0; i < TIER_PART_UUID_LEN; i++) {
		if (uuid[i] != 0) {
			all_zero = false;
		}
		out[2 * i] = hex[uuid[i] >> 4];
		out[2 * i + 1] = hex[uuid[i] & 0xF];
	}
	out[all_zero ? 0 : 2 * TIER_PART_UUID_LEN] = '\0';
	return out;
}

/*
 * One band == one physical base bdev. band_id is a STABLE monotone slot, never
 * reused: a retired disk keeps its slot.
 */
struct tier_band {
	uint32_t		band_id;
	enum tier_class		tier;
	enum tier_band_state	state;

	char			base_bdev_name[TIER_BDEV_NAME_LEN];
	char			wwn[TIER_WWN_LEN];	/* disk identity — detect a swapped disk in a slot */
	char			serial[TIER_SERIAL_LEN];
	/* Unit identity (see struct tier_sb_band): control-plane-supplied, opaque.
	 * All-zero part_uuid = whole disk. */
	uint8_t			part_uuid[TIER_PART_UUID_LEN];
	uint64_t		part_start_lba;
	uint64_t		part_size_blocks;

	/* Position in the composite linear address space (in blocks). */
	uint64_t		lba_start;	/* composite start of this band's contribution */
	uint64_t		num_blocks;	/* usable blocks contributed by this band */
	uint64_t		phys_offset;	/* base-bdev physical block where lba_start maps
						 * (>= sb_blocks; mirror band A adds md_num_blocks) */

	/* Open handle to the underlying disk (NULL while retired or hot-removed). */
	struct spdk_bdev_desc	*desc;

	/* A hot-remove landing while an SB fan-out is in flight defers this band's
	 * channel-drain+close: closing the desc under an in-flight SB write would
	 * violate the channel-before-desc contract. Cleared by
	 * vbdev_tier_sb_fanout_idle() once the fan-out drains. */
	bool			close_pending;

	/* Drain state. A band drain must not put a reactor's base channel while host
	 * legs are in flight on it (bdev_channel_destroy asserts io_outstanding == 0),
	 * nor close the desc while a relocate/resync engine still submits on it.
	 * The last drain reference performs the close, on the app thread. */
	bool			draining;
	uint32_t		drain_refs;	/* the drain fan-out (+1) plus every reactor that
						 * deferred its channel put; atomic, released from
						 * reactor threads */
	bool			close_deferred;	/* close requested while pinned; runs at unpin */
	uint32_t		desc_pins;	/* engines using this desc; app-thread only */
	void			(*drain_cb)(void *cb_arg, int rc);
	void			*drain_cb_arg;

	/* Back-pointer to the composite (needed by the hot-remove event callback,
	 * which only receives the band as event_ctx). */
	struct vbdev_tier	*t;

	TAILQ_ENTRY(tier_band)	link;
};

/* Per-band fill accounting provider. The composite only knows geometry; which
 * blocks are LIVE is logical state owned by the blobstore sitting on top. The
 * lvol layer registers a provider when an lvolstore loads on a tier composite,
 * and bdev_tier_get_bands calls it per band (synchronously, on the app/RPC
 * thread) to fill used_blocks. No provider = used_blocks reported as 0. */
typedef int (*vbdev_tier_usage_fn)(void *ctx, uint64_t lba_start, uint64_t num_blocks,
				   uint64_t *used_blocks_out);

/*
 * The composite vbdev. One per node.
 */
struct vbdev_tier {
	struct spdk_bdev	bdev;		/* the bdev we register */

	TAILQ_HEAD(, tier_band)	bands;		/* ordered fast -> slow, by lba_start */
	uint32_t		num_bands;
	uint32_t		next_band_id;	/* monotone slot allocator */

	/* Mirrored metadata region [0, md_num_blocks) — RAID1 across two bands.
	 * md_mirror_a / md_mirror_b are band_ids; both hold an identical copy of the
	 * low LBA range so blobstore metadata survives a single disk loss. */
	uint64_t		md_num_blocks;	/* size of the mirrored md region, in blocks */
	uint32_t		md_mirror_a;
	uint32_t		md_mirror_b;

	uint32_t		blocklen;	/* common block size of all bands (must match) */
	uint32_t		sb_blocks;	/* reserved superblock blocks at the start of EACH base bdev */
	uint8_t			gen_uuid[TIER_SB_GEN_UUID_LEN];	/* composite instance uuid, minted at
							 * create and stored in every SB copy */
	uint64_t		cluster_blocks;	/* blobstore cluster size in blocks; ALL band/md boundaries are
						 * aligned to this so no cluster ever straddles a band/region
						 * boundary — a straddling cluster would fail I/O (-EIO). */
	uint64_t		seq;		/* current superblock generation (monotone). RESERVED
						 * (incremented) at write_all entry, so no two write_all
						 * generations can ever share a seq — gaps are harmless
						 * ("highest seq wins"), duplicates are fatal. */
	uint64_t		total_num_blocks;	/* md region + Σ data bands (excludes per-disk sb reserve) */
	bool			registered;
	/* At least one band was placed by assemble_band (stored geometry). From then
	 * on add_band may only append a plain concat band: the md-leg auto-promotion
	 * branches would hand a NEW device an md slot left empty by an ABSENT disk
	 * (blank ACTIVE mirror leg, no resync) and, worse, the leg-A branch would
	 * clobber total_num_blocks accumulated by the assembled bands. */
	bool			assembled;

	/* Fill accounting provider (see vbdev_tier_usage_fn). */
	vbdev_tier_usage_fn	usage_fn;
	void			*usage_ctx;

	/* The module thread, captured at create (the app/RPC thread). ALL
	 * composite-global SB-persist state (seq, sb_write_inflight/queued,
	 * sb_pending_cbs, delete/close_pending) is owned by it; reactor-side events
	 * funnel their persist through spdk_thread_send_msg instead of mutating it. */
	struct spdk_thread	*thread;

	/* Serialize tier_sb_write_all — one fan-out in flight at a time; concurrent
	 * requests queue their callbacks and are coalesced into ONE follow-up
	 * fan-out that persists the latest state. */
	bool			sb_write_inflight;
	bool			sb_write_queued;
	/* A bdev_tier_delete that arrives while ANY async op holds this composite is
	 * deferred here: tearing down now would free `t` and its bands under the
	 * in-flight op. Honored once every async op drains. */
	bool			delete_pending;
	/* In-flight composite async ops whose context holds pointers into `t`
	 * (bands/descs) but which do NOT run through `t`'s io_device, so nothing else
	 * defers the teardown for them: relocate copies, md resyncs, register-time seq
	 * rehydrate reads. The last op to finish runs the deferred teardown. Drains
	 * going through spdk_for_each_channel are covered by the io_device refcount
	 * and are NOT counted here. */
	uint32_t		async_inflight;
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

	/* Legs in flight per band on THIS reactor. Single-thread access: legs complete
	 * on their submitting thread, so no atomics are needed. A band drain finding
	 * inflight > 0 defers the channel put to the last completion. */
	uint32_t		inflight[TIER_MAX_BANDS];
	bool			drain_deferred[TIER_MAX_BANDS];
};

/* ---- Internal API (vbdev_tier.c, vbdev_tier_rpc.c) ------------------------- */

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

/* Round an LBA/length down/up to the composite cluster grain, which keeps every
 * band/region boundary cluster-aligned so no blobstore cluster straddles one.
 * cluster_blocks <= 1 means no alignment. */
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

/* Lifecycle (RPC-driven). */
struct vbdev_tier *vbdev_tier_create(const char *name, uint64_t md_num_blocks, uint64_t cluster_blocks);
struct vbdev_tier *vbdev_tier_get_by_name(const char *name);
/* part_uuid is TIER_PART_UUID_LEN bytes or NULL (= all-zero, whole disk). */
int vbdev_tier_add_band(struct vbdev_tier *t, const char *base_bdev_name,
			enum tier_class tier, const char *wwn, const char *serial,
			const uint8_t *part_uuid, uint64_t part_start_lba,
			uint64_t part_size_blocks, uint32_t *out_band_id);
/* Place a band at explicit stored geometry (superblock-authoritative reassembly). */
int vbdev_tier_assemble_band(struct vbdev_tier *t, const char *base_bdev_name, uint32_t band_id,
			     enum tier_class tier, const char *wwn, const char *serial,
			     const uint8_t *part_uuid, uint64_t part_start_lba,
			     uint64_t part_size_blocks, uint64_t lba_start, uint64_t num_blocks,
			     enum tier_band_state state, bool is_md);
/* Async: cb fires AFTER the retirement is persisted to the surviving bands'
 * superblocks. rc != 0 means NOT durable and the caller must retry, while the
 * in-memory state is already RETIRED. Retiring an md-mirror band is refused. */
int vbdev_tier_retire_band(struct vbdev_tier *t, uint32_t band_id,
			   void (*cb)(void *cb_arg, int rc), void *cb_arg);
/* Resync the mirrored md region onto a DEGRADED md leg (typically a replacement
 * disk assembled DEGRADED into an md slot), then activate it and persist. Runs
 * under a quiesce of the composite md range, which is identity-mapped, so held
 * writes replay to BOTH legs after activation. On failure the leg stays DEGRADED. */
int vbdev_tier_resync_md(struct vbdev_tier *t, uint32_t target_band_id,
			 void (*cb)(void *cb_arg, int rc), void *cb_arg);
/* Register the composite bdev once its bands are configured. */
int vbdev_tier_register(struct vbdev_tier *t);
/* Tear down + unregister. If an SB fan-out is in flight the teardown is deferred
 * (see delete_pending). */
int vbdev_tier_delete(struct vbdev_tier *t);
/* Called by tier_sb_write_all's fan-out completion once the fan-out has drained.
 * Runs any teardown deferred behind it (a pending delete, or a hot-removed band's
 * channel-drain+close). Returns true if a deferred delete consumed the composite —
 * the caller must not touch `t` afterward. */
bool vbdev_tier_sb_fanout_idle(struct vbdev_tier *t);

/* Register/clear the fill-accounting provider of a composite (lvol layer; see
 * vbdev_tier_usage_fn). Both tolerate an unknown tier_name: the composite may
 * already be gone when the lvolstore unloads. */
void vbdev_tier_set_usage_provider(const char *tier_name, vbdev_tier_usage_fn fn, void *ctx);
void vbdev_tier_clear_usage_provider(const char *tier_name);

/* Superblock (vbdev_tier_sb.c). */
void tier_sb_serialize(struct vbdev_tier *t, struct tier_band *self, uint64_t seq,
		       uint64_t created_epoch_sec, struct tier_superblock *sb);
bool tier_sb_valid(const struct tier_superblock *sb);	/* magic + crc check (little-endian only) */
/* Pick the best (valid, highest-seq) slot inside a full reserve buffer (slot A at
 * 0, slot B at TIER_SB_SLOT_BYTES). NULL if neither is valid. Pure — unit-tested
 * host-side. */
const struct tier_superblock *tier_sb_select(const void *reserve_buf, size_t reserve_len);
/* Async-write the serialized superblock to every ACTIVE band (DEGRADED bands are
 * excluded — their stale copy is out-voted by seq at reassembly), then FLUSH the
 * written slot. Serialized: a call while a fan-out is in flight queues cb and
 * coalesces into one follow-up fan-out of the LATEST state. cb fires once, rc != 0
 * if any band failed. cb may be NULL. */
int tier_sb_write_all(struct vbdev_tier *t, void (*cb)(void *cb_arg, int rc), void *cb_arg);
/* Async-read BOTH superblock slots from a base bdev desc; cb receives the best
 * valid slot per tier_sb_select (NULL + rc on failure). The sb pointer is only
 * valid for the duration of the callback. */
int tier_sb_read_desc(struct spdk_bdev_desc *desc, uint32_t blocklen,
		      void (*cb)(void *cb_arg, const struct tier_superblock *sb, int rc), void *cb_arg);

/* Copy num_blocks from src composite-LBA to dst composite-LBA by resolving each to
 * its band + physical offset and doing a direct base-bdev read+write, bypassing the
 * composite. The caller must hold spdk_blob_freeze_io on the owning blob for the
 * whole copy+commit: a composite-level quiesce is NOT a valid barrier here, since it
 * holds writes below the blob-to-LBA translation and replays them to the OLD lba. */
typedef void (*tier_relocate_cb)(void *cb_arg, int status);
/* verify: read the destination back and CRC32c-compare it after the write, which
 * detects a silent media/write corruption at relocate time. Optional per disk class. */
int vbdev_tier_relocate_copy(struct vbdev_tier *t, uint64_t src_lba, uint64_t dst_lba,
			     uint64_t num_blocks, bool verify, tier_relocate_cb cb_fn, void *cb_arg);

#ifdef __cplusplus
}
#endif

#endif /* SPDK_VBDEV_TIER_H */
