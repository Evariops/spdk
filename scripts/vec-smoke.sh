#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Evariops.
#
# vec-smoke.sh — single-node raid5f EC smoke (SPEC-75G G1 pre-flight).
#
# Exercises the fork's EC data-plane contract on file-backed AIO bdevs:
#   1. create raid5f 2+1 (strip 64K, incarnation) and write full stripes
#      (spdk_dd) — sub-stripe writes are REJECTED (EC-3, negative test);
#   2. syndrome verify_ranges (patch 0022, F-b) → zero divergence;
#   3. corrupt the PARITY strip of stripe 0 on the backing file →
#      verify reports the stripe DIVERGENT (mode=syndrome, report-only);
#   4. bdev_raid_rebuild_ranges (patch 0008) over that stripe → re-verify
#      clean. (Parity-strip corruption is the honestly repairable case; a
#      DATA-strip corruption would be "repaired" into consistent-but-wrong,
#      which is exactly why verify is report-only and the CP re-folds from
#      the source — SPEC-75G §4.3.)
#   5. delete one member → syndrome verify refuses -EAGAIN ("repair first");
#   6. unaligned range → -EINVAL; degraded-service counters visible (0023).
#
# This smoke is NOT the V-EC certification (cross-node, real disk pulls, on
# turing — spdk-csi QA harness). It is the fork-local pre-flight that must
# be green before any image ships raid5f primitives.
#
# Requirements: linux, spdk_tgt + spdk_dd binaries (builder image has both),
# python3. Hugepages if available, else falls back to --no-huge.
#
# Usage:
#   scripts/vec-smoke.sh [workdir]
#   SPDK_TGT=/build/spdk/build/bin/spdk_tgt SPDK_DD=/build/spdk/build/bin/spdk_dd \
#     scripts/vec-smoke.sh /tmp/vecsmoke

set -euo pipefail

WORK="${1:-/tmp/vecsmoke}"
SOCK="$WORK/rpc.sock"
SPDK_TGT="${SPDK_TGT:-spdk_tgt}"
SPDK_DD="${SPDK_DD:-spdk_dd}"
INCARNATION="vecsmoke-$$"

STRIP_KB=64                      # per-member strip
STRIPE_BYTES=$((2 * STRIP_KB * 1024))   # k=2 data strips = 128 KiB full stripe
STRIPE_BLOCKS=$((STRIPE_BYTES / 512))   # 256 blocks @512
MEMBER_MB=64
WRITE_MB=16                      # 128 full stripes of payload

TGT_PID=""
FAILURES=0

say()  { printf '\n\033[1m== %s ==\033[0m\n' "$*"; }
pass() { printf '\033[32mPASS\033[0m %s\n' "$*"; }
fail() { printf '\033[31mFAIL\033[0m %s\n' "$*"; FAILURES=$((FAILURES + 1)); }

cleanup() {
	[ -n "$TGT_PID" ] && kill "$TGT_PID" 2>/dev/null || true
	wait 2>/dev/null || true
}
trap cleanup EXIT

# Hugepages or fallback. 2 GiB: the iscsi subsystem compiled into spdk_tgt
# fails its PDU pool allocation below ~1 GiB of legacy mem.
MEMOPTS=(-s 2048)
if ! grep -q '^HugePages_Total:\s*[1-9]' /proc/meminfo 2>/dev/null; then
	MEMOPTS+=(--no-huge)
	echo "(no hugepages — using --no-huge)"
fi

# spdk_dd reads/writes FILES through io_uring by default; Docker's default
# seccomp profile blocks io_uring_setup → force AIO (bdev side unaffected).
DDOPTS=(--aio)

# ── raw JSON-RPC over the unix socket (no rpc.py wrappers: the fork RPCs
#    and the incarnation param are not in vanilla rpc.py) ──────────────────
rpc_raw() {
	# rpc_raw <method> <params-json>  → prints the raw JSON response
	python3 - "$SOCK" "$1" "$2" <<'EOF'
import json, socket, sys
sock_path, method, params = sys.argv[1], sys.argv[2], sys.argv[3]
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sock_path)
req = {"jsonrpc": "2.0", "id": 1, "method": method}
if params.strip():
    req["params"] = json.loads(params)
s.sendall(json.dumps(req).encode())
buf = b""
while True:
    chunk = s.recv(65536)
    if not chunk:
        break
    buf += chunk
    try:
        json.loads(buf)
        break
    except ValueError:
        continue
print(buf.decode())
EOF
}

rpc_ok() {           # asserts the call succeeds, prints result
	local out
	out=$(rpc_raw "$1" "$2")
	if echo "$out" | grep -q '"error"'; then
		echo "$out" >&2
		return 1
	fi
	echo "$out"
}

