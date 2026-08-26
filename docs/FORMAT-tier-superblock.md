# bdev_tier on-disk superblock — format v2

Authoritative description of the `struct tier_superblock` written by `vbdev_tier_sb.c`. The C structs in `module/bdev/tier/vbdev_tier.h` are the source of truth; this document explains the layout and the invariants. `SPDK_STATIC_ASSERT`s in the header fail the build if the layout drifts from what is described here.

## Scope & compatibility

**Clean break — there is NO migration from v1 and none is planned.** A v1 superblock (`magic "TIERSB01"`, `version 1`) is deliberately not readable by v2 code: `tier_sb_valid` rejects any `version != 2`. Upgrading means wiping the disks and re-provisioning.

## Placement

One superblock reserve lives at the very start of **every** base bdev of a composite (LBA 0). The reserve is `TIER_SB_RESERVE_BYTES` = 256 KiB per disk, outside the composite's usable address space (`phys_offset >= sb_blocks`).

Every copy self-describes the **whole** composite (all bands). There is no SPDK `examine` path: assembly is driven by the CSI agent, which reads each disk's superblock (`bdev_tier_read_sb`), picks the highest-seq valid copy across disks, and replays `bdev_tier_create` + `bdev_tier_assemble_band` at the stored geometry.

## A/B slots

The 256 KiB reserve is split into **two 128 KiB slots**:

```
 disk LBA 0                    128 KiB                   256 KiB
 |------------- slot A --------|------------- slot B --------|
 seq 0,2,4,...                 seq 1,3,5,...
```

Generation `seq = N` is written to slot `N % 2` (`tier_sb_slot_for_seq`), so a torn write damages **at most one slot** and the other still holds the previous generation. Readers validate both slots and take the valid one with the highest `seq` (`tier_sb_select`). Only the slot for the current generation is written and flushed on each update (128 KiB of I/O), never the whole reserve.

## Header layout (256 bytes)

| offset | size | field | notes |
|-------:|-----:|:------|:------|
| 0   | 8   | `magic` | `0x5449455253423032` = "TIERSB02" (LE) |
| 8   | 4   | `version` | `2` |
| 12  | 4   | `crc` | CRC32c over the whole struct with `crc = 0` |
| 16  | 8   | `seq` | monotone generation; highest valid wins |
| 24  | 8   | `created_epoch_sec` | wall clock at serialization (informative) |
| 32  | 16  | `generation_uuid` | composite **instance** id (fencing) |
| 48  | 64  | `composite_name` | |
| 112 | 8   | `md_num_blocks` | size of the mirrored md region (composite blocks) |
| 120 | 8   | `cluster_blocks` | blobstore cluster grain; u64 |
| 128 | 4   | `md_mirror_a` | band slot id of md RAID1 leg A |
| 132 | 4   | `md_mirror_b` | band slot id of md RAID1 leg B |
| 136 | 4   | `num_bands` | |
| 140 | 4   | `this_band_id` | which slot this copy physically sits on (`UINT32_MAX` if unknown) |
| 144 | 4   | `blocklen` | common block size |
| 148 | 4   | `reserved0` | |
| 152 | 104 | `reserved[104]` | zero-filled; future header fields |
| 256 | …   | `bands[64]` | 64 × 192-byte band descriptors |

Total = 256 + 64 × 192 = **12544 bytes**, which fits one 128 KiB slot with room to spare.

## Band descriptor layout (192 bytes)

| offset | size | field |
|-------:|-----:|:------|
| 0   | 4  | `band_id` |
| 4   | 4  | `tier` (enum tier_class) |
| 8   | 4  | `state` (enum tier_band_state) |
| 12  | 4  | `reserved0` |
| 16  | 8  | `lba_start` |
| 24  | 8  | `num_blocks` |
| 32  | 64 | `wwn` |
| 96  | 64 | `serial` |
| 160 | 16 | `part_uuid` — opaque unit identity supplied by the control plane (a partition's PARTUUID). **All-zero = whole disk / no partition identity** |
| 176 | 8  | `part_start_lba` — the unit's start LBA on its **parent** device (0 for a whole disk) |
| 184 | 8  | `part_size_blocks` — the unit's size on its parent device (0 for a whole disk) |

The three unit-identity fields consumed the former `reserved[32]` **exactly** (offsets pinned by `SPDK_STATIC_ASSERT`s): the struct size and every prior offset are unchanged, and there is **no version bump and no migration** — the previously-zero bytes are precisely the "whole disk" encoding, so every superblock written before the fields existed reads back as a composite of whole-disk bands, which is what it was. The module stores and reports these fields verbatim; it never probes a partition table. The live-vs-stored comparison (identity match, geometry **equality**) belongs to the control plane's reassembly, which reads them via `bdev_tier_read_sb`.

## Invariants

- **Endianness**: little-endian only (amd64/arm64). `tier_sb_valid` rejects a byte-swapped magic explicitly — a big-endian writer is named rather than failing the CRC silently.
- **Generation uniqueness**: `seq` is reserved (`t->seq++`) at the entry of `tier_sb_write_all`, before any I/O, so two concurrent fan-outs can never share a `seq`. Gaps are harmless ("highest seq wins"); duplicates would be fatal.
- **Cross-restart monotonicity**: `bdev_tier_register` re-reads every band's on-disk superblock and seeds `t->seq` to the highest seq found **before** the first persist, so that persist writes `max_on_disk + 1` and wins. Without rehydration a fresh in-RAM composite starts at `seq 0`, writes `seq 1`, and is out-voted forever by the pre-restart superblock — it would reassemble the STALE geometry.
- **Durability**: each written slot is FLUSHed before the generation counts as committed.
- **DEGRADED exclusion**: only `ACTIVE` bands are written; a DEGRADED disk's stale copy is out-voted by `seq` at reassembly.
- **Fencing**: `generation_uuid` is minted once at `bdev_tier_create` and copied into every superblock. A re-created composite gets a fresh uuid, so disks left over from a previous instance cannot be silently cross-assembled. The CSI agent compares uuids across a candidate disk set before assembling.

- **Reserved space**: the 104 header bytes are zero-filled and CRC-covered. A new field is added by shrinking a `reserved` array while keeping the surrounding offsets fixed — the meaning of previously-zero bytes changes, the struct size does not, so a reader predating the field sees zero (a safe default) and the static asserts still pass. Any field that changes size, or any reordering of existing fields, is a v3 break. The per-band `reserved[32]` was consumed this way by `part_uuid`/`part_start_lba`/`part_size_blocks` (all-zero = whole disk).

## Test coverage

`module/bdev/tier/test/test_tier_sb.c` (host-compiled, ASAN+UBSAN, linking the PRODUCTION `vbdev_tier_sb.c`) covers ABI offsets and sizes (including the pinned `part_uuid`/`part_start_lba`/`part_size_blocks` offsets), serialize/validate roundtrip (including the unit-identity fields), CRC/version/byte-swapped-magic rejection, u64 `cluster_blocks`, A/B slot selection (highest-seq, torn-slot fallback, short-buffer safety), a binary golden header vector that catches a field reorder even when `sizeof` is unchanged, and the backward-compat vector: a band without partition identity serializes bytes 160–191 as zero, byte-identical to the pre-identity format.
