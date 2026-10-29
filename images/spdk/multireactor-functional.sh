#!/usr/bin/env bash
# Two reactors under load (0076-0078): the stack a node's one process runs, on -m 0x3.
#   - a ublk disk with two queues, served by a ublk thread on each core;
#   - over a raid1 whose two members are reached through the LOCAL pair (0076);
#   - over two lvols of one tier composite (its band state, 0078's registries, iobuf, 0077).
# Writers fill regions of the disk while the second member's subsystem is paused and resumed
# and both members' controllers are reset. Then every region reads back, both mirrors hold
# the same data, both reactors served I/O, and the process is alive. Each case prints PASS;
# the script exits non-zero on the first failure. Run it inside the debug image, privileged,
# with the host's /dev and ublk_drv loaded, as ublk-functional.sh.
set -uo pipefail

WORK=$(mktemp -d)
SOCK=$WORK/spdk.sock
TGT=${SPDK_TGT:-/usr/local/bin/spdk_tgt}
ID=11
PID=
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
fail() { echo "FAIL $*"; tail -30 "$WORK/tgt.log"; exit 1; }
digest() { sha256sum | cut -d' ' -f1; }
dev() { echo "/dev/ublkb$1"; }
wait_dev() { for _ in $(seq 50); do [ -b "$(dev "$1")" ] && return 0; sleep 0.1; done; fail "$(dev "$1") did not appear"; }
gone_dev() { for _ in $(seq 50); do [ -e "$(dev "$1")" ] || return 0; sleep 0.1; done; fail "$(dev "$1") did not go"; }

if [ "$(nproc)" -lt 2 ]; then
	echo "SKIP two reactors: the host has one CPU"
	exit 0
fi

rm -f "$SOCK"
# shellcheck disable=SC2086
setsid "$TGT" --no-huge -s 2048 -m 0x3 -r "$SOCK" --num-trace-entries 0 ${SPDK_EXTRA:-} > "$WORK/tgt.log" 2>&1 < /dev/null &
PID=$!
for _ in $(seq 100); do [ -S "$SOCK" ] && R spdk_get_version > /dev/null 2>&1 && break; sleep 0.1; done
R spdk_get_version > /dev/null || fail "spdk_tgt did not start"
reactors=$(R framework_get_reactors | python3 -c 'import json,sys; print(len(json.load(sys.stdin)["reactors"]))')
[ "$reactors" = 2 ] || fail "spdk_tgt runs $reactors reactors, not 2"

# A member that resets must not fail the raid's writes: they wait for the reconnect.
R bdev_nvme_set_options '{"bdev_retry_count":-1}' > /dev/null || fail "bdev_nvme options"

# ── The stack ──
truncate -s 512M "$WORK/band0.img" "$WORK/band1.img"
R bdev_uring_create "{\"filename\":\"$WORK/band0.img\",\"name\":\"ub0\",\"block_size\":4096}" > /dev/null || fail "band ub0"
R bdev_uring_create "{\"filename\":\"$WORK/band1.img\",\"name\":\"ub1\",\"block_size\":4096}" > /dev/null || fail "band ub1"
R bdev_tier_create '{"name":"tc","md_num_blocks":4096,"cluster_blocks":256}' > /dev/null || fail "tier"
R bdev_tier_add_band '{"name":"tc","base_bdev_name":"ub0","tier":1}' > /dev/null || fail "band 0"
R bdev_tier_add_band '{"name":"tc","base_bdev_name":"ub1","tier":2}' > /dev/null || fail "band 1"
R bdev_tier_register '{"name":"tc"}' > /dev/null || fail "tier register"
R bdev_lvol_create_lvstore '{"bdev_name":"tc","lvs_name":"lvs","cluster_sz":1048576}' > /dev/null || fail "lvol store"
for m in 0 1; do
	R bdev_lvol_create "{\"lvol_name\":\"m$m\",\"size_in_mib\":128,\"lvs_name\":\"lvs\",\"thin_provision\":true}" > /dev/null || fail "lvol m$m"
done
# MEMBERS=TCP plays the same over NVMe/TCP loopback, today's path, to compare.
if [ "${MEMBERS:-LOCAL}" = TCP ]; then
	R nvmf_create_transport '{"trtype":"TCP"}' > /dev/null || fail "TCP transport"
	L='"trtype":"TCP","adrfam":"IPv4","traddr":"127.0.0.1","trsvcid":"4420"'
