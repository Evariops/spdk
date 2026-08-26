# Evariops fork — JSON-RPC contract

Normative contract sheet for the JSON-RPC surface the CSI control-plane drives: per RPC, preconditions, idempotence, and behavior across a target crash.

**Probe before you assume.** Call `evariops_get_capabilities` first. Everything here is fork-only; on vanilla SPDK these methods return JSON-RPC `-32601`.

Terms. A **tier** is a composite bdev built from **bands**, fixed-geometry extents each carved out of one disk. An **epoch** is a bounded change-tracking (cbt) round: a bitmap of dirty chunks the control-plane freezes, reads and consumes. An **incarnation** identifies the control-plane process that created a raid. A **seeded rebuild** copies only the ranges the caller supplies. An **envelope** is a bandwidth cap on a class of background work.

## Global assumptions

- **Single reactor (`-m 0x1`).** The standing-pause registry (patch 0003), `g_relocate_inflight` (patch 0005), `band->state` and the heat counters assume all RPC handlers run on one reactor, lock-free; `nvmf_subsystem_pause` logs an error if it sees more than one. Widening the CPU mask requires a concurrency pass first. `evariops_get_capabilities` reports `single_reactor_assumed: true`.
- **Volatile state dies with the process.** Standing pauses, in-flight relocations, cbt epochs and rebuilds are RAM-only; a restart loses them all. Detect it via `boot_id` and reconcile.
- **Destructive RPCs are audited.** Every mutation handler (`bdev_tier_delete`, `bdev_tier_retire_band`, `bdev_tier_resync_md`, `bdev_lvol_relocate_cluster` / `_clusters` / `remap_cluster`, `bdev_raid_rebuild_ranges`, `bdev_raid_start_seeded_rebuild`, `nvmf_subsystem_pause`) emits an `audit rpc=<method> peer=pid:…,uid:…,gid:… <params>` NOTICELOG line with the caller's Unix-socket credentials (SO_PEERCRED, patch 0011); `peer=unknown` over TCP. This is an audit trail, **not** an authorization gate: access control is the socket mode (0600, patch 0010) plus a NetworkPolicy. `umask(077)` wraps the `bind()`, so the socket is owner-only atomically with no TOCTOU window; the chmod remains as belt-and-braces.

## evariops_get_capabilities

No params, read-only, idempotent. Returns `boot_id` (per-process uuid), `tier_sb_version`, `capabilities_schema`, `single_reactor_assumed`, `methods[]`. A changed `boot_id` across polls means the target restarted: treat all volatile state as lost. `methods[]` is filtered through the live RPC registry, so it never reports a method that then fails `-32601`; a method built in but absent from the list is a harmless false negative, and the per-call `-32601` probe stays ground truth.

## Tier lifecycle