rpc_expect_err() {   # rpc_expect_err <method> <params> <grep-pattern>
	local out
	out=$(rpc_raw "$1" "$2")
	echo "$out" | grep -q '"error"' && echo "$out" | grep -qi "$3"
}

json_field() {       # json_field <json> <pyexpr over r=result>
	python3 -c "import json,sys; r=json.loads(sys.argv[1])['result']; print(sys.argv[2] and eval(sys.argv[2]))" "$1" "$2"
}

wait_sock() {
	for _ in $(seq 1 100); do
		[ -S "$SOCK" ] && rpc_raw framework_wait_init "" >/dev/null 2>&1 && return 0
		sleep 0.2
	done
	echo "spdk_tgt socket never came up" >&2
	return 1
}

start_tgt() {
	"$SPDK_TGT" -r "$SOCK" -m 0x1 "${MEMOPTS[@]}" &
	TGT_PID=$!
	wait_sock
}

stop_tgt() {
	kill "$TGT_PID" 2>/dev/null || true
	wait "$TGT_PID" 2>/dev/null || true
	TGT_PID=""
}

create_bdevs() {     # aio members + raid5f (no superblock: file offset == member LBA)
	for i in 0 1 2; do
		rpc_ok bdev_aio_create "{\"filename\": \"$WORK/m$i.img\", \"name\": \"aio$i\", \"block_size\": 512}" >/dev/null
	done
	rpc_ok bdev_raid_create "{\"name\": \"ecsmoke\", \"raid_level\": \"raid5f\", \"strip_size_kb\": $STRIP_KB, \"base_bdevs\": [\"aio0\", \"aio1\", \"aio2\"], \"incarnation\": \"$INCARNATION\"}" >/dev/null
}

