/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Evariops. All rights reserved.
 *
 *   bdev_tier JSON-RPC surface.
 *
 *   The CSI control-plane (source of truth) replays, on agent startup:
 *     bdev_tier_create  -> bdev_tier_add_band (xN) -> bdev_tier_register
 *   reproducing the identical composite layout. Runtime ops: retire_band,
 *   get_bands, delete.
 */

#include "vbdev_tier.h"

#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"
#include "spdk/uuid.h"

/* ---- bdev_tier_create {name, md_num_blocks} ---------------------------------- */

struct rpc_tier_create {
	char		*name;
	uint64_t	md_num_blocks;
	uint64_t	cluster_blocks;	/* boundary alignment grain (blobstore cluster size in blocks) */
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
		free(req.name);	/* decode may have strdup'd name before a later field failed */
		return;
	}
	if (vbdev_tier_get_by_name(req.name) != NULL) {
		spdk_jsonrpc_send_error_response_fmt(request, -EEXIST, "tier '%s' already exists", req.name);
		free(req.name);
		return;
	}
	/* A composite without an md region silently disables the mirrored-metadata
	 * design: vbdev_tier_is_md_range() would never match. Refuse it. */
	if (req.md_num_blocks == 0) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "md_num_blocks must be > 0");
		free(req.name);
		return;
	}
	/* cluster_blocks is the alignment grain. 0 means alignment intentionally
	 * dormant; >= 2 is a real grain. 1 is neither: it silently no-ops tier_align_*
	 * AND the register alignment guard (t->cluster_blocks > 1), so an unaligned
	 * composite registers and later fails -EIO on a straddling blobstore cluster. */
	if (req.cluster_blocks == 1) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "cluster_blocks must be 0 (legacy, no alignment) or >= 2");
		free(req.name);
		return;
	}
	/* Reject an md_num_blocks so large that the cluster-align round-up
	 * (tier_align_up: v + cluster_blocks - 1) overflows u64 and wraps the md region
	 * to a tiny value, silently disabling the mirrored metadata. */
	if (req.cluster_blocks > 1 && req.md_num_blocks > UINT64_MAX - (req.cluster_blocks - 1)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "md_num_blocks too large (cluster-align overflow)");
		free(req.name);
		return;
	}
	/* cluster_blocks is stored as u64 on disk — no u32 cap to enforce. */
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
		free(req.name);	/* decode may have strdup'd name before a later field failed */
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
		free(req.name);	/* decode may have strdup'd name before a later field failed */
		return;
	}
	/* Audit this destructive op (tears down the whole composite). */
	spdk_jsonrpc_request_audit(request, "bdev_tier_delete", req.name);
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

/* The RPC acks only once the retirement is durably persisted to the surviving
 * bands' superblocks; on rc != 0 the caller retries the idempotent retire. */