| RPC | Preconditions | Idempotence | Crash behavior |
|:----|:--------------|:------------|:---------------|
| `bdev_tier_create` | name free; `md_num_blocks > 0`, bounded so alignment cannot overflow; `cluster_blocks` is 0 (legacy, unaligned) or ≥ 2 — **1 is rejected** | `-EEXIST` if the name is taken | in-RAM until the first superblock write |
| `bdev_tier_add_band` | tier exists, **not registered**; coherent identity tuple (see Unit identity); unique unit identity (`-ENOTUNIQ`); disk ≥ superblock + md; block length divides the 128 KiB superblock slot (T10-DIF 520/4160 rejected); no range overlap (`-EADDRINUSE`, defense-in-depth — auto-layout cannot produce one on its own); **refused `-EPROTO` on any composite that has assembled bands** — the on-disk SB may list bands whose disks are ABSENT (no in-memory placeholder), so auto-placement at the in-memory high-water mark could silently reuse an absent band's range and slot; grow an assembled composite with `assemble_band` at explicit geometry (see the mono-band growth recipe below) or reprovision | band_id auto-assigned; the caller replays deterministically | — |
| `bdev_tier_assemble_band` | tier **not registered**; `band_id < 64`; state ≤ RETIRED; coherent identity tuple; **unique unit identity** — wwn or non-zero `part_uuid` already present is `-ENOTUNIQ`, never `-EEXIST`; no range overlap (`-EADDRINUSE`); fits the disk; block length divides the slot | **`-EEXIST` exclusively on duplicate band_id** — the idempotent-replay signal, checked before the identity guards so a true replay is never misreported; **`-EBUSY` once registered** | places the band at its stored geometry |
| `bdev_tier_register` | ≥ 1 band; cluster-aligned geometry | **`-EEXIST` if already registered** | rehydrates `t->seq` from the on-disk superblocks, then persists on success |
| `bdev_tier_retire_band` | not an md-mirror band, else `-EBUSY` | idempotent: a re-run re-persists and re-closes | async; acks only once the superblock is durable. `rc ≠ 0` means retry |
| `bdev_tier_resync_md` | target is a DEGRADED md leg; a healthy source leg exists | re-runnable; the leg stays DEGRADED on failure | copies under an md-range quiesce; acks after activate + persist |
| `bdev_tier_delete` | — | `-ENODEV` if absent | unregister, then destruct |
| `bdev_tier_get_bands`, `bdev_tier_read_sb` | — | read-only | `read_sb` returns the highest-seq valid slot plus `generation_uuid` |

**Assembly rules.** `bdev_tier_read_sb` exposes `version`, `seq`, `generation_uuid`, `created_epoch_sec`. Read every candidate disk's superblock, group by `generation_uuid` (this fences stale disks from a previous instance), take the highest `seq` per band, and when the two md legs disagree on `seq`, assemble the higher one ACTIVE and the other DEGRADED, then `bdev_tier_resync_md`. The fork persists DEGRADED but cannot arbitrate a split-brain across disks; that is the control-plane's.

**Growing a mono-band composite.** A composite created with a single band (one md leg, no mirror) grows a second leg without `add_band`: pre-register, `assemble_band` the new disk with `is_md: true`, `state: DEGRADED` and the geometry computed by the caller (the module accepts an md assemble while a mirror slot is free), then after `register`, `bdev_tier_resync_md` onto that slot copies the md range under quiesce and activates it. This is the ONLY sanctioned growth path for a composite with assembled bands — `add_band`'s auto-promotion would hand the blank disk an ACTIVE md slot with no resync.

**Unit identity.** `bdev_tier_add_band` and `bdev_tier_assemble_band` accept three optional parameters: `part_uuid` (**exactly 32 lowercase hex chars** decoding to the 16-byte opaque unit identity — a partition's PARTUUID; absent/empty = all-zero = whole disk), `part_start_lba` and `part_size_blocks` (the unit's geometry on its **parent** device; 0/0 for a whole disk). Exactly **two encodings are admitted** (`-EINVAL` otherwise): all-zero uuid + 0/0 geometry (whole disk), or a non-zero uuid + a real geometry (`part_size_blocks > 0`) — a mixed tuple is refused at admission instead of persisting a state the reassembly contract declares impossible. The module stores what it is told and persists it per band in the superblock — it never probes partitions; the live-vs-stored comparison (identity match, geometry **equality**, not "still fits") is the control plane's, at reassembly. `bdev_tier_read_sb`, `bdev_tier_get_bands` and the diagnostic `bdev_get_bdevs` → `driver_specific.tier` band list all emit the three fields per band (`part_uuid` as a hex string, **empty when all-zero**). No superblock version bump: an all-zero identity is byte-identical to the pre-field format (see `docs/FORMAT-tier-superblock.md`, including the **rollback caveat** — an older image's first persist silently zeroes the stored identities).

