/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Evariops.
 *
 *   T1: host-compilable unit tests of the PRODUCTION tier code (not a copy):
 *   this translation unit #includes vbdev_tier_sb.c directly, compiled against
 *   the minimal mock SPDK headers in mock/. Covers the v2 on-disk format:
 *     - ABI (sizeof/offsetof, doubling the SPDK_STATIC_ASSERTs at runtime)
 *     - tier_sb_serialize / tier_sb_valid roundtrip + corruption + version +
 *       byte-swapped-magic (F-4) rejection
 *     - F-5 A/B slot selection (tier_sb_select): highest valid seq wins,
 *       torn/invalid slot tolerated
 *     - F-2 generation_uuid + created_epoch_sec + u64 cluster_blocks (F-3)
 *     - band table serialization content, geometry inlines
 *     - a binary GOLDEN vector: the serialized header bytes are pinned so an
 *       accidental field reorder is caught even if sizeof stays equal
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

/* ---- mock bdev I/O stubs (unused by the serialize/valid/select tests) ------- */

int
spdk_bdev_write_blocks(struct spdk_bdev_desc *desc, struct spdk_io_channel *ch,
		       void *buf, uint64_t offset_blocks, uint64_t num_blocks,
		       spdk_bdev_io_completion_cb cb, void *cb_arg)
{
	(void)desc; (void)ch; (void)buf; (void)offset_blocks; (void)num_blocks;
	(void)cb; (void)cb_arg;
	return -ENOTSUP;
}

struct spdk_bdev *
spdk_bdev_desc_get_bdev(struct spdk_bdev_desc *desc)
{
	return (struct spdk_bdev *)desc;	/* opaque round-trip for the mock */
}

