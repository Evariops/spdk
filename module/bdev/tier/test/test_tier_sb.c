/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Evariops.
 *
 *   T1: host-compilable unit tests of the PRODUCTION tier code (not a copy):
 *   this translation unit #includes vbdev_tier_sb.c directly, compiled against
 *   the minimal mock SPDK headers in mock/. Covers:
 *     - on-disk ABI (sizeof/offsetof, doubling the SPDK_STATIC_ASSERTs at runtime)
 *     - tier_sb_serialize / tier_sb_valid roundtrip + corruption + version +
 *       byte-swapped-magic (F-4) rejection
 *     - band table serialization content (slots, wwn, this_band_id)
 *     - geometry inlines from vbdev_tier.h (tier_align_up/down,
 *       vbdev_tier_is_md_range)
 *
 *   NOT covered here (needs the full bdev runtime — see docs/audits T3 bench):
 *   I/O routing, fan-out, hot-remove, resync, write_all channel plumbing.
 */

#include "vbdev_tier_sb.c"	/* PRODUCTION code under test */

static int g_failures;

#define CHECK(cond) do {							\
	if (!(cond)) {								\
		g_failures++;							\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);	\
	}									\
} while (0)

/* ---- mock bdev I/O stubs (unused by the serialize/valid tests) -------------- */

int
spdk_bdev_write_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       void *buf, uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	(void)desc; (void)ch; (void)buf; (void)offset_blocks; (void)num_blocks;
	(void)cb; (void)cb_arg;
	return -ENOTSUP;
}

int
spdk_bdev_read_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		      void *buf, uint64_t offset_blocks, uint64_t num_blocks,
		      spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	(void)desc; (void)ch; (void)buf; (void)offset_blocks; (void)num_blocks;
	(void)cb; (void)cb_arg;
	return -ENOTSUP;
}

int
spdk_bdev_flush_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	(void)desc; (void)ch; (void)offset_blocks; (void)num_blocks; (void)cb; (void)cb_arg;
	return -ENOTSUP;
}

void
spdk_bdev_free_io(struct spdk_bdev_io *bdev_io)
{
	(void)bdev_io;
}

struct spdk_io_channel *
spdk_bdev_get_io_channel(struct spdk_bdev_desc *desc)
{
	(void)desc;
	return NULL;
}

void
spdk_put_io_channel(struct spdk_io_channel *ch)
{
	(void)ch;
}

/* ---- fixtures ---------------------------------------------------------------- */

static struct tier_band *
add_band(struct vbdev_tier *t, uint32_t id, enum tier_class cls, enum tier_band_state st,
	 uint64_t lba_start, uint64_t num_blocks, const char *wwn)
{
	struct tier_band *b = calloc(1, sizeof(*b));

	assert(b != NULL);
	b->band_id = id;
	b->tier = cls;
	b->state = st;
	b->lba_start = lba_start;
	b->num_blocks = num_blocks;
	snprintf(b->wwn, sizeof(b->wwn), "%s", wwn);
	snprintf(b->serial, sizeof(b->serial), "serial-%u", id);
	snprintf(b->base_bdev_name, sizeof(b->base_bdev_name), "nvme%un1", id);
	b->t = t;
	TAILQ_INSERT_TAIL(&t->bands, b, link);
	t->num_bands++;
	return b;
}

static void
make_composite(struct vbdev_tier *t)
{
	memset(t, 0, sizeof(*t));
	TAILQ_INIT(&t->bands);
	TAILQ_INIT(&t->sb_pending_cbs);
	t->bdev.name = (char *)"tier0";
	t->blocklen = 4096;
	t->sb_blocks = 64;			/* 256 KiB / 4096 */
	t->cluster_blocks = 256;		/* 1 MiB clusters */
	t->md_num_blocks = 4096;		/* cluster-aligned */
	t->md_mirror_a = 0;
	t->md_mirror_b = 1;
	t->seq = 41;
	add_band(t, 0, TIER_ULTRA, TIER_BAND_ACTIVE, 4096, 100352, "wwn-a");
	add_band(t, 1, TIER_PREMIUM, TIER_BAND_ACTIVE, 104448, 50176, "wwn-b");
	add_band(t, 2, TIER_ECONOMY, TIER_BAND_DEGRADED, 154624, 25088, "wwn-c");
	t->total_num_blocks = 4096 + 100352 + 50176 + 25088;
}

/* ---- tests ------------------------------------------------------------------- */

static void
test_abi(void)
{
	/* F-1: runtime double-check of the compile-time asserts. */
	CHECK(sizeof(struct tier_sb_band) == 160);
	CHECK(offsetof(struct tier_superblock, bands) == 120);
	CHECK(sizeof(struct tier_superblock) == 10360);
	CHECK(offsetof(struct tier_superblock, magic) == 0);
	CHECK(offsetof(struct tier_superblock, version) == 8);
	CHECK(offsetof(struct tier_superblock, crc) == 12);
	CHECK(offsetof(struct tier_superblock, seq) == 16);
}