**Per-band fill accounting (patch 0041).** `bdev_tier_get_bands` emits `capacity_blocks` (= `num_blocks`), `used_blocks` and **`usage_valid`** per band. `used_blocks` comes from a usage provider the **lvol layer** registers on the composite when an lvolstore loads on it: the provider counts the blobstore's allocated clusters in the band's LBA range — word-wise popcount over the `used_clusters` bit pool under `used_lock`, so a whole-disk band costs microseconds per poll, never a bit-by-bit reactor stall — and returns `count × cluster_blocks`. `usage_valid: false` means the number was NOT measured (no provider — no lvolstore on the composite, the target just restarted and the load has not completed, or the tier grain is not a whole multiple of the lvolstore cluster so counting was refused at registration — or a provider error): `used_blocks` is then 0, never a stale number, and the consumer reads it as "unknown", not "empty", without any out-of-band knowledge. The provider registers only when `cluster_blocks ≥ 2` divides evenly by the lvolstore's cluster (a legacy grain-0 composite reports unknown forever), and is **cleared at teardown initiation** — before the lvolstore unload starts — so a poll can never race the store's destruction. `used_blocks` deliberately has this ONE surface: the `bdev_get_bdevs` diagnostic dump carries identity and capacity but no usage, so an unqualified copy of the number cannot circulate.

**Sequence monotonicity is fork-owned.** The control-plane never threads `seq` back through `create`/`assemble`/`register` — it reads `seq` only to pick the authoritative superblock. So `bdev_tier_register` re-reads the bands' on-disk superblocks and seeds `t->seq` above the highest value found before its first persist; otherwise a fresh instance would write `seq 1`, be out-voted forever under highest-seq-wins, and silently reassemble stale geometry.

## Data plane — relocate and remap (patch 0005)

| RPC | Preconditions | Execution | Result and crash behavior |
|:----|:--------------|:----------|:--------------------------|
| `bdev_lvol_relocate_cluster {name, tier_name, cluster_num, dst_lba_start, dst_lba_count}` | lvol lives on `tier_name`'s composite, else `-EINVAL`; not a snapshot blob, else `-EBUSY`; one relocate or remap in flight per blob, else `-EBUSY` | copy + commit under an **lvol-bdev quiesce** (drain outstanding host I/O, then hold new I/O above the L2P translation) plus an inner blob freeze; the drain is what makes the copy source stable. That lvol's I/O stalls ≈ drain + 3 cluster times. The blob is pinned by an own open-ref throughout | a crash mid-operation leaves an orphan cluster that native blobstore replay reclaims — never a lost ACKed write. An ambiguous commit failure (extent write dispatched, error completion) **quarantines** the destination cluster until restart instead of releasing it, since the durable extent may reference it |
| `bdev_lvol_relocate_clusters {…, clusters:[…], verify?}` | same per item | same per item, but one quiesce + freeze amortized over N ≤ 4096 clusters; `verify` (default `true`) forwards to each copy, `false` skips the read-back on trusted media | `{relocated, requested, error}`; **partial success is a 200** — retry the tail from `relocated`. Per-item crash behavior identical |
| `bdev_lvol_remap_cluster {…}` | source band DEGRADED, destination band ACTIVE | no copy: re-homes the cluster only. Intentionally relaxes the no-lost-write invariant (the cluster is already lost) — the subsequent `bdev_raid_rebuild_ranges` fills the new one | journal the remap durably **before** calling, and re-drive the range rebuild at restart until confirmed |
| `bdev_lvol_remap_clusters {name, tier_name, clusters:[{cluster_num, dst_lba_start, dst_lba_count}]}` | same per item (shared `remap_one_precheck`) | **no freeze, no copy, no `verify`** — the source is dead, so there is nothing to drain or read back. Sequential, N ≤ 4096 | **stops at the first per-item error**, releasing that item's un-committed claim; returns `{remapped, requested, error}` with **partial success as a 200**. Journal the set and re-drive the tail plus the range rebuild at restart. Re-issuing a partially committed batch is safe: clusters already moved no longer sit on a DEGRADED band, so their guard returns `-EINVAL` instead of double-moving them |

