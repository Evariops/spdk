# Evariops SPDK patches

Out-of-tree patches applied on top of upstream SPDK during the container build
(`images/spdk/Dockerfile`). They add the primitives the tiering data plane and
the group-snapshot barrier need, and harden a few upstream paths. The two
out-of-tree bdev **modules** (`module/bdev/cbt`, `module/bdev/tier`) are copied
in whole, not patched.

## Application order

Patches are applied in **lexicographic order of filename** (`0001` … `0040`) —
the Dockerfile globs `patches/*.patch` and `git apply`s each. The numeric prefix
IS the contract; do not rely on any other ordering. Order matters:

| # | Patch | Touches | Depends on |
|--:|:------|:--------|:-----------|
| 0001 | raid `skip_rebuild` — `bdev_raid_add_base_bdev` promotes a member without a full-surface rebuild when the copy was made externally | bdev_raid | — |
| 0002 | lvol `get_allocated_ranges` — reports the allocated cluster ranges of an lvol so a caller copies only live data | bdev_lvol | — |
| 0003 | nvmf pause/resume — RPCs that stop and restart subsystem admission to open a barrier window | lib/nvmf | 0011 (audit hook) |
| 0004 | blob relocate primitives + `freeze_io` — blobstore support for moving clusters and freezing I/O around the move | lib/blob | — |
| 0005 | lvol placement/relocate/remap RPCs — expose per-cluster placement and let the control plane move clusters between tier bands | bdev_lvol, module/bdev/tier | 0004, 0011, tier module |
| 0006 | raid5f degraded-read — serves reads by reconstruction when a member is missing instead of failing them | bdev_raid | — |
| 0007 | raid nexus heat — per-member access heat accounting, exported for placement decisions | bdev_raid | — |
| 0008 | raid `rebuild_ranges` + `full_stripe_blocks` — rebuilds an explicit list of ranges under LBA locks instead of the whole surface | bdev_raid, lib/bdev (lock_lba_range) | 0006, 0011 |
| 0009 | ENOSPC → CAPACITY_EXCEEDED — maps out-of-space to a distinct status so callers stop reading it as a generic I/O error | bdev_lvol, bdev_raid | — |
| 0010 | rpc socket chmod 0600 — restricts the JSON-RPC unix socket to its owner | lib/rpc | — |
| 0011 | jsonrpc SO_PEERCRED audit hook — exposes peer credentials and the audit entry point every destructive RPC calls | lib/jsonrpc | — |
| 0012 | lvol shutdown-unload observability — logs what the lvolstore unload is still waiting on at shutdown | bdev_lvol | — |
| 0013 | raid1 seeded rebuild — attaches a member write-only and backfills only the historical delta, with no pause window (`docs/SEEDED-REBUILD-DESIGN.md`) | bdev_raid | 0001, 0008, 0011 |
| 0014 | raid incarnation identity — a raid carries a control-plane incarnation; RPCs passing `expected_incarnation` fail `-ESTALE` on mismatch | bdev_raid | 0013 |
| 0015 | raid rebuild-outcome registry — records how each rebuild ended (including a CANCELED outcome) and exposes `bdev_raid_get_rebuild_outcomes` | bdev_raid (adds `bdev_raid_outcomes.{c,h}`); `module/bdev/cbt` consults it for `epoch_close(consumed)` | 0013, 0014 |
| 0016 | raid extended superblock — per-member `content_generation`/`view_epoch` persisted in the same transaction | bdev_raid (SB minor 0→1, carved from reserved bytes) | 0013 |
| 0017 | raid per-member observation — `state`/`since`/generations plus live cbt epoch facts in `get_bdevs` | bdev_raid; `#include`s `../cbt/vbdev_cbt_query.h` (requires the cbt module, like 0005 requires tier) | 0013, 0016, cbt module |
| 0018 | raid integrated verify — sampled windows compared post-copy under `quiesce_range`, re-copied before declaring DIVERGENT, with a `verified` seal gating `epoch_close(consumed)` | bdev_raid | 0013, 0015, 0016 |
| 0019 | raid auto epoch at member ejection — opens a cbt epoch on the survivors so their tracking bounds the later delta; the nonce is reported in `get_bdevs` | bdev_raid; calls `vbdev_cbt_auto_epoch_open` (cbt module) | 0013, cbt module |
| 0020 | nvmf audited force-resume — lets the fence path break a standing pause barrier deliberately, leaving an audit record | lib/nvmf (nvmf_pause_rpc.c) | 0003, 0011 |
| 0021 | raid envelopes — per-class bandwidth caps × (nominal, maintenance) and a rebuild concurrency bound, via `bdev_raid_set/get_envelopes` | bdev_raid (adds `bdev_raid_envelopes.{c,h}`) | 0013 |
| 0022 | raid `verify_ranges` — exhaustive divergence detector, LBA-locked and envelope-paced, which reports and never repairs; raid1 compares copies, raid5f checks the XOR syndrome per stripe | bdev_raid (adds `bdev_raid_verify_ranges.c`) | 0008, 0014, 0015, 0021 |
| 0023 | raid5f degraded-service observability — reconstruct-read and degraded-write counters plus a last-event timestamp in `get_bdevs` | bdev_raid (raid5f.c increments, bdev_raid.c emission) | 0006, 0008 |
| 0024 | spdk_dd propagates bdev I/O errors — upstream ignores `success` in all three completion callbacks, so dd exits 0 while every I/O is rejected | app/spdk_dd | — |
| 0025 | `bdev_raid_clear_superblock` — authorized clear of a stale raid superblock on an unclaimed base bdev, so a new incarnation can be created over already-stamped legs | bdev_raid (bdev_raid_rpc.c only) | 0011 (audit hook) |
| 0026 | examine never auto-re-adds a returning member into a CLAIMED raid — re-admission belongs to the control plane; unclaimed raids keep upstream boot reassembly | bdev_raid (bdev_raid.c examine_sb) | 0014 (incarnation field) |
| 0027 | a dead-socket DISCONNECTING qpair re-aborts its stragglers — a request parked in an accel op is otherwise never completed, so the disconnect never ends and the dead controller pins its TRID | lib/nvme (nvme_tcp.c process_completions) | — |
| 0028 | a delete landing inside the reconnect-delay window completes its deferred destruct — upstream returns without unregistering, leaking the dead ctrlr and its TRID until the loss timeout | module/bdev/nvme (bdev_nvme.c) | 0027 |
| 0029 | nvme_ctrlr refs named by holder — every get/put is tagged into a per-category ledger, so a deferred destruct says who is holding it | module/bdev/nvme (bdev_nvme.c + bdev_nvme.h) | 0028 |
| 0030 | raid `remove_base_bdev` names the unaddressable slot — when a member's bdev is gone the open fails and the slot squats the raid, so every slot still referencing the name is logged with its full state | module/bdev/raid (bdev_raid_rpc.c only) | 0014 |
| 0031 | a failed base-bdev configure releases what it took — the superblock-read failure path kept desc, module claim and io channel, pinning the dead member's controller destruct | module/bdev/raid (bdev_raid.c) | 0029 |
| 0032 | the delayed-reconnect path yields instead of spinning — a sticky disconnect failure re-entered itself unpaced, so the delay timer is armed directly for one attempt per `reconnect_delay_sec` | module/bdev/nvme (bdev_nvme.c) | 0028 |
| 0033 | member removal tells the truth — `-EALREADY` (removal in flight) is distinct from `-ENODEV` (not a member), and a failed removal carries `remove_error` instead of returning the member to `configured` | module/bdev/raid (bdev_raid.c + bdev_raid.h) | 0014, 0017, 0030 |
| 0034 | raid verify is single-flight and bounded — one verify at a time per raid, named and dated in `get_bdevs`, resumable at `start_lba` under a `max_blocks` budget that answers `complete`/`next_lba` | module/bdev/raid (bdev_raid.{c,h}, bdev_raid_verify_ranges.c) | 0022 (the verify it disciplines) |
| 0035 | a raid delete stops its running background process instead of waiting for it, the abort path skips QoS pacing, and the final nvme_ctrlr ref release is logged like every deferred one | module/bdev/raid, module/bdev/nvme | 0021 (the QoS pacing its abort bypasses), 0029 (the ref ledger it completes) |
| 0036 | an absent nvmf subsystem is a fact — `nvmf_subsystem_query` answers `-ENODEV` rather than INVALID_PARAMS, so absent-tolerant callers stop reading "gone" as "bad request" | lib/nvmf (nvmf_rpc.c) | — |
| 0037 | `bdev_raid_claim` — compare-and-set claim of an examine-reassembled raid for a control-plane incarnation; `-EEXIST` names the current owner | module/bdev/raid (bdev_raid_rpc.c) | 0014 |
| 0038 | a PREEMPT-ABORT from a non-registrant no longer kills the target — the refusal path returns before allocating `ns->preempt_abort`, while both readers test only the command bytes and dereference the NULL | lib/nvmf (subsystem.c) | — |
| 0039 | an armed delayed reconnect owns the continuation — the path returns instead of re-driving a failed disconnect, ending a mutual recursion that overflows the reactor stack | module/bdev/nvme (bdev_nvme.c) | 0032 |
| 0040 | the delete-stop only MARKS — 0035's stop drove the window machinery from a foreign thread, unquiescing under in-flight requests; it now sets STOPPING with `-ECANCELED` and each resting state concludes | module/bdev/raid (bdev_raid.c) | 0035, 0039 |

