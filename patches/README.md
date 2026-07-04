# Evariops SPDK patches

Out-of-tree patches applied on top of upstream SPDK during the container build
(`images/spdk/Dockerfile`). They add the primitives the SPEC-73 tiering
data-plane and the SPEC-66 group-snapshot barrier need, and harden a few
upstream paths. The two out-of-tree bdev **modules** (`module/bdev/cbt`,
`module/bdev/tier`) are copied in whole, not patched.

## Application order (U-5/U-6)

Patches are applied in **lexicographic order of filename** (`0001` … `0010`) —
the Dockerfile globs `patches/*.patch` and `git apply`s each. The numeric prefix
IS the contract; do not rely on any other ordering. Order matters:

| # | Patch | Touches | Depends on |
|--:|:------|:--------|:-----------|
| 0001 | raid skip_rebuild | bdev_raid | — |
| 0002 | lvol get_allocated_ranges | bdev_lvol | — |
| 0003 | nvmf pause/resume | lib/nvmf | — |
| 0004 | blob relocate primitives + freeze_io | lib/blob | — |
| 0005 | lvol placement/relocate/remap RPCs | bdev_lvol, module/bdev/tier | 0004, tier module |
| 0006 | raid5f degraded-read | bdev_raid | — |
| 0007 | raid nexus heat | bdev_raid | — |
| 0008 | raid rebuild_ranges | bdev_raid, lib/bdev (lock_lba_range) | 0006 |
| 0009 | ENOSPC → CAPACITY_EXCEEDED | bdev_lvol, bdev_raid | — |
| 0010 | rpc socket chmod 0600 | lib/rpc | — |

0005 `#include`s `vbdev_tier.h`; the Dockerfile adds `-I module/bdev/tier` to the
lvol module CFLAGS and injects the module dirs before applying patches.

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

1. Apply the series into a clean SPDK checkout at the pinned commit, committing
   one patch per commit (in order).
2. Edit the source, `git commit --amend` (or a fixup) into the owning commit.
3. `scripts/patches.sh regen <worktree>` to rewrite `patches/*.patch`.
4. `scripts/patches.sh verify` to confirm the whole series still applies.

`git format-patch --zero-commit` is used so the `From <sha>` line is a constant
(all-zero) instead of a per-regeneration hash — this keeps the patch files stable
under review. The `From: 000…` header line is therefore expected, not a bug.

## Upstreaming (UP4)

Candidates, easiest first: 0006 (degraded-read, small/general), 0003
(pause/resume), 0002 (allocated_ranges). 0004 (blob relocate) needs an RFC. Each
merged upstream removes rebase surface here.