else
	R nvmf_create_transport '{"trtype":"LOCAL"}' > /dev/null || fail "LOCAL transport"
	L='"trtype":"LOCAL","traddr":"node0","trsvcid":"1"'
fi
for m in 0 1; do
	nqn=nqn.2026-10.io.evariops:member-$m
	R nvmf_create_subsystem "{\"nqn\":\"$nqn\",\"allow_any_host\":true}" > /dev/null || fail "subsystem $m"
	R nvmf_subsystem_add_ns "{\"nqn\":\"$nqn\",\"namespace\":{\"bdev_name\":\"lvs/m$m\",\"uuid\":\"0b7a1f3e-3333-4c2d-9e8f-00000000000$m\",\"nguid\":\"0B7A1F3E33334C2D9E8F00000000000$m\"}}" > /dev/null || fail "namespace $m"
	R nvmf_subsystem_add_listener "{\"nqn\":\"$nqn\",\"listen_address\":{$L}}" > /dev/null || fail "listener $m"
	# A member that loses its target reconnects and its writes wait, however long it takes.
	R bdev_nvme_attach_controller "{\"name\":\"L$m\",$L,\"subnqn\":\"$nqn\",\"hostnqn\":\"nqn.2026-10.io.evariops:writer\",\"ctrlr_loss_timeout_sec\":-1,\"reconnect_delay_sec\":1}" > /dev/null || fail "attach L$m"
done
R bdev_raid_create '{"name":"r0","raid_level":"raid1","strip_size_kb":0,"base_bdevs":["L0n1","L1n1"],"superblock":false,"incarnation":"multireactor"}' > /dev/null || fail "raid1"
R ublk_create_target '{"cpumask":"0x3"}' > /dev/null || fail "ublk target on both cores"
R ublk_start_disk "{\"bdev_name\":\"r0\",\"ublk_id\":$ID,\"num_queues\":2,\"queue_depth\":64}" > /dev/null || fail "ublk disk with two queues"
wait_dev $ID
pass "the stack: a two-queue ublk disk over a raid1 whose members are reached through ${MEMBERS:-LOCAL}, over a tier composite, on two reactors"

# ── Writers, while the members are paused and reset ──
REGIONS=8
for i in $(seq 0 $((REGIONS - 1))); do head -c 8388608 /dev/urandom > "$WORK/region$i"; done
writers=()
for i in $(seq 0 $((REGIONS - 1))); do
	# Each writer on its own CPU where taskset exists, so that both queues (and both ublk
	# threads) carry writes; blk-mq maps a CPU to a queue.
	pin=()
	command -v taskset > /dev/null && pin=(taskset -c $((i % $(nproc))))
	( "${pin[@]}" dd if="$WORK/region$i" of="$(dev $ID)" bs=64k seek=$((i * 128)) oflag=direct conv=nocreat status=none ||
		echo "$i" >> "$WORK/writers.err" ) &
	writers+=($!)
done
# A member's target goes away for a moment, as a member's node would: its listener is removed
# (nvmf closes the member's qpairs) and added back. Not bdev_nvme_reset_controller: an explicit
# reset fails what it aborts with "do not retry", and the raid ejects the member, by design.
for round in $(seq 1 6); do
	token=$(R nvmf_subsystem_pause '{"nqn":"nqn.2026-10.io.evariops:member-1","ttl_ms":5000}' |
		python3 -c 'import json,sys; print(json.load(sys.stdin)["token"])') || fail "round $round: pause"
	sleep 0.2
	R nvmf_subsystem_resume "{\"nqn\":\"nqn.2026-10.io.evariops:member-1\",\"token\":\"$token\"}" > /dev/null || fail "round $round: resume"
	away=nqn.2026-10.io.evariops:member-$((round % 2))
	R nvmf_subsystem_remove_listener "{\"nqn\":\"$away\",\"listen_address\":{$L}}" > /dev/null || fail "round $round: remove $away's listener"
	sleep 0.3
	R nvmf_subsystem_add_listener "{\"nqn\":\"$away\",\"listen_address\":{$L}}" > /dev/null || fail "round $round: add $away's listener back"
	sleep 0.2
