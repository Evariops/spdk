# Evariops fork — RPC contract (SPEC-73 / SPEC-66)

Contract sheet for the JSON-RPC surface the CSI control-plane drives (PR1). For
each RPC: preconditions, idempotence, and crash behavior. The invariants P1–P5
and the decisions D5/CBT-7 that the control-plane must honor are stated at the
end. This is the fork-side half; the paired control-plane guards live in
`spdk-csi/docs/reports/2026-07-04_remediations-appariees-fork-spdk.md`.

Probe support with `evariops_get_capabilities` (below) before assuming any of
these exist — on vanilla SPDK they return JSON-RPC -32601.

## Global assumptions

- **D5 — single reactor (`-m 0x1`).** The standing-pause registry (0003),
  `g_relocate_inflight` (0005), `band->state`, and the heat counters assume all
  RPC handlers run on one reactor, lock-free. `nvmf_subsystem_pause` logs an
  error if it ever sees >1 reactor. Widening the CPU mask requires a concurrency
  pass first. `evariops_get_capabilities` reports `single_reactor_assumed: true`.
- **Volatile state dies with the process.** Standing pauses, in-flight
  relocations, cbt epochs and rebuilds are RAM-only. A target restart loses them
  all — detect it via `boot_id` (below) and reconcile.
- **SEC1 — destructive RPCs are audited.** Every mutation handler
  (`bdev_tier_delete`, `bdev_tier_retire_band`, `bdev_tier_resync_md`,
  `bdev_lvol_relocate_cluster` / `_clusters` / `remap_cluster`,
  `bdev_raid_rebuild_ranges`, `nvmf_subsystem_pause`) emits an
  `audit rpc=<method> peer=pid:…,uid:…,gid:… <params>` NOTICELOG line, with the
  caller's Unix-socket credentials (SO_PEERCRED, patch 0011). `peer=unknown` over
  a TCP transport. This is an audit trail, **not** an authorization gate — access
  control is still the socket mode (0600, patch 0010) + a NetworkPolicy (D4). The
  socket is created owner-only **atomically** — `umask(077)` wraps the `bind()`
  (R14), closing the TOCTOU window that a chmod-after-listen left open; the chmod
  stays as belt-and-braces.

## evariops_get_capabilities

- **Params**: none.
- **Returns**: `boot_id` (per-process uuid), `tier_sb_version`,
  `capabilities_schema`, `single_reactor_assumed`, `methods[]`.
- **Use**: a changed `boot_id` across polls ⇒ the target restarted ⇒ treat all
  volatile state as lost. `methods[]` membership is the capability probe.
- `methods[]` is **filtered through the live RPC registry** (deferred #3): a fork
  method left in the candidate list but not built into this binary is NOT emitted,
  so `methods[]` never yields a false positive (a method reported present that then
  fails -32601). A method built in but absent from the list is a harmless false
  negative — the control-plane's per-call -32601 probe remains the ground truth.
- Idempotent, read-only.

## Lifecycle — tier

| RPC | Preconditions | Idempotence | Crash behavior |
|:----|:--------------|:------------|:---------------|
| `bdev_tier_create` | name free; `md_num_blocks>0`; `cluster_blocks` is 0 (legacy, no alignment) or ≥2 — **1 is rejected** (R13); `md_num_blocks` bounded (no align overflow, R17) | -EEXIST if name taken | in-RAM only until first SB write |
| `bdev_tier_add_band` | tier exists, **not registered**; unique wwn; disk ≥ sb+md; blocklen divides the 128 KiB SB slot (R7 — T10-DIF 520/4160 rejected) | band_id auto-assigned; caller replays deterministically | — |
| `bdev_tier_assemble_band` | **not registered (R8)**; band_id<64; state≤RETIRED; no overlap; fits disk; unique wwn; blocklen divides SB slot (R7) | -EEXIST on duplicate band_id; **-EBUSY after register (R8)** | places at stored geometry |
| `bdev_tier_register` | ≥1 band, cluster-aligned geometry | **-EEXIST if already registered (W1)** | **rehydrates `t->seq` from the on-disk SBs (R2), then persists SB** on success |
| `bdev_tier_retire_band` | not an md-mirror band (-EBUSY, T-7) | **idempotent**: re-run re-persists + re-closes | **async: acks only after SB durable (MJ6)**; rc≠0 ⇒ retry |
| `bdev_tier_resync_md` | target is a DEGRADED md leg; healthy source leg exists | re-runnable (leg stays DEGRADED on failure) | copy under md-range quiesce; acks after activate+persist |
| `bdev_tier_delete` | — | -ENODEV if absent | unregister → destruct |
| `bdev_tier_get_bands` / `bdev_tier_read_sb` | — | read-only | read_sb returns highest-seq valid slot + `generation_uuid` |

`bdev_tier_read_sb` exposes `version`, `seq`, `generation_uuid`,
`created_epoch_sec`. **Assembly (control-plane) must**: read every candidate
disk's SB, group by `generation_uuid` (fence stale disks from a previous
instance), take the highest `seq` per band, and if the two md legs disagree on
`seq`, assemble the higher one ACTIVE and the other DEGRADED then
`bdev_tier_resync_md` (G-CSI-2). The fork persists DEGRADED but cannot arbitrate
a split-brain across disks.

