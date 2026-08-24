# SPDK CBT (Change Block Tracking) Module

## What problem this solves

SPDK RAID-1 has no dirty tracking. When a backend disconnects — for planned maintenance or through failure — the only recovery path is a full surface rebuild: reading and rewriting every block of the volume. For a 2 TB volume that takes over five hours.

This module removes that cost. It sits between the target and the RAID bdev as a transparent passthrough, recording which blocks have been modified. When the backend comes back, only the modified blocks are copied — turning hours into seconds.

## How it works

The CBT vbdev intercepts every write, unmap and write_zeroes I/O flowing through it and marks the corresponding bits in an in-memory bitmap. The bitmap is indexed by *chunks* — configurable regions of 4 KB to 64 MB, 64 KB by default — so a 2 TB volume costs 4 MB of memory at the default granularity. Chunk size is forced to a power of two, so a chunk index is a shift rather than a division.

The bitmap is never explicitly started or stopped: it accumulates from the moment the vbdev is created. This guarantees no missed writes whenever a failure is detected, and the orchestrator never has to race the first write.

Multiple I/O threads set bits concurrently with `__atomic_fetch_or` under relaxed ordering (and `memset(0xFF)` for full bytes in large ranges). There are no locks on the I/O path — setting a bit to 1 is idempotent, so concurrent ORs cannot lose information — and no per-chunk counter is maintained: the dirty count is computed lazily by popcount when an RPC or the poller asks for it. Epoch operations, by contrast, all run on the SPDK app thread and are not thread-safe against each other; `assert`s on the app thread enforce that.

## The epoch protocol

An *epoch* represents a single backend outage and its recovery. Up to four epochs coexist, so backends failing at different times are tracked independently. Each holds its own frozen bitmap, so peak memory is `5 × bitmap_size` (live plus four frozen).

Per-RPC preconditions, error codes, idempotence and crash behaviour are specified in `docs/RPC-CONTRACT.md`; this section explains the shape of the protocol, not its contract.

The shape is **open → freeze → get_dirty_ranges → close**:

| Step | RPC | What it guarantees |
|:-----|:----|:-------------------|
| open | `bdev_cbt_epoch_open` | the stale backend's identity and generation are recorded; tracking is unaffected |
| freeze | `bdev_cbt_epoch_freeze` | the delta since the previous freeze becomes an immutable snapshot; the live bitmap then accumulates only NEW writes |
| get_dirty_ranges | `bdev_cbt_epoch_get_dirty_ranges` | that snapshot reads back as coalesced `{offset, length}` pairs — a FIXED set the copier can work through |
| close | `bdev_cbt_epoch_close` | the epoch is released without losing history: an unconsumed delta is merged back into the live bitmap first |

`bdev_cbt_epoch_rebuild_start` and `bdev_cbt_epoch_invalidate` are state markers around that spine: a copy is under way, or this epoch is unrecoverable and the backend needs a full rebuild.

Two invariants are worth understanding before calling anything:

**A frozen delta is never dropped until a rebuild proves it copied.** Bits exchanged out of the live bitmap exist only in the frozen buffer. Any path that would discard that buffer without a completed rebuild — a re-freeze after a failed one, a close, an eviction — first ORs it back into the live bitmap, so the next snapshot is exactly (unconsumed delta ∪ new writes). A failed iteration therefore loses nothing; already-copied chunks are pessimistically re-copied.

**An aborted rebuild is not a completed one.** Only a rebuild answering `completed` licenses discarding the delta it consumed; anything that stops early, fails or is cancelled leaves the frozen bitmap in force and its delta is folded back. A freeze racing an in-flight write is harmless for the same reason: the dirty bit is set at submission AND re-set at completion before the host ack, so a chunk consumed at submit time lands in the next delta and no host-I/O drain is needed around freeze.

## Copying and converging

`bdev_cbt_partial_rebuild` copies asynchronously inside the SPDK process, reading dirty chunks from the CBT bdev and writing them to a target bdev under an optional bandwidth cap and queue depth. It does not issue one I/O per chunk: it coalesces up to 16 consecutive dirty chunks into a single read+write pair (`CBT_REBUILD_MAX_COALESCE_CHUNKS = 16`), which at the default chunk size means I/Os of up to 1 MiB — the sweet spot for NVMe-oF TCP, where per-command overhead dominates at small sizes.

The orchestrator drives the loop: freeze, copy, read `residual_dirty_ratio` from the response — the fraction dirtied by writes that arrived during the copy — then re-freeze and copy again. Each pass is smaller than the last, so the loop converges geometrically:

```
OPEN ──freeze──► FROZEN ──partial_rebuild──► REBUILDING
                   ▲                              │
                   └───────────freeze─────────────┘
                   │
                   └──close──► (removed)
```

The module cannot converge on its own when the write rate approaches rebuild bandwidth. Termination comes from an ANA drain — a short quiesce at the NVMe-oF target level, above the module and invisible to it — before the final freeze, which makes the last delta zero by construction.

Clearing is reset-driven: there is no automatic clear. Once the backend is re-added and all backends are synchronized, the orchestrator MUST call `bdev_cbt_reset`, or the bitmap grows monotonically and "partial" rebuilds degrade toward full-surface copies.

## RAID integration

The companion patch (`patches/0001-raid-add-skip_rebuild-parameter.patch`) adds a `skip_rebuild` boolean to `bdev_raid_add_base_bdev`. When true, the RAID module skips its full surface rebuild and instead quiesces the raid, opens `base_channel[slot]` on every existing I/O channel for the re-added bdev — without this, existing channels would never write to the backend — then unquiesces and writes the superblock.

## Build and tests

In the SPDK tree (`make`) the module builds into `spdk_tgt`; the Dockerfile copies the source in, registers it in the bdev Makefile via `sed`, applies the patch series and builds. `make -f Makefile.test` runs standalone ASAN/UBSAN tests with no SPDK dependency, covering the bitmap primitives, randomized property checks, the epoch lifecycle and concurrent marking across 8 threads.
