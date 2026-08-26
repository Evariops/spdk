/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Evariops.
 *
 *   Host-compilable unit tests of the PRODUCTION tier code (not a copy): this
 *   translation unit #includes vbdev_tier_sb.c directly, compiled against the
 *   minimal mock SPDK headers in mock/. Covers the v2 on-disk format: ABI,
 *   serialize/validate roundtrip and rejections, A/B slot selection, the
 *   geometry inlines, and a binary golden vector of the header bytes.
 *
 *   NOT covered here (needs the full bdev runtime): I/O routing, fan-out,
 *   hot-remove, resync, write_all channel plumbing.
 */

/* Countdown-armed calloc fault injection for the production code compiled below
 * (-1 = pass-through). test_calloc's own call binds to the real libc calloc,
 * resolved before the macro exists. */
#include <stdlib.h>
static int g_calloc_fail_countdown = -1;
static void *
test_calloc(size_t n, size_t sz)
{
	if (g_calloc_fail_countdown == 0) {
		g_calloc_fail_countdown = -1;
		return NULL;
	}
	if (g_calloc_fail_countdown > 0) {
		g_calloc_fail_countdown--;
	}
	return calloc(n, sz);
}
#define calloc(n, sz) test_calloc((n), (sz))

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
	return false;	/* mirrors the -ENOTSUP flush stub: a base without FLUSH support */
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

/* Lives in vbdev_tier.c, which is not host-compilable. A no-op reporting "nothing
 * deferred" satisfies the link; the tests count invocations to pin the rule that
 * every fan-out termination resolves deferred teardown. */
static int g_fanout_idle_calls;
bool
vbdev_tier_sb_fanout_idle(struct vbdev_tier *t)
{
	(void)t;
	g_fanout_idle_calls++;
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

/* Free the heap band nodes; the composite `t` itself is stack-owned. */
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

/* The on-disk ABI (field offsets, struct sizes, format constants) has not drifted. */
static void
test_abi(void)
{
	CHECK(sizeof(struct tier_sb_band) == 192);
	CHECK(offsetof(struct tier_sb_band, wwn) == 32);
	CHECK(offsetof(struct tier_sb_band, serial) == 96);
	/* The unit-identity fields consume the former reserved[32] exactly. */
	CHECK(offsetof(struct tier_sb_band, part_uuid) == 160);
	CHECK(offsetof(struct tier_sb_band, part_start_lba) == 176);
	CHECK(offsetof(struct tier_sb_band, part_size_blocks) == 184);
	CHECK(offsetof(struct tier_superblock, bands) == 256);
	CHECK(sizeof(struct tier_superblock) == 12544);
	CHECK(offsetof(struct tier_superblock, magic) == 0);
	CHECK(offsetof(struct tier_superblock, version) == 8);
	CHECK(offsetof(struct tier_superblock, crc) == 12);
	CHECK(offsetof(struct tier_superblock, seq) == 16);
	CHECK(offsetof(struct tier_superblock, created_epoch_sec) == 24);
	CHECK(offsetof(struct tier_superblock, generation_uuid) == 32);
	CHECK(offsetof(struct tier_superblock, composite_name) == 48);
	CHECK(TIER_SB_MAGIC == 0x5449455253423032ULL);
	CHECK(TIER_SB_VERSION == 2u);
	CHECK(TIER_SB_SLOT_BYTES * 2 == TIER_SB_RESERVE_BYTES);
	/* A full superblock must fit in one slot (write_all precondition). */
	CHECK(sizeof(struct tier_superblock) <= TIER_SB_SLOT_BYTES);
}

/* Serializing a composite reproduces every header and band field, and validates. */
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

	/* The unit-identity fields roundtrip; bands without one serialize as zeros. */
	{
		struct tier_band *b1 = TAILQ_NEXT(self, link);
		uint32_t i;

		for (i = 0; i < TIER_PART_UUID_LEN; i++) {
			b1->part_uuid[i] = (uint8_t)(0x10 + i);
		}
		b1->part_start_lba = 264192;
		b1->part_size_blocks = 50176 + 64;
		tier_sb_serialize(&t, self, 42, 0, &sb);
		CHECK(tier_sb_valid(&sb));
		CHECK(sb.bands[1].part_uuid[0] == 0x10 && sb.bands[1].part_uuid[15] == 0x1F);
		CHECK(sb.bands[1].part_start_lba == 264192);
		CHECK(sb.bands[1].part_size_blocks == 50176 + 64);
		for (i = 0; i < TIER_PART_UUID_LEN; i++) {
			CHECK(sb.bands[0].part_uuid[i] == 0);
			CHECK(sb.bands[2].part_uuid[i] == 0);
		}
		CHECK(sb.bands[0].part_start_lba == 0 && sb.bands[0].part_size_blocks == 0);
	}

	/* cluster_blocks must survive a value > UINT32_MAX. */
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

/* Any bit flip, wrong version or byte-swapped magic makes a superblock invalid. */
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

	sb.generation_uuid[7] ^= 0x55;
	CHECK(!tier_sb_valid(&sb));
	sb.generation_uuid[7] ^= 0x55;
	CHECK(tier_sb_valid(&sb));

	sb.version = TIER_SB_VERSION + 1;	/* only the current version reads */
	CHECK(!tier_sb_valid(&sb));
	sb.version = 1u;
	CHECK(!tier_sb_valid(&sb));
	sb.version = TIER_SB_VERSION;

	sb.magic ^= 1;
	CHECK(!tier_sb_valid(&sb));
	sb.magic ^= 1;

	sb.magic = __builtin_bswap64(TIER_SB_MAGIC);	/* big-endian writer */
	CHECK(!tier_sb_valid(&sb));
	free_composite(&t);
}

