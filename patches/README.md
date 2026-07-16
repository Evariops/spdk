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
