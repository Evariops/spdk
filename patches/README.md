# Evariops SPDK patches

Out-of-tree patches applied on top of upstream SPDK during the container build
(`images/spdk/Dockerfile`). They add the primitives the SPEC-73 tiering
data-plane and the SPEC-66 group-snapshot barrier need, and harden a few
upstream paths. The two out-of-tree bdev **modules** (`module/bdev/cbt`,
`module/bdev/tier`) are copied in whole, not patched.

## Application order (U-5/U-6)

Patches are applied in **lexicographic order of filename** (`0001` … `0012`) —
the Dockerfile globs `patches/*.patch` and `git apply`s each. The numeric prefix
IS the contract; do not rely on any other ordering. Order matters:

| # | Patch | Touches | Depends on |
|--:|:------|:--------|:-----------|
| 0001 | raid skip_rebuild | bdev_raid | — |
| 0002 | lvol get_allocated_ranges | bdev_lvol | — |
| 0003 | nvmf pause/resume | lib/nvmf | 0011 (audit hook) |
| 0004 | blob relocate primitives + freeze_io | lib/blob | — |
| 0005 | lvol placement/relocate/remap RPCs | bdev_lvol, module/bdev/tier | 0004, 0011, tier module |
| 0006 | raid5f degraded-read | bdev_raid | — |
| 0007 | raid nexus heat | bdev_raid | — |
| 0008 | raid rebuild_ranges + full_stripe_blocks (C3) | bdev_raid, lib/bdev (lock_lba_range) | 0006, 0011 |
| 0009 | ENOSPC → CAPACITY_EXCEEDED | bdev_lvol, bdev_raid | — |
| 0010 | rpc socket chmod 0600 | lib/rpc | — |
| 0011 | jsonrpc SO_PEERCRED audit hook (SEC1) | lib/jsonrpc | — |
| 0012 | lvol shutdown-unload observability | bdev_lvol | — |
| 0013 | raid1 seeded rebuild (write_only attach + range-seeded backfill) | bdev_raid | 0001, 0008, 0011 |
| 0014 | raid incarnation identity + `expected_incarnation` ⇒ -ESTALE (GCCP 0014.4) | bdev_raid | 0013 |
| 0015 | raid rebuild-outcome registry + CANCELED contract + `bdev_raid_get_rebuild_outcomes` (GCCP 0014.5) | bdev_raid (adds `bdev_raid_outcomes.{c,h}`); `module/bdev/cbt` consults it for `epoch_close(consumed)` | 0013, 0014 |
| 0016 | raid extended superblock — per-member `content_generation`/`view_epoch`, same-transaction (GCCP 0014.7, V-2) | bdev_raid (SB minor 0→1, carved from reserved bytes) | 0013 |
| 0017 | raid per-member observation — `state`/`since`/generations + cbt live-epoch facts in get_bdevs (GCCP 0014.6) | bdev_raid; `#include`s `../cbt/vbdev_cbt_query.h` (requires the cbt module, like 0005 requires tier) | 0013, 0016, cbt module |
| 0018 | raid integrated verify — K=64 sampled windows post-copy under quiesce_range, re-copy before DIVERGENT, `verified` registry seal gating `epoch_close(consumed)` (GCCP 0014.8) | bdev_raid | 0013, 0015, 0016 |
| 0019 | raid auto epoch at member ejection — survivors' cbt bounds the delta, nonce reported via get_bdevs (GCCP 0014.10) | bdev_raid; calls `vbdev_cbt_auto_epoch_open` (cbt module) | 0013, cbt module |
| 0020 | nvmf explicit audited force-resume — fence path breaks a standing barrier by design, DÉC-11 (GCCP 0014.11) | lib/nvmf (nvmf_pause_rpc.c) | 0003, 0011 |
| 0021 | raid envelopes — per-class caps ×(nominal, maintenance) + rebuild concurrency bound, `bdev_raid_set/get_envelopes` (GCCP 0014.9) | bdev_raid (adds `bdev_raid_envelopes.{c,h}`) | 0013 |
| 0022 | raid verify_ranges — exhaustive divergence detector, LBA-locked chunks, verify-envelope paced, reports & never repairs (GCCP 0014.12). **Bi-mode** (SPEC-75G F-b): raid1 = copy-compare; raid5f = XOR-syndrome per stripe (all members required, -EAGAIN when degraded; ranges full-stripe-aligned) | bdev_raid (adds `bdev_raid_verify_ranges.c`) | 0008, 0014, 0015, 0021 |
| 0023 | raid5f degraded-service observability (SPEC-75G F-d) — `reconstruct_reads_absent`/`reconstruct_reads_error`/`degraded_write_stripes`/`last_degraded_ts` in get_bdevs, relaxed-atomic counters on the I/O paths | bdev_raid (raid5f.c increments, bdev_raid.c emission) | 0006, 0008 |
| 0024 | spdk_dd propagates bdev I/O errors — upstream ignores `success` in all three bdev completion callbacks, so dd exited 0 while every write was rejected (found by vec-smoke); a silently failed READ is worse (stale buffer written as data) | app/spdk_dd | — |
| 0025 | `bdev_raid_clear_superblock` — CP-authorized clear of a stale raid SB on an UNCLAIMED base bdev (cross-node nexus republish: upstream delete never erases SBs, so a new-incarnation create over stamped legs is refused; found live by QA SurvivesPodCrash 2026-08-03). Write-open refuses claimed legs; no valid SB = idempotent no-op (never writes to a non-stamped bdev); zeroes the signature block only | bdev_raid (bdev_raid_rpc.c only) | 0011 (audit hook) |
| 0026 | examine never auto-re-adds a returning member into a CLAIMED raid — re-admission belongs to the control plane (D7 write_only + seeded chain). The examine re-add + auto-rebuild raced the CP's own attach on a live raid with a flapping remote member and corrupted the heap (SIGSEGV, caught live 2026-08-03). Unclaimed (examine-assembled) raids keep upstream behavior — boot reassembly is the SB's purpose | bdev_raid (bdev_raid.c examine_sb) | 0014 (incarnation field) |
| 0027 | a dead-socket DISCONNECTING qpair re-aborts its stragglers — the disconnect-time abort skips requests inside an accel op; when accel completes, the request can't complete alone (its send_ack died with the closed socket) and nothing re-runs the abort, so on a no-poll-group qpair (the admin qpair a reset/destruct drives) the flush-failure branch of process_completions returns 0 forever: DISCONNECTING never ends, the pending ctrlr destruct wedges, and the dead controller PINS its TRID against every re-attach (silent-death reintegration parked 8+ min, caught live turing runs 10-11, SPEC-77L Q-010-2 couche 3 / Q-009-1 family). Run 12 proved it necessary but NOT sufficient — see 0028 | lib/nvme (nvme_tcp.c process_completions) | — |
| 0028 | a delete landing inside the reconnect-delay window completes its deferred destruct — `bdev_nvme_reconnect_delay_timer_expired`'s destruct branch returned without acting: no reconnect (right), but adminq poller left paused and NOBODY ever unregisters (wrong) — the leaked continuation kept the dead ctrlr (and its TRID) alive until ctrlr_loss_timeout (~10 min, the observed self-resolution ×3). Mirrors OP_COMPLETE_PENDING_DESTRUCT. Plus destruct-deferral observability (NOTICE at delete-request, put_ref deferral, timer expiry) so a residual wedge NAMES its holder (run 13 is a measurement, not a coin flip). Upstream master has the same hole | module/bdev/nvme (bdev_nvme.c) | 0027 (same incident family) |
| 0029 | nvme_ctrlr refs ventilated by named holder — run 14 (powercycle) showed the other 0028 form: delete OUTSIDE the delay window, `ref 3 → destruct deferred: ref 2` held the WHOLE run with nothing naming the holders. Every get/put is tagged (`base/ns/qpair/disable/cache_clear/ana/keys`, per-category ledger in nvme_ctrlr, underflow self-reports), the three 0028 probes print the breakdown, a ref TAKEN on a destructing ctrlr logs its taker (the zombie-reconnect re-pin vector), and the `-EALREADY` re-delete path — which flattens to rc=0 so the CP's detach loop reads "Applied" ×308 while nothing changes — samples the breakdown on change + every 64th as heartbeat: the futile detach loop becomes a free periodic probe | module/bdev/nvme (bdev_nvme.c + bdev_nvme.h) | 0028 (extends its probes) |
| 0030 | raid remove_base_bdev names the unaddressable slot — the RPC resolves the member with `spdk_bdev_open_ext(name)` FIRST, so a slot whose bdev died is unreachable: open fails, the agent reads "already removed", and the phantom slot squats the raid forever (run 14: DetachMember Applied ×308, slot "configuring", desc gone). On open failure, every slot still referencing the name is NOTICEd with its full state (raid state, slot idx, configured/failed/remove_scheduled/desc) — observability only, the remove-by-slot fix waits on the run 15 measurement | module/bdev/raid (bdev_raid_rpc.c only) | 0014 (incarnation plumbing in the same RPC) |
| 0031 | a failed base-bdev configure releases what it took — the `default:` branch of `raid_bdev_configure_base_bdev_check_sb_cb` (SB read fails, e.g. -EIO against a member that just died) logged and broke while keeping desc + module claim + app-thread io channel, so the slot squatted "configuring" and the leak PINNED the dead member's nvme_ctrlr destruct for the whole run (turing run 15: `holders[ns:1 qpair:1]` for 9 min 05, released to the second when teardown destroyed the raid). Upstream has the same hole — exotic there, systematic under a power-cycled node | module/bdev/raid (bdev_raid.c) | 0029 (its named holders are what identified the leak) |
| 0032 | the delayed-reconnect path YIELDS instead of spinning — `spdk_nvme_ctrlr_disconnect()` returns -EBUSY/-ENXIO (sticky), `nvme_ctrlr_disconnect` completes the reset as failed, `check_op_after_reset` answers OP_DELAYED_RECONNECT, whose action is that same disconnect: a hot loop with NOTHING that yields, since the pacing timer is armed by a callback that never runs. It ends only when `ctrlr_loss_timeout_sec` flips the answer to OP_DESTRUCT. Measured live (SPEC-77L runs 22-23, replica node power-cycled under load): **173 418 and 144 488 iterations**, ~8 000 spins/s, 28 s each, from the instant the raid failed the dead member. The nexus is single-core: its nvmf target stopped answering the CONSUMER's keep-alive within 5 s, the initiator kernel declared the path dead (opcode 0x18 QID 0 timeout → error recovery) and the volume froze **28 s where the failover itself costs 2 s**. Fix: arm the delay timer directly when that was the requested callback — one attempt per `reconnect_delay_sec`, the pacing upstream intended. Run 21, same code, never entered the loop (0 occurrences, stall 2,03 s): entry depends on the ctrlr state when the raid ejects it. Upstream master has the same hole | module/bdev/nvme (bdev_nvme.c) | 0028 (same reconnect-delay machinery) |

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

## Upstream pin (W4)

The upstream commit is pinned in `images/spdk/Dockerfile`
(`ARG SPDK_COMMIT_SHA`) and verified after clone — a moved tag fails the build.
`scripts/patches.sh` reads the same ARG, so tooling and build never disagree.

## Tooling — `scripts/patches.sh`

Never hand-edit hunk offsets (the audit's U-3 finding: a `fix: correct patch
hunk line count` commit is proof of manual editing gone wrong). Instead:

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

**Series format (homogenized 2026-07-16): RAW `git diff` output, no mail
header.** `git apply` (Dockerfile + `check`/`apply`) is the only consumer;
**`git am` is NOT part of the contract** (it chokes on raw diffs — the series
was format-mixed mbox/raw until 2026-07-16, which is how that was learned).
`regen` emits raw per-commit diffs with the commit subject as filename, so a
regen of an untouched series is byte-stable. Never hand-edit hunks.

## Upstreaming (UP4)

Candidates, easiest first: 0006 (degraded-read, small/general), 0003
(pause/resume), 0002 (allocated_ranges). 0004 (blob relocate) needs an RFC. Each
merged upstream removes rebase surface here.
