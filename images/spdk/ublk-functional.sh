#!/usr/bin/env bash
# The ublk patches (0058-0070) on a real kernel: run inside the debug image, privileged, with the
# host's /dev mounted and ublk_drv loaded on the host. Each case prints PASS, or SKIP when the
# host's driver lacks the feature it needs; the script exits non-zero on the first failure.
# Upstream has no unit suite for lib/ublk nor bdev_uring: this is where their behaviour is pinned.
#
#   docker run --rm --privileged -v /dev:/dev -v "$PWD/images/spdk/ublk-functional.sh:/t.sh:ro" \
#     --entrypoint bash ghcr.io/evariops/spdk-debug:<tag> /t.sh
set -uo pipefail

WORK=$(mktemp -d)
SOCK=$WORK/spdk.sock
TGT=${SPDK_TGT:-/usr/local/bin/spdk_tgt}
PID=
ID=7
FEATURES=
trap '[ -n "$PID" ] && kill -KILL "$PID" 2>/dev/null' EXIT

cat > "$WORK/rpc.py" <<'PY'
import json, socket, sys
path, method = sys.argv[1], sys.argv[2]
params = json.loads(sys.argv[3]) if len(sys.argv) > 3 else None
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(60)
s.connect(path)
req = {"jsonrpc": "2.0", "id": 1, "method": method}
if params is not None:
    req["params"] = params
s.sendall(json.dumps(req).encode())
buf, dec = b"", json.JSONDecoder()
while True:
    chunk = s.recv(1 << 20)
    if not chunk:
        sys.exit(f"{method}: connection closed")
    buf += chunk
    try:
        obj, _ = dec.raw_decode(buf.decode())
        break
    except ValueError:
        continue
if "error" in obj:
    print(json.dumps(obj["error"]))
    sys.exit(1)
print(json.dumps(obj.get("result")))
PY

R() { python3 "$WORK/rpc.py" "$SOCK" "$@"; }
pass() { echo "PASS $*"; }
skip() { echo "SKIP $* (the host's ublk driver lacks it)"; }
fail() { echo "FAIL $*"; tail -20 "$WORK/tgt.log"; exit 1; }
has() { [[ " $FEATURES " == *" $1 "* ]]; }
dev() { echo "/dev/ublkb$ID"; }
digest() { sha256sum | cut -d' ' -f1; }