static void
rpc_tier_retire_done(void *cb_arg, int rc)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc,
						     "retire_band not durable (retry): %s",
						     spdk_strerror(-rc));
		return;
	}
	spdk_jsonrpc_send_bool_response(request, true);
}

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
		free(req.name);	/* decode may have strdup'd name before a later field failed */
		return;
	}
	/* Audit this destructive op (evacuates + removes a band). */
	{
		char detail[128];

		snprintf(detail, sizeof(detail), "name=%s band_id=%u", req.name, req.band_id);
		spdk_jsonrpc_request_audit(request, "bdev_tier_retire_band", detail);
	}
	t = vbdev_tier_get_by_name(req.name);
	free(req.name);
	if (t == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, "tier not found");
		return;
	}
	rc = vbdev_tier_retire_band(t, req.band_id, rpc_tier_retire_done, request);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc, "retire_band failed: %s",
						     spdk_strerror(-rc));
		return;
	}
	/* Response sent from rpc_tier_retire_done after persistence. */
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

	if (spdk_json_decode_object(params, rpc_tier_name_decoders,
				    SPDK_COUNTOF(rpc_tier_name_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "invalid parameters");
		free(req.name);	/* decode may have strdup'd name before a later field failed */
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
		/* Geometry + state only. Fill accounting (used/capacity) is logical and
		 * lives in the blobstore, where the CSI derives it, so the composite does
		 * not emit a duplicate/always-zero copy. */
		spdk_json_write_object_begin(w);
		spdk_json_write_named_uint32(w, "band_id", b->band_id);
		spdk_json_write_named_uint32(w, "tier", b->tier);
		spdk_json_write_named_uint32(w, "state", b->state);
		spdk_json_write_named_string(w, "base_bdev_name", b->base_bdev_name);
		spdk_json_write_named_string(w, "wwn", b->wwn);
		spdk_json_write_named_string(w, "serial", b->serial);
		spdk_json_write_named_uint64(w, "lba_start", b->lba_start);
		spdk_json_write_named_uint64(w, "num_blocks", b->num_blocks);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("bdev_tier_get_bands", rpc_bdev_tier_get_bands, SPDK_RPC_RUNTIME)

/* ---- bdev_tier_assemble_band {name, base_bdev_name, band_id, tier, wwn?, serial?,
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

/* ---- bdev_tier_resync_md {name, band_id} — rebuild a DEGRADED md leg -------- */

static void
rpc_tier_resync_md_done(void *cb_arg, int rc)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc, "resync_md failed: %s",
						     spdk_strerror(-rc));
		return;
	}
	spdk_jsonrpc_send_bool_response(request, true);
}

static void
rpc_bdev_tier_resync_md(struct spdk_jsonrpc_request *request, const struct spdk_json_val *params)
{
	struct rpc_tier_retire req = {};
	struct vbdev_tier *t;
	int rc;

	if (spdk_json_decode_object(params, rpc_tier_retire_decoders,
				    SPDK_COUNTOF(rpc_tier_retire_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "invalid parameters");
		free(req.name);	/* decode may have strdup'd name before a later field failed */
		return;
	}
	/* Audit this op (rewrites redundancy state of the mirrored md region). */
	{
		char detail[128];

		snprintf(detail, sizeof(detail), "name=%s target_band_id=%u", req.name, req.band_id);
		spdk_jsonrpc_request_audit(request, "bdev_tier_resync_md", detail);
	}
	t = vbdev_tier_get_by_name(req.name);
	free(req.name);
	if (t == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, "tier not found");
		return;
	}
	rc = vbdev_tier_resync_md(t, req.band_id, rpc_tier_resync_md_done, request);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response_fmt(request, rc, "resync_md failed: %s",
						     spdk_strerror(-rc));
		return;
	}
	/* Response sent from rpc_tier_resync_md_done. */
}
SPDK_RPC_REGISTER("bdev_tier_resync_md", rpc_bdev_tier_resync_md, SPDK_RPC_RUNTIME)

/* ---- bdev_tier_read_sb {name=base_bdev} -> the on-disk superblock ----------- */

struct rpc_read_sb_ctx {
	struct spdk_jsonrpc_request	*request;
	struct spdk_bdev_desc		*desc;
};

