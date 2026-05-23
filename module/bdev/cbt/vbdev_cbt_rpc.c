/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (c) 2026 Evariops.
 *   All rights reserved.
 */

/*
 * JSON-RPC handlers for the CBT vbdev module.
 *
 * RPCs:
 *   bdev_cbt_create          — create CBT wrapper
 *   bdev_cbt_delete          — delete CBT wrapper
 *   bdev_cbt_epoch_open      — open a tracking epoch
 *   bdev_cbt_epoch_freeze    — freeze bitmap for reading
 *   bdev_cbt_epoch_close     — close epoch after rebuild
 *   bdev_cbt_epoch_invalidate— invalidate epoch (fallback)
 *   bdev_cbt_epoch_get_dirty_ranges — read frozen dirty ranges
 *   bdev_cbt_epoch_list      — list active epochs
 *   bdev_cbt_get_dirty_ranges— legacy alias
 *   bdev_cbt_start_tracking  — legacy alias
 *   bdev_cbt_stop_tracking   — legacy alias
 *   bdev_cbt_reset           — reset bitmap
 */

#include "vbdev_cbt_internal.h"
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/log.h"

/* ================================================================== */
/* bdev_cbt_create                                                    */
/* ================================================================== */

struct rpc_bdev_cbt_create {
	char     *base_bdev_name;
	char     *name;
	uint32_t  chunk_size_kb;
};

