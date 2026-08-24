# SPEC-60A — `bdev_lvol_get_allocated_ranges` JSON-RPC

| Field | Value |
|:------|:-------|
| **Target repository** | **`Evariops/spdk`** (SPDK container-image build: upstream-from-source + out-of-tree modules + patches). This document is authored in `spdk-csi` for review and is to be ported to `Evariops/spdk` (e.g. `docs/SPEC-allocated-ranges.md`) when implemented. |
| **Companion of** | `spdk-csi` **SPEC-60** (Snapshot Consumption & Backup Integration — Tier 2) — sole consumer initially |
| **Upstream base** | `spdk/spdk` v26.01 (per `images/spdk/Dockerfile` `ARG SPDK_VERSION=v26.01`) |
| **Delivery vehicle** | `patches/0002-bdev-lvol-add-get-allocated-ranges-rpc.patch` (same mechanism as `patches/0001-raid-add-skip_rebuild-parameter.patch`) |
| **Priority** | Medium-High — blocks SPEC-60 T2-G1/T2-G2 (exact allocated layout, true snapshot deltas) |

---

## 1. Motivation

SPDK's blobstore tracks, per blob and in memory, exactly which clusters are allocated — this is the native changed-block-tracking for thin snapshots (each CoW layer's allocated clusters are precisely the blocks written since its parent). The public C API exposes this by offset:

- `spdk_blob_get_next_allocated_io_unit` / `spdk_blob_get_next_unallocated_io_unit` (`lib/blob/blobstore.c:6237-6247`, exported in `lib/blob/spdk_blob.map:25-26`) — a pure in-memory walk (`blob_find_io_unit`, `:6221`) that steps by **cluster boundary**, issues no I/O, and already powers `SEEK_DATA`/`SEEK_HOLE` on lvol bdevs (`module/bdev/lvol/vbdev_lvol.c:854-903`).

**No JSON-RPC surfaces these offsets.** `bdev_get_bdevs` emits only the *count* (`num_allocated_clusters`, `vbdev_lvol.c:761`). Consumers behind the RPC socket (the spdk-csi target-agent) therefore cannot learn the allocation *layout*, which blocks:

- CSI `SnapshotMetadata` / KEP-3314 (`GetMetadataAllocated`: exact extents of a snapshot; `GetMetadataDelta`: extents of the layers between two snapshots) → true incremental backups via Velero/Kasten;
- any future consumer needing sparse-aware copy (smarter rebuild seeding, migration pre-copy).

**Prior art:** Longhorn's v2 data engine carries the equivalent out-of-tree (`bdev_lvol_get_fragmap` — a base64 *bitmap* per range; see longhorn/longhorn enhancement `20230809-support-backup-and-restore-for-volumes-with-v2-data-engine` and `longhorn-spdk-engine/pkg/spdk/client/basic.go`). We deliberately diverge on the wire format (§2.4): merged **extents** instead of a bitmap.

### Non-goals

- **No ancestor/chain merging server-side.** The RPC reports **one blob layer**. Chain composition (union over ancestors, delta between two snapshots) is the consumer's job (SPEC-60 §3.4) — it keeps this C surface minimal, stateless, and per-call bounded.
- **No `bdev_lvol_get_changed_ranges(base, target)`.** Same rationale: composition over primitives.
- **No bitmap output / Longhorn API compatibility** (§2.4).
- **No blobstore changes** — stock exported APIs only.

---

## 2. API specification

### 2.1 Request

```json
{
  "jsonrpc": "2.0",
  "method": "bdev_lvol_get_allocated_ranges",
  "id": 1,
  "params": {
    "name": "lvs0/snap_a1b2c3",
    "offset_bytes": 0,
    "max_ranges": 4096
  }
}
```

| Param | Type | Required | Semantics |
|:------|:-----|:--------:|:----------|
| `name` | string | ✅ | lvol bdev name — alias (`lvs/name`) or UUID, resolved like every other lvol RPC (`spdk_bdev_get_by_name` → `vbdev_lvol_get_from_bdev`, cf. `vbdev_lvol_rpc.c:441-448`) |
| `offset_bytes` | uint64 | ◻ (default 0) | resume point; rounded **down** to the containing io_unit by the implementation |
| `max_ranges` | uint32 | ◻ (default 4096, max 65536) | bound on `ranges[]` entries in this response — caps both response size and time-on-thread |

### 2.2 Response

```json
{
  "jsonrpc": "2.0",
  "id": 1,
  "result": {
    "name": "lvs0/snap_a1b2c3",
    "lvol_size_bytes": 10737418240,
    "cluster_size_bytes": 1048576,
    "io_unit_size_bytes": 4096,
    "ranges": [
      { "offset_bytes": 0,         "length_bytes": 3145728 },
      { "offset_bytes": 104857600, "length_bytes": 1048576 }
    ],
    "next_offset_bytes": 0
  }
}
```