**The old cluster is quarantined, not freed** (both remap forms). The commit keeps it marked-allocated (`release_old=false`); freeing it would let the allocator re-serve that LBA to a host write, routing it back into the dead band and returning `-EIO` on a healthy volume. The quarantine is in-RAM: after a reboot `used_clusters` is rebuilt from the blob extents, which now point at the new cluster, and the control-plane reassembles the dead band DEGRADED or RETIRED so its range is never re-served. Relocate, copying from a healthy source, still frees the old cluster (`release_old=true`).

**Cluster claim cost (patch 0004).** The claim behind both batch RPCs (`spdk_blob_claim_cluster_in_range`) scans the `used_clusters` bit-pool word-wise and takes an optional resume cursor (`spdk_blob_claim_cluster_in_range_from`), so a batch filling one band claims in O(bandsize + N) rather than O(N²). Semantics are unchanged — first free cluster in the window, `-ENOSPC` when full — and the cursor is a hint, never a correctness input: the live windowed scan bounds every result, so a stale or over-shot cursor is safe. The batch threads one cursor per destination band window and resets it when the window changes.

## Capacity and ENOSPC (patch 0009)

A raid1 write to a **thin** member that is full returns NVMe `CAPACITY_EXCEEDED` to the host **without** failing the member: the write is NACKed and no leg is silently degraded away. **Divergence follows, by design**: both legs stay ONLINE but now disagree on that block. A NACKed write has indeterminate content, so this is correct block semantics, and the fork does **not** auto-reconverge. Treat that LBA as undefined until it is rewritten, and keep thin reservations symmetric across legs (placement-side) so the case stays rare.

## Repair and rebuild

`bdev_raid_rebuild_ranges {name, ranges:[{start_lba, num_blocks}]}`

- Each chunk is repaired under a channel-owned LBA lock; host writes are held and replayed.
- Parity raids require **full-stripe-aligned** ranges, else `-EINVAL`. The caller aligns.
- A member REMOVE aborts at the next chunk with `-ENODEV`; re-drive.
- An unrecoverable stripe (more than one fault) fails the whole call `-EIO` and logs the offending chunk range (`raid repair: unrecoverable read at chunk …`). Repair is idempotent: re-drive the tail once redundancy is restored.
- **Geometry is published, not guessed.** Read `full_stripe_blocks` from `bdev_get_bdevs` → `driver_specific.raid` (emitted for any striped raid); it is exactly the alignment this RPC validates, `strip_size × min_base_bdevs_operational` (for raid5f, `min == num - 1 == k`, the data-chunk count). Do **not** re-derive `k` from `num_base_bdevs_operational`: on a healthy raid5f that field is `n`, all members. Only `full_stripe_blocks` or `min_base_bdevs_operational` gives the data-chunk count; `strip_size_kb`, `num_base_bdevs` and `num_base_bdevs_operational` remain for cross-checks.
- **Order: remap, then rebuild.** Drive `rebuild_ranges` while the target member is still DEGRADED or absent, never after swapping in a fresh member. The repair reconstructs a lost chunk only because the member returns read errors (band present and DEGRADED, patch 0006) or is absent (NULL channel, upstream); a healthy zeroed replacement reads its zeros *successfully*, no parity reconstruct fires, and the repair rewrites zeros over live data.

`bdev_raid_add_base_bdev {…, skip_rebuild}` — `skip_rebuild` is a "trust me" primitive: prove the residual delta is zero before calling. A channel-promotion failure unwinds fully and returns an error, never leaving the member half-wired; treat it as retryable. The superblock flips to CONFIGURED only after the unquiesce, so a crash between the two costs a full rebuild at reboot.

## Seeded rebuild and write-only attach (patch 0013)