start() { # $1: extra arguments
	rm -f "$SOCK"
	# No trace file: a killed process would leave it in the host's /dev/shm.
	# shellcheck disable=SC2086
	setsid "$TGT" --no-huge -s 1024 -m 0x1 -r "$SOCK" --num-trace-entries 0 $1 >> "$WORK/tgt.log" 2>&1 < /dev/null &
	PID=$!
	disown "$PID"
	for _ in $(seq 100); do
		[ -S "$SOCK" ] && R spdk_get_version > /dev/null 2>&1 && return 0
		sleep 0.1
	done
	fail "spdk_tgt did not start"
}
stop_term() {
	kill -TERM "$PID" 2>/dev/null
	for _ in $(seq 100); do kill -0 "$PID" 2>/dev/null || return 0; sleep 0.1; done
	kill -KILL "$PID" 2>/dev/null
}
stop_kill() { kill -KILL "$PID" 2>/dev/null; sleep 1; }
target() { # $1: parameters
	local params='{}'
	[ $# -gt 0 ] && params=$1
	R ublk_create_target "$params" > /dev/null || fail "ublk_create_target"
}
aio() {
	R bdev_aio_create "{\"filename\":\"$WORK/aio.img\",\"name\":\"aio0\",\"block_size\":4096}" > /dev/null
	R bdev_wait_for_examine > /dev/null
}
restart() { # $1: stop_term or stop_kill
	"$1"
	start ""
	target
	aio
}
recover() { R ublk_recover_disk "{\"bdev_name\":\"lvs/lv\",\"ublk_id\":$ID,\"identity\":$1}" > /dev/null 2>&1; }
wait_dev() { for _ in $(seq 50); do [ -b "$(dev)" ] && return 0; sleep 0.1; done; fail "$(dev) did not appear"; }

# A paced writer: 64 KiB block i holds the byte i % 251, one direct write every 20 ms. A request
# issued while the disk has no server waits in the kernel, and the loop with it.
writer() { # $1: blocks
	rm -f "$WORK/writer.done" "$WORK/writer.err"
	( for i in $(seq 0 $(( $1 - 1 ))); do
		head -c 65536 /dev/zero | tr '\0' "\\$(printf '%03o' $(( i % 251 )))" |
			dd of="$(dev)" bs=64k seek=$(( 512 + i )) oflag=direct conv=nocreat status=none || echo "$i" >> "$WORK/writer.err"
		sleep 0.02
	  done; touch "$WORK/writer.done" ) &
}
writer_check() { # $1: blocks
	for _ in $(seq 300); do [ -f "$WORK/writer.done" ] && break; sleep 0.1; done
	[ -f "$WORK/writer.done" ] || fail "the writer did not finish"
	[ -f "$WORK/writer.err" ] && fail "the writer saw $(wc -l < "$WORK/writer.err") failed writes"
	for i in $(seq 0 $(( $1 - 1 ))); do
		want=$(printf '%02x' $(( i % 251 )))
		got=$(dd if="$(dev)" bs=64k skip=$(( 512 + i )) count=1 iflag=direct status=none | od -An -v -tx1 | tr -s ' \n' '\n' | sort -u | grep -v '^$')
		[ "$got" = "$want" ] || fail "block $i holds [$got], wants $want"
	done
}

[ -c /dev/ublk-control ] || fail "/dev/ublk-control is missing: load ublk_drv on the host"
# A write to a vanished disk must fail, not leave a regular file in its place (conv=nocreat).
[ -e /dev/ublkc$ID ] || [ -e "$(dev)" ] && fail "/dev/ublkc$ID or $(dev) is already there"
echo "kernel $(uname -r)"
truncate -s 512M "$WORK/aio.img"
head -c 8388608 /dev/urandom > "$WORK/pattern"
PATTERN=$(digest < "$WORK/pattern")

echo "=== 0058, 0059, 0060, 0064, 0065: a disk over an lvol"
start ""
target
aio
R bdev_lvol_create_lvstore '{"bdev_name":"aio0","lvs_name":"lvs"}' > /dev/null
R bdev_lvol_create '{"lvol_name":"lv","size_in_mib":64,"lvs_name":"lvs"}' > /dev/null
REPORT=$(R ublk_get_target) || fail "0065 ublk_get_target"
FEATURES=$(python3 -c 'import json,sys; print(" ".join(json.loads(sys.argv[1])["features_found"]))' "$REPORT")
echo "driver features: $FEATURES"
[[ $REPORT == *'"user_copy": true'* ]] && pass "0065 ublk_get_target reports user copy" || fail "0065 user copy"
R ublk_start_disk "{\"bdev_name\":\"lvs/lv\",\"ublk_id\":$ID,\"identity\":4660}" > /dev/null || fail "ublk_start_disk"
wait_dev
dd if="$WORK/pattern" of="$(dev)" bs=1M oflag=direct conv=nocreat status=none
[ "$(dd if="$(dev)" bs=1M count=8 iflag=direct status=none | digest)" = "$PATTERN" ] && pass "8 MiB written and read back" || fail "read back"
[ "$(cat /sys/block/ublkb$ID/queue/max_hw_sectors_kb)" = 1024 ] && pass "0058 a request carries 1 MiB" || fail "0058 max_hw_sectors_kb"
[[ $(R ublk_get_disks) == *'"identity": 4660'* ]] && pass "0064 the disk reports its identity" || fail "0064 identity"
R nvmf_create_transport '{"trtype":"TCP"}' > /dev/null
R nvmf_create_subsystem '{"nqn":"nqn.2016-06.io.spdk:t","allow_any_host":true}' > /dev/null
R nvmf_subsystem_add_ns '{"nqn":"nqn.2016-06.io.spdk:t","namespace":{"bdev_name":"lvs/lv"}}' > /dev/null 2>&1 &&
	fail "0059 a namespace was added over the lvol a disk serves" || pass "0059 the disk's claim refuses a second writer"
if has update_size; then
	R bdev_lvol_resize '{"name":"lvs/lv","size_in_mib":128}' > /dev/null
	for _ in $(seq 30); do [ "$(cat /sys/block/ublkb$ID/size)" = 262144 ] && break; sleep 0.1; done
	[ "$(cat /sys/block/ublkb$ID/size)" = 262144 ] && pass "0060 the device follows the lvol's resize" || fail "0060 resize"
else
	skip "0060 resize"
fi

if has user_recovery && has user_recovery_reissue; then
	if has quiesce; then
		echo "=== 0061, 0063, 0064: a planned restart, and a recovery under another identity"
		writer 150
		sleep 0.5
		R ublk_quiesce_disk "{\"ublk_id\":$ID,\"timeout_ms\":5000}" > /dev/null || fail "0061 quiesce"
		restart stop_term
		recover 1 && fail "0064 a device of another identity was recovered" || pass "0064 a device of another identity is refused"
		recover 4660 || fail "0063 recover"
		writer_check 150 && pass "0061 the writer's blocks survive a planned restart"

		echo "=== 0069: a SIGTERM with nobody releasing the disk"
		writer 150
		sleep 0.5
		restart stop_term
		recover 4660 || fail "0069 the disk was not left to the kernel"
		writer_check 150 && pass "0069 the writer's blocks survive a SIGTERM"
	else
		skip "0061, 0069 planned restart"
	fi

	echo "=== recovery with reissue: SIGKILL mid-write"
	writer 150
	sleep 0.5
	restart stop_kill
	recover 4660 || fail "recover after SIGKILL"
	writer_check 150 && pass "the writer's blocks survive a SIGKILL"
else
	skip "recovery"
fi
R ublk_stop_disk "{\"ublk_id\":$ID}" > /dev/null
stop_term

echo "=== 0066: a thread short of buffers"
start "--wait-for-rpc"
R iobuf_set_options '{"small_pool_count":1024,"large_pool_count":72}' > /dev/null
R framework_start_init > /dev/null
target
aio
[[ $(R ublk_start_disk "{\"bdev_name\":\"lvs/lv\",\"ublk_id\":$ID}" 2>&1) == *'"code": -12'* ]] &&
	pass "0066 the start answers -ENOMEM" || fail "0066 the start did not answer -ENOMEM"
kill -0 "$PID" && pass "0066 the process is up" || fail "0066 the process died"
[ -e /sys/class/ublk-char/ublkc$ID ] && fail "0066 a device was added" || pass "0066 nothing was added in the kernel"
stop_term

echo "=== 0070: a disk served from an offset, as a nexus lays a volume out"
start ""
target
aio
LV_BYTES=$(R bdev_get_bdevs '{"name":"lvs/lv"}' | python3 -c 'import json,sys; b=json.load(sys.stdin)[0]; print(b["num_blocks"]*b["block_size"])')
R ublk_start_disk "{\"bdev_name\":\"lvs/lv\",\"ublk_id\":$ID,\"identity\":4661,\"offset_bytes\":1048577}" > /dev/null 2>&1 &&
	fail "0070 an offset that is no whole number of blocks was taken" || pass "0070 an offset off the block grain is refused"
R ublk_start_disk "{\"bdev_name\":\"lvs/lv\",\"ublk_id\":$ID,\"identity\":4661,\"offset_bytes\":1048576}" > /dev/null || fail "0070 start with an offset"
wait_dev
[ $(( $(cat /sys/block/ublkb$ID/size) * 512 )) = $(( LV_BYTES - 1048576 )) ] && pass "0070 the disk is its offset smaller than the lvol" || fail "0070 size"
[[ $(R ublk_get_disks) == *'"offset_bytes": 1048576'* ]] && pass "0070 the disk reports its offset" || fail "0070 offset reported"
dd if="$WORK/pattern" of="$(dev)" bs=1M oflag=direct conv=nocreat status=none
if has user_recovery && has user_recovery_reissue; then
	restart stop_kill
	R ublk_recover_disk "{\"bdev_name\":\"lvs/lv\",\"ublk_id\":$ID,\"identity\":4661}" > /dev/null 2>&1 &&
		fail "0070 a device started with an offset was recovered without it" || pass "0070 a recovery with another offset is refused"
	[ -e /sys/class/ublk-char/ublkc$ID ] && pass "0070 the refused device is left in the kernel" || fail "0070 the refused device was deleted"
	R ublk_recover_disk "{\"bdev_name\":\"lvs/lv\",\"ublk_id\":$ID,\"identity\":4661,\"offset_bytes\":1048576}" > /dev/null || fail "0070 recover with its offset"
	[ "$(dd if="$(dev)" bs=1M count=8 iflag=direct status=none | digest)" = "$PATTERN" ] && pass "0070 recovered, it reads what it wrote" || fail "0070 read back after recovery"
fi
R ublk_stop_disk "{\"ublk_id\":$ID}" > /dev/null
for _ in $(seq 50); do [ -e "$(dev)" ] || break; sleep 0.1; done
R ublk_start_disk "{\"bdev_name\":\"lvs/lv\",\"ublk_id\":$ID}" > /dev/null || fail "0070 start at 0"
wait_dev
[ "$(dd if="$(dev)" bs=1M skip=1 count=8 iflag=direct status=none | digest)" = "$PATTERN" ] && pass "0070 the data lie 1 MiB into the lvol" || fail "0070 the data are not 1 MiB in"
R ublk_stop_disk "{\"ublk_id\":$ID}" > /dev/null
stop_term

echo "=== 0067, 0068: zero copy over bdev_uring"
truncate -s 512M "$WORK/uring.img"
start ""
# The bdevs register before the target: the bdev layer reads their domains then, as it does for
# the lvols a restarted process loads from their store.
R bdev_uring_create "{\"filename\":\"$WORK/uring.img\",\"name\":\"u0\",\"block_size\":4096}" > /dev/null || fail "bdev_uring_create"
R bdev_lvol_create_lvstore '{"bdev_name":"u0","lvs_name":"lvsu","cluster_sz":1048576}' > /dev/null
R bdev_lvol_create '{"lvol_name":"lv","size_in_mib":64,"lvs_name":"lvsu","thin_provision":true}' > /dev/null
target '{"zero_copy":true}'
R ublk_start_disk "{\"bdev_name\":\"lvsu/lv\",\"ublk_id\":$ID}" > /dev/null || fail "zero copy start"
wait_dev
if has support_zero_copy; then
	[[ $(R ublk_get_disks) == *'"enabled": true'* ]] || fail "0067 the disk does not serve with no copy"
	dd if="$WORK/pattern" of="$(dev)" bs=1M oflag=direct conv=nocreat status=none
	[ "$(dd if="$(dev)" bs=1M count=8 iflag=direct status=none | digest)" = "$PATTERN" ] && pass "0067 8 MiB with no copy read back" || fail "0067 read back"
	[ "$(dd if="$(dev)" bs=1M skip=40 count=4 iflag=direct status=none | digest)" = "$(head -c 4194304 /dev/zero | digest)" ] &&
		pass "0067 a hole reads as zeros (memzero)" || fail "0067 hole"
	R ublk_get_disks | python3 -c 'import json,sys; sys.exit(0 if json.load(sys.stdin)[0]["zero_copy"]["zero_copy"] > 0 else 1)' &&
		pass "0067 requests went with no copy" || fail "0067 no request went with no copy"
	# 0070 with no copy: the registered pages land at the offset as the copied ones do.
	R ublk_stop_disk "{\"ublk_id\":$ID}" > /dev/null
	for _ in $(seq 50); do [ -e "$(dev)" ] || break; sleep 0.1; done
	R ublk_start_disk "{\"bdev_name\":\"lvsu/lv\",\"ublk_id\":$ID,\"offset_bytes\":1048576}" > /dev/null || fail "0070 zero copy start with an offset"
	wait_dev
	head -c 8388608 /dev/urandom > "$WORK/pattern2"
	dd if="$WORK/pattern2" of="$(dev)" bs=1M oflag=direct conv=nocreat status=none
	R ublk_stop_disk "{\"ublk_id\":$ID}" > /dev/null
	for _ in $(seq 50); do [ -e "$(dev)" ] || break; sleep 0.1; done
	R ublk_start_disk "{\"bdev_name\":\"lvsu/lv\",\"ublk_id\":$ID}" > /dev/null || fail "0070 zero copy start at 0"
	wait_dev
	[ "$(dd if="$(dev)" bs=1M skip=1 count=8 iflag=direct status=none | digest)" = "$(digest < "$WORK/pattern2")" ] &&
		pass "0070 with no copy, the data lie 1 MiB into the lvol" || fail "0070 zero copy offset"
	# Through a tier composite, as a Local volume is served: an lvol over the composite, over
	# bdev_uring bands. The composite declares the domains every band declares.
	R ublk_stop_disk "{\"ublk_id\":$ID}" > /dev/null
	for _ in $(seq 50); do [ -e "$(dev)" ] || break; sleep 0.1; done
	truncate -s 256M "$WORK/band0.img" "$WORK/band1.img" "$WORK/band2.img" "$WORK/band3.img"
	tier() { # $1: composite, $2: lvol store, $3 $4: its bands
		R bdev_tier_create "{\"name\":\"$1\",\"md_num_blocks\":4096,\"cluster_blocks\":256}" > /dev/null || fail "bdev_tier_create $1"
		R bdev_tier_add_band "{\"name\":\"$1\",\"base_bdev_name\":\"$3\",\"tier\":1}" > /dev/null || fail "add $3 to $1"
		R bdev_tier_add_band "{\"name\":\"$1\",\"base_bdev_name\":\"$4\",\"tier\":2}" > /dev/null || fail "add $4 to $1"
		R bdev_tier_register "{\"name\":\"$1\"}" > /dev/null || fail "bdev_tier_register $1"
		R bdev_lvol_create_lvstore "{\"bdev_name\":\"$1\",\"lvs_name\":\"$2\",\"cluster_sz\":1048576}" > /dev/null || fail "lvol store on $1"
		R bdev_lvol_create "{\"lvol_name\":\"lv\",\"size_in_mib\":64,\"lvs_name\":\"$2\",\"thin_provision\":true}" > /dev/null || fail "lvol on $1"
	}
	R bdev_uring_create "{\"filename\":\"$WORK/band0.img\",\"name\":\"ub0\",\"block_size\":4096}" > /dev/null || fail "band ub0"
	R bdev_uring_create "{\"filename\":\"$WORK/band1.img\",\"name\":\"ub1\",\"block_size\":4096}" > /dev/null || fail "band ub1"
	tier tc lvst ub0 ub1
	R ublk_start_disk "{\"bdev_name\":\"lvst/lv\",\"ublk_id\":$ID}" > /dev/null || fail "zero copy start over a composite"
	wait_dev
	[[ $(R ublk_get_disks) == *'"enabled": true'* ]] || fail "0067 a disk over a tier composite does not serve with no copy"
	dd if="$WORK/pattern" of="$(dev)" bs=1M oflag=direct conv=nocreat status=none
	[ "$(dd if="$(dev)" bs=1M count=8 iflag=direct status=none | digest)" = "$PATTERN" ] &&
		pass "0067 8 MiB with no copy through a tier composite read back" || fail "0067 read back through a composite"
	R ublk_get_disks | python3 -c 'import json,sys; sys.exit(0 if json.load(sys.stdin)[0]["zero_copy"]["zero_copy"] > 0 else 1)' &&
		pass "0067 requests went with no copy through a tier composite" || fail "0067 no request went with no copy through a composite"
	# 0075: a restore larger than its snapshot. A clone of the composite's lvol, grown past its
	# snapshot, read with no copy: the part past the snapshot is zeroed by the domain. memset
	# into those pages killed the process at the kernel's first read of the device's end.
	R ublk_stop_disk "{\"ublk_id\":$ID}" > /dev/null
	for _ in $(seq 50); do [ -e "$(dev)" ] || break; sleep 0.1; done
	R bdev_lvol_snapshot '{"lvol_name":"lvst/lv","snapshot_name":"snap"}' > /dev/null || fail "0075 snapshot"
	R bdev_lvol_clone '{"snapshot_name":"lvst/snap","clone_name":"cl"}' > /dev/null || fail "0075 clone"
	R bdev_lvol_resize '{"name":"lvst/cl","size_in_mib":96}' > /dev/null || fail "0075 the clone grown"
	R ublk_start_disk "{\"bdev_name\":\"lvst/cl\",\"ublk_id\":$ID}" > /dev/null || fail "0075 start over the grown clone"
	wait_dev
	[ "$(dd if="$(dev)" bs=1M count=8 iflag=direct status=none | digest)" = "$PATTERN" ] &&
		pass "0075 the grown clone reads its snapshot with no copy" || fail "0075 the grown clone does not read its snapshot"
	[ "$(dd if="$(dev)" bs=1M skip=80 count=16 iflag=direct status=none | digest)" = "$(head -c 16777216 /dev/zero | digest)" ] &&
		pass "0075 past the snapshot it reads zeros, zeroed by the domain" || fail "0075 past the snapshot"
	kill -0 "$PID" 2> /dev/null || fail "0075 the process died reading past the snapshot"
	# A band without the domain (bdev_aio declares none): the composite declares none, and its
	# disk is served with copies, as before.
	R ublk_stop_disk "{\"ublk_id\":$ID}" > /dev/null
	for _ in $(seq 50); do [ -e "$(dev)" ] || break; sleep 0.1; done
	R bdev_uring_create "{\"filename\":\"$WORK/band2.img\",\"name\":\"ub2\",\"block_size\":4096}" > /dev/null || fail "band ub2"
	R bdev_aio_create "{\"filename\":\"$WORK/band3.img\",\"name\":\"ab3\",\"block_size\":4096}" > /dev/null || fail "band ab3"
	tier tm lvsm ub2 ab3
	R ublk_start_disk "{\"bdev_name\":\"lvsm/lv\",\"ublk_id\":$ID}" > /dev/null || fail "start over a mixed composite"
	wait_dev
	[[ $(R ublk_get_disks) == *'"enabled": false'* ]] || fail "0067 a composite with a band lacking the domain declared it"
	dd if="$WORK/pattern" of="$(dev)" bs=1M oflag=direct conv=nocreat status=none
	[ "$(dd if="$(dev)" bs=1M count=8 iflag=direct status=none | digest)" = "$PATTERN" ] &&
		pass "0067 a composite with a band lacking the domain is served with copies" || fail "0067 read back through a mixed composite"
else
	skip "0067 zero copy"
fi

echo "=== ublk_destroy_target stops its disks"
R ublk_destroy_target > /dev/null
sleep 1
[ -e /sys/class/ublk-char/ublkc$ID ] && fail "the device outlived ublk_destroy_target" || pass "ublk_destroy_target deleted the device"
stop_term
echo "ALL PASSED"