| Field | Semantics |
|:------|:----------|
| `lvol_size_bytes` | logical size of the lvol (`num_blocks × block_size` of the bdev) |
| `cluster_size_bytes` | `spdk_bs_get_cluster_size(lvs->blobstore)` — allocation granularity (default 1 MiB, `SPDK_BLOB_OPTS_CLUSTER_SZ`) |
| `io_unit_size_bytes` | `spdk_bs_get_io_unit_size(...)` — informational |
| `ranges[]` | allocated extents of **this blob layer only**, ascending, non-overlapping, **adjacent runs merged**, cluster-aligned (offset and length are multiples of `cluster_size_bytes`, except a possible final extent truncated at `lvol_size_bytes`) |
| `next_offset_bytes` | `0` ⇒ walk exhausted; non-zero ⇒ more extents exist, resume with `offset_bytes = next_offset_bytes` |

**Invariants:** idempotent; read-only; for a **snapshot** (frozen, read-only blob) the result is immutable and bit-stable across calls. For a writable head lvol the map may change concurrently — the walk is still memory-safe, but the result is only a point-in-time approximation (documented; SPEC-60 only ever queries snapshot layers).

### 2.3 Errors

| Condition | JSON-RPC error |
|:----------|:---------------|
| `name` missing / unparsable params | `-32602` (invalid params, decode failure — standard pattern) |
| bdev not found, or bdev is not an lvol | `-ENODEV` (`spdk_strerror`, same as other lvol RPCs) |
| `offset_bytes ≥ lvol_size_bytes` | success with empty `ranges`, `next_offset_bytes: 0` (simplifies resume loops; consumers treat past-end as exhausted) |
| `max_ranges` > 65536 | clamped to 65536 (not an error) |

### 2.4 Design decision — extents, not a bitmap (vs Longhorn `get_fragmap`)

| | Extents (this spec) | Bitmap (Longhorn fragmap) |
|:--|:--|:--|
| Wire size, sparse volume | O(extents) — tiny | O(volume/cluster) regardless of sparsity (1 TiB @1 MiB = 128 KiB b64 *per call window*) |
| Consumer decode | none (offsets ready for CSI `VARIABLE_LENGTH`) | base64 + bit-walk + offset math |
| Pathological fragmentation | worst case = bitmap size, bounded by `max_ranges` pagination | constant |
| JSON friendliness | native | binary-in-JSON |

CSI `BlockMetadata` is extent-shaped (`byte_offset` + `size_bytes`); emitting extents end-to-end avoids two format conversions. Pagination (§2.1) bounds the fragmentation worst case.

---

## 3. Implementation design

### 3.1 Placement & integration

A **new source file** `module/bdev/lvol/vbdev_lvol_ranges_rpc.c`, added by the patch, plus a one-line hunk in `module/bdev/lvol/Makefile` (`C_SRCS += vbdev_lvol_ranges_rpc.c`).

Why a new-file patch (vs the two alternatives):
- *Appending to `vbdev_lvol_rpc.c`*: that file changes upstream every release → recurring conflicts. A new file's only conflict surface is the one-line Makefile hunk.
- *Out-of-tree module à la `module/bdev/cbt/`*: the cbt pattern (COPY + 3 `sed` registrations in the Dockerfile) is justified for a full bdev module; an RPC-only addition needs `module/bdev/lvol/vbdev_lvol.h` internals (`vbdev_lvol_get_from_bdev`) and gains nothing from module isolation. RPC registration is constructor-based (`SPDK_RPC_REGISTER`) and works from any linked object.

### 3.2 Reference implementation (~90 lines)