- **Seq monotonicity across a restart is fork-owned (R2).** The control-plane
  does NOT thread `seq` back through `create`/`assemble`/`register` (it reads seq
  only to pick the authoritative SB). So `bdev_tier_register` **re-reads the bands'
  on-disk SBs and seeds `t->seq` above the highest seq found** before its first
  persist. Without this the fresh instance would write `seq 1`, which a pre-restart
  SB at a high seq out-votes forever (highest-seq-wins), reassembling the STALE
  geometry and silently undoing every retire/relocate persisted at the high seq. No
  CSI change is required; this is the behavior the contract already assumed.

## Data-plane — relocate / remap (patch 0005)

- `bdev_lvol_relocate_cluster {name, tier_name, cluster_num, dst_lba_start, dst_lba_count}`
  - **P-2**: the lvol must live on `tier_name`'s composite (-EINVAL else).
  - **F2**: refuses snapshot blobs (-EBUSY).
  - **F4**: one relocate/remap in flight per blob (-EBUSY else).
  - **C1 + C1-DRAIN**: runs under an **lvol-bdev quiesce** (drains outstanding
    host I/O, then holds new I/O above the L2P translation) plus an inner
    **blob freeze**, for copy+commit; ALL of the lvol's I/O stalls
    ~drain + 3× one cluster. The drain is what makes the copy's source stable —
    the freeze alone never covered writes already in flight. **N-2**: the blob
    is pinned by an own open-ref for the whole chain.
  - Crash-safe (invariants A/B): a crash mid-op leaves an orphan cluster the
    native blobstore replay reclaims; never a lost ACKed write.
  - **H4**: an ambiguous commit failure (extent write dispatched, error
    completion) QUARANTINES the destination cluster until restart instead of
    releasing it — the durable extent may reference it; replay reconciles.
- `bdev_lvol_relocate_clusters {…, clusters:[…], verify?}` — **PF3 batch**: one
  quiesce+freeze amortized over N clusters (≤4096). Correctness identical to the
  single form (same C1-DRAIN and H4 contracts). `verify` (**PF4**, default true)
  forwards to each copy; false skips the C5 read-back on trusted media. Returns
  `{relocated, requested, error}` — **partial success is a 200** (caller retries
  the tail from `relocated`).
- `bdev_lvol_remap_cluster {…}` — **N-7/W6**: source band must be DEGRADED,
  destination band ACTIVE. Relaxes invariant A intentionally (the cluster is
  already lost; the subsequent `bdev_raid_rebuild_ranges` fills the new one). The
  control-plane MUST journal the remap durably BEFORE calling and re-drive the
  range rebuild at restart until confirmed (**PR3**, remap-before-rebuild).
  - **R11 — the old cluster is QUARANTINED, not freed.** A remap re-homes a cluster
    whose old copy is on the DEGRADED band; the commit keeps that old cluster
    marked-allocated (`release_old=false`) rather than returning it to the thin
    pool. Otherwise the allocator could re-serve that LBA to a normal host write,
    routing it back into the dead band → `-EIO` on a healthy volume. The quarantine
    is in-RAM for the running instance; a reboot rebuilds `used_clusters` from the
    blob extents (now pointing at the new cluster) and the control-plane reassembles
    the dead band DEGRADED/RETIRED so its range is not re-served. Relocate (copy,
    healthy source) still frees the old cluster normally (`release_old=true`).