bool
spdk_bdev_io_type_supported(struct spdk_bdev *bdev, enum spdk_bdev_io_type io_type)
{
	(void)bdev; (void)io_type;
	return false;	/* mirrors the -ENOTSUP flush stub: no-FLUSH base (bdev_uring) */
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

/* T-4b: lives in vbdev_tier.c (not host-compilable). The SB serialize/valid/select
 * tests never drive a fan-out to completion, so a no-op that reports "nothing
 * deferred" (false) is sufficient to satisfy the link. */
bool
vbdev_tier_sb_fanout_idle(struct vbdev_tier *t)
{
	(void)t;
	return false;
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
	uint32_t i;

	memset(t, 0, sizeof(*t));
	TAILQ_INIT(&t->bands);
	TAILQ_INIT(&t->sb_pending_cbs);
	t->bdev.name = (char *)"tier0";
	t->blocklen = 4096;
	t->sb_blocks = TIER_SB_RESERVE_BYTES / 4096;	/* 64 */
	t->cluster_blocks = 256;			/* 1 MiB clusters */
	t->md_num_blocks = 4096;			/* cluster-aligned */
	t->md_mirror_a = 0;
	t->md_mirror_b = 1;
	t->seq = 41;
	for (i = 0; i < TIER_SB_GEN_UUID_LEN; i++) {
		t->gen_uuid[i] = (uint8_t)(0xA0 + i);
	}
	add_band(t, 0, TIER_ULTRA, TIER_BAND_ACTIVE, 4096, 100352, "wwn-a");
	add_band(t, 1, TIER_PREMIUM, TIER_BAND_ACTIVE, 104448, 50176, "wwn-b");
	add_band(t, 2, TIER_ECONOMY, TIER_BAND_DEGRADED, 154624, 25088, "wwn-c");
	t->total_num_blocks = 4096 + 100352 + 50176 + 25088;
}

/* Free the bands make_composite/add_band allocated (the composite `t` itself is
 * stack-owned; only the band nodes are heap). Keeps the ASAN+LSAN build clean. */
static void
free_composite(struct vbdev_tier *t)
{
	struct tier_band *b;

	while ((b = TAILQ_FIRST(&t->bands)) != NULL) {
		TAILQ_REMOVE(&t->bands, b, link);
		free(b);
	}
	t->num_bands = 0;
}

/* ---- tests ------------------------------------------------------------------- */

static void
test_abi(void)
{
	/* F-1: runtime double-check of the compile-time asserts. */
	CHECK(sizeof(struct tier_sb_band) == 192);
	CHECK(offsetof(struct tier_superblock, bands) == 256);
	CHECK(sizeof(struct tier_superblock) == 12544);
	CHECK(offsetof(struct tier_superblock, magic) == 0);
	CHECK(offsetof(struct tier_superblock, version) == 8);
	CHECK(offsetof(struct tier_superblock, crc) == 12);
	CHECK(offsetof(struct tier_superblock, seq) == 16);
	CHECK(offsetof(struct tier_superblock, created_epoch_sec) == 24);
	CHECK(offsetof(struct tier_superblock, generation_uuid) == 32);
	CHECK(offsetof(struct tier_superblock, composite_name) == 48);
	/* v2 constants. */
	CHECK(TIER_SB_MAGIC == 0x5449455253423032ULL);
	CHECK(TIER_SB_VERSION == 2u);
	CHECK(TIER_SB_SLOT_BYTES * 2 == TIER_SB_RESERVE_BYTES);
	/* A full superblock must fit in one slot (write_all precondition). */
	CHECK(sizeof(struct tier_superblock) <= TIER_SB_SLOT_BYTES);
}

static void
test_serialize_roundtrip(void)
{
	struct vbdev_tier t;
	struct tier_superblock sb;
	struct tier_band *self;

	make_composite(&t);
	self = TAILQ_FIRST(&t.bands);

	tier_sb_serialize(&t, self, 42, 1751630000ULL, &sb);
	CHECK(tier_sb_valid(&sb));
	CHECK(sb.magic == TIER_SB_MAGIC);
	CHECK(sb.version == TIER_SB_VERSION);
	CHECK(sb.seq == 42);
	CHECK(sb.created_epoch_sec == 1751630000ULL);
	CHECK(sb.generation_uuid[0] == 0xA0 && sb.generation_uuid[15] == 0xAF);
	CHECK(strcmp(sb.composite_name, "tier0") == 0);
	CHECK(sb.md_num_blocks == 4096);
	CHECK(sb.cluster_blocks == 256);	/* u64 field */
	CHECK(sb.md_mirror_a == 0 && sb.md_mirror_b == 1);
	CHECK(sb.num_bands == 3);
	CHECK(sb.this_band_id == 0);
	CHECK(sb.blocklen == 4096);
	CHECK(sb.bands[0].band_id == 0 && sb.bands[0].state == TIER_BAND_ACTIVE);
	CHECK(strcmp(sb.bands[0].wwn, "wwn-a") == 0);
	CHECK(sb.bands[1].lba_start == 104448 && sb.bands[1].num_blocks == 50176);
	CHECK(sb.bands[2].state == TIER_BAND_DEGRADED);
	CHECK(strcmp(sb.bands[2].serial, "serial-2") == 0);
	CHECK(sb.bands[3].band_id == 0 && sb.bands[3].num_blocks == 0);

	/* u64 cluster_blocks must survive a value > UINT32_MAX (F-3). */
	t.cluster_blocks = 0x100000001ULL;
	tier_sb_serialize(&t, self, 42, 0, &sb);
	CHECK(sb.cluster_blocks == 0x100000001ULL);
	CHECK(tier_sb_valid(&sb));

	/* NULL self => this_band_id sentinel. */
	tier_sb_serialize(&t, NULL, 43, 0, &sb);
	CHECK(sb.this_band_id == UINT32_MAX);
	CHECK(tier_sb_valid(&sb));
	free_composite(&t);
}

static void
test_reject_corruption(void)
{
	struct vbdev_tier t;
	struct tier_superblock sb;

	make_composite(&t);
	tier_sb_serialize(&t, TAILQ_FIRST(&t.bands), 42, 0, &sb);

	((uint8_t *)&sb)[sizeof(sb) - 1] ^= 0xFF;
	CHECK(!tier_sb_valid(&sb));
	((uint8_t *)&sb)[sizeof(sb) - 1] ^= 0xFF;
	CHECK(tier_sb_valid(&sb));

	sb.seq++;
	CHECK(!tier_sb_valid(&sb));
	sb.seq--;
	CHECK(tier_sb_valid(&sb));

	/* A tampered generation_uuid must invalidate the CRC (fencing integrity). */
	sb.generation_uuid[7] ^= 0x55;
	CHECK(!tier_sb_valid(&sb));
	sb.generation_uuid[7] ^= 0x55;
	CHECK(tier_sb_valid(&sb));

	sb.version = TIER_SB_VERSION + 1;	/* v1/v3 not accepted (clean break) */
	CHECK(!tier_sb_valid(&sb));
	sb.version = 1u;			/* the abandoned v1 must NOT read */
	CHECK(!tier_sb_valid(&sb));
	sb.version = TIER_SB_VERSION;

	sb.magic ^= 1;
	CHECK(!tier_sb_valid(&sb));
	sb.magic ^= 1;

	sb.magic = __builtin_bswap64(TIER_SB_MAGIC);	/* F-4 big-endian writer */
	CHECK(!tier_sb_valid(&sb));
	free_composite(&t);
}

/* F-5: two-slot selection. */
static void
test_slot_select(void)
{
	struct vbdev_tier t;
	uint8_t *reserve;
	struct tier_superblock *a, *b;

	make_composite(&t);
	reserve = calloc(1, TIER_SB_RESERVE_BYTES);
	assert(reserve != NULL);
	a = (struct tier_superblock *)reserve;
	b = (struct tier_superblock *)(reserve + TIER_SB_SLOT_BYTES);

	/* slot layout matches the writer: seq N -> slot N%2. */
	CHECK(tier_sb_slot_for_seq(42) == 0);
	CHECK(tier_sb_slot_for_seq(43) == 1);

	/* Both valid: higher seq wins regardless of which slot holds it. */
	tier_sb_serialize(&t, NULL, 44, 0, a);	/* slot A: even seq */
	tier_sb_serialize(&t, NULL, 45, 0, b);	/* slot B: odd seq  */
	CHECK(tier_sb_select(reserve, TIER_SB_RESERVE_BYTES) == b);
	tier_sb_serialize(&t, NULL, 46, 0, a);	/* now A newer */
	CHECK(tier_sb_select(reserve, TIER_SB_RESERVE_BYTES) == a);

	/* Torn newer slot (B) -> fall back to the older valid slot (A). */
	tier_sb_serialize(&t, NULL, 100, 0, a);
	tier_sb_serialize(&t, NULL, 101, 0, b);
	((uint8_t *)b)[64] ^= 0xFF;		/* corrupt B's CRC-covered bytes */
	CHECK(tier_sb_select(reserve, TIER_SB_RESERVE_BYTES) == a);

	/* Both torn -> NULL. */
	((uint8_t *)a)[64] ^= 0xFF;
	CHECK(tier_sb_select(reserve, TIER_SB_RESERVE_BYTES) == NULL);

	/* Short buffer -> NULL (no OOB read). */
	tier_sb_serialize(&t, NULL, 1, 0, a);
	CHECK(tier_sb_select(reserve, TIER_SB_RESERVE_BYTES - 1) == NULL);
	CHECK(tier_sb_select(NULL, TIER_SB_RESERVE_BYTES) == NULL);

	free(reserve);
	free_composite(&t);
}

static void
test_geometry_inlines(void)
{
	struct vbdev_tier t;

	make_composite(&t);	/* cluster_blocks = 256, md = 4096 */

	CHECK(tier_align_down(&t, 255) == 0);
	CHECK(tier_align_down(&t, 256) == 256);
	CHECK(tier_align_up(&t, 1) == 256);
	CHECK(tier_align_up(&t, 257) == 512);
	t.cluster_blocks = 0;
	CHECK(tier_align_up(&t, 257) == 257);
	t.cluster_blocks = 256;

	CHECK(vbdev_tier_is_md_range(&t, 0, 4096));
	CHECK(vbdev_tier_is_md_range(&t, 4095, 1));
	CHECK(!vbdev_tier_is_md_range(&t, 4095, 2));
	CHECK(!vbdev_tier_is_md_range(&t, 4096, 1));
	t.md_num_blocks = 0;
	CHECK(!vbdev_tier_is_md_range(&t, 0, 1));
	free_composite(&t);
}

/* Binary golden vector: pin the serialized header bytes so a field reorder that
 * preserves sizeof is still caught. Only deterministic header fields are pinned
 * (crc/created_epoch excluded — crc is derived, epoch is wall-clock). */
static void
test_golden_header(void)
{
	struct vbdev_tier t;
	struct tier_superblock sb;
	const uint8_t *p = (const uint8_t *)&sb;

	make_composite(&t);
	tier_sb_serialize(&t, TAILQ_FIRST(&t.bands), 0x42, 0, &sb);

	/* magic "TIERSB02" little-endian at offset 0. */
	CHECK(p[0] == 0x32 && p[1] == 0x30 && p[2] == 0x42 && p[3] == 0x53);
	CHECK(p[4] == 0x52 && p[5] == 0x45 && p[6] == 0x49 && p[7] == 0x54);
	/* version = 2 at offset 8 (LE u32). */
	CHECK(p[8] == 0x02 && p[9] == 0 && p[10] == 0 && p[11] == 0);
	/* seq = 0x42 at offset 16 (LE u64). */
	CHECK(p[16] == 0x42 && p[17] == 0 && p[23] == 0);
	/* generation_uuid at offset 32. */
	CHECK(p[32] == 0xA0 && p[47] == 0xAF);
	/* composite_name "tier0" at offset 48. */
	CHECK(p[48] == 't' && p[49] == 'i' && p[52] == '0' && p[53] == '\0');
	free_composite(&t);
}

int
main(void)
{
	test_abi();
	test_serialize_roundtrip();
	test_reject_corruption();
	test_slot_select();
	test_geometry_inlines();
	test_golden_header();

	if (g_failures != 0) {
		fprintf(stderr, "test_tier_sb: %d FAILURE(S)\n", g_failures);
		return 1;
	}
	printf("test_tier_sb: all tests passed\n");
	return 0;
}