/* Slot selection takes the valid slot with the highest seq, tolerating a torn one. */
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

	CHECK(tier_sb_slot_for_seq(42) == 0);
	CHECK(tier_sb_slot_for_seq(43) == 1);

	tier_sb_serialize(&t, NULL, 44, 0, a);
	tier_sb_serialize(&t, NULL, 45, 0, b);
	CHECK(tier_sb_select(reserve, TIER_SB_RESERVE_BYTES) == b);
	tier_sb_serialize(&t, NULL, 46, 0, a);
	CHECK(tier_sb_select(reserve, TIER_SB_RESERVE_BYTES) == a);

	tier_sb_serialize(&t, NULL, 100, 0, a);
	tier_sb_serialize(&t, NULL, 101, 0, b);
	((uint8_t *)b)[64] ^= 0xFF;		/* corrupt CRC-covered bytes of a slot */
	CHECK(tier_sb_select(reserve, TIER_SB_RESERVE_BYTES) == a);

	((uint8_t *)a)[64] ^= 0xFF;
	CHECK(tier_sb_select(reserve, TIER_SB_RESERVE_BYTES) == NULL);

	/* A short buffer yields NULL rather than reading out of bounds. */
	tier_sb_serialize(&t, NULL, 1, 0, a);
	CHECK(tier_sb_select(reserve, TIER_SB_RESERVE_BYTES - 1) == NULL);
	CHECK(tier_sb_select(NULL, TIER_SB_RESERVE_BYTES) == NULL);

	free(reserve);
	free_composite(&t);
}

/* The alignment and md-range inlines agree with the composite geometry. */
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

/* The pinned header bytes catch a field reorder that preserves sizeof. Only
 * deterministic fields are pinned: crc is derived and the epoch is wall-clock. */
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

/* A band without partition identity serializes its former reserved[32] region as
 * all-zero bytes — byte-identical to the pre-identity format — so superblocks
 * written before the fields existed validate and read back unchanged (all-zero
 * part_uuid = whole disk, no version bump). With an identity, the bytes land at
 * the pinned offsets, little-endian. */