static void
test_serialize_roundtrip(void)
{
	struct vbdev_tier t;
	struct tier_superblock sb;
	struct tier_band *self;

	make_composite(&t);
	self = TAILQ_FIRST(&t.bands);

	tier_sb_serialize(&t, self, 42, &sb);
	CHECK(tier_sb_valid(&sb));
	CHECK(sb.magic == TIER_SB_MAGIC);
	CHECK(sb.version == TIER_SB_VERSION);
	CHECK(sb.seq == 42);
	CHECK(strcmp(sb.composite_name, "tier0") == 0);
	CHECK(sb.md_num_blocks == 4096);
	CHECK(sb.md_mirror_a == 0 && sb.md_mirror_b == 1);
	CHECK(sb.num_bands == 3);
	CHECK(sb.this_band_id == 0);
	CHECK(sb.blocklen == 4096);
	CHECK(sb.cluster_blocks == 256);
	/* Band table content, in insertion order. */
	CHECK(sb.bands[0].band_id == 0 && sb.bands[0].state == TIER_BAND_ACTIVE);
	CHECK(strcmp(sb.bands[0].wwn, "wwn-a") == 0);
	CHECK(sb.bands[1].lba_start == 104448 && sb.bands[1].num_blocks == 50176);
	CHECK(sb.bands[2].state == TIER_BAND_DEGRADED);
	CHECK(strcmp(sb.bands[2].serial, "serial-2") == 0);
	/* Unused slots stay zeroed (stable on-disk bytes). */
	CHECK(sb.bands[3].band_id == 0 && sb.bands[3].num_blocks == 0);

	/* NULL self => this_band_id sentinel. */
	tier_sb_serialize(&t, NULL, 43, &sb);
	CHECK(sb.this_band_id == UINT32_MAX);
	CHECK(tier_sb_valid(&sb));
}

static void
test_reject_corruption(void)
{
	struct vbdev_tier t;
	struct tier_superblock sb;

	make_composite(&t);
	tier_sb_serialize(&t, TAILQ_FIRST(&t.bands), 42, &sb);

	/* Single flipped byte in the band table -> CRC mismatch. */
	((uint8_t *)&sb)[sizeof(sb) - 1] ^= 0xFF;
	CHECK(!tier_sb_valid(&sb));
	((uint8_t *)&sb)[sizeof(sb) - 1] ^= 0xFF;
	CHECK(tier_sb_valid(&sb));

	/* Flipped seq (header) -> CRC mismatch. */
	sb.seq++;
	CHECK(!tier_sb_valid(&sb));
	sb.seq--;
	CHECK(tier_sb_valid(&sb));

	/* Unknown version (v1 is strict; v2 must go through U-1 migration). */
	sb.version = TIER_SB_VERSION + 1;
	CHECK(!tier_sb_valid(&sb));
	sb.version = TIER_SB_VERSION;

	/* Wrong magic. */
	sb.magic ^= 1;
	CHECK(!tier_sb_valid(&sb));
	sb.magic ^= 1;

	/* F-4: byte-swapped magic (big-endian writer) must be rejected even if
	 * the CRC were fixed up. */
	sb.magic = __builtin_bswap64(TIER_SB_MAGIC);
	CHECK(!tier_sb_valid(&sb));
}

static void
test_geometry_inlines(void)
{
	struct vbdev_tier t;

	make_composite(&t);	/* cluster_blocks = 256, md = 4096 */

	/* F1 alignment helpers. */
	CHECK(tier_align_down(&t, 0) == 0);
	CHECK(tier_align_down(&t, 255) == 0);
	CHECK(tier_align_down(&t, 256) == 256);
	CHECK(tier_align_down(&t, 511) == 256);
	CHECK(tier_align_up(&t, 0) == 0);
	CHECK(tier_align_up(&t, 1) == 256);
	CHECK(tier_align_up(&t, 256) == 256);
	CHECK(tier_align_up(&t, 257) == 512);
	/* cluster_blocks <= 1 => no alignment (legacy). */
	t.cluster_blocks = 0;
	CHECK(tier_align_up(&t, 257) == 257);
	CHECK(tier_align_down(&t, 257) == 257);
	t.cluster_blocks = 1;
	CHECK(tier_align_up(&t, 257) == 257);
	t.cluster_blocks = 256;

	/* md range membership: [0, 4096). */
	CHECK(vbdev_tier_is_md_range(&t, 0, 1));
	CHECK(vbdev_tier_is_md_range(&t, 0, 4096));
	CHECK(vbdev_tier_is_md_range(&t, 4095, 1));
	CHECK(!vbdev_tier_is_md_range(&t, 4095, 2));	/* straddles the boundary */
	CHECK(!vbdev_tier_is_md_range(&t, 4096, 1));	/* first data block */
	CHECK(!vbdev_tier_is_md_range(&t, 0, 4097));
	/* md_num_blocks == 0 => no md region at all. */
	t.md_num_blocks = 0;
	CHECK(!vbdev_tier_is_md_range(&t, 0, 1));
}

static void
test_seq_semantics(void)
{
	struct vbdev_tier t;
	struct tier_superblock a, b;

	make_composite(&t);
	/* M5(a): two serializations at different generations must never compare
	 * equal ("highest seq wins" needs distinguishable copies). */
	tier_sb_serialize(&t, NULL, 100, &a);
	tier_sb_serialize(&t, NULL, 101, &b);
	CHECK(a.seq != b.seq);
	CHECK(a.crc != b.crc);
	CHECK(tier_sb_valid(&a) && tier_sb_valid(&b));
}

int
main(void)
{
	test_abi();
	test_serialize_roundtrip();
	test_reject_corruption();
	test_geometry_inlines();
	test_seq_semantics();

	if (g_failures != 0) {
		fprintf(stderr, "test_tier_sb: %d FAILURE(S)\n", g_failures);
		return 1;
	}
	printf("test_tier_sb: all tests passed\n");
	return 0;
}
