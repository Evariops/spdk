/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Evariops. All rights reserved.
 *
 *   bdev_tier JSON-RPC surface (SPEC-73A §9.1 / 73B C-OBS-2, C-MUT-2).
 *
 *   The CSI control-plane (source of truth in CRD) replays, on agent startup:
 *     bdev_tier_create  -> bdev_tier_add_band (xN) -> bdev_tier_register
 *   reproducing the identical composite layout. Runtime ops: retire_band,
 *   get_bands, delete.
 */

#include "vbdev_tier.h"

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"

/* ---- bdev_tier_create {name, md_num_blocks} ---------------------------------- */

struct rpc_tier_create {
	char		*name;
	uint64_t	md_num_blocks;
	uint64_t	cluster_blocks;	/* F1: boundary alignment grain (blobstore cluster size in blocks) */
};

static const struct spdk_json_object_decoder rpc_tier_create_decoders[] = {
	{"name", offsetof(struct rpc_tier_create, name), spdk_json_decode_string},
	{"md_num_blocks", offsetof(struct rpc_tier_create, md_num_blocks), spdk_json_decode_uint64},
	{"cluster_blocks", offsetof(struct rpc_tier_create, cluster_blocks), spdk_json_decode_uint64, true},
};

static void
rpc_bdev_tier_create(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_tier_create req = {};
	struct vbdev_tier *t;

	if (spdk_json_decode_object(params, rpc_tier_create_decoders,
				    SPDK_COUNTOF(rpc_tier_create_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "invalid parameters");
		return;
	}
	if (vbdev_tier_get_by_name(req.name) != NULL) {
		spdk_jsonrpc_send_error_response_fmt(request, -EEXIST, "tier '%s' already exists", req.name);
		free(req.name);
		return;
	}
	t = vbdev_tier_create(req.name, req.md_num_blocks, req.cluster_blocks);
	free(req.name);
	if (t == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENOMEM, "could not create tier");
		return;
	}
	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("bdev_tier_create", rpc_bdev_tier_create, SPDK_RPC_RUNTIME)

/* ---- bdev_tier_add_band {name, base_bdev_name, tier, wwn?, serial?} ---------- */

struct rpc_tier_add_band {
	char		*name;
	char		*base_bdev_name;
	uint32_t	tier;
	char		*wwn;
	char		*serial;
};

static const struct spdk_json_object_decoder rpc_tier_add_band_decoders[] = {
	{"name", offsetof(struct rpc_tier_add_band, name), spdk_json_decode_string},
	{"base_bdev_name", offsetof(struct rpc_tier_add_band, base_bdev_name), spdk_json_decode_string},
	{"tier", offsetof(struct rpc_tier_add_band, tier), spdk_json_decode_uint32},
	{"wwn", offsetof(struct rpc_tier_add_band, wwn), spdk_json_decode_string, true},
	{"serial", offsetof(struct rpc_tier_add_band, serial), spdk_json_decode_string, true},
};

static void
rpc_bdev_tier_add_band(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_tier_add_band req = {};
	struct vbdev_tier *t;
	uint32_t band_id = 0;
	int rc;

	if (spdk_json_decode_object(params, rpc_tier_add_band_decoders,
				    SPDK_COUNTOF(rpc_tier_add_band_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "invalid parameters");
		goto cleanup;
	}
	if (req.tier >= TIER_CLASS_MAX) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "invalid tier");
		goto cleanup;
	}
	t = vbdev_tier_get_by_name(req.name);
	if (t == NULL) {
		spdk_jsonrpc_send_error_response_fmt(request, -ENODEV, "tier '%s' not found", req.name);
		goto cleanup;
	}
	rc = vbdev_tier_add_band(t, req.base_bdev_name, (enum tier_class)req.tier, req.wwn, req.serial,
				 &band_id);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc, "add_band failed: %s", spdk_strerror(-rc));
		goto cleanup;
	}
	struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint32(w, "band_id", band_id);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free(req.name);
	free(req.base_bdev_name);
	free(req.wwn);
	free(req.serial);
}
SPDK_RPC_REGISTER("bdev_tier_add_band", rpc_bdev_tier_add_band, SPDK_RPC_RUNTIME)

