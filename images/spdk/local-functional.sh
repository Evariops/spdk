#!/usr/bin/env bash
# The LOCAL transport pair (0076) on a real target: NVMe initiators and an NVMe-oF target of one
# spdk_tgt, joined in memory. Every check runs through the target's own code: hosts, reservations,
# pause, controllers, namespace and ANA events. Needs no device and no privilege, so it runs in the
# debug image as is. Each case prints PASS; the script exits non-zero on the first failure. A wait
# that never ends (a synchronous wait on a reactor the target shares) fails as an RPC timeout.
#
#   docker run --rm -v "$PWD/images/spdk/local-functional.sh:/t.sh:ro" \
#     --entrypoint bash ghcr.io/evariops/spdk-debug:<tag> /t.sh
set -uo pipefail

WORK=$(mktemp -d)
SOCK=$WORK/spdk.sock
SOCK2=$WORK/spdk2.sock
TGT=${SPDK_TGT:-/usr/local/bin/spdk_tgt}
PID=
PID2=
trap 'for p in $PID $PID2; do kill -KILL "$p" 2>/dev/null; done' EXIT

cat > "$WORK/t.py" <<'PY'
# t.py <socket> call <method> [json]          prints the result, exits 1 on an error
# t.py <socket> write|read <ctrlr> <slba> <nlb> <seed>
#                                              a raw I/O command; read compares with the pattern
# t.py <socket> resv <ctrlr> register|acquire|release <cdw10> <key1> [<key2>]
# The I/O and reservation verbs print the completion's status as "sct/sc" (0/0x0 = success).
import base64, json, socket, struct, sys

BLOCK = 4096
path, verb, args = sys.argv[1], sys.argv[2], sys.argv[3:]

def rpc(method, params=None):
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
            return obj
        except ValueError:
            continue

def b64(raw):
    return base64.urlsafe_b64encode(raw).decode()

def cmd(opc, cdw10=0, cdw11=0, cdw12=0):
    b = bytearray(64)
    b[0] = opc
    struct.pack_into("<I", b, 4, 1)
    struct.pack_into("<III", b, 40, cdw10, cdw11, cdw12)
    return b64(bytes(b))

def pattern(seed, n):
    return bytes((seed * 31 + i * 7 + (i >> 12)) & 0xFF for i in range(n))

def status(obj):
    if "error" in obj:
        print(json.dumps(obj["error"]))
        sys.exit(1)
    cpl = base64.urlsafe_b64decode(obj["result"]["cpl"])
    word = struct.unpack_from("<H", cpl, 14)[0]
    return (word >> 9) & 0x7, (word >> 1) & 0xFF

def send(ctrlr, direction, cmdbuf, data=None, length=None):
    params = {"name": ctrlr, "cmd_type": "io", "data_direction": direction, "cmdbuf": cmdbuf,
              "timeout_ms": 30000}
    if data is not None:
        params["data"] = b64(data)
    if length is not None:
        params["data_len"] = length
    return rpc("bdev_nvme_send_cmd", params)

if verb == "call":
    obj = rpc(args[0], json.loads(args[1]) if len(args) > 1 else None)
    if "error" in obj:
        print(json.dumps(obj["error"]))
        sys.exit(1)
    print(json.dumps(obj.get("result")))
elif verb in ("write", "read"):
    ctrlr, slba, nlb, seed = args[0], int(args[1]), int(args[2]), int(args[3])
    c = cmd(1 if verb == "write" else 2, slba & 0xFFFFFFFF, slba >> 32, nlb - 1)
    if verb == "write":
        obj = send(ctrlr, "h2c", c, data=pattern(seed, nlb * BLOCK))
        sct, sc = status(obj)
        print(f"{sct}/{sc:#x}")
    else:
        obj = send(ctrlr, "c2h", c, length=nlb * BLOCK)
        sct, sc = status(obj)
        got = base64.urlsafe_b64decode(obj["result"].get("data", "")) if (sct, sc) == (0, 0) else b""
        same = "same" if got == pattern(seed, nlb * BLOCK) else "different"
        print(f"{sct}/{sc:#x} {same}" if (sct, sc) == (0, 0) else f"{sct}/{sc:#x}")
