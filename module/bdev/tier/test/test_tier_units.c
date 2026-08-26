/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Evariops.
 *
 *   Host-compilable unit tests of the PRODUCTION pure helpers in vbdev_tier.h
 *   (not a copy): the part_uuid wire grammar and its hex round-trip, the range
 *   overlap predicate, the unit-identity admission helper shared by
 *   add_band/assemble_band, and the identity-tuple encoding validator. These
 *   are the guards whose drift would otherwise only surface in the label-gated
 *   container build.
 */

#include <stdio.h>
#include <string.h>

#include "../vbdev_tier.h"

static int g_failures;

#define CHECK(cond) do {							\
	if (!(cond)) {								\
		g_failures++;							\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);	\
	}									\
} while (0)

/* ---- tier_part_uuid_parse: the strict 32-lowercase-hex wire grammar --------- */

static void
test_part_uuid_parse_grammar(void)
{
	uint8_t out[TIER_PART_UUID_LEN];
	uint8_t expect[TIER_PART_UUID_LEN] = {
		0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
		0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78,
	};

	/* Absent identity: NULL and "" both parse to all-zero, rc 0. */
	memset(out, 0xff, sizeof(out));
	CHECK(tier_part_uuid_parse(NULL, out) == 0);
	CHECK(spdk_mem_all_zero(out, sizeof(out)));
	memset(out, 0xff, sizeof(out));
	CHECK(tier_part_uuid_parse("", out) == 0);
	CHECK(spdk_mem_all_zero(out, sizeof(out)));

	/* The nominal form. */
	CHECK(tier_part_uuid_parse("0123456789abcdef0f1e2d3c4b5a6978", out) == 0);
	CHECK(memcmp(out, expect, sizeof(out)) == 0);

	/* Length: 31 and 33 chars refuse. */
	CHECK(tier_part_uuid_parse("0123456789abcdef0f1e2d3c4b5a697", out) == -EINVAL);
	CHECK(tier_part_uuid_parse("0123456789abcdef0f1e2d3c4b5a69788", out) == -EINVAL);

	/* Upper case refuses: the wire value is the byte-exact mirror of
	 * tier_part_uuid_hex, which only ever emits lower case. */
	CHECK(tier_part_uuid_parse("0123456789ABCDEF0f1e2d3c4b5a6978", out) == -EINVAL);

	/* Dashed GPT form refuses (the control plane canonicalizes upstream). */
	CHECK(tier_part_uuid_parse("01234567-89ab-cdef-0f1e-2d3c4b5a6978", out) == -EINVAL);

	/* Non-hex refuses. */
	CHECK(tier_part_uuid_parse("g123456789abcdef0f1e2d3c4b5a6978", out) == -EINVAL);

	/* A refused parse still leaves out zeroed (memset before scan). */
	CHECK(spdk_mem_all_zero(out, sizeof(out)));
}

static void
test_part_uuid_hex_roundtrip(void)
{
	uint8_t in[TIER_PART_UUID_LEN] = {
		0xde, 0xad, 0xbe, 0xef, 0x00, 0x11, 0x22, 0x33,
		0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
	};
	uint8_t zero[TIER_PART_UUID_LEN] = {0};
	uint8_t back[TIER_PART_UUID_LEN];
	char hex[2 * TIER_PART_UUID_LEN + 1];

	/* hex(parse(hex(x))) == hex(x): the two sides share one grammar. */
	tier_part_uuid_hex(in, hex);
	CHECK(strcmp(hex, "deadbeef00112233445566778899aabb") == 0);
	CHECK(tier_part_uuid_parse(hex, back) == 0);
	CHECK(memcmp(in, back, sizeof(in)) == 0);

	/* All-zero renders as the EMPTY string (wire form of "no identity"), and
	 * the empty string parses back to all-zero: the round trip closes. */
	tier_part_uuid_hex(zero, hex);
	CHECK(hex[0] == '\0');
	memset(back, 0xff, sizeof(back));
	CHECK(tier_part_uuid_parse(hex, back) == 0);
	CHECK(spdk_mem_all_zero(back, sizeof(back)));
}

/* ---- tier_ranges_overlap ---------------------------------------------------- */

static void
test_ranges_overlap(void)
{
	/* Disjoint. */
	CHECK(!tier_ranges_overlap(0, 10, 20, 10));
	CHECK(!tier_ranges_overlap(20, 10, 0, 10));
	/* Adjacent (half-open ranges: [0,10) and [10,10) do not intersect). */
	CHECK(!tier_ranges_overlap(0, 10, 10, 10));
	CHECK(!tier_ranges_overlap(10, 10, 0, 10));
	/* Partial overlap, both directions. */
	CHECK(tier_ranges_overlap(0, 11, 10, 10));
	CHECK(tier_ranges_overlap(10, 10, 0, 11));
	/* Containment and identity. */
	CHECK(tier_ranges_overlap(0, 100, 40, 10));
	CHECK(tier_ranges_overlap(40, 10, 0, 100));
	CHECK(tier_ranges_overlap(40, 10, 40, 10));
	/* First-block collision at the boundary. */
	CHECK(tier_ranges_overlap(9, 1, 0, 10));
}

