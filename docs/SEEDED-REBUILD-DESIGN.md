# Seeded rebuild (raid1) — design (SPEC-74 M6, spdk-csi side)

Status: design accepted for implementation on this branch (stacked on PR#13).
Goal: replace the control-plane reintegration pause window (`nvmf pause → final
freeze → copy → add(skip_rebuild) → resume`, RPC-CONTRACT P5) with a **monotonic**
join: the member enters the write path immediately, the historical delta is a
**fixed** set backfilled by the in-raid rebuild machinery — region-locked, no
admission stop, no barrier/TTL/keepalive protocol.

## Ground truth this design builds on (verified in-tree)

- The raid rebuild engine (`bdev_raid.c` process) is whole-range: progress is the
  scalar `window_offset` over `[0, blockcnt)`, 1 MiB windows, each window
  `spdk_bdev_quiesce_range`-locked during copy; host I/O below the watermark runs
  on `ch_processed` (target populated ⇒ write-replicated + readable), above it the
  target slot is NULL (excluded). Completion promotes the target into
  `base_channel[slot]` (`raid_bdev_channel_process_finish`).
- raid1 keys BOTH read selection (`raid1_channel_next_read_base_bdev`) and write
  fan-out (`raid1_submit_write_request`) on `base_channel[slot] != NULL` — there is
  no per-member read/write decoupling today.
- CBT exports a frozen epoch as coalesced block ranges:
  `bdev_cbt_epoch_get_dirty_ranges` → `{offset_blocks, length_blocks}[]`; freeze is
  an atomic snapshot-and-clear with H1 delta-preservation (re-freeze folds
  unconsumed delta); freeze is refused `-EBUSY` while a rebuild consumes the epoch.
- No raid↔cbt in-process linkage exists; the orchestrator carries range lists
  between modules (house pattern: patch 0008 `bdev_raid_rebuild_ranges`).

## Correctness argument (why the delta is FIXED)

Ordering: **attach write-only → epoch freeze → seed ranges → backfill**.

1. At attach, the member is write-replicated across the WHOLE range (read-excluded).
   From this instant, every host write reaches the joiner directly.
2. Writes between epoch-open (when the member left) and the freeze are in the
   cumulative bitmap; writes between attach and freeze are BOTH replicated and
   marked dirty — the freeze folds them (H1), so copying them again is idempotent.
3. After the freeze, the dirty set can only shrink: a clean block is either
   untouched since before the epoch (joiner content still valid) or freshly
   written (replicated at write time). Skipping clean gaps is therefore safe —
   **because and only because** the joiner is already write-replicated everywhere.
4. Dirty windows are copied under the existing per-window `quiesce_range` region
   lock, reading the CURRENT healthy-leg content: a host write racing the copy is
   either held by the quiesce or lands after and is replicated. No external copy
   path exists (the iter29 clobbering race is structurally excluded).
5. Completion clears read-exclusion (promotion). The control plane then records
   set-after (`syncGeneration`) — SPEC-74 M5.

No pause, no barrier, no token, no zero-check, no post-add re-verify: the
protocol family the pause window required does not exist on this path.

## Changes

### raid1: decouple read-eligibility from write-membership

- `struct raid_base_bdev_info` gains `bool read_excluded` (set while a seeded
  backfill is in flight for this member).
- `raid1_channel_next_read_base_bdev` skips slots whose base_info has
  `read_excluded` (in addition to NULL channels). Write fan-out unchanged —
  channel presence keeps meaning "receives writes".
- The rebuild process's own healthy-leg reads already go through raid1 read
  selection ⇒ the flag also keeps the backfill from reading the joiner.

### bdev_raid: seeded process

- `struct raid_bdev_process` gains an optional sorted, non-overlapping
  `seed_ranges` list (block units).
- Window driver fast-advance: when `[window_offset, +window)` intersects no seed
  range, advance the watermark (and per-channel `process.offset`) WITHOUT
  quiesce/copy, jumping to the next dirty range start. Dirty windows proceed
  exactly as today (quiesce → copy from healthy leg → unquiesce → advance).
- Attach mode: the seeded target's channel is populated in ALL raid_ch at attach
  (like the skip_rebuild promotion path, whole-raid quiesce for wiring only,
  with the 0001 CBT-6 unwind discipline), `read_excluded = true`; the process
  runs with `ch_processed` degenerating to the same channels (kept for engine
  symmetry). On completion: `read_excluded = false`, superblock slot →
  CONFIGURED (CBT-7 ordering: flip after the wiring is live).

### RPCs (two-phase, house style)

1. `bdev_raid_add_base_bdev` gains optional `"write_only": bool` — attach the
   member write-replicated/read-excluded, NO process started, no SB CONFIGURED
   flip. (Mirrors 0001's optional-decoder style.)
2. New `bdev_raid_start_seeded_rebuild {name, base_bdev, ranges:[{offset_blocks,
   length_blocks}]}` — starts the seeded process on a write-only member. Async:
   immediate `{rebuild_id}` reply + progress via existing get_bdevs process info;
   completion promotes read-eligibility + SB. SEC1 audit
   (`spdk_jsonrpc_request_audit`) like every destructive RPC (patch 0011).
   Ranges come from `bdev_cbt_epoch_get_dirty_ranges` — the orchestrator carries
   them (P3), no raid↔cbt link is introduced.
3. Failure semantics: a failed seeded rebuild removes the member (vanilla
   process-failure path); the epoch stays FROZEN (H1) — the control plane
   re-freezes and retries or falls back to full rebuild. A failed write to a
   write-only member fails the member (vanilla), never the array.

### Contract (RPC-CONTRACT.md addendum)

New reintegration ordering P5b (replaces P5 when seeded mode is enabled):
`add_base_bdev(write_only) → cbt epoch_freeze → epoch_get_dirty_ranges →
start_seeded_rebuild(ranges) → completion → control-plane set-after`.
P5 (pause window) remains valid and documented while both paths coexist; the
decision to delete it is gated on the SPEC-74 C4 bench (p99-under-roll within
policy — burden of proof on the window).

## Trade being measured (C4 gate)

The joiner is in the write path for the WHOLE backfill: host write latency
couples to the joiner's media for the duration (vs ~4 s total pause today).
Mitigations: backfill under the existing QoS governor; write completions to the
joiner are replicated raid1 writes (parallel, latency = max of members). The
gate measures p99/p999-under-roll A/B before the pause window may be deleted.