elif verb == "resv":
    ctrlr, kind, cdw10 = args[0], args[1], int(args[2], 0)
    keys = [int(k, 0) for k in args[3:]]
    opc = {"register": 0x0D, "acquire": 0x11, "release": 0x15}[kind]
    data = struct.pack("<" + "Q" * len(keys), *keys)
    sct, sc = status(send(ctrlr, "h2c", cmd(opc, cdw10), data=data))
    print(f"{sct}/{sc:#x}")
else:
    sys.exit("unknown verb " + verb)
PY

T() { python3 "$WORK/t.py" "$SOCK" "$@"; }
T2() { python3 "$WORK/t.py" "$SOCK2" "$@"; }
R() { T call "$@"; }
pass() { echo "PASS $*"; }
fail() { echo "FAIL $*"; tail -30 "$WORK/tgt.log"; exit 1; }

S1=nqn.2026-10.io.evariops:local-1
S2=nqn.2026-10.io.evariops:local-2
S3=nqn.2026-10.io.evariops:local-3
S4=nqn.2026-10.io.evariops:local-4
HA=nqn.2026-10.io.evariops:host-a
HB=nqn.2026-10.io.evariops:host-b
HC=nqn.2026-10.io.evariops:host-c
L='"trtype":"LOCAL","traddr":"node0","trsvcid":"1"'
OK=0/0x0
CONFLICT=0/0x83

# $1 bdev, $2 a distinct namespace identity: a bdev_nvme of the same process registers the
# namespace's UUID as an alias, which the namespace's own bdev already holds.
ns() { R nvmf_subsystem_add_ns "{\"nqn\":\"$3\",\"namespace\":{\"bdev_name\":\"$1\",\"uuid\":\"0b7a1f3e-1111-4c2d-9e8f-00000000000$2\",\"nguid\":\"0B7A1F3E11114C2D9E8F00000000000$2\"}}" > /dev/null; }
attach() { # $1 name, $2 subsystem, $3 host
	R bdev_nvme_attach_controller "{\"name\":\"$1\",$L,\"subnqn\":\"$2\",\"hostnqn\":\"$3\",\"ctrlr_loss_timeout_sec\":2,\"reconnect_delay_sec\":1}" > /dev/null
}
blocks() { R bdev_get_bdevs "{\"name\":\"$1\"}" 2> /dev/null | python3 -c 'import json,sys; print(json.load(sys.stdin)[0]["num_blocks"])'; }
# A controller that cannot reconnect is deleted once its ctrlr_loss_timeout_sec has run.
deleted() { ! R bdev_nvme_get_controllers "{\"name\":\"$1\"}" > /dev/null 2>&1; }
until_true() { # $1 seconds, then a command
	local end=$((SECONDS + $1))
	shift
	while ((SECONDS < end)); do "$@" && return 0; sleep 0.2; done
	return 1
}

rm -f "$SOCK"
# No trace file: a killed process would leave it in /dev/shm.
setsid "$TGT" --no-huge -s 1024 -m 0x1 -r "$SOCK" --num-trace-entries 0 > "$WORK/tgt.log" 2>&1 < /dev/null &
PID=$!
until_true 20 test -S "$SOCK" || fail "spdk_tgt did not open its socket"
R spdk_get_version > /dev/null || fail "spdk_tgt does not answer"

R nvmf_create_transport '{"trtype":"LOCAL"}' > /dev/null || fail "LOCAL transport"
R bdev_malloc_create '{"name":"M1","num_blocks":16384,"block_size":4096}' > /dev/null || fail "malloc"
R bdev_malloc_create '{"name":"M4","num_blocks":16384,"block_size":4096}' > /dev/null || fail "malloc"
R bdev_null_create '{"name":"N3","num_blocks":16384,"block_size":4096}' > /dev/null || fail "null"
R nvmf_create_subsystem "{\"nqn\":\"$S1\",\"allow_any_host\":true}" > /dev/null || fail "subsystem"
R nvmf_create_subsystem "{\"nqn\":\"$S2\"}" > /dev/null || fail "subsystem"
R nvmf_create_subsystem "{\"nqn\":\"$S3\",\"allow_any_host\":true}" > /dev/null || fail "subsystem"
R nvmf_create_subsystem "{\"nqn\":\"$S4\",\"allow_any_host\":true,\"ana_reporting\":true}" > /dev/null || fail "subsystem"
ns M1 1 "$S1" && ns N3 3 "$S3" && ns M4 4 "$S4" || fail "namespaces"
R bdev_malloc_create '{"name":"M2","num_blocks":4096,"block_size":4096}' > /dev/null && ns M2 2 "$S2" || fail "namespace"
for s in "$S1" "$S2" "$S3" "$S4"; do
	R nvmf_subsystem_add_listener "{\"nqn\":\"$s\",\"listen_address\":{$L}}" > /dev/null || fail "listener on $s"