# ── setup ────────────────────────────────────────────────────────────────
say "setup: $WORK (strip ${STRIP_KB}K, full stripe ${STRIPE_BYTES} B / ${STRIPE_BLOCKS} blocks)"
# $WORK may be a mount point (container volume): clear its CONTENT, never
# the directory itself.
mkdir -p "$WORK"
rm -rf "${WORK:?}"/* 2>/dev/null || true
for i in 0 1 2; do
	dd if=/dev/zero of="$WORK/m$i.img" bs=1M count=$MEMBER_MB status=none
done
dd if=/dev/urandom of="$WORK/payload.bin" bs=1M count=$WRITE_MB status=none
dd if=/dev/urandom of="$WORK/payload4k.bin" bs=4096 count=1 status=none

# spdk_dd runs its own app instance: same bdevs via a JSON config.
cat > "$WORK/dd-config.json" <<EOF
{"subsystems": [{"subsystem": "bdev", "config": [
  {"method": "bdev_aio_create", "params": {"filename": "$WORK/m0.img", "name": "aio0", "block_size": 512}},
  {"method": "bdev_aio_create", "params": {"filename": "$WORK/m1.img", "name": "aio1", "block_size": 512}},
  {"method": "bdev_aio_create", "params": {"filename": "$WORK/m2.img", "name": "aio2", "block_size": 512}},
  {"method": "bdev_raid_create", "params": {"name": "ecsmoke", "raid_level": "raid5f", "strip_size_kb": $STRIP_KB, "base_bdevs": ["aio0", "aio1", "aio2"], "incarnation": "$INCARNATION"}}
]}]}
EOF

# ── 1. writer: full stripes OK, sub-stripe REJECTED (EC-3) ───────────────
# CAUTION: spdk_dd exits 0 even when the bdev FAILS its writes (it logs the
# IO error and keeps its "Copying: N/N" accounting) — its exit code is NOT
# an oracle. Write success = readback byte-identical; write rejection = the
# bdev layer's write_unit error in the log.
say "1. spdk_dd full-stripe write ($WRITE_MB MiB) + sub-stripe rejection"
"$SPDK_DD" "${MEMOPTS[@]}" "${DDOPTS[@]}" -c "$WORK/dd-config.json" \
	--if "$WORK/payload.bin" --ob ecsmoke --bs "$STRIPE_BYTES" > "$WORK/dd-write.log" 2>&1 || true
"$SPDK_DD" "${MEMOPTS[@]}" "${DDOPTS[@]}" -c "$WORK/dd-config.json" \
	--ib ecsmoke --of "$WORK/readback.bin" --bs 1048576 --count $WRITE_MB \
	> "$WORK/dd-readback.log" 2>&1 || true
if cmp -s -n $((WRITE_MB * 1024 * 1024)) "$WORK/payload.bin" "$WORK/readback.bin"; then
	pass "full-stripe writes landed (readback byte-identical over $WRITE_MB MiB)"
else
	fail "readback differs from payload (write did not land — see dd-write.log)"
	tail -5 "$WORK/dd-write.log" || true
fi
"$SPDK_DD" "${MEMOPTS[@]}" "${DDOPTS[@]}" -c "$WORK/dd-config.json" \
	--if "$WORK/payload4k.bin" --ob ecsmoke --bs 4096 > "$WORK/dd-4k.log" 2>&1 || true
if grep -q "does not match the write_unit_size" "$WORK/dd-4k.log"; then
	pass "4K sub-stripe writes rejected (write_unit_size discipline)"
else
	fail "4K write was NOT rejected by the write_unit discipline"
	tail -5 "$WORK/dd-4k.log" || true
fi

# ── 2. clean syndrome verify ─────────────────────────────────────────────
say "2. syndrome verify on pristine data"
start_tgt
create_bdevs
OUT=$(rpc_ok bdev_raid_verify_ranges '{"name": "ecsmoke", "token": "smoke:clean"}')
MODE=$(json_field "$OUT" "r['mode']")
DIV=$(json_field "$OUT" "r['divergent_blocks']")
[ "$MODE" = "syndrome" ] && pass "mode=syndrome" || fail "mode=$MODE (expected syndrome)"
[ "$DIV" = "0" ] && pass "pristine raid: 0 divergent blocks" || fail "pristine raid reports $DIV divergent blocks"

# unaligned range → -EINVAL
if rpc_expect_err bdev_raid_verify_ranges \
	"{\"name\": \"ecsmoke\", \"ranges\": [{\"start_lba\": 8, \"num_blocks\": $STRIPE_BLOCKS}]}" \
	"not aligned"; then
	pass "unaligned range refused"
else
	fail "unaligned range was not refused"
fi
stop_tgt

# ── 3. corrupt the PARITY strip of stripe 0 → divergent ──────────────────
# p_idx(stripe 0) = k - 0 % n = 2 → slot 2 (aio2) holds stripe 0's parity.
# No superblock → member LBA == file offset; flip 64 bytes at offset 0.
say "3. corrupt stripe-0 parity strip (aio2 @0) → verify must report it"
dd if=/dev/urandom of="$WORK/m2.img" bs=64 count=1 conv=notrunc status=none
start_tgt
create_bdevs
OUT=$(rpc_ok bdev_raid_verify_ranges '{"name": "ecsmoke", "token": "smoke:corrupt"}')
DIV=$(json_field "$OUT" "r['divergent_blocks']")
FIRST=$(json_field "$OUT" "r['divergent_ranges'][0]['start_lba'] if r['divergent_ranges'] else -1")
if [ "$DIV" -ge "$STRIPE_BLOCKS" ] 2>/dev/null && [ "$FIRST" = "0" ]; then
	pass "divergence detected on stripe 0 ($DIV blocks)"
else
	fail "divergence NOT detected (divergent_blocks=$DIV first=$FIRST)"
fi

# ── 4. rebuild_ranges over the stripe → re-verify clean ──────────────────
say "4. rebuild_ranges stripe 0 (parity recompute) → re-verify clean"
rpc_ok bdev_raid_rebuild_ranges "{\"name\": \"ecsmoke\", \"ranges\": [{\"start_lba\": 0, \"num_blocks\": $STRIPE_BLOCKS}]}" >/dev/null
OUT=$(rpc_ok bdev_raid_verify_ranges '{"name": "ecsmoke", "token": "smoke:repaired"}')
DIV=$(json_field "$OUT" "r['divergent_blocks']")
[ "$DIV" = "0" ] && pass "post-repair verify clean" || fail "post-repair verify still divergent ($DIV blocks)"

# ── 5. degraded → -EAGAIN + counters (0023) ──────────────────────────────
say "5. degraded raid refuses syndrome verify (-EAGAIN) + 0023 counters"
rpc_ok bdev_aio_delete '{"name": "aio2"}' >/dev/null
sleep 0.5
if rpc_expect_err bdev_raid_verify_ranges '{"name": "ecsmoke"}' "repair first"; then
	pass "degraded raid refused with 'repair first'"
else
	fail "degraded raid did NOT refuse the syndrome verify"
fi
OUT=$(rpc_ok bdev_get_bdevs '{"name": "ecsmoke"}')
CTR=$(python3 -c "import json,sys; r=json.loads(sys.argv[1])['result'][0]['driver_specific']['raid']; print(r.get('reconstruct_reads_absent', 'MISSING'), r.get('last_degraded_ts', 'MISSING'))" "$OUT")
case "$CTR" in
	*MISSING*) fail "0023 counters missing from get_bdevs ($CTR)";;
	*)         pass "0023 counters present (reconstruct_reads_absent last_degraded_ts = $CTR)";;
esac
stop_tgt

# ── verdict ──────────────────────────────────────────────────────────────
say "verdict"
if [ "$FAILURES" -eq 0 ]; then
	echo "vec-smoke: ALL GREEN"
else
	echo "vec-smoke: $FAILURES FAILURE(S)"
	exit 1
fi