```c
/* module/bdev/lvol/vbdev_lvol_ranges_rpc.c — SPDX-License-Identifier: BSD-3-Clause */
#include "spdk/rpc.h"
#include "spdk/util.h"
#include "spdk/string.h"
#include "spdk/blob.h"
#include "spdk/bdev.h"
#include "vbdev_lvol.h"

#define RPC_RANGES_DEFAULT_MAX 4096
#define RPC_RANGES_HARD_MAX    65536

struct rpc_lvol_ranges {
	char     *name;
	uint64_t  offset_bytes;
	uint32_t  max_ranges;
};

static const struct spdk_json_object_decoder rpc_lvol_ranges_decoders[] = {
	{"name", offsetof(struct rpc_lvol_ranges, name), spdk_json_decode_string},
	{"offset_bytes", offsetof(struct rpc_lvol_ranges, offset_bytes), spdk_json_decode_uint64, true},
	{"max_ranges", offsetof(struct rpc_lvol_ranges, max_ranges), spdk_json_decode_uint32, true},
};

static void
rpc_bdev_lvol_get_allocated_ranges(struct spdk_jsonrpc_request *request,
				   const struct spdk_json_val *params)
{
	struct rpc_lvol_ranges req = { .offset_bytes = 0, .max_ranges = RPC_RANGES_DEFAULT_MAX };
	struct spdk_json_write_ctx *w;
	struct spdk_bdev *bdev;
	struct spdk_lvol *lvol;
	struct spdk_blob *blob;
	uint64_t io_unit_size, cluster_size, lvol_size, total_io_units;
	uint64_t io_unit, next_unalloc, emitted = 0, next_offset = 0;

	if (spdk_json_decode_object(params, rpc_lvol_ranges_decoders,
				    SPDK_COUNTOF(rpc_lvol_ranges_decoders), &req)) {
		spdk_jsonrpc_send_error_response(request, SPDK_JSONRPC_ERROR_INVALID_PARAMS,
						 "spdk_json_decode_object failed");
		goto cleanup;
	}
	if (req.max_ranges == 0 || req.max_ranges > RPC_RANGES_HARD_MAX) {
		req.max_ranges = spdk_min(req.max_ranges ? req.max_ranges : RPC_RANGES_DEFAULT_MAX,
					  (uint32_t)RPC_RANGES_HARD_MAX);
	}

	bdev = spdk_bdev_get_by_name(req.name);
	if (bdev == NULL || (lvol = vbdev_lvol_get_from_bdev(bdev)) == NULL) {
		spdk_jsonrpc_send_error_response(request, -ENODEV, spdk_strerror(ENODEV));
		goto cleanup;
	}

	blob = lvol->blob;
	io_unit_size  = spdk_bs_get_io_unit_size(lvol->lvol_store->blobstore);
	cluster_size  = spdk_bs_get_cluster_size(lvol->lvol_store->blobstore);
	lvol_size     = spdk_bdev_get_num_blocks(bdev) * spdk_bdev_get_block_size(bdev);
	total_io_units = lvol_size / io_unit_size;

	w = spdk_jsonrpc_begin_result(request);
	spdk_json_write_object_begin(w);
	spdk_json_write_named_string(w, "name", req.name);
	spdk_json_write_named_uint64(w, "lvol_size_bytes", lvol_size);
	spdk_json_write_named_uint64(w, "cluster_size_bytes", cluster_size);
	spdk_json_write_named_uint64(w, "io_unit_size_bytes", io_unit_size);
	spdk_json_write_named_array_begin(w, "ranges");

	io_unit = req.offset_bytes / io_unit_size;
	while (io_unit < total_io_units && emitted < req.max_ranges) {
		io_unit = spdk_blob_get_next_allocated_io_unit(blob, io_unit);
		if (io_unit == UINT64_MAX || io_unit >= total_io_units) {
			break;
		}
		next_unalloc = spdk_blob_get_next_unallocated_io_unit(blob, io_unit);
		if (next_unalloc == UINT64_MAX || next_unalloc > total_io_units) {
			next_unalloc = total_io_units;
		}
		spdk_json_write_object_begin(w);
		spdk_json_write_named_uint64(w, "offset_bytes", io_unit * io_unit_size);
		spdk_json_write_named_uint64(w, "length_bytes", (next_unalloc - io_unit) * io_unit_size);
		spdk_json_write_object_end(w);
		emitted++;
		io_unit = next_unalloc;
	}
	if (emitted == req.max_ranges && io_unit < total_io_units &&
	    spdk_blob_get_next_allocated_io_unit(blob, io_unit) != UINT64_MAX) {
		next_offset = io_unit * io_unit_size;
	}

	spdk_json_write_array_end(w);
	spdk_json_write_named_uint64(w, "next_offset_bytes", next_offset);
	spdk_json_write_object_end(w);
	spdk_jsonrpc_end_result(request, w);
cleanup:
	free(req.name);
}
SPDK_RPC_REGISTER("bdev_lvol_get_allocated_ranges", rpc_bdev_lvol_get_allocated_ranges,
		  SPDK_RPC_RUNTIME)
```

Notes for the implementer:
- **Extent merging is implicit**: `get_next_allocated` → `get_next_unallocated` yields maximal runs directly; adjacent clusters never produce two extents.
- **Cluster alignment**: `blob_find_io_unit` advances by cluster boundaries, so emitted extents are cluster-aligned by construction (final extent clamped to `total_io_units`).
- **`vbdev_lvol_get_from_bdev` guard** also rejects non-lvol bdevs (returns NULL on module mismatch) — single check covers both error cases.