static void
test_part_identity_compat(void)
{
	struct vbdev_tier t;
	struct tier_superblock sb;
	const uint8_t *p = (const uint8_t *)&sb;
	char hex[2 * TIER_PART_UUID_LEN + 1];
	uint32_t band, i;

	make_composite(&t);
	tier_sb_serialize(&t, TAILQ_FIRST(&t.bands), 42, 0, &sb);
	CHECK(tier_sb_valid(&sb));
	for (band = 0; band < 3; band++) {
		uint32_t off = 256 + band * 192 + 160;

		for (i = 0; i < 32; i++) {
			CHECK(p[off + i] == 0);
		}
	}

	{
		struct tier_band *b0 = TAILQ_FIRST(&t.bands);

		for (i = 0; i < TIER_PART_UUID_LEN; i++) {
			b0->part_uuid[i] = (uint8_t)(0xC0 + i);
		}
		b0->part_start_lba = 0x0123456789ABCDEFULL;
		b0->part_size_blocks = 0x1122334455667788ULL;
		tier_sb_serialize(&t, b0, 43, 0, &sb);
		CHECK(tier_sb_valid(&sb));
		CHECK(p[256 + 160] == 0xC0 && p[256 + 175] == 0xCF);
		CHECK(p[256 + 176] == 0xEF && p[256 + 183] == 0x01);	/* LE u64 */
		CHECK(p[256 + 184] == 0x88 && p[256 + 191] == 0x11);	/* LE u64 */
	}

	/* The hex renderer: empty for no identity, 32 lowercase chars otherwise. */
	{
		uint8_t zero[TIER_PART_UUID_LEN] = {0};
		uint8_t u[TIER_PART_UUID_LEN];

		CHECK(strcmp(tier_part_uuid_hex(zero, hex), "") == 0);
		for (i = 0; i < TIER_PART_UUID_LEN; i++) {
			u[i] = (uint8_t)(i * 0x11);
		}
		CHECK(strcmp(tier_part_uuid_hex(u, hex),
			     "00112233445566778899aabbccddeeff") == 0);
	}
	free_composite(&t);
}

/* ---- durability and fan-out termination contracts --------------------------- */

static void
persist_rc_cb(void *cb_arg, int rc)
{
	*(int *)cb_arg = rc;
}

/* A fan-out that writes zero superblock copies must not report rc == 0, which
 * callers read as "durably persisted". */
static void
test_m3_zero_copy_persist_not_durable(void)
{
	struct vbdev_tier t;
	struct tier_band *b;
	int rc = 12345;

	make_composite(&t);
	TAILQ_FOREACH(b, &t.bands, link) {
		b->state = TIER_BAND_DEGRADED;
	}
	CHECK(tier_sb_write_all(&t, persist_rc_cb, &rc) == 0);
	CHECK(rc == -ENODEV);
	CHECK(t.sb_write_inflight == false);
	free_composite(&t);
}

/* The ctx-calloc ENOMEM path is a fan-out termination, so it must still resolve
 * teardown deferred behind the fan-out. */
static void
test_m2_enomem_termination_resolves_deferred_teardown(void)
{
	struct vbdev_tier t;
	int rc = 12345;

	make_composite(&t);
	t.delete_pending = true;
	g_fanout_idle_calls = 0;
	/* calloc #1 = the pending-cb wrapper (pass), #2 = the write ctx (fail). */
	g_calloc_fail_countdown = 1;
	CHECK(tier_sb_write_all(&t, persist_rc_cb, &rc) == 0);
	g_calloc_fail_countdown = -1;
	CHECK(rc == -ENOMEM);
	CHECK(t.sb_write_inflight == false);
	CHECK(g_fanout_idle_calls == 1);
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
	test_part_identity_compat();
	test_m3_zero_copy_persist_not_durable();
	test_m2_enomem_termination_resolves_deferred_teardown();

	if (g_failures != 0) {
		fprintf(stderr, "test_tier_sb: %d FAILURE(S)\n", g_failures);
		return 1;
	}
	printf("test_tier_sb: all tests passed\n");
	return 0;
}