done

# ── The data stay in the initiator's buffers and reach the target's bdev ──
attach A "$S1" "$HA" || fail "attach A"
attach B "$S1" "$HB" || fail "attach B"
[ "$(T write A 0 1 1)" = "$OK" ] || fail "write through A"
[ "$(T read B 0 1 1)" = "$OK same" ] || fail "read through B of what A wrote"
[ "$(T write A 64 32 2)" = "$OK" ] || fail "128 KiB write (the MDTS) through A"
[ "$(T read B 64 32 2)" = "$OK same" ] || fail "128 KiB read through B"
# 160 KiB: above the MDTS (128 KiB), within what one RPC carries (256 KiB of JSON).
[ "$(T write A 128 40 3)" = "0/0xf" ] || fail "a command above the MDTS is refused (SGL length)"
pass "data: written through one LOCAL controller, read through another; the MDTS holds"

# ── L-7: the target lists its LOCAL controllers and their address ──
ctrls=$(R nvmf_subsystem_get_controllers "{\"nqn\":\"$S1\"}") || fail "controllers"
[[ "$ctrls" == *"$HA"* && "$ctrls" == *"$HB"* ]] || fail "the target lists A and B: $ctrls"
qpairs=$(R nvmf_subsystem_get_qpairs "{\"nqn\":\"$S1\"}") || fail "qpairs"
[[ "$qpairs" == *'"trtype": "LOCAL"'* && "$qpairs" == *'"traddr": "node0"'* ]] || fail "qpairs: $qpairs"
pass "controllers: the target lists them, with their hosts and the LOCAL address"

# ── L-1: the subsystem's hosts ──
attach C "$S2" "$HC" 2> /dev/null && fail "a host the subsystem does not allow connects"
R nvmf_subsystem_add_host "{\"nqn\":\"$S2\",\"host\":\"$HC\"}" > /dev/null || fail "add host"
attach C "$S2" "$HC" || fail "an allowed host connects"
[ "$(T write C 0 1 4)" = "$OK" ] || fail "an allowed host writes"
R nvmf_subsystem_remove_host "{\"nqn\":\"$S2\",\"host\":\"$HC\"}" > /dev/null || fail "remove host"
gone() { [ "$(T write C 0 1 4 2> /dev/null)" != "$OK" ]; }
until_true 10 gone || fail "a host removed keeps writing"
until_true 15 deleted C || fail "C, refused at each reconnect, is not deleted"
pass "hosts: refused, allowed, then cut off when removed (and refused at the reconnect)"

# ── L-2: reservations, between the LOCAL controller and a host of another process ──
# The controllers of one process share its host ID (bdev_nvme takes the driver's): A and B are one
# host to the target. The other host is a second spdk_tgt, over TCP, as another node would be.
R nvmf_create_transport '{"trtype":"TCP"}' > /dev/null || fail "TCP transport"
R nvmf_subsystem_add_listener "{\"nqn\":\"$S1\",\"listen_address\":{\"trtype\":\"TCP\",\"adrfam\":\"IPv4\",\"traddr\":\"127.0.0.1\",\"trsvcid\":\"4420\"}}" > /dev/null || fail "TCP listener"
# The same core as the first: without the core's lock, which the first holds.
setsid "$TGT" --no-huge -s 1024 -m 0x1 -r "$SOCK2" --num-trace-entries 0 --disable-cpumask-locks \
	> "$WORK/tgt2.log" 2>&1 < /dev/null &