- `bdev_lvol_remap_clusters {name, tier_name, clusters:[{cluster_num, dst_lba_start, dst_lba_count}]}`
  — **batch no-copy remap**: the no-copy analogue of `bdev_lvol_relocate_clusters`.
  Re-homes N lost clusters (DEGRADED source → ACTIVE dst) under the **same per-item
  guards** as the single `bdev_lvol_remap_cluster` (shared `remap_one_precheck`);
  **no freeze, no copy, no `verify`** (the source is dead — nothing to drain or
  read back). Sequential; **stops at the first per-item error** releasing that
  item's un-committed claim, and returns `{remapped, requested, error}` —
  **partial success is a 200** (caller retries the tail from `remapped`). Bounded
  to 4096 items. Old clusters are QUARANTINED per item (`release_old=false`, R11).
  Crash-safety per item is identical to the single remap (invariant B); the
  control-plane journals the remap set and re-drives the tail + range rebuild at
  restart (**PR3**). Re-issuing a partially-committed batch is safe: already-moved
  clusters no longer sit on a DEGRADED band, so their per-item guard cleanly
  `-EINVAL`s rather than double-moving.
- **Cluster claim performance (deferred #4, patch 0004).** The claim under both
  batch RPCs (`spdk_blob_claim_cluster_in_range`) is **word-wise** (skips
  fully-allocated 64-bit words of the `used_clusters` bit-pool in one step) with an
  optional **resume cursor** (`spdk_blob_claim_cluster_in_range_from`): a batch that
  fills one band claims in **O(bandsize + N)** rather than O(N²), so a long
  demotion/evac campaign shows flat per-cluster claim cost. Semantics are
  **unchanged** (first free cluster in the window, `-ENOSPC` when full); the cursor
  is a hint only — never a correctness input — so a stale/over-shot cursor is always
  safe (the live windowed scan bounds every result). The batch threads one cursor
  per dst band window and resets it when the window changes.

## Capacity / ENOSPC (patch 0009)

- A raid1 write to a **thin** member that is full returns NVMe
  `CAPACITY_EXCEEDED` to the host **without** failing the member (**G3** — no
  silent degradation). The write is NACKed.
- **Divergence, by design.** Both legs stay ONLINE, but the leg whose write
  failed and the leg whose write succeeded now **disagree on that block**. This
  is correct block semantics (a NACKed write has indeterminate content) but the
  fork does **NOT** auto-reconverge. The control-plane must treat that LBA's
  content as undefined until it is rewritten, and must keep thin reservations
  symmetric across legs (placement-side) so the case stays rare.

## Repair / rebuild

- `bdev_raid_rebuild_ranges {name, ranges:[{start_lba,num_blocks}]}` — **C2**:
  each chunk repaired under a channel-owned LBA lock (host writes held +
  replayed). **P-3**: parity raids require **full-stripe-aligned** ranges
  (-EINVAL else); the caller must align. **N-6**: a REMOVE aborts at the next
  chunk (-ENODEV) — re-drive.
  - **C3 — geometry is published, not guessed.** Read `full_stripe_blocks` from
    the raid bdev's `bdev_get_bdevs` → `driver_specific.raid` (emitted for any
    striped raid). It is EXACTLY the alignment `rebuild_ranges` validates:
    `strip_size × min_base_bdevs_operational` (for raid5f, `min == num-1 == k`,
    the data-chunk count). Align every range to it. Do **not** re-derive `k`
    from `num_base_bdevs_operational` — for a healthy raid5f that field is `n`
    (all members), not `k`; only `full_stripe_blocks` (or `min_base_bdevs_operational`)
    gives the data-chunk count. `strip_size_kb`, `num_base_bdevs`,
    `num_base_bdevs_operational` remain available for cross-checks.
  - **EC reconstruct precondition (load-bearing).** The repair reconstructs a
    lost chunk only because the degraded member returns **read errors** (present
    band DEGRADED, patch 0006) or is **absent** (NULL channel, upstream) — either
    triggers a parity reconstruct-read that the write-back then re-lays with fresh
    parity. A member replaced by a **healthy, zeroed** disk reads its zeros
    *successfully*, so no reconstruct fires and the repair would rewrite zeros.
    The control-plane MUST therefore drive `rebuild_ranges` while the target
    member is still **DEGRADED/absent** (the `remap → rebuild` order, PR3), NOT
    after swapping in a fresh member. See
    `docs/audits/2026-07-04_revue-ec-rebuild-ranges.md`.
  - An unrecoverable stripe (>1 fault) fails the whole call with -EIO; the target
    logs the offending chunk LBA/range (`raid repair: unrecoverable read at
    chunk …`). Repair is idempotent — re-drive the tail after fixing redundancy.