- `bdev_raid_add_base_bdev` accepts optional `"write_only": bool` — attach the member write-replicated and read-excluded, with no rebuild process and no superblock CONFIGURED flip. Re-attaching the same bdev in the same mode is an idempotent no-op.
- `bdev_raid_start_seeded_rebuild {name, base_bdev, ranges:[{offset_blocks, length_blocks}], rebuild_token?, expected_incarnation?}` starts the in-raid rebuild seeded with those dirty ranges, fast-advancing across clean gaps; the per-window `quiesce_range` region lock is unchanged. Async: returns `{rebuild_id}` immediately; completion promotes read-eligibility and flips the superblock to CONFIGURED. Refused `-EINVAL` if the member is not write_only-attached, `-EAGAIN` beyond the concurrent-rebuild bound, `-ESTALE` on an incarnation mismatch. Audited.
- A failed seeded rebuild removes the member (the vanilla process-failure path) and leaves the epoch FROZEN so the delta can be retried.
- Seeded rebuild is **raid1-only**: on a raid5f it refuses `-EINVAL`.

## Member observation and the extended superblock (patches 0016/0017)

- **Extended superblock**, minor version 0 → 1, carved from reserved bytes, so a minor-0 superblock reads back as generation 0 and epoch 0. Per member, `content_generation`: survivors increment it in the **same** superblock transaction that records a member's ejection, and a member completing a rebuild or a `skip_rebuild` promotion adopts the survivors' maximum generation in the same transaction as its CONFIGURED flip, so a lagging generation is durable proof of staleness for cold recovery. `view_epoch` is persisted here too. Reassembly restores both fields for **all** slots: a FAILED slot keeps its stale generation.
- **Per-member observation.** `bdev_get_bdevs` → `driver_specific.raid.base_bdevs_list[]` carries `state` (derived, precedence `failed > write_only > configured > configuring > absent`), `since` (unix seconds of the last observable state flip — the anti-flap input), `content_generation`, `view_epoch`, and, when the member is a cbt bdev tracking a live epoch, `epoch_nonce`, `epoch_state` and `truncated` (absent otherwise). Those cbt facts come from the most recently opened live epoch; closed epochs leave the list.

## Envelopes (patch 0021)

`bdev_raid_set_envelopes {rebuild|verify|relocate}_{nominal|maintenance}_mb_sec, max_concurrent_rebuilds, regime?` and `bdev_raid_get_envelopes`.

- Envelopes are **caps, never shares**. The data-plane default 0 means UNLIMITED, so a failed `set` is a control-plane incident, never a silent fallback. An unknown `regime` is `-EINVAL`.
- The rebuild cap for the current regime is frozen at process allocation and takes precedence over the legacy `bdev_raid_set_options` bandwidth setting.
- An RPC-driven seeded rebuild beyond `max_concurrent_rebuilds` is refused `-EAGAIN` (retryable); the attach-triggered automatic rebuild is admitted loudly, since refusing it would strand an attached member.
- The relocate cap is stored for the tier module; that wiring is not yet in place.

## Epoch opened at ejection (patch 0019)

On the ejection path the raid opens an automatic epoch — id `auto-<ticks>`, nonce `auto<hex>` — on **every** surviving member's cbt, with `stale_backend_id` set to the ejected member. It returns `-EEXIST` when an OPEN epoch already bounds the round (never an implicit takeover) and `-ENODEV` when the member is not cbt-wrapped; both are benign. The automatic nonce reaches the control-plane through `bdev_get_bdevs`, and that is how the round is adopted. Raid1-only.

## Pause barrier (patch 0003)

- `nvmf_subsystem_pause {nqn, nsid?, ttl_ms?}` — a standing, drain-certified barrier: the 200 OK certifies the drain. Returns an opaque `token` (`<instance>:<epoch>`) and the applied `ttl_ms`, clamped to 60 s and logged when clamped. An idempotent re-pause refreshes the TTL; the TTL auto-resumes a leaked pause. The registry entry is preallocated **before** the pause dispatch, so a Paused subsystem always has its entry and TTL — no orphan pause on OOM.
- `nvmf_subsystem_resume {nqn, token?}` reports `barrier_intact`. False means the freeze broke between pause and resume (TTL, crash, another actor), so drop any snapshot taken under it. A stale `boot_id` implies the token is dead.
- `nvmf_subsystem_resume {…, force: true}` breaks any standing barrier, loudly (WARNLOG) and audited; the barrier's own tokened resume then reports `resumed: false` / `barrier_intact: false`, so a group-snapshot saga fails cleanly and replays.
- **A pause never fails a rebuild.** A paused subsystem queues the rebuild's writes target-side and nothing in the process path times out, so an outcome is never FAILED because of a pause.