PID2=$!
until_true 20 test -S "$SOCK2" || fail "the second spdk_tgt did not open its socket"
T2 call bdev_nvme_attach_controller "{\"name\":\"X\",\"trtype\":\"TCP\",\"adrfam\":\"IPv4\",\"traddr\":\"127.0.0.1\",\"trsvcid\":\"4420\",\"subnqn\":\"$S1\",\"hostnqn\":\"$HC\"}" > /dev/null || fail "attach X over TCP"
[ "$(T resv A register 0x0 0 0xA)" = "$OK" ] || fail "A registers"
[ "$(T resv A acquire 0x100 0xA 0)" = "$OK" ] || fail "A takes Write Exclusive"
[ "$(T2 write X 0 1 5)" = "$CONFLICT" ] || fail "the other host writes through A's reservation"
[ "$(T2 read X 0 1 1)" = "$OK same" ] || fail "the other host reads under Write Exclusive"
[ "$(T resv B acquire 0x100 0xA 0)" = "$OK" ] || fail "B, the same host as A, holds the reservation"
[ "$(T2 resv X register 0x0 0 0xB)" = "$OK" ] || fail "the other host registers"
[ "$(T2 resv X acquire 0x101 0xB 0xA)" = "$OK" ] || fail "the other host preempts A"
[ "$(T write A 0 1 6)" = "$CONFLICT" ] || fail "A, preempted, still writes"
[ "$(T2 write X 0 1 7)" = "$OK" ] || fail "the holder writes"
[ "$(T read A 0 1 7)" = "$OK same" ] || fail "A reads what the holder wrote"
[ "$(T2 resv X release 0x1 0xB)" = "$OK" ] || fail "the holder clears"
[ "$(T write A 0 1 8)" = "$OK" ] || fail "A writes once cleared"
T2 call bdev_nvme_detach_controller '{"name":"X"}' > /dev/null || fail "detach X"
pass "reservations: a LOCAL writer is held off, then preempted, by a host of another process"

# ── L-3: a preempt-and-abort lands once the preempted LOCAL writer's writes are done ──
# The namespace's writes take 2 s (a delay bdev): one is in flight when the other host preempts.
S5=nqn.2026-10.io.evariops:local-5
R bdev_malloc_create '{"name":"M5","num_blocks":256,"block_size":4096}' > /dev/null || fail "malloc"
R bdev_delay_create '{"base_bdev_name":"M5","name":"W5","avg_read_latency":0,"p99_read_latency":0,"avg_write_latency":2000000,"p99_write_latency":2000000}' > /dev/null || fail "delay"
R nvmf_create_subsystem "{\"nqn\":\"$S5\",\"allow_any_host\":true}" > /dev/null || fail "subsystem"
ns W5 5 "$S5" || fail "namespace"
R nvmf_subsystem_add_listener "{\"nqn\":\"$S5\",\"listen_address\":{$L}}" > /dev/null || fail "listener"
R nvmf_subsystem_add_listener "{\"nqn\":\"$S5\",\"listen_address\":{\"trtype\":\"TCP\",\"adrfam\":\"IPv4\",\"traddr\":\"127.0.0.1\",\"trsvcid\":\"4420\"}}" > /dev/null || fail "TCP listener"
attach F "$S5" "$HA" || fail "attach F"
T2 call bdev_nvme_attach_controller "{\"name\":\"Y\",\"trtype\":\"TCP\",\"adrfam\":\"IPv4\",\"traddr\":\"127.0.0.1\",\"trsvcid\":\"4420\",\"subnqn\":\"$S5\",\"hostnqn\":\"$HC\"}" > /dev/null || fail "attach Y over TCP"
[ "$(T resv F register 0x0 0 0xA)" = "$OK" ] || fail "F registers"
[ "$(T resv F acquire 0x100 0xA 0)" = "$OK" ] || fail "F takes Write Exclusive"
[ "$(T2 resv Y register 0x0 0 0xB)" = "$OK" ] || fail "Y registers"
T write F 0 1 12 > "$WORK/inflight.out" &
inflight=$!
sleep 0.3
start=$(date +%s%N)
[ "$(T2 resv Y acquire 0x102 0xB 0xA)" = "$OK" ] || fail "Y preempts and aborts F"
waited=$((($(date +%s%N) - start) / 1000000))
wait "$inflight"
[ "$(cat "$WORK/inflight.out")" = "$OK" ] || fail "F's write in flight before the preempt: $(cat "$WORK/inflight.out")"
((waited >= 1200)) || fail "the preempt landed after ${waited} ms, before F's write in flight (about 1700 ms left)"
[ "$(T write F 0 1 13)" = "$CONFLICT" ] || fail "F, preempted, still writes"
T2 call bdev_nvme_detach_controller '{"name":"Y"}' > /dev/null || fail "detach Y"
R bdev_nvme_detach_controller '{"name":"F"}' > /dev/null || fail "detach F"
kill -TERM "$PID2"
wait "$PID2"
PID2=
pass "preempt-and-abort: it lands ${waited} ms later, once the LOCAL write in flight is done"

