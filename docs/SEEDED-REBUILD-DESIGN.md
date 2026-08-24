# Seeded rebuild (raid1) — design

The seeded rebuild replaces the control-plane reintegration pause window (`nvmf pause → final freeze → copy → add(skip_rebuild) → resume`) with a **monotonic** join: the member enters the write path immediately, and the historical delta is a **fixed** set backfilled by the in-raid rebuild machinery — region-locked, with no admission stop and no barrier, TTL or keepalive protocol.

## Correctness argument (why the delta is FIXED)

Ordering: **attach write-only → epoch freeze → seed ranges → backfill**.

1. At attach, the member is write-replicated across the WHOLE range (and read-excluded). From that instant, every host write reaches the joiner.
2. Writes between epoch-open (when the member left) and the freeze are in the cumulative bitmap; writes between attach and freeze are BOTH replicated and marked dirty. The freeze folds them in, so copying them again is idempotent.
3. After the freeze, the dirty set can only shrink: a clean block is either untouched since before the epoch (the joiner's content is still valid) or freshly written (replicated at write time). Skipping clean gaps is therefore safe — **because and only because** the joiner is already write-replicated everywhere.
4. Dirty windows are copied under the existing per-window `quiesce_range` region lock, reading the CURRENT healthy-leg content: a host write racing the copy is either held by the quiesce or lands after it and is replicated. No external copy path exists, so a clobbering race is structurally excluded.
5. Completion clears read-exclusion (promotion). The control plane then records the set-after generation (`syncGeneration`).

No pause, no barrier, no token, no zero-check, no post-add re-verify: the protocol family the pause window required does not exist on this path.

## Changes

### raid1: decouple read-eligibility from write-membership

- `struct raid_base_bdev_info` gains `bool read_excluded`, set while a seeded backfill is in flight for that member.
- `raid1_channel_next_read_base_bdev` skips slots whose base_info is `read_excluded` (in addition to NULL channels). Write fan-out is unchanged — channel presence keeps meaning "receives writes". The rebuild's own healthy-leg reads go through the same selection, so the flag also keeps the backfill from reading the joiner.

### bdev_raid: seeded process

- `struct raid_bdev_process` gains an optional sorted, non-overlapping `seed_ranges` list (block units).
- Window driver fast-advance: when `[window_offset, +window)` intersects no seed range, advance the watermark (and per-channel `process.offset`) WITHOUT quiesce or copy, jumping to the next dirty range start. Dirty windows proceed exactly as before (quiesce → copy from healthy leg → unquiesce → advance).
- Attach mode: the seeded target's channel is populated in ALL raid_ch at attach (like the skip_rebuild promotion path, with a whole-raid quiesce for wiring only and the unwind discipline of patch 0001), `read_excluded = true`; the process runs with `ch_processed` degenerating to the same channels, kept for engine symmetry. On completion: `read_excluded = false`, then superblock slot → CONFIGURED — the flip happens after the wiring is live, never before.

### RPCs (two-phase, house style)

1. `bdev_raid_add_base_bdev` gains an optional `"write_only": bool` — attach the member write-replicated and read-excluded, with NO process started and no superblock CONFIGURED flip (mirrors the optional-decoder style of 0001).
2. New `bdev_raid_start_seeded_rebuild {name, base_bdev, ranges:[{offset_blocks, length_blocks}]}` starts the seeded process on a write-only member. It is async: an immediate `{rebuild_id}` reply, progress through the existing get_bdevs process info, and a completion that promotes read-eligibility and the superblock. It is audited (`spdk_jsonrpc_request_audit`, patch 0011) like every destructive RPC. The ranges come from `bdev_cbt_epoch_get_dirty_ranges`; the orchestrator carries them, so no raid↔cbt in-process link is introduced.
3. Failure semantics: a failed seeded rebuild removes the member (the vanilla process-failure path) and the epoch stays FROZEN, so the control plane re-freezes and retries, or falls back to a full rebuild. A failed write to a write-only member fails the member (vanilla), never the array.

### Contract

The seeded reintegration ordering is `add_base_bdev(write_only) → cbt epoch_freeze → epoch_get_dirty_ranges → start_seeded_rebuild(ranges) → completion → control-plane set-after`. The pause-window ordering documented in `docs/RPC-CONTRACT.md` remains valid while both paths coexist.