0005 `#include`s `vbdev_tier.h` and adds `-I module/bdev/tier` to the lvol module
CFLAGS via its own Makefile hunk; the Dockerfile injects the module dirs before
applying patches (copy-before-apply ordering matters).

**0011 is a shared substrate, not a leaf.** It adds `spdk_jsonrpc_request_audit()`
+ `spdk_jsonrpc_request_get_peer_ucred()` to `lib/jsonrpc`, which the destructive
handlers in 0003 (pause), 0005 (relocate/remap), and 0008 (rebuild_ranges) call —
so those numerically-earlier patches reference a symbol added by 0011. This is
sound because the series is applied **as a whole** before anything is compiled
(the Dockerfile `git apply`s all of `patches/*.patch`, then builds once); the
number is a *filename apply order*, not an incremental-compile order. It sits last
to avoid renumbering the existing `Evariops 000X` labels baked into every commit.

## Makefile / build wiring (NOT patches)

The module registration is done by four `sed -i` edits in the Dockerfile, not by
patches, because they are one-line list insertions that would conflict on every
upstream bump. They are **fragile** (a `sed` that finds no anchor silently does
nothing, unlike `git apply` which errors) — if a build ever links without
`bdev_tier`/`bdev_cbt`, check those `sed` anchors first:

- `module/bdev/Makefile`: `DIRS-y += … cbt tier`
- `mk/spdk.modules.mk`: `BLOCKDEV_MODULES_LIST += bdev_cbt` / `bdev_tier`
- `mk/spdk.lib_deps.mk`: `DEPDIRS-bdev_cbt`/`bdev_tier` + `DEPDIRS-bdev_lvol … bdev_tier`