# ── L-6: the subsystem's pause holds the LOCAL controllers' I/O ──
token=$(R nvmf_subsystem_pause "{\"nqn\":\"$S1\",\"ttl_ms\":30000}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["token"])') || fail "pause"
T write B 8 1 9 > "$WORK/paused.out" &
bg=$!
sleep 1
kill -0 "$bg" 2> /dev/null || fail "a write completed while the subsystem was paused: $(cat "$WORK/paused.out")"
R nvmf_subsystem_resume "{\"nqn\":\"$S1\",\"token\":\"$token\"}" > /dev/null || fail "resume"
wait "$bg"
[ "$(cat "$WORK/paused.out")" = "$OK" ] || fail "the write held by the pause: $(cat "$WORK/paused.out")"
[ "$(T read A 8 1 9)" = "$OK same" ] || fail "the write held by the pause landed"
pass "pause: a LOCAL write waits for the resume, then lands"

# ── Namespace change event (0072): an expansion reaches the initiator ──
attach D "$S3" "$HA" || fail "attach D"
[ "$(blocks Dn1)" = 16384 ] || fail "Dn1 size"
R bdev_null_resize '{"name":"N3","new_size":128}' > /dev/null || fail "resize"
grown() { [ "$(blocks Dn1)" = 32768 ]; }
until_true 10 grown || fail "the initiator did not see the namespace grow"
pass "namespace event: the expansion reaches the LOCAL initiator"

# ── ANA change event (0073) ──
attach E "$S4" "$HA" || fail "attach E"
ana() { R bdev_get_bdevs '{"name":"En1"}' | grep -q "\"ana_state\": \"$1\""; }
until_true 10 ana optimized || fail "En1 not optimized"
R nvmf_subsystem_listener_set_ana_state "{\"nqn\":\"$S4\",\"listen_address\":{$L},\"ana_state\":\"inaccessible\"}" > /dev/null || fail "set ANA"
until_true 10 ana inaccessible || fail "the initiator did not see the ANA change"
R nvmf_subsystem_listener_set_ana_state "{\"nqn\":\"$S4\",\"listen_address\":{$L},\"ana_state\":\"optimized\"}" > /dev/null || fail "set ANA"
until_true 10 ana optimized || fail "the initiator did not see the ANA change back"
pass "ANA event: the change reaches the LOCAL initiator, both ways"

# ── 0074: in this process nothing waits synchronously on an admin queue ──
if grep -q 'synchronous wait #' "$WORK/tgt.log"; then
	grep 'synchronous wait #' "$WORK/tgt.log" | head -5
	fail "a synchronous admin wait ran: with LOCAL, it spins the reactor the target needs"
fi
pass "no synchronous admin wait"

# ── Teardown: a subsystem deleted under its controllers, then the process ──
R nvmf_delete_subsystem "{\"nqn\":\"$S1\"}" > /dev/null || fail "delete S1"
lost() { [ "$(T write A 0 1 10 2> /dev/null)" != "$OK" ] && [ "$(T write B 0 1 10 2> /dev/null)" != "$OK" ]; }
until_true 10 lost || fail "A and B still write to a deleted subsystem"
R spdk_get_version > /dev/null || fail "spdk_tgt died with the subsystem"
until_true 15 deleted A || fail "A, with no subsystem to reconnect to, is not deleted"
until_true 15 deleted B || fail "B, with no subsystem to reconnect to, is not deleted"
# D and E stay attached: the stop takes the target down under them.
kill -TERM "$PID"
until_true 20 bash -c "! kill -0 $PID 2> /dev/null" || fail "spdk_tgt did not stop on SIGTERM"
wait "$PID"
rc=$?
PID=
[ "$rc" = 0 ] || fail "spdk_tgt exited $rc"
pass "teardown: controllers cut off by a deleted subsystem, then a clean stop"