static void
rpc_read_sb_event_cb(enum spdk_bdev_event_type type, struct spdk_bdev *bdev, void *ctx)
{
	/* The desc lives only for the duration of one async SB read and is closed in
	 * rpc_read_sb_done. A REMOVE during that window just delays the base bdev's
	 * unregister until the read completes (bounded); log it. */
	if (type == SPDK_BDEV_EVENT_REMOVE) {
		SPDK_WARNLOG("tier read_sb: '%s' removed mid-read; desc closes at read completion\n",
			     bdev->name);
	}
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
		char uuid_str[SPDK_UUID_STRING_LEN];

		spdk_json_write_named_bool(w, "valid", true);
		spdk_json_write_named_string(w, "composite_name", sb->composite_name);
		spdk_json_write_named_uint64(w, "seq", sb->seq);
		spdk_json_write_named_uint32(w, "version", sb->version);
		spdk_json_write_named_uint64(w, "created_epoch_sec", sb->created_epoch_sec);
		/* Expose the generation uuid so the CSI can fence stale disks. */
		spdk_uuid_fmt_lower(uuid_str, sizeof(uuid_str),
				    (const struct spdk_uuid *)sb->generation_uuid);
		spdk_json_write_named_string(w, "generation_uuid", uuid_str);
		spdk_json_write_named_uint64(w, "md_num_blocks", sb->md_num_blocks);
		spdk_json_write_named_uint64(w, "cluster_blocks", sb->cluster_blocks);
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
		free(req.name);	/* decode may have strdup'd name before a later field failed */
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

/* ---- evariops_get_capabilities — boot_id + schema versions + methods ------- */

extern char g_tier_boot_id[];	/* minted at module init (vbdev_tier.c) */

/* Candidate list of the fork RPC surface the control-plane probes for. It is NOT
 * emitted verbatim: each entry is filtered through the LIVE RPC registry
 * (spdk_rpc_get_method_state_mask), so methods[] reflects what this binary really
 * registers. The list scopes WHICH methods to report as fork capabilities; the
 * registry decides WHETHER each one is present. */
static const char *g_evariops_methods[] = {
	"bdev_tier_create", "bdev_tier_add_band", "bdev_tier_assemble_band",
	"bdev_tier_register", "bdev_tier_delete", "bdev_tier_retire_band",
	"bdev_tier_resync_md", "bdev_tier_get_bands", "bdev_tier_read_sb",
	"bdev_lvol_get_cluster_placement", "bdev_lvol_relocate_cluster",
	"bdev_lvol_relocate_clusters", "bdev_lvol_remap_cluster",
	"bdev_lvol_remap_clusters",
	"bdev_lvol_get_allocated_ranges",
	"bdev_raid_rebuild_ranges", "vbdev_nexus_enable_heat",
	"vbdev_nexus_disable_heat", "vbdev_nexus_get_heat",
	"nvmf_subsystem_pause", "nvmf_subsystem_resume",
	"bdev_cbt_create", "bdev_cbt_delete", "bdev_cbt_epoch_open",
	"bdev_cbt_epoch_freeze", "bdev_cbt_epoch_close", "bdev_cbt_epoch_invalidate",
	"bdev_cbt_epoch_get_dirty_ranges", "bdev_cbt_partial_rebuild",
	"bdev_cbt_start_rebuild", "bdev_cbt_get_rebuild_status",
	"bdev_cbt_cancel_rebuild", "bdev_cbt_reset",
	"evariops_get_capabilities",
};

static void
rpc_evariops_get_capabilities(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct spdk_json_write_ctx *w;
	size_t i;

	/* No params. A spurious body is tolerated (forward-compat with a probe that
	 * sends options), but an explicit non-object is rejected. */
	if (params != NULL && params->type != SPDK_JSON_VAL_OBJECT_BEGIN) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "no parameters expected");
		return;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	/* boot_id: a fresh value across polls means the target restarted — the CSI
	 * must then treat all volatile state (pauses, relocations, cbt epochs) as lost. */
	spdk_json_write_named_string(w, "boot_id", g_tier_boot_id);
	spdk_json_write_named_uint32(w, "tier_sb_version", TIER_SB_VERSION);
	spdk_json_write_named_uint32(w, "capabilities_schema", 1);
	spdk_json_write_named_bool(w, "single_reactor_assumed", true);
	spdk_json_write_named_array_begin(w, "methods");
	for (i = 0; i < SPDK_COUNTOF(g_evariops_methods); i++) {
		uint32_t state_mask;

		/* Emit only methods present in the LIVE registry. A method built in but
		 * missing from the candidate list is a false NEGATIVE, and harmless: the
		 * CSI falls back to its per-call -32601 probe. A false POSITIVE is not. */
		if (spdk_rpc_get_method_state_mask(g_evariops_methods[i], &state_mask) == 0) {
			spdk_json_write_string(w, g_evariops_methods[i]);
		}
	}
	spdk_json_write_array_end(w);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
}
SPDK_RPC_REGISTER("evariops_get_capabilities", rpc_evariops_get_capabilities, SPDK_RPC_RUNTIME)