/* ---- bdev_tier_register {name} ----------------------------------------------- */

struct rpc_tier_name {
	char	*name;
};

static const struct spdk_json_object_decoder rpc_tier_name_decoders[] = {
	{"name", offsetof(struct rpc_tier_name, name), spdk_json_decode_string},
};

static void
rpc_bdev_tier_register(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_tier_name req = {};
	struct vbdev_tier *t;
	int rc;

	if (spdk_json_decode_object(params, rpc_tier_name_decoders,
				    SPDK_COUNTOF(rpc_tier_name_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "invalid parameters");
		return;
	}
	t = vbdev_tier_get_by_name(req.name);
	free(req.name);
	if (t == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, "tier not found");
		return;
	}
	rc = vbdev_tier_register(t);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc, "register failed: %s", spdk_strerror(-rc));
		return;
	}
	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("bdev_tier_register", rpc_bdev_tier_register, SPDK_RPC_RUNTIME)

/* ---- bdev_tier_delete {name} ------------------------------------------------- */

static void
rpc_bdev_tier_delete(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_tier_name req = {};
	struct vbdev_tier *t;

	if (spdk_json_decode_object(params, rpc_tier_name_decoders,
				    SPDK_COUNTOF(rpc_tier_name_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "invalid parameters");
		return;
	}
	t = vbdev_tier_get_by_name(req.name);
	free(req.name);
	if (t == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, "tier not found");
		return;
	}
	vbdev_tier_delete(t);
	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("bdev_tier_delete", rpc_bdev_tier_delete, SPDK_RPC_RUNTIME)

/* ---- bdev_tier_retire_band {name, band_id} ---------------------------------- */

struct rpc_tier_retire {
	char		*name;
	uint32_t	band_id;
};

static const struct spdk_json_object_decoder rpc_tier_retire_decoders[] = {
	{"name", offsetof(struct rpc_tier_retire, name), spdk_json_decode_string},
	{"band_id", offsetof(struct rpc_tier_retire, band_id), spdk_json_decode_uint32},
};

static void
rpc_bdev_tier_retire_band(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_tier_retire req = {};
	struct vbdev_tier *t;
	int rc;

	if (spdk_json_decode_object(params, rpc_tier_retire_decoders,
				    SPDK_COUNTOF(rpc_tier_retire_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "invalid parameters");
		return;
	}
	t = vbdev_tier_get_by_name(req.name);
	free(req.name);
	if (t == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, "tier not found");
		return;
	}
	rc = vbdev_tier_retire_band(t, req.band_id);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc, "retire_band failed: %s",
						     spdk_strerror(-rc));
		return;
	}
	spdk_jsonrpc_send_bool_response(request, true);
}
SPDK_RPC_REGISTER("bdev_tier_retire_band", rpc_bdev_tier_retire_band, SPDK_RPC_RUNTIME)

/* ---- bdev_tier_get_bands {name} -> [{band_id, tier, state, lba_start, ...}] -- */