static void
free_rpc_bdev_cbt_create(struct rpc_bdev_cbt_create *r)
{
	free(r->base_bdev_name);
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_cbt_create_decoders[] = {
	{"base_bdev_name", offsetof(struct rpc_bdev_cbt_create, base_bdev_name), spdk_json_decode_string},
	{"name",           offsetof(struct rpc_bdev_cbt_create, name),           spdk_json_decode_string},
	{"chunk_size_kb",  offsetof(struct rpc_bdev_cbt_create, chunk_size_kb),  spdk_json_decode_uint32, true},
};

static void
rpc_bdev_cbt_create(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_bdev_cbt_create req = {NULL};
	struct spdk_json_write_ctx *w;
	int rc;

	if (spdk_json_decode_object(params, rpc_bdev_cbt_create_decoders,
				    SPDK_COUNTOF(rpc_bdev_cbt_create_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to decode parameters");
		goto cleanup;
	}

	/* Validate chunk_size_kb bounds. */
	if (req.chunk_size_kb != 0 &&
	    (req.chunk_size_kb < CBT_CHUNK_SIZE_MIN_KB ||
	     req.chunk_size_kb > CBT_CHUNK_SIZE_MAX_KB)) {
		spdk_jsonrpc_send_error_response_fmt(request, -EINVAL,
			"chunk_size_kb must be between %u and %u (got %u)",
			CBT_CHUNK_SIZE_MIN_KB, CBT_CHUNK_SIZE_MAX_KB, req.chunk_size_kb);
		goto cleanup;
	}

	rc = bdev_cbt_create_disk(req.base_bdev_name, req.name, req.chunk_size_kb);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_bdev_cbt_create(&req);
}
SPDK_RPC_REGISTER("bdev_cbt_create", rpc_bdev_cbt_create, SPDK_RPC_RUNTIME)

/* ================================================================== */
/* bdev_cbt_delete                                                    */
/* ================================================================== */

struct rpc_bdev_cbt_delete {
	char *name;
};

static void
free_rpc_bdev_cbt_delete(struct rpc_bdev_cbt_delete *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_bdev_cbt_delete_decoders[] = {
	{"name", offsetof(struct rpc_bdev_cbt_delete, name), spdk_json_decode_string},
};

static void
rpc_bdev_cbt_delete_cb(void *cb_arg, int bdeverrno)
{
	struct spdk_jsonrpc_request *request = cb_arg;

	if (bdeverrno == 0) {
		struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);
		spdk_json_write_bool(w, true);
		spdk_jsonrpc_end_result(request, w);
	} else {
		spdk_jsonrpc_send_error_response(request, bdeverrno,
						 spdk_strerror(-bdeverrno));
	}
}

static void
rpc_bdev_cbt_delete(struct spdk_jsonrpc_request *request,
		    const struct spdk_json_val *params)
{
	struct rpc_bdev_cbt_delete req = {NULL};

	if (spdk_json_decode_object(params, rpc_bdev_cbt_delete_decoders,
				    SPDK_COUNTOF(rpc_bdev_cbt_delete_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to decode parameters");
		free_rpc_bdev_cbt_delete(&req);
		return;
	}

	bdev_cbt_delete_disk(req.name, rpc_bdev_cbt_delete_cb, request);
	free_rpc_bdev_cbt_delete(&req);
}
SPDK_RPC_REGISTER("bdev_cbt_delete", rpc_bdev_cbt_delete, SPDK_RPC_RUNTIME)

/* ================================================================== */
/* bdev_cbt_epoch_open                                                */
/* ================================================================== */

struct rpc_epoch_open {
	char     *name;
	char     *epoch_id;
	char     *stale_backend_id;
	uint64_t  generation;
};

static void
free_rpc_epoch_open(struct rpc_epoch_open *r)
{
	free(r->name);
	free(r->epoch_id);
	free(r->stale_backend_id);
}

static const struct spdk_json_object_decoder rpc_epoch_open_decoders[] = {
	{"name",             offsetof(struct rpc_epoch_open, name),             spdk_json_decode_string},
	{"epoch_id",         offsetof(struct rpc_epoch_open, epoch_id),         spdk_json_decode_string},
	{"stale_backend_id", offsetof(struct rpc_epoch_open, stale_backend_id), spdk_json_decode_string},
	{"generation",       offsetof(struct rpc_epoch_open, generation),       spdk_json_decode_uint64},
};

static void
rpc_bdev_cbt_epoch_open(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_epoch_open req = {NULL};
	int rc;

	if (spdk_json_decode_object(params, rpc_epoch_open_decoders,
				    SPDK_COUNTOF(rpc_epoch_open_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to decode parameters");
		goto cleanup;
	}

	rc = bdev_cbt_epoch_open(req.name, req.epoch_id, req.stale_backend_id,
				 req.generation);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_epoch_open(&req);
}
SPDK_RPC_REGISTER("bdev_cbt_epoch_open", rpc_bdev_cbt_epoch_open, SPDK_RPC_RUNTIME)

/* ================================================================== */
/* bdev_cbt_epoch_freeze                                              */
/* ================================================================== */

struct rpc_epoch_simple {
	char *name;
	char *epoch_id;
};

static void
free_rpc_epoch_simple(struct rpc_epoch_simple *r)
{
	free(r->name);
	free(r->epoch_id);
}

static const struct spdk_json_object_decoder rpc_epoch_simple_decoders[] = {
	{"name",     offsetof(struct rpc_epoch_simple, name),     spdk_json_decode_string},
	{"epoch_id", offsetof(struct rpc_epoch_simple, epoch_id), spdk_json_decode_string},
};

static void
rpc_bdev_cbt_epoch_freeze(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_epoch_simple req = {NULL};
	int rc;

	if (spdk_json_decode_object(params, rpc_epoch_simple_decoders,
				    SPDK_COUNTOF(rpc_epoch_simple_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to decode parameters");
		goto cleanup;
	}

	rc = bdev_cbt_epoch_freeze(req.name, req.epoch_id);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_epoch_simple(&req);
}
SPDK_RPC_REGISTER("bdev_cbt_epoch_freeze", rpc_bdev_cbt_epoch_freeze, SPDK_RPC_RUNTIME)

/* ================================================================== */
/* bdev_cbt_epoch_close                                               */
/* ================================================================== */

static void
rpc_bdev_cbt_epoch_close(struct spdk_jsonrpc_request *request,
			  const struct spdk_json_val *params)
{
	struct rpc_epoch_simple req = {NULL};
	int rc;

	if (spdk_json_decode_object(params, rpc_epoch_simple_decoders,
				    SPDK_COUNTOF(rpc_epoch_simple_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to decode parameters");
		goto cleanup;
	}

	rc = bdev_cbt_epoch_close(req.name, req.epoch_id);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_epoch_simple(&req);
}
SPDK_RPC_REGISTER("bdev_cbt_epoch_close", rpc_bdev_cbt_epoch_close, SPDK_RPC_RUNTIME)

/* ================================================================== */
/* bdev_cbt_epoch_invalidate                                          */
/* ================================================================== */

static void
rpc_bdev_cbt_epoch_invalidate(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_epoch_simple req = {NULL};
	int rc;

	if (spdk_json_decode_object(params, rpc_epoch_simple_decoders,
				    SPDK_COUNTOF(rpc_epoch_simple_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to decode parameters");
		goto cleanup;
	}

	rc = bdev_cbt_epoch_invalidate(req.name, req.epoch_id);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_epoch_simple(&req);
}
SPDK_RPC_REGISTER("bdev_cbt_epoch_invalidate", rpc_bdev_cbt_epoch_invalidate, SPDK_RPC_RUNTIME)

/* ================================================================== */
/* bdev_cbt_epoch_get_dirty_ranges                                    */
/* ================================================================== */

struct rpc_epoch_get_dirty {
	char     *name;
	char     *epoch_id;
	uint32_t  max_ranges;
};

static void
free_rpc_epoch_get_dirty(struct rpc_epoch_get_dirty *r)
{
	free(r->name);
	free(r->epoch_id);
}

static const struct spdk_json_object_decoder rpc_epoch_get_dirty_decoders[] = {
	{"name",       offsetof(struct rpc_epoch_get_dirty, name),       spdk_json_decode_string},
	{"epoch_id",   offsetof(struct rpc_epoch_get_dirty, epoch_id),   spdk_json_decode_string},
	{"max_ranges", offsetof(struct rpc_epoch_get_dirty, max_ranges), spdk_json_decode_uint32, true},
};

static void
rpc_bdev_cbt_epoch_get_dirty_ranges(struct spdk_jsonrpc_request *request,
				    const struct spdk_json_val *params)
{
	struct rpc_epoch_get_dirty req = {NULL};
	struct cbt_dirty_range    *ranges = NULL;
	uint32_t                   count = 0;
	uint64_t                   dirty_chunks = 0, total_chunks = 0;
	uint32_t                   chunk_size_kb = 0;
	bool                       truncated = false;
	int rc;

	if (spdk_json_decode_object(params, rpc_epoch_get_dirty_decoders,
				    SPDK_COUNTOF(rpc_epoch_get_dirty_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to decode parameters");
		goto cleanup;
	}

	/* Bound max_ranges to prevent excessive allocation. */
	if (req.max_ranges > CBT_MAX_RANGES_LIMIT) {
		req.max_ranges = CBT_MAX_RANGES_LIMIT;
	}

	rc = bdev_cbt_epoch_get_dirty_ranges(req.name, req.epoch_id,
					     req.max_ranges,
					     &ranges, &count,
					     &dirty_chunks, &total_chunks,
					     &chunk_size_kb, &truncated);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);

	spdk_json_write_named_uint64(w, "dirty_chunks", dirty_chunks);
	spdk_json_write_named_uint64(w, "total_chunks", total_chunks);
	spdk_json_write_named_uint32(w, "chunk_size_kb", chunk_size_kb);
	spdk_json_write_named_bool(w, "truncated", truncated);

	spdk_json_write_named_array_begin(w, "ranges");
	for (uint32_t i = 0; i < count; i++) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_uint64(w, "offset_blocks", ranges[i].offset_blocks);
		spdk_json_write_named_uint64(w, "length_blocks", ranges[i].length_blocks);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free(ranges);
	free_rpc_epoch_get_dirty(&req);
}
SPDK_RPC_REGISTER("bdev_cbt_epoch_get_dirty_ranges", rpc_bdev_cbt_epoch_get_dirty_ranges, SPDK_RPC_RUNTIME)

/* ================================================================== */
/* bdev_cbt_epoch_list                                                */
/* ================================================================== */

struct rpc_epoch_list {
	char *name;
};

static void
free_rpc_epoch_list(struct rpc_epoch_list *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_epoch_list_decoders[] = {
	{"name", offsetof(struct rpc_epoch_list, name), spdk_json_decode_string},
};

static const char *
epoch_state_str(enum cbt_epoch_state state)
{
	switch (state) {
	case CBT_EPOCH_OPEN:        return "open";
	case CBT_EPOCH_FROZEN:      return "frozen";
	case CBT_EPOCH_REBUILDING:  return "rebuilding";
	case CBT_EPOCH_COMPLETED:   return "completed";
	case CBT_EPOCH_INVALID:     return "invalid";
	default:                    return "unknown";
	}
}

static void
rpc_bdev_cbt_epoch_list(struct spdk_jsonrpc_request *request,
			const struct spdk_json_val *params)
{
	struct rpc_epoch_list req = {NULL};
	struct vbdev_cbt     *cbt;
	struct cbt_epoch     *ep;

	if (spdk_json_decode_object(params, rpc_epoch_list_decoders,
				    SPDK_COUNTOF(rpc_epoch_list_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to decode parameters");
		free_rpc_epoch_list(&req);
		return;
	}

	cbt = cbt_find_by_name(req.name);
	if (!cbt) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, "CBT bdev not found");
		free_rpc_epoch_list(&req);
		return;
	}

	/* Compute popcount once to avoid redundant O(bitmap) scans. */
	uint64_t dirty_chunks = cbt_popcount_bitmap(cbt);
	double dirty_ratio = (cbt->bitmap_size_bits > 0)
		? (double)dirty_chunks / (double)cbt->bitmap_size_bits
		: 0.0;
	uint64_t writes_tracked = __atomic_load_n(&cbt->total_writes_tracked, __ATOMIC_RELAXED);

	struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);

	spdk_json_write_named_uint64(w, "dirty_chunks", dirty_chunks);
	spdk_json_write_named_uint64(w, "total_chunks", cbt->bitmap_size_bits);
	spdk_json_write_named_double(w, "dirty_ratio", dirty_ratio);
	spdk_json_write_named_uint64(w, "total_writes_tracked", writes_tracked);
	spdk_json_write_named_bool(w, "healthy_clear_suspended", cbt->healthy_clear_suspended);
	spdk_json_write_named_bool(w, "backends_healthy", cbt->backends_healthy);

	spdk_json_write_named_array_begin(w, "epochs");
	TAILQ_FOREACH(ep, &cbt->epochs, link) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_string(w, "epoch_id", ep->epoch_id);
		spdk_json_write_named_string(w, "stale_backend_id", ep->stale_backend_id);
		spdk_json_write_named_uint64(w, "generation", ep->generation);
		spdk_json_write_named_string(w, "state", epoch_state_str(ep->state));
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

	free_rpc_epoch_list(&req);
}
SPDK_RPC_REGISTER("bdev_cbt_epoch_list", rpc_bdev_cbt_epoch_list, SPDK_RPC_RUNTIME)

/* ================================================================== */
/* Legacy RPCs                                                        */
/* ================================================================== */

/* bdev_cbt_start_tracking */
struct rpc_cbt_name_only {
	char *name;
};

static void
free_rpc_cbt_name_only(struct rpc_cbt_name_only *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_cbt_name_only_decoders[] = {
	{"name", offsetof(struct rpc_cbt_name_only, name), spdk_json_decode_string},
};

static void
rpc_bdev_cbt_start_tracking(struct spdk_jsonrpc_request *request,
			    const struct spdk_json_val *params)
{
	struct rpc_cbt_name_only req = {NULL};
	int rc;

	if (spdk_json_decode_object(params, rpc_cbt_name_only_decoders,
				    SPDK_COUNTOF(rpc_cbt_name_only_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to decode parameters");
		goto cleanup;
	}

	rc = bdev_cbt_start_tracking(req.name);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_cbt_name_only(&req);
}
SPDK_RPC_REGISTER("bdev_cbt_start_tracking", rpc_bdev_cbt_start_tracking, SPDK_RPC_RUNTIME)

/* bdev_cbt_stop_tracking */
static void
rpc_bdev_cbt_stop_tracking(struct spdk_jsonrpc_request *request,
			   const struct spdk_json_val *params)
{
	struct rpc_cbt_name_only req = {NULL};
	int rc;

	if (spdk_json_decode_object(params, rpc_cbt_name_only_decoders,
				    SPDK_COUNTOF(rpc_cbt_name_only_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to decode parameters");
		goto cleanup;
	}

	rc = bdev_cbt_stop_tracking(req.name);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_cbt_name_only(&req);
}
SPDK_RPC_REGISTER("bdev_cbt_stop_tracking", rpc_bdev_cbt_stop_tracking, SPDK_RPC_RUNTIME)

/* bdev_cbt_get_dirty_ranges */
struct rpc_cbt_get_dirty_legacy {
	char     *name;
	uint32_t  max_ranges;
};

static void
free_rpc_cbt_get_dirty_legacy(struct rpc_cbt_get_dirty_legacy *r)
{
	free(r->name);
}

static const struct spdk_json_object_decoder rpc_cbt_get_dirty_legacy_decoders[] = {
	{"name",       offsetof(struct rpc_cbt_get_dirty_legacy, name),       spdk_json_decode_string},
	{"max_ranges", offsetof(struct rpc_cbt_get_dirty_legacy, max_ranges), spdk_json_decode_uint32, true},
};

static void
rpc_bdev_cbt_get_dirty_ranges(struct spdk_jsonrpc_request *request,
			      const struct spdk_json_val *params)
{
	struct rpc_cbt_get_dirty_legacy req = {NULL};
	struct cbt_dirty_range *ranges = NULL;
	uint32_t count = 0;
	uint64_t dirty_chunks = 0, total_chunks = 0;
	uint32_t chunk_size_kb = 0;
	bool truncated = false;
	int rc;

	if (spdk_json_decode_object(params, rpc_cbt_get_dirty_legacy_decoders,
				    SPDK_COUNTOF(rpc_cbt_get_dirty_legacy_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to decode parameters");
		goto cleanup;
	}

	if (req.max_ranges > CBT_MAX_RANGES_LIMIT) {
		req.max_ranges = CBT_MAX_RANGES_LIMIT;
	}

	rc = bdev_cbt_get_dirty_ranges(req.name, req.max_ranges,
				       &ranges, &count,
				       &dirty_chunks, &total_chunks,
				       &chunk_size_kb, &truncated);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_uint64(w, "dirty_chunks", dirty_chunks);
	spdk_json_write_named_uint64(w, "total_chunks", total_chunks);
	spdk_json_write_named_uint32(w, "chunk_size_kb", chunk_size_kb);
	spdk_json_write_named_bool(w, "truncated", truncated);

	spdk_json_write_named_array_begin(w, "ranges");
	for (uint32_t i = 0; i < count; i++) {
		spdk_json_write_object_begin(w);
		spdk_json_write_named_uint64(w, "offset_blocks", ranges[i].offset_blocks);
		spdk_json_write_named_uint64(w, "length_blocks", ranges[i].length_blocks);
		spdk_json_write_object_end(w);
	}
	spdk_json_write_array_end(w);

	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free(ranges);
	free_rpc_cbt_get_dirty_legacy(&req);
}
SPDK_RPC_REGISTER("bdev_cbt_get_dirty_ranges", rpc_bdev_cbt_get_dirty_ranges, SPDK_RPC_RUNTIME)

/* bdev_cbt_reset */
static void
rpc_bdev_cbt_reset(struct spdk_jsonrpc_request *request,
		   const struct spdk_json_val *params)
{
	struct rpc_cbt_name_only req = {NULL};
	int rc;

	if (spdk_json_decode_object(params, rpc_cbt_name_only_decoders,
				    SPDK_COUNTOF(rpc_cbt_name_only_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INTERNAL_ERROR,
						 "Failed to decode parameters");
		goto cleanup;
	}

	rc = bdev_cbt_reset(req.name);
	if (rc != 0) {
		spdk_jsonrpc_send_error_response(request, rc, spdk_strerror(-rc));
		goto cleanup;
	}

	struct spdk_json_write_ctx *w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_bool(w, true);
	spdk_jsonrpc_end_result(request, w);

cleanup:
	free_rpc_cbt_name_only(&req);
}
SPDK_RPC_REGISTER("bdev_cbt_reset", rpc_bdev_cbt_reset, SPDK_RPC_RUNTIME)