/* ---- tier_band_identity_conflict: one unit, one band ------------------------ */

static void
test_identity_conflict(void)
{
	struct vbdev_tier t;
	struct tier_band b0, b1;
	uint8_t uuid_a[TIER_PART_UUID_LEN] = {0xaa, 0x01};
	uint8_t uuid_b[TIER_PART_UUID_LEN] = {0xbb, 0x02};
	uint8_t zero[TIER_PART_UUID_LEN] = {0};

	memset(&t, 0, sizeof(t));
	memset(&b0, 0, sizeof(b0));
	memset(&b1, 0, sizeof(b1));
	TAILQ_INIT(&t.bands);

	/* Empty composite: nothing conflicts. */
	CHECK(tier_band_identity_conflict(&t, "wwn-0", uuid_a) == NULL);

	snprintf(b0.wwn, sizeof(b0.wwn), "%s", "wwn-0");
	b0.band_id = 0;
	TAILQ_INSERT_TAIL(&t.bands, &b0, link);
	memcpy(b1.part_uuid, uuid_a, sizeof(b1.part_uuid));
	b1.band_id = 1;
	TAILQ_INSERT_TAIL(&t.bands, &b1, link);

	/* Duplicate wwn is found; a fresh wwn is not. */
	CHECK(tier_band_identity_conflict(&t, "wwn-0", zero) == &b0);
	CHECK(tier_band_identity_conflict(&t, "wwn-9", zero) == NULL);

	/* Duplicate part_uuid is found — the partition case, where sibling
	 * partitions share the parent's wwn so the uuid is the ONLY identity. */
	CHECK(tier_band_identity_conflict(&t, NULL, uuid_a) == &b1);
	CHECK(tier_band_identity_conflict(&t, NULL, uuid_b) == NULL);

	/* No identity at all (empty wwn + zero uuid) never conflicts: whole-disk
	 * bands without a wwn are admitted side by side. */
	CHECK(tier_band_identity_conflict(&t, NULL, zero) == NULL);
	CHECK(tier_band_identity_conflict(&t, "", zero) == NULL);

	/* An all-zero STORED uuid is never matched by an incoming zero uuid (b0
	 * stores zero; the zero probe above already proved it), and a stored
	 * uuid is matched even when the incoming wwn is fresh. */
	CHECK(tier_band_identity_conflict(&t, "wwn-9", uuid_a) == &b1);
}

/* ---- tier_unit_identity_validate: exactly two encodings --------------------- */

static void
test_identity_tuple_validate(void)
{
	uint8_t uuid[TIER_PART_UUID_LEN] = {0x01};
	uint8_t zero[TIER_PART_UUID_LEN] = {0};

	/* Whole disk: all-zero uuid + 0/0 geometry. */
	CHECK(tier_unit_identity_validate(zero, 0, 0) == 0);
	/* Partition: non-zero uuid + real geometry. */
	CHECK(tier_unit_identity_validate(uuid, 2048, 1048576) == 0);
	/* A partition may legitimately start at LBA 0 of its parent window as long
	 * as it has a size. */
	CHECK(tier_unit_identity_validate(uuid, 0, 1048576) == 0);

	/* Geometry without identity: the tuple the contract declares impossible. */
	CHECK(tier_unit_identity_validate(zero, 2048, 1048576) == -EINVAL);
	CHECK(tier_unit_identity_validate(zero, 0, 1048576) == -EINVAL);
	CHECK(tier_unit_identity_validate(zero, 2048, 0) == -EINVAL);
	/* Identity without a size: a zero-block partition identifies nothing. */
	CHECK(tier_unit_identity_validate(uuid, 2048, 0) == -EINVAL);
	CHECK(tier_unit_identity_validate(uuid, 0, 0) == -EINVAL);
}

int
main(void)
{
	test_part_uuid_parse_grammar();
	test_part_uuid_hex_roundtrip();
	test_ranges_overlap();
	test_identity_conflict();
	test_identity_tuple_validate();

	if (g_failures != 0) {
		fprintf(stderr, "test_tier_units: %d FAILURE(S)\n", g_failures);
		return 1;
	}
	printf("test_tier_units: all tests passed\n");
	return 0;
}