static void
rpc_bdev_tier_get_bands(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_tier_name req = {};
	struct vbdev_tier *t;
	struct tier_band *b;
	struct spdk_json_write_ctx *w;
	uint64_t capacity_blocks, used_blocks;

	if (spdk_json_decode_object(params, rpc_tier_name_decoders,
				    SPDK_COUNTOF(rpc_tier_name_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "invalid parameters");
		return;
	}
	t = vbdev_tier_get_by_name(req.name);
	free(req.name);
	if (t == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, "tier not found");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_array_begin(w, "bands");
	TAILQ_FOREACH(b, &t->bands, link) {
		capacity_blocks = b->num_blocks;
		/* used_blocks is tracked by the blobstore (allocator), not the composite;
		 * the CSI brain derives fill from get_cluster_placement (C-OBS-1). Here we
		 * expose geometry + state; capacity accounting is logical. */
		used_blocks = 0;
		spdk_json_write_object_begin(w);
		spdk_json_write_named_uint32(w, "band_id", b->band_id);
		spdk_json_write_named_uint32(w, "tier", b->tier);
		spdk_json_write_named_uint32(w, "state", b->state);
		spdk_json_write_named_string(w, "base_bdev_name", b->base_bdev_name);
		spdk_json_write_named_string(w, "wwn", b->wwn);
		spdk_json_write_named_string(w, "serial", b->serial);
		spdk_json_write_named_uint64(w, "lba_start", b->lba_start);
		spdk_json_write_named_uint64(w, "num_blocks", b->num_blocks);
		spdk_json_write_named_uint64(w, "capacity_blocks", capacity_blocks);
		spdk_json_write_named_uint64(w, "used_blocks", used_blocks);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("bdev_tier_get_bands", rpc_bdev_tier_get_bands, SPDK_RPC_RUNTIME)

/* ---- SPEC-73 A2: bdev_tier_assemble_band {name, base_bdev_name, band_id, tier, wwn?, serial?,
 *      lba_start, num_blocks, state, is_md} — place a band at explicit stored geometry ------------ */

struct rpc_tier_assemble {
	char		*name;
	char		*base_bdev_name;
	uint32_t	band_id;
	uint32_t	tier;
	char		*wwn;
	char		*serial;
	uint64_t	lba_start;
	uint64_t	num_blocks;
	uint32_t	state;
	bool		is_md;
};

static const struct spdk_json_object_decoder rpc_tier_assemble_decoders[] = {
	{"name", offsetof(struct rpc_tier_assemble, name), spdk_json_decode_string},
	{"base_bdev_name", offsetof(struct rpc_tier_assemble, base_bdev_name), spdk_json_decode_string},
	{"band_id", offsetof(struct rpc_tier_assemble, band_id), spdk_json_decode_uint32},
	{"tier", offsetof(struct rpc_tier_assemble, tier), spdk_json_decode_uint32},
	{"wwn", offsetof(struct rpc_tier_assemble, wwn), spdk_json_decode_string, true},
	{"serial", offsetof(struct rpc_tier_assemble, serial), spdk_json_decode_string, true},
	{"lba_start", offsetof(struct rpc_tier_assemble, lba_start), spdk_json_decode_uint64},
	{"num_blocks", offsetof(struct rpc_tier_assemble, num_blocks), spdk_json_decode_uint64},
	{"state", offsetof(struct rpc_tier_assemble, state), spdk_json_decode_uint32, true},
	{"is_md", offsetof(struct rpc_tier_assemble, is_md), spdk_json_decode_bool, true},
};

static void
rpc_bdev_tier_assemble_band(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_tier_assemble req = {};
	struct vbdev_tier *t;
	int rc;

	if (spdk_json_decode_object(params, rpc_tier_assemble_decoders,
				    SPDK_COUNTOF(rpc_tier_assemble_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "invalid parameters");
		goto cleanup;
	}
	if (req.tier >= TIER_CLASS_MAX) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "invalid tier");
		goto cleanup;
	}
	t = vbdev_tier_get_by_name(req.name);
	if (t == NULL) {
		spdk_jsonrpc_send_error_response_fmt(request, -ENODEV, "tier '%s' not found", req.name);
		goto cleanup;
	}
	rc = vbdev_tier_assemble_band(t, req.base_bdev_name, req.band_id, (enum tier_class)req.tier,
				      req.wwn, req.serial, req.lba_start, req.num_blocks,
				      (enum tier_band_state)req.state, req.is_md);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc, "assemble_band failed: %s", spdk_strerror(-rc));
		goto cleanup;
	}
	spdk_jsonrpc_send_bool_response(request, true);

cleanup:
	free(req.name);
	free(req.base_bdev_name);
	free(req.wwn);
	free(req.serial);
}
SPDK_RPC_REGISTER("bdev_tier_assemble_band", rpc_bdev_tier_assemble_band, SPDK_RPC_RUNTIME)

