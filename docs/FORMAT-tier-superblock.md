# bdev_tier on-disk superblock — format v2

Authoritative description of the `struct tier_superblock` written by `vbdev_tier_sb.c`.
The C structs in `module/bdev/tier/vbdev_tier.h` are the source of truth; this
document explains the layout and the invariants. `SPDK_STATIC_ASSERT`s in the
header fail the build if the layout drifts from what is described here.

## Scope & compatibility

**Clean break — there is NO migration from v1 and none is planned.** The project
predates any public deployment; every environment is redeploy-by-wipe. A v1
superblock (`magic "TIERSB01"`, `version 1`) is deliberately **not** readable by
v2 code (`tier_sb_valid` rejects any `version != 2`). Upgrading = wipe the disks
and re-provision.

## Placement

One superblock reserve lives at the very start of **every** base bdev of a
composite (LBA 0). The reserve is `TIER_SB_RESERVE_BYTES` = 256 KiB, per disk,
outside the composite's usable address space (`phys_offset >= sb_blocks`).

Every copy self-describes the **whole** composite (all bands). There is no SPDK
`examine` path: assembly is driven by the CSI agent, which reads each disk's
superblock (`bdev_tier_read_sb`), picks the highest-seq valid copy across disks,
and replays `bdev_tier_create` + `bdev_tier_assemble_band` at the stored
geometry.

## A/B slots (F-5)

The 256 KiB reserve is split into **two 128 KiB slots**:

```
 disk LBA 0                    128 KiB                   256 KiB
 |------------- slot A --------|------------- slot B --------|
 seq 0,2,4,...                 seq 1,3,5,...
```

Generation `seq = N` is written to slot `N % 2` (`tier_sb_slot_for_seq`). A torn
write (crash mid-write) therefore damages **at most one slot**; the other still
holds the previous generation. Readers validate both slots and take the valid
one with the highest `seq` (`tier_sb_select`). This is the standard mdadm/LVM
double-copy technique and closes the v1 single-copy hole (F-5): a mid-write crash
no longer bricks a disk's superblock.

Only the slot for the current generation is written and flushed each update
(128 KiB I/O), not the whole reserve.

## Header layout (256 bytes)

| offset | size | field | notes |
|-------:|-----:|:------|:------|
| 0   | 8   | `magic` | `0x5449455253423032` = "TIERSB02" (LE) |
| 8   | 4   | `version` | `2` |
| 12  | 4   | `crc` | CRC32c over the whole struct with `crc = 0` |
| 16  | 8   | `seq` | monotone generation; highest valid wins |
| 24  | 8   | `created_epoch_sec` | wall clock at serialization (informative) |
| 32  | 16  | `generation_uuid` | composite **instance** id (fencing, F-2) |
| 48  | 64  | `composite_name` | |
| 112 | 8   | `md_num_blocks` | size of the mirrored md region (composite blocks) |
| 120 | 8   | `cluster_blocks` | blobstore cluster grain; **u64 since v2 (F-3)** |
| 128 | 4   | `md_mirror_a` | band slot id of md RAID1 leg A |
| 132 | 4   | `md_mirror_b` | band slot id of md RAID1 leg B |
| 136 | 4   | `num_bands` | |
| 140 | 4   | `this_band_id` | which slot this copy physically sits on (`UINT32_MAX` if unknown) |
| 144 | 4   | `blocklen` | common block size |
| 148 | 4   | `reserved0` | |
| 152 | 104 | `reserved[104]` | zero-filled; future header fields (F-2) |
| 256 | …   | `bands[64]` | 64 × 192-byte band descriptors |

Total = 256 + 64 × 192 = **12544 bytes** (fits one 128 KiB slot with room to spare).

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
| 160 | 32 | `reserved[32]` (F-2) |

## Invariants

- **Endianness (F-4)**: little-endian only (amd64/arm64). `tier_sb_valid` rejects
  a byte-swapped magic explicitly (a big-endian writer) rather than failing the
  CRC silently.
- **Generation uniqueness (M5)**: `seq` is reserved (`t->seq++`) at the entry of
  `tier_sb_write_all`, before any I/O. Two concurrent fan-outs can never share a
  `seq`; gaps are harmless ("highest seq wins"), duplicates would be fatal.
- **Cross-restart monotonicity (R2)**: `bdev_tier_register` re-reads every band's
  on-disk SB and seeds `t->seq` to the highest seq found **before** the first
  persist. A fresh in-RAM composite starts at `seq 0` (the CSI replays
  `create`+`assemble` without threading seq); without rehydration the first persist
  writes `seq 1`, which a pre-restart SB at a high seq out-votes forever, so the
  composite would reassemble the STALE geometry. Rehydration makes the first
  post-register persist `max_on_disk + 1`, which wins. The generation_uuid still
  changes each `create` (F-2), but after rehydration the current instance always
  holds the highest seq, so its slot wins per disk.
- **Durability (F-6)**: each written slot is FLUSHed before the generation is
  considered committed.
- **DEGRADED exclusion (M5b)**: only `ACTIVE` bands are written; a DEGRADED
  disk's stale copy is out-voted by `seq` at reassembly.
- **Fencing (F-2)**: `generation_uuid` is minted once at `bdev_tier_create` and
  copied into every superblock. A re-created composite gets a fresh uuid, so
  disks left over from a previous instance cannot be silently cross-assembled.
  The CSI compares uuids across a candidate disk set before assembling.

## Reserved space

104 header bytes + 32 bytes/band are zero-filled and CRC-covered. New fields are
added by shrinking a `reserved` array (keeping the surrounding offsets fixed) —
which changes the meaning of previously-zero bytes but NOT the struct size, so a
reader that predates the field sees zero (a safe default) and the static asserts
still pass. A field that changes size or reorders existing fields is a v3 break.

## Test coverage

`module/bdev/tier/test/test_tier_sb.c` (host-compiled, ASAN+UBSAN, links the
PRODUCTION `vbdev_tier_sb.c`): ABI offsets/sizes, serialize/validate roundtrip,
CRC/version/byte-swapped-magic rejection, u64 cluster_blocks, A/B slot selection
(highest-seq, torn-slot fallback, short-buffer safety), and a binary golden
header vector that catches a field reorder even when `sizeof` is unchanged.