- `bdev_raid_add_base_bdev {…, skip_rebuild}` — **CBT-3/P5**: skip_rebuild is a
  "trust me" primitive; the control-plane MUST prove the residual delta is zero
  (garde résidu-nul + INV-37) before calling. **CBT-6**: a channel-promotion
  failure now unwinds fully and returns an error (the member is not left
  half-wired) — treat as retryable. **CBT-7**: the SB flips to CONFIGURED only
  after the unquiesce; a crash between the two costs a full rebuild at reboot
  (conservative, safe).

## CBT epochs / rebuild

- `bdev_cbt_epoch_freeze` / `epoch_close` / `epoch_open` (higher generation):
  return **-EBUSY** while a rebuild is RUNNING on the epoch (**CBT-1/2/c5**) —
  the control-plane must `bdev_cbt_cancel_rebuild` first.
- `bdev_cbt_partial_rebuild` / `bdev_cbt_start_rebuild`: one rebuild per
  (cbt, epoch); a second returns -EBUSY (interpret as "already running", not a
  failure). **CBT-4**: the rebuild FLUSHes the target before COMPLETED.
  - **R1 — an aborted rebuild is NOT `completed`.** A source/target/cbt hot-remove
    mid-rebuild yields `bdev_cbt_get_rebuild_status` state **`aborted`** (distinct
    from `failed` = an I/O error, and `completed`) with `completed: false`. The
    delta was NOT fully copied, so the control-plane must **resume** the rebuild,
    never treat the member as synced. (Previously a mid-rebuild abort with no failed
    I/O reported `completed` → silent under-replication.)
- `bdev_cbt_reset`: refused while any epoch is active. Bitmap clearing is
  reset-driven (**D3** — no automatic healthy-clear).
- **Breaking (D3)**: `bdev_cbt_epoch_list` no longer emits
  `healthy_clear_suspended` nor `backends_healthy` — both fields' backing state
  was removed with the dead healthy-clear poller (`backends_healthy` was
  constant `false`: `bdev_cbt_set_backends_healthy` never had a caller). Strict
  parsers must drop these keys.
- `bdev_cbt_epoch_invalidate`: refused with `-EBUSY` while a rebuild is RUNNING
  on the epoch (**C3** — an INVALID epoch is evictable; evicting it under the
  rebuild would free the frozen bitmap the rebuild is scanning). Cancel first.
- **H1 (delta preservation)**: a frozen delta that was exchanged out of the
  live bitmap and never proven copied (rebuild aborted/failed/never run) is
  merged back into the live bitmap before its buffer is discarded (re-freeze,
  close, evict). A freeze retry therefore captures (unconsumed delta ∪ new
  writes) — a failed iteration never loses chunks under `skip_rebuild`.
- After a base-bdev hot-remove the cbt vbdev is NOT silently recreated with a
  virgin bitmap (**c4**); recreation is an explicit `bdev_cbt_create` and the
  delta history is considered lost (epoch_invalidate → full rebuild if needed).

## Pause barrier (patch 0003, SPEC-66 H10)

- `nvmf_subsystem_pause {nqn, nsid?, ttl_ms?}` — a **standing, drain-certified**
  barrier: the 200 OK certifies the drain. Returns an opaque `token`
  (`<instance>:<epoch>`) and the applied `ttl_ms` (clamped to 60 s, logged if
  clamped). Idempotent re-pause refreshes the TTL. TTL auto-resumes a leaked
  pause. **The registry entry is preallocated before the pause dispatch** — a
  Paused subsystem always gets its entry+TTL (no orphan pause on OOM).
- `nvmf_subsystem_resume {nqn, token?}` — reports `barrier_intact`: false means
  the freeze broke (TTL/crash/other) between pause and resume, so the controller
  must drop any snapshot taken under it. A stale `boot_id` implies the token is
  dead.

## Invariants the control-plane owns (P1–P5)

- **P1** geometry is SB-authoritative (highest seq); CRD is intent.
- **P2** relocate/remap target the lvol's own composite (enforced fork-side).
- **P3** remap journaled before the call; range rebuild re-driven at restart.
- **P4** skip_rebuild only after a proven-zero residual delta.
- **P5** the reintegration order is `pause → final freeze → copy delta →
  add_base_bdev(skip_rebuild) → await callback → resume`; verify by conformance
  test.
