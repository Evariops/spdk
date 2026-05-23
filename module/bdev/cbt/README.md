# SPDK CBT (Change Block Tracking) Module

## What problem this solves

SPDK RAID-1 has no dirty tracking. When a backend disconnects — whether for planned maintenance or unexpected failure — the only recovery path is a full surface rebuild: reading and rewriting every block on the volume. For a 2 TB volume, that takes over five hours.

This module eliminates that cost. It sits between the target and the RAID bdev as a transparent passthrough, recording which blocks have been modified. When the backend comes back, only the modified blocks are copied — turning hours into seconds.

## How it works

The CBT vbdev intercepts every write, unmap, and write_zeroes IO that flows through it. For each operation, it marks the corresponding bits in an in-memory bitmap. The bitmap is indexed by *chunks* — configurable regions of 4 KB to 64 MB (default 64 KB). This means the memory cost for a 2 TB volume at 64 KB granularity is 4 MB.

The bitmap is never explicitly started or stopped. It accumulates from the moment the vbdev is created. This guarantees zero missed writes regardless of when a failure is detected — the orchestrator never needs to race against the first write.

Multiple IO threads set bits concurrently using atomic OR. There are no locks on the IO path. A per-chunk counter is not maintained — instead, the dirty count is computed lazily via popcount when requested by RPCs or the poller.

## Interface contract

The module exposes its functionality through JSON-RPC. The orchestrator drives the lifecycle; the module never takes autonomous decisions about rebuild or recovery.

### Creating and destroying the vbdev

`bdev_cbt_create` wraps any existing bdev with a CBT tracking layer. The base bdev may not exist yet — in that case, the configuration is saved and applied when the bdev appears (deferred create). `bdev_cbt_delete` tears it down and cleans up the deferred entry if the base bdev never materialized.

### The epoch protocol

An *epoch* represents a single backend outage and its associated recovery. The orchestrator opens an epoch when it detects that a backend is stale, identifying which backend and at which generation. Up to four epochs may coexist simultaneously — allowing independent tracking when multiple backends fail at different times.

**Opening** (`bdev_cbt_epoch_open`) records the stale backend identity and suspends the healthy-clear poller. The bitmap continues accumulating all writes as before — the epoch is purely an administrative marker.

**Freezing** (`bdev_cbt_epoch_freeze`) takes a point-in-time snapshot of the live bitmap into the epoch. An atomic fence ensures visibility of all prior relaxed stores from IO threads. After freeze, the live bitmap continues accumulating new writes independently of the frozen copy. Re-freezing the same epoch replaces the previous snapshot — useful for iterative convergence.

**Reading ranges** (`bdev_cbt_epoch_get_dirty_ranges`) walks the frozen bitmap and coalesces contiguous dirty chunks into offset+length pairs. The result is capped to avoid unbounded allocations, and a `truncated` flag signals when the caller must paginate.

**Rebuild start** (`bdev_cbt_epoch_rebuild_start`) transitions the epoch from FROZEN to REBUILDING. This is a state marker — the actual data copy happens externally. While REBUILDING, the epoch cannot be evicted, reset, or have the bitmap cleared underneath it.

**Partial rebuild** (`bdev_cbt_partial_rebuild`) performs the actual data copy asynchronously within the SPDK process. It reads dirty chunks from the CBT bdev (which passes through to the RAID base) and writes them to a specified target bdev. The operation is fully async and callback-driven — the RPC response is deferred until the copy completes.

Parameters:
- `name`: the CBT bdev to read from
- `epoch_id`: must be in FROZEN state (transitions to REBUILDING)
- `target_bdev_name`: the destination bdev (must exist in the same spdk_tgt process)
- `max_bandwidth_mb_sec`: bandwidth throttle in MB/s (0 = unlimited)
- `queue_depth`: concurrent I/Os (1–128, default 16)

The implementation allocates `queue_depth` DMA buffers of chunk_size each, opens IO channels on both source and target bdevs, and walks the frozen bitmap submitting read/write pairs. A per-second bandwidth window limits throughput when throttling is configured.

On completion, the response includes:
- `completed`: boolean success indicator
- `chunks_copied`: number of dirty chunks copied
- `bytes_copied`: total bytes transferred
- `duration_ms`: wall-clock time for the operation
- `residual_dirty_ratio`: fraction of the bitmap that is now dirty (writes that arrived during the copy). This ratio drives the orchestrator's convergence loop — when it drops below a threshold, a final quiesce+freeze+copy achieves zero delta.

**Closing** (`bdev_cbt_epoch_close`) discards the epoch and its frozen bitmap. If no epochs remain active, the healthy-clear poller is re-enabled.

**Invalidation** (`bdev_cbt_epoch_invalidate`) marks an epoch as unrecoverable. The orchestrator knows it must fall back to a full rebuild for that backend.