## verify_ranges — exhaustive divergence detection (patch 0022)

`bdev_raid_verify_ranges {name, ranges?:[{start_lba, num_blocks}], token?, expected_incarnation?}` is the exhaustive detector; the verify phase integrated into rebuilds is the probabilistic one. The mode follows the raid level and is reported back as `mode`: raid1 copy-compare, raid5f syndrome.

Both modes: `-EBUSY` when a background process is live, and a clean abort if one appears mid-run. Chunks are read under a channel-owned LBA lock with host writes held and replayed, paced by the verify envelope frozen at dispatch. It **never repairs** — arbitration belongs to the control-plane. No `ranges` means the whole raid; callers drive bounded batches and re-drive, as with `rebuild_ranges`. The outcome registry mirrors progress under `token` (`verifying`, then `succeeded` with `verified` on zero divergence, or `divergent`), and `expected_incarnation` carries the same `-ESTALE` semantics as elsewhere.

| Mode | Extra preconditions | Check | Reporting |
|:-----|:--------------------|:------|:----------|
| **raid1** — copy-compare | plain-data raid1 | the first configured leg is the implicit arbiter, compared against every other readable leg in 1 MiB chunks | `{verified_blocks, divergent_blocks, divergent_ranges, mode}`; `divergent_ranges` holds ≤ 128 entries alongside the exact count and a truncated flag |
| **raid5f** — syndrome | raid ONLINE; **every member readable, else `-EAGAIN`** ("repair first") — with one parity strip, a stripe already missing a strip has nothing to check against. A member read error or a membership change mid-run also aborts `-EAGAIN`. Plain-data bdevs only: interleaved metadata gives `-ENOTSUP`. Ranges must be full-stripe aligned (`full_stripe_blocks` from `bdev_get_bdevs`), else `-EINVAL`; no ranges = the whole raid, aligned by construction | recomputes the RAID-5 consistency relation per stripe: the XOR of all k+1 members' strips must be zero. The rotating parity position is irrelevant, so no raid5f internals are involved | per stripe in raid LBA space, `mode: "syndrome"`, report-only. Divergence handling is the control-plane's: re-fold the stripe from its source, or raise a content divergence. The verify envelope is charged on the k+1 strips actually read |

## Erasure coding — raid5f (patches 0022/0023)

The raid5f layer is control-plane-written only; no host ever writes to a raid5f. These contracts serve the erasure-coded image lifecycle: seal-verify, periodic verify, degraded-service observation.

- **Sealing is explicit.** The verify phase integrated into rebuilds is raid1-only and completes a raid5f rebuild UNVERIFIED (clean no-op, registry `verified=false`). A raid5f is therefore **never** auto-sealed by a rebuild: run a syndrome `verify_ranges` pass to seal it, including after `bdev_raid_rebuild_ranges` and after a full rebuild process.
- **Degraded-service counters (patch 0023).** `bdev_get_bdevs` → `driver_specific.raid` carries `reconstruct_reads_absent` (member missing — the vanilla reconstruct-read), `reconstruct_reads_error` (member present but erroring — the patch 0006 fallback), `degraded_write_stripes` (stripes written at zero margin; the control-plane freezes folds on a degraded image, so a nonzero value is a contract-violation gauge) and `last_degraded_ts` (unix seconds, 0 = never). Written on the I/O paths with relaxed atomics, valid under the single-reactor assumption, they feed the "serving degraded since T" escalation. The rebuild's own reconstruct reads are deliberately not counted: that is repair work, not degraded host service.
- **Repairing erasure-coded data** means re-provisioning the chunk and running `bdev_raid_rebuild_ranges` bounded to the allocated ranges (thin-preserving), or the native full rebuild process for a near-full image. Seeded rebuild and the ejection auto-epoch stay raid1-only.