/* ---- SPEC-73 A2: bdev_tier_read_sb {name=base_bdev} -> the on-disk superblock (swap detection) ---- */

struct rpc_read_sb_ctx {
	struct spdk_jsonrpc_request	*request;
	struct spdk_bdev_desc		*desc;
};

static void
rpc_read_sb_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
	(void)type;
	(void)bdev;
	(void)ctx;
}

static void
rpc_read_sb_done(void *cb_arg, const struct tier_superblock *sb, int rc)
{
	struct rpc_read_sb_ctx *c = cb_arg;
	struct spdk_json_write_ctx *w;
	uint32_t i;

	w = spdk_jsonrpc_begin_result(c->request);
	spdk_json_write_object_begin(w);
	if (rc != 0 || sb == NULL) {
		spdk_json_write_named_bool(w, "valid", false);
	} else {
		spdk_json_write_named_bool(w, "valid", true);
		spdk_json_write_named_string(w, "composite_name", sb->composite_name);
		spdk_json_write_named_uint64(w, "seq", sb->seq);
		spdk_json_write_named_uint64(w, "md_num_blocks", sb->md_num_blocks);
		spdk_json_write_named_uint32(w, "cluster_blocks", sb->cluster_blocks);
		spdk_json_write_named_uint32(w, "md_mirror_a", sb->md_mirror_a);
		spdk_json_write_named_uint32(w, "md_mirror_b", sb->md_mirror_b);
		spdk_json_write_named_uint32(w, "num_bands", sb->num_bands);
		spdk_json_write_named_uint32(w, "this_band_id", sb->this_band_id);
		spdk_json_write_named_array_begin(w, "bands");
		for (i = 0; i < sb->num_bands && i < TIER_MAX_BANDS; i++) {
			spdk_json_write_object_begin(w);
			spdk_json_write_named_uint32(w, "band_id", sb->bands[i].band_id);
			spdk_json_write_named_uint32(w, "tier", sb->bands[i].tier);
			spdk_json_write_named_uint32(w, "state", sb->bands[i].state);
			spdk_json_write_named_uint64(w, "lba_start", sb->bands[i].lba_start);
			spdk_json_write_named_uint64(w, "num_blocks", sb->bands[i].num_blocks);
			spdk_json_write_named_string(w, "wwn", sb->bands[i].wwn);
			spdk_json_write_named_string(w, "serial", sb->bands[i].serial);
			spdk_json_write_object_end(w);
		}
		spdk_json_write_array_end(w);
	}
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(c->request, w);

	spdk_bdev_close(c->desc);
	free(c);
}

static void
rpc_bdev_tier_read_sb(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_tier_name req = {};
	struct rpc_read_sb_ctx *c;
	struct spdk_bdev_desc *desc = NULL;
	uint32_t blocklen;
	int rc;

	if (spdk_json_decode_object(params, rpc_tier_name_decoders,
				    SPDK_COUNTOF(rpc_tier_name_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS, "invalid parameters");
		return;
	}
	rc = spdk_bdev_open_ext(req.name, false, rpc_read_sb_event_cb, NULL, &desc);
	free(req.name);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc, "open failed: %s", spdk_strerror(-rc));
		return;
	}
	blocklen = spdk_bdev_get_block_size(spdk_bdev_desc_get_bdev(desc));
	c = calloc(1, sizeof(*c));
	if (c == NULL) {
		spdk_bdev_close(desc);
		spdk_jsonrpc_send_error_response(request, -ENOMEM, "oom");
		return;
	}
	c->request = request;
	c->desc = desc;
	rc = tier_sb_read_desc(desc, blocklen, rpc_read_sb_done, c);
	if (rc != 0) {
		spdk_bdev_close(desc);
		free(c);
		spdk_jsonrpc_send_error_response_fmt(request, rc, "read_sb failed: %s", spdk_strerror(-rc));
	}
}
SPDK_RPC_REGISTER("bdev_tier_read_sb", rpc_bdev_tier_read_sb, SPDK_RPC_RUNTIME)