### Health signaling

The orchestrator must explicitly call `bdev_cbt_set_backends_healthy(true)` to confirm that all backends are synchronized. Only then will the poller clear the bitmap. This double-guard prevents premature clearing — even if all epochs are closed, the bitmap persists until health is confirmed.

### Reset

`bdev_cbt_reset` zeroes the bitmap. It refuses with `-EBUSY` if any epoch is active — protecting the delta needed for in-progress rebuilds.

## Convergence and quiesce

The module provides both the tracking primitives and the async copy engine. The orchestrator drives the convergence loop but does not implement the data copy — that happens inside the SPDK process via `bdev_cbt_partial_rebuild`.

A typical orchestrator sequence for partial rebuild under sustained write load:

1. Open epoch. The bitmap accumulates all writes.
2. Freeze. Snapshot the dirty state.
3. Call `bdev_cbt_partial_rebuild` — copies dirty chunks to the stale backend.
4. During the copy, new writes accumulate in the live bitmap.
5. On completion, read `residual_dirty_ratio` from the response.
6. Re-freeze. The new snapshot captures only what arrived since the last freeze.
7. Repeat steps 3–6 until residual_dirty_ratio is below threshold.
8. ANA drain (2s quiesce at the NVMe-oF target level). No more writes arrive.
9. Final freeze. Delta is zero by construction.
10. Final `bdev_cbt_partial_rebuild` (nothing or near-nothing). Close epoch.
11. Re-add the backend with `skip_rebuild=true`. Signal healthy.

The module cannot converge on its own when write rate approaches rebuild bandwidth. The ANA drain at step 7 is mandatory to guarantee termination. This quiesce happens above the module — at the target or multipath level — and is invisible to the CBT code.

## RAID integration

The companion patch (`patches/0001-raid-add-skip_rebuild-parameter.patch`) adds a `skip_rebuild` boolean to the `bdev_raid_add_base_bdev` RPC. When true, the RAID module skips its full surface rebuild process and instead:

Quiesces the RAID bdev to halt IO dispatch. Iterates all existing IO channels and opens `base_channel[slot]` for the re-added bdev — without this, existing channels would never write to the backend. Unquiesces to resume IO with the backend fully integrated. Writes the superblock to persist the configuration across reboots.

This is modeled on SPDK's own `raid_bdev_process_finish` sequence (the normal rebuild completion path) but without the process infrastructure, since the data copy was already performed externally by the CBT-driven orchestrator.

## Concurrency model

All epoch operations and the healthy-clear poller run on the SPDK app thread. They are not thread-safe against each other — SPDK guarantees single-threaded execution on that thread. The `assert(spdk_get_thread() == spdk_thread_get_app_thread())` guards enforce this.

IO submission (`cbt_mark_dirty`) runs on any reactor thread. It uses `__atomic_fetch_or` with relaxed ordering for individual bitmap bytes, and `memset(0xFF)` for full bytes in large ranges. The combination is safe because setting a bit to 1 is idempotent — concurrent ORs cannot lose information.

The `total_writes_tracked` counter uses relaxed atomics. It is a statistical counter with no correctness semantics.

## Memory layout

The bitmap is a flat `uint8_t` array sized at `ceil(device_blocks / chunk_size_blocks) / 8` bytes. Chunk size is forced to a power of two so that the chunk index can be computed with a shift instead of a division.

Each epoch holds its own `bitmap_frozen` — a `malloc`'d copy created at freeze time. At most 4 epochs coexist, so peak memory is `5 × bitmap_size` (live + 4 frozen).

## Build

The module compiles in two modes. As part of the SPDK tree (`make`), it produces a shared object linked into `spdk_tgt`. The Dockerfile handles this: it copies the module source into the cloned SPDK tree, registers it in the bdev Makefile via `sed`, applies the RAID patch, and builds everything together.

For development and CI, `make -f Makefile.test` compiles standalone tests with AddressSanitizer, UndefinedBehaviorSanitizer, and `-Werror`. These tests simulate the CBT logic without any SPDK dependency — they can run on any machine with a C11 compiler and pthreads.

## Testing strategy

The test suite validates four concerns. `test_cbt_bitmap` exercises the fundamental bitmap operations: set, get, popcount, and boundary conditions at byte and bit granularity. `test_cbt_props` uses property-based random testing to verify invariants across thousands of randomized operation sequences. `test_cbt_resilience` simulates the full epoch lifecycle including edge cases like double-freeze, eviction under pressure, and invalid state transitions. `test_cbt_postfix` validates the specific behaviors introduced by the security and correctness audit: reset refusal during active epochs, truncation signaling, deferred cleanup, eviction safety, healthy-clear gating, and concurrent mark_dirty correctness under 8 threads.