## Incarnation — ownership of a raid (patch 0014)

`bdev_raid_create {…, incarnation}` is required (≤ 63 characters): no creation without the creating control-plane incarnation's identity. The engaging RPCs — `bdev_raid_delete`, `bdev_raid_add_base_bdev`, `bdev_raid_remove_base_bdev`, `bdev_raid_start_seeded_rebuild` — accept `expected_incarnation?` and refuse **`-ESTALE`** on mismatch, never executing best-effort. Omitting it is reserved for manual rescue tooling; the control-plane client always sends it. Superblock-reassembled raids are *unclaimed*: `incarnation` is absent from `bdev_get_bdevs`, and any engaging RPC carrying `expected_incarnation` against an unclaimed raid is `-ESTALE`. `driver_specific.raid.incarnation` exposes ownership.

## Rebuild outcome registry (patches 0015/0018)

Process-wide, survives the raid bdev; terminal entries are purged 15 minutes after finishing, on a lazy TTL.

- `bdev_raid_start_seeded_rebuild {…, rebuild_token?}` records the attempt under the control-plane token, deterministic per attempt: a retry re-opens the same entry. Full rebuilds feed the same registry under `auto:<raid>:<member>:<ticks>` tokens.
- `bdev_raid_get_rebuild_outcomes {token?}` returns `[{token, raid_bdev, base_bdev, state, bytes, verified, finished_at}]`, `state ∈ running|verifying|succeeded|failed|divergent|canceled` (lowercase on the wire), `bytes` the bytes actually copied (seeded fast-skip windows do not count), `finished_at` in unix seconds and 0 while non-terminal.
- **Cancellation.** A `bdev_raid_delete` or member removal with a rebuild in flight cancels the process, seals the outcome `canceled`, and emits **no process write after the RPC returns** — the delete reply is gated behind the process fully stopping. There is deliberately no `Cancel` RPC.
- **`verified` and the integrated verify phase.** After the copy completes and before the process concludes — the CONFIGURED flip is gated on it — 64 uniform-stride windows across the copied extent (the seed ranges for a seeded rebuild, the whole raid otherwise) are compared across two legs under `quiesce_range`, arbiter the source leg. A mismatch is re-copied from the arbiter and re-verified once; a second mismatch under the same lock seals the outcome DIVERGENT (`-EILSEQ`). Success seals `verified=true`, which unlocks `epoch_close(consumed)` for that token. The registry shows `verifying` while the phase runs, and re-copied windows count in `bytes`. Scope: a probabilistic process-bug detector over roughly 64 MiB sampled — the exhaustive check is `bdev_raid_verify_ranges`. Raid1 plain-data only; anything else completes unverified.

## CBT epochs and rebuild

Every `-EBUSY` below clears the same way: call `bdev_cbt_cancel_rebuild` on the epoch first.

| RPC | Refused when | Contract |
|:----|:-------------|:---------|
| `bdev_cbt_epoch_open {…, nonce?}` | `-EBUSY` while a rebuild is RUNNING on the epoch (opening at a higher generation) | `nonce` is an opaque control-plane string (≤ 31 chars), stored on the epoch and echoed in `bdev_get_bdevs`; it kills the epoch-id ABA across maintenance rounds. A generation takeover re-stamps the nonce for the new round |
| `bdev_cbt_epoch_freeze` | `-EBUSY` while a rebuild is RUNNING on the epoch | exchanges the live bitmap for a frozen delta |
| `bdev_cbt_epoch_close {…, mode?: "preserve"\|"consumed", rebuild_token?}` | `-EBUSY` while a rebuild is RUNNING; `-EPERM` if `consumed` without a `rebuild_token` naming a **local, `succeeded` and `verified`** outcome-registry entry; `-EINVAL` on an unknown `mode` — never a silent preserve | `preserve` (default) restores any unconsumed frozen delta to the live bitmap; `consumed` deliberately discards it under caller certification |
| `bdev_cbt_epoch_invalidate` | `-EBUSY` while a rebuild is RUNNING — an INVALID epoch is evictable, and eviction would free the frozen bitmap the rebuild is scanning | cancel the rebuild first |
| `bdev_cbt_partial_rebuild`, `bdev_cbt_start_rebuild` | `-EBUSY` if one already runs for that (cbt, epoch) pair — read it as "already running", not a failure | one rebuild per (cbt, epoch); the rebuild FLUSHes the target before reporting COMPLETED |
| `bdev_cbt_reset` | refused while **any** epoch is active | bitmap clearing is reset-driven; there is no automatic healthy-clear |