## Upstream pin

The upstream commit is pinned in `images/spdk/Dockerfile` (`ARG SPDK_COMMIT_SHA`)
and verified after clone — a moved tag fails the build. `scripts/patches.sh`
reads the same ARG, so tooling and build never disagree.

## Tooling — `scripts/patches.sh`

Never hand-edit hunk offsets: a patch whose hunk line counts were corrected by
hand is proof of manual editing gone wrong. Instead:

```sh
# Apply the series into a local SPDK checkout that is at the pinned commit.
scripts/patches.sh apply   /path/to/spdk

# Dry-run the whole series in order; stops at the first that fails.
scripts/patches.sh check   /path/to/spdk

# Zero-network-state check: clone the pinned upstream to a temp dir and check.
scripts/patches.sh verify

# Regenerate patches/*.patch from a worktree with one commit per patch,
# in order, on top of the pinned base commit.
scripts/patches.sh regen   /path/to/spdk-worktree
```

### To change a patch

1. Build a worktree: clean SPDK checkout at the pinned commit (`vendor/spdk`
   clones offline), apply the series **one commit per patch, in order**, with
   `git apply` + `git commit -m "<patch filename stem>"` — the commit SUBJECT
   must be the `NNNN-name` stem, it becomes the regenerated filename.
2. Edit the source, `git commit --amend` (or a fixup) into the owning commit;
   a NEW patch = a new commit with the next `NNNN-name` subject.
3. `scripts/patches.sh regen <worktree>` to rewrite `patches/*.patch`.
4. `scripts/patches.sh verify` (or `check` against a pinned clone) to confirm
   the whole series still applies.

**Series format: RAW `git diff` output, no mail header.** `git apply` (Dockerfile
+ `check`/`apply`) is the only consumer; **`git am` is NOT part of the contract**
— it chokes on raw diffs. `regen` emits raw per-commit diffs with the commit
subject as filename, so a regen of an untouched series is byte-stable. Never
hand-edit hunks.

## Upstreaming

Candidates, easiest first: 0006 (degraded-read, small/general), 0003
(pause/resume), 0002 (allocated_ranges). 0004 (blob relocate) needs an RFC. Each
patch merged upstream removes rebase surface here.