done
# The writers alone: a bare wait would also wait for spdk_tgt.
wait "${writers[@]}"
[ -f "$WORK/writers.err" ] && fail "writes failed in regions: $(tr '\n' ' ' < "$WORK/writers.err")"
kill -0 "$PID" 2> /dev/null || fail "the process died under the writers"
grep -q 'Failing base bdev' "$WORK/tgt.log" && fail "the raid ejected a member: $(grep -m1 'Failing base bdev' "$WORK/tgt.log")"
pass "8 regions of 8 MiB written from every CPU through 6 pauses and 6 losses of a member's target, no member ejected"

# Both reactors served: a ublk thread busy on each core, and LOCAL I/O qpairs on both of
# nvmf's poll groups.
R thread_get_stats > "$WORK/threads.json" || fail "thread stats"
python3 - "$WORK/threads.json" <<'PY' || fail "a ublk thread did no work: $(cat "$WORK/threads.json" | head -c 2000)"
import json, sys
threads = json.load(open(sys.argv[1]))["threads"]
busy = [t for t in threads if t["name"].startswith("ublk") and t["busy"] > 0]
sys.exit(0 if len(busy) >= 2 else 1)
PY
R nvmf_get_stats > "$WORK/nvmf.json" || fail "nvmf stats"
python3 - "$WORK/nvmf.json" <<'PY' || fail "the members' I/O qpairs sit on one poll group: $(head -c 2000 "$WORK/nvmf.json")"
import json, sys
groups = json.load(open(sys.argv[1]))["poll_groups"]
sys.exit(0 if sum(1 for g in groups if g.get("current_io_qpairs", 0) > 0) >= 2 else 1)
PY
pass "both reactors served: two busy ublk threads, the members' I/O qpairs on two poll groups"

# ── Read back through the raid, then each mirror on its own ──
for i in $(seq 0 $((REGIONS - 1))); do
	[ "$(dd if="$(dev $ID)" bs=64k skip=$((i * 128)) count=128 iflag=direct status=none | digest)" = "$(digest < "$WORK/region$i")" ] ||
		fail "region $i reads back wrong through the raid"
done
pass "every region reads back through the raid"
R ublk_stop_disk "{\"ublk_id\":$ID}" > /dev/null || fail "stop the raid's disk"
gone_dev $ID
R bdev_raid_delete '{"name":"r0"}' > /dev/null || fail "raid delete"
for m in 0 1; do
	R bdev_nvme_detach_controller "{\"name\":\"L$m\"}" > /dev/null || fail "detach L$m"
	# nvmf claims its namespaces' bdevs: the member leaves its subsystem first.
	R nvmf_delete_subsystem "{\"nqn\":\"nqn.2026-10.io.evariops:member-$m\"}" > /dev/null || fail "delete subsystem $m"
	# The deletion releases the namespace's bdev a little later.
	started=
	for _ in $(seq 50); do
		R ublk_start_disk "{\"bdev_name\":\"lvs/m$m\",\"ublk_id\":$((ID + 1 + m))}" > /dev/null 2>&1 && { started=1; break; }
		sleep 0.1
	done
	[ -n "$started" ] || fail "disk on m$m: $(R ublk_start_disk "{\"bdev_name\":\"lvs/m$m\",\"ublk_id\":$((ID + 1 + m))}" 2>&1)"
	wait_dev $((ID + 1 + m))
done
for i in $(seq 0 $((REGIONS - 1))); do
	for m in 0 1; do
		[ "$(dd if="$(dev $((ID + 1 + m)))" bs=64k skip=$((i * 128)) count=128 iflag=direct status=none | digest)" = "$(digest < "$WORK/region$i")" ] ||
			fail "region $i differs on mirror m$m"
	done
done
pass "both mirrors hold every region"

for m in 0 1; do R ublk_stop_disk "{\"ublk_id\":$((ID + 1 + m))}" > /dev/null; done
R ublk_destroy_target > /dev/null
kill -TERM "$PID"
for _ in $(seq 100); do kill -0 "$PID" 2> /dev/null || break; sleep 0.1; done
kill -0 "$PID" 2> /dev/null && fail "spdk_tgt did not stop on SIGTERM"
wait "$PID"
rc=$?
PID=
[ "$rc" = 0 ] || fail "spdk_tgt exited $rc"
pass "a clean stop"
echo "ALL PASSED"