- **An aborted rebuild is not `completed`.** A source, target or cbt hot-remove mid-rebuild yields `bdev_cbt_get_rebuild_status` state **`aborted`** — distinct from `failed` (an I/O error) and from `completed` — with `completed: false`. The delta was not fully copied: resume the rebuild, and never treat the member as synced.
- **Resize under a live epoch** sets `truncated: true` on that epoch: the live bitmap does not cover the growth zone, so the delta is a lie and the control-plane must route to a FULL rebuild. Never cleared — it survives generation takeovers and dies with the epoch.
- **Delta preservation.** A frozen delta exchanged out of the live bitmap and never proven copied — rebuild aborted, failed, or never run — is merged back into the live bitmap before its buffer is discarded, on re-freeze, close and evict. A freeze retry therefore captures (unconsumed delta ∪ new writes), so a failed iteration never loses chunks under `skip_rebuild`.
- After a base-bdev hot-remove the cbt vbdev is **not** silently recreated with a virgin bitmap: recreation is an explicit `bdev_cbt_create` and the delta history is then lost (`epoch_invalidate`, then a full rebuild if needed).

**Observation surfaces.** `bdev_get_bdevs` → `driver_specific.cbt.epochs[]` gives `{epoch_id, nonce, state: open|frozen|rebuilding|completed|invalid, generation, truncated}`, the epoch-observation source, so no dedicated poll RPC is needed. `bdev_cbt_epoch_list` carries the same `nonce` and `truncated`, making the list the union of the two views; it alone carries `stale_backend_id`. The nonce is the ABA-free identity that `freeze` and `close` are addressed with: without it in this reply, no real nonce resolves and an epoch can never be frozen or consumed. **Breaking change**: `bdev_cbt_epoch_list` no longer emits `healthy_clear_suspended` nor `backends_healthy` — both fields' backing state went with the dead healthy-clear poller (`backends_healthy` was a constant `false`; `bdev_cbt_set_backends_healthy` never had a caller). Strict parsers must drop these keys.

## Invariants the control-plane owns

- **Geometry authority.** The superblock is authoritative, at the highest `seq`; the CRD is intent.
- **Relocate and remap scope.** Both target the lvol's own composite; the fork enforces this.
- **Remap durability.** Journal the remap before the call; re-drive the range rebuild at restart.
- **skip_rebuild proof.** Attach with `skip_rebuild` only after proving the residual delta is zero.
- **Reintegration order, paused path.** `pause → final freeze → copy delta → add_base_bdev(skip_rebuild) → await callback → resume`. Verified by a conformance test.
- **Reintegration order, seeded path** (see `docs/SEEDED-REBUILD-DESIGN.md`). The monotonic order is `add_base_bdev(write_only) → cbt epoch_freeze → epoch_get_dirty_ranges → start_seeded_rebuild(ranges) → completion → control-plane set-after`, with no pause anywhere. Correctness rests on the write-only attach preceding the freeze: from the attach instant every host write is replicated to the joiner, so the frozen delta is fixed and clean gaps may be skipped. The member stays read-excluded until the seeded rebuild completes. The paused path remains valid while both paths coexist.