### 3.3 Threading & safety

- Runs on the RPC/app thread, exactly like the existing read-only lvol RPCs (`rpc_bdev_lvol_get_lvols` iterates lvol/blob state on the same thread, `vbdev_lvol_rpc.c:1190-1257`). The walk reads only the in-memory cluster map — **no blobstore md operation, no callback, no I/O submitted** → no completion context to manage, the RPC responds synchronously.
- Bounded time-on-thread: ≤ `max_ranges` extent emissions per call (hard cap 65536); a fully fragmented 1 TiB / 1 MiB-cluster blob completes in ≤ 16 paginated calls.
- Snapshot blobs are frozen → stable iteration. Writable head lvols: map may grow concurrently on other threads; the cluster-map reads are word-sized loads on a stable array for the blob's current size — same exposure as the existing `SEEK_DATA` path (`vbdev_lvol.c:892`), accepted upstream. Documented as point-in-time for non-snapshots (§2.2).

### 3.4 Patch & build integration

```
patches/0002-bdev-lvol-add-get-allocated-ranges-rpc.patch
  ├─ A module/bdev/lvol/vbdev_lvol_ranges_rpc.c   (new file, §3.2)
  └─ M module/bdev/lvol/Makefile                  (C_SRCS += vbdev_lvol_ranges_rpc.c)
```

`images/spdk/Dockerfile`: extend the existing apply step (`:59`) — `git apply /build/patches/0001-*.patch /build/patches/0002-*.patch` (or a `for p in` loop so future patches need no Dockerfile edits). The `vendor/spdk` checkout in this repo is the development/test bed for the patch before image builds.

---

## 4. Tests

| Layer | Test |
|:------|:-----|
| **Compile/apply** | CI: patch applies cleanly on `${SPDK_VERSION}` (already implied by image build); add a build-stage `spdk_tgt --version` smoke |
| **RPC schema** | `rpc_get_methods` includes `bdev_lvol_get_allocated_ranges`; malformed params → `-32602`; unknown bdev → `-ENODEV` |
| **Functional smoke** (`test/allocated_ranges_smoke.sh`, run in the debug image against `spdk_tgt` + malloc-backed lvs) | create lvs (`cluster_sz=1MiB`) → thin lvol 64 MiB → write 4 KiB at offsets {0, 10 MiB, 33 MiB} via `bdevperf`/`dd` over nbd → expect exactly 3 extents of 1 MiB at cluster-rounded offsets → snapshot → write 1 cluster → snapshot's ranges unchanged (immutability), head's ranges = 1 extent |
| **Pagination** | same fixture with `max_ranges=2` → 2 calls, `next_offset_bytes` resume, concatenation equals the unpaginated result |
| **Consumer contract** | mirrored in `spdk-csi` `Spdk.ContractTests` (SPEC-60 §6.3) — request/response pinned to §2 |

---

## 5. Versioning & release

- Ships as a patch-level bump of the image: `v26.01.x` (scheme `v<upstream>.<patch>`, immutable tags, cosign-signed, SBOM-attested — unchanged repo conventions).
- **No lockstep with spdk-csi**: the consumer probes `rpc_get_methods` (SPEC-60 R4). Old image + new CSI → graceful degradation (`FAILED_PRECONDITION` deltas); new image + old CSI → RPC simply unused.
- The RPC name is the API contract: never change semantics under the same name; additive fields only (consumers must ignore unknown fields).

---

## 6. Upstreaming plan (SPEC-60 §4, Option E)

1. Open an spdk.io issue referencing the CSI SnapshotMetadata/KEP-3314 use case (SP-neutral motivation; Longhorn's parallel out-of-tree `get_fragmap` as demand evidence).
2. Submit to SPDK Gerrit: the §3.2 file + Makefile line + `scripts/rpc.py` plugin subcommand + `doc/jsonrpc.md` entry + unit test (`test/unit/lib/blob` extension), per upstream conventions.
3. Carry the patch locally regardless of review latency; on merge, drop `0002-*.patch` at the next `SPDK_VERSION` bump — consumer-invisible thanks to the capability probe.

---

## 7. Definition of Done

- Image `v26.01.x` exposes `bdev_lvol_get_allocated_ranges` per §2; smoke + pagination tests green in CI.
- `spdk-csi` contract test green against the built image.
- SPEC-60 QA (`QA_SMS_AllocatedMatchesWrittenLayout`, `QA_SMS_DeltaMatchesInterSnapshotWrites`) green on a cluster running this image — the end-to-end proof that the extents are exact.
- Upstream submission opened (tracking issue linked in the repo README).
