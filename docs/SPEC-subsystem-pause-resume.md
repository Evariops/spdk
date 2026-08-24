# SPEC-66A — `nvmf_subsystem_pause` / `nvmf_subsystem_resume` JSON-RPC

| Field | Value |
|:------|:-------|
| **Target repository** | **`Evariops/spdk`** (SPDK container-image build: upstream-from-source + out-of-tree modules + patches). Companion fork-side spec, same family as `docs/SPEC-allocated-ranges.md` (SPEC-60A). |
| **Companion of** | `spdk-csi` **SPEC-66** action **H10** (Phase 2 — fork barrier RPC) and SPEC-58 (amended: "one quiesce barrier" → capability-gated). Sole consumer: the spdk-csi controller (`GroupSnapshotSaga`, future restore/migration barriers). |
| **Upstream base** | `spdk/spdk` v26.01 (per `images/spdk/Dockerfile` `ARG SPDK_VERSION=v26.01`) |
| **Delivery vehicle** | `patches/0003-nvmf-add-subsystem-pause-resume-rpc.patch` (same mechanism as `0001`/`0002`; the Dockerfile already applies `patches/*.patch` in a loop — no Dockerfile edit) |
| **Priority** | Medium — unblocks SPEC-66 INV-41b (true cross-volume crash-consistent group snapshots); strictly optional behind a capability probe (vanilla path unchanged) |

---

## 1. Motivation

SPEC-58 demanded **one quiesce barrier** so that multi-replica / multi-volume snapshots are captured at a single consistent instant. Vanilla SPDK 26.01 has **no JSON-RPC that freezes and drains a live exposure**:

- `bdev_lvol_snapshot`'s `blob_freeze_io` blocks *new submissions* but does **not** drain in-flight I/O (`lib/blob/blobstore.c:525-546,3151-3164`) → crash-consistent via cluster sharing, never an application-consistent point.
- There is **no `bdev_quiesce` RPC** and no `nvmf_subsystem_pause` RPC in the vanilla registry (`lib/nvmf/nvmf_rpc.c` — pause is used *internally* by `add_ns`/`remove_ns`/`add_listener` but never exposed as a standalone, standing operation).

Consequently the spdk-csi controller falls back to a best-effort **ANA-drain** barrier on vanilla (mark listeners `inaccessible`, sample `queue_depth==0`) and honestly refuses true atomicity: group snapshots return `FAILED_PRECONDITION` unless explicitly opted-in as non-atomic (spdk-csi SPEC-66 H9). That is correct but degraded — there is no *proof* the replicas were frozen together.

### 1.1 Mechanism — already decided by source verification (SPEC-66 H10)

Three barriers were evaluated in the SPDK source; **`spdk_nvmf_subsystem_pause` is the chosen mechanism**, not `spdk_bdev_quiesce`:

| Candidate | Verdict |
|:--|:--|
| ANA `inaccessible` drain (vanilla) | Best-effort only: in-flight I/O get **no drain signal**, only indirect `queue_depth` sampling. Kept as the vanilla degraded mode. |
| `spdk_bdev_quiesce` (C API) | True drain, but module-check needs the registering module, range-collision risk with bdev_raid's internal quiesces (`bdev.c:11126-11131`), **no TTL**, QoS-throttled I/O lengthen the drain. Fallback only. |
| **`spdk_nvmf_subsystem_pause` (chosen)** | **Drain certified by callback** (`lib/nvmf/nvmf.c:1964-1995`, completion `lib/nvmf/ctrlr.c:5306-5325`); new I/O **queued target-side, zero host errors** (`ctrlr.c:5398-5402`, resumed `nvmf.c:2034-2039`); public API already used by every nvmf RPC; per-subsystem scope = exactly "freeze this replica's exposure". |

**This spec turns that public C API into two standing JSON-RPCs**, with the one property the existing internal callers don't need and we critically do: the subsystem **stays Paused across RPC calls** (pause → caller snapshots all members via other RPCs → resume), guarded by a **mandatory server-side TTL/auto-resume** so a leaked pause self-heals.

### 1.2 Why a *standing* pause is the hard part

Every existing nvmf RPC that pauses (`rpc_nvmf_subsystem_add_ns`, `…add_listener`, `…set_ns_ana_group`) pauses, performs its mutation **inside the paused callback**, and resumes **within the same operation** (`nvmf_rpc.c:943,1595,1712`). The subsystem is never observably paused after the RPC returns. Our barrier is the opposite: the pause must **outlive the RPC** so the controller can capture every replica between a `pause` and a `resume`. That introduces two obligations absent upstream:

1. **A registry of standing pauses** (so `resume` finds the paused subsystem and cancels its timer, and so a re-`pause` is idempotent).
2. **A TTL auto-resume** (so a controller crash / lost `resume` cannot strand a replica Paused — a Paused namespace queues host I/O indefinitely; without a ceiling the replica is silently offline).

### Non-goals

- **No multi-subsystem barrier in one RPC.** One RPC = one subsystem. Fan-out across a group's replica subsystems (pause-all → snapshot-all → resume-all in `finally`) is the **controller's** job — keeps this C surface minimal and stateless beyond the single pause it owns. (Mirrors SPEC-60A's "composition is the consumer's job".)
- **No persistence of pause state.** Pause is process-RAM (PR-8 persistence class): an SPDK restart brings every subsystem back **Active**, never stuck-Paused. This is the *safe* default and is relied upon (a crashed target self-clears all barriers). Documented, not a bug.
- **No `bdev_quiesce` RPC.** Rejected above; do not add it under the same capability.
- **No change to the vanilla data path.** The RPCs are additive; absent them, the controller's ANA-drain / honest-refusal path (SPEC-66 H9) is unchanged.

---

## 2. API specification

Two RPCs, both `SPDK_RPC_RUNTIME`, both responding **only after the state-change callback fires** (this is the correctness contract — the response *certifies* the drain / the resume).

### 2.1 `nvmf_subsystem_pause`

**Request**

```json
{
  "jsonrpc": "2.0",
  "method": "nvmf_subsystem_pause",
  "id": 1,
  "params": {
    "nqn": "nqn.2024-01.io.evariops:replica-a1b2c3-0",
    "nsid": 1,
    "ttl_ms": 30000,
    "tgt_name": "nvmf_tgt"
  }
}
```

| Param | Type | Required | Semantics |
|:------|:-----|:--------:|:----------|
| `nqn` | string | ✅ | subsystem NQN, resolved via `spdk_nvmf_tgt_find_subsystem` (`nvmf.h:686`) |
| `nsid` | uint32 | ◻ (default = first namespace) | namespace to drain. **`0` is rewritten to the subsystem's first namespace id**, never passed through: `spdk_nvmf_subsystem_pause(ss, 0, …)` freezes only admin queues and does **not** queue/drain namespace I/O (`nvmf.h:642-660`) — that is not a barrier. Our replicas expose exactly one namespace, so the default is correct; the param exists for explicitness. If the subsystem has **no** namespace (resolution still yields `0`), the pause is refused with `-EINVAL` rather than acking a barrier that drains nothing. |
| `ttl_ms` | uint32 | ◻ (default 30000, max 60000) | server-side auto-resume deadline; `0`→default, `>max`→clamped. Refreshed on every re-pause. **Keep below the host I/O timeout** (`nvme_core.io_timeout`, Linux default 30 s): a pause that outlasts it makes the host abort/reset despite the target queuing without error (R2, §8). |
| `tgt_name` | string | ◻ (default: the single default target) | multi-target deployments only |

**Response** — sent from the pause-done callback ⇒ in-flight I/O has completed, new I/O is now queued target-side, host sees **zero** errors:

```json
{ "jsonrpc": "2.0", "id": 1,
  "result": { "nqn": "…:replica-a1b2c3-0", "nsid": 1, "paused": true, "ttl_ms": 30000, "token": "7341199287:7" } }
```

**Semantics & invariants**

- **Drain-certified:** success ⇒ the namespace is quiesced (callback `ctrlr.c:5306-5325`). A caller that snapshots after a `200 OK` is guaranteed no in-flight writes mid-capture.
- **Idempotent:** re-`pause` of an already-standing-paused subsystem **succeeds** and **refreshes the TTL** (does not re-issue the state change). Makes controller retries safe (PR-7). A re-`pause` that races an in-flight resume is refused with `-EAGAIN` ("resume in progress"; see §2.3) instead of acking a barrier the subsystem is already leaving.
- **Self-healing:** if no `resume` arrives within `ttl_ms`, the server auto-resumes and logs a `WARN`. A leaked pause can never strand a replica.
- **Barrier token:** the response carries an opaque `token` identifying *this* standing pause. Pass it back to `resume` to learn whether the freeze stayed intact (§2.2). A fresh token is minted on every new pause (so a TTL auto-resume + re-pause changes it) and the token namespace changes across a process restart, so a stale token can never falsely validate a broken barrier (R1/R3, §8).

### 2.2 `nvmf_subsystem_resume`

**Request**

```json
{ "jsonrpc": "2.0", "method": "nvmf_subsystem_resume", "id": 2,
  "params": { "nqn": "nqn.2024-01.io.evariops:replica-a1b2c3-0", "token": "7341199287:7", "tgt_name": "nvmf_tgt" } }
```

| Param | Type | Required | Semantics |
|:------|:-----|:--------:|:----------|
| `nqn` | string | ✅ | subsystem NQN |
| `token` | string | ◻ | the `pause` token; when present, `resume` reports `barrier_intact` and only resumes the matching standing pause |
| `tgt_name` | string | ◻ | as above |

**Response** — sent from the resume-done callback:

```json
{ "jsonrpc": "2.0", "id": 2, "result": { "resumed": true, "barrier_intact": true } }
```

**Semantics & invariants**

- **Idempotent no-op:** `resume` of a subsystem with **no standing pause** (already resumed, or TTL auto-resumed) returns `{ "resumed": false }` success — never an error. Critical: after a controller restart it may `resume` defensively without knowing the prior state.
- **Cancels the TTL:** explicit resume wins over the timer; the registry entry is freed only in the resume-done callback (so a failed resume keeps the entry retryable).
- **Barrier integrity (`barrier_intact`):** when a `token` is supplied, it is `true` iff the namespace stayed continuously frozen under *that* pause until this resume; `false` if the TTL auto-resumed early, the target restarted (RAM-only registry, §3.3), or a different pause is now live. This lets the controller **invalidate a snapshot whose barrier broke** instead of silently accepting a non-atomic group snapshot (R1/R3, §8). A token mismatch resumes nothing (the live barrier is left untouched). Without a token, `barrier_intact` mirrors `resumed` and carries no continuity guarantee.

### 2.3 Errors

| Condition | JSON-RPC error |
|:----------|:---------------|
| missing/unparsable params | `-32602` (invalid params) |
| target not found | `-ENODEV` |
| subsystem NQN not found | `-ENOENT` |
| `spdk_nvmf_subsystem_pause/resume` dispatch returns `< 0` (`-EINVAL`/`-ENOMEM` only — **not** wrong-state) | that negated errno, with `spdk_strerror` text |
| wrong-state transition (subsystem not Active) — surfaced via the completion-callback `status`, **not** the dispatch return | `-1` (`-EPERM` text) from the callback path; a **debug** build asserts (`subsystem.c:516,551,582`) |
| re-pause while a resume is in flight | `-EAGAIN` ("resume in progress") |
| subsystem has no namespace (nsid resolves to `0`) | `-EINVAL` ("no namespace to drain") |
| OOM arming the TTL after a successful pause | subsystem is **resumed back** (best-effort) before returning `-ENOMEM` — never leave a pause we cannot auto-heal |

---

## 3. Implementation design

### 3.1 Placement & integration

A **new source file** `lib/nvmf/nvmf_pause_rpc.c`, added by the patch, plus a one-line hunk in `lib/nvmf/Makefile` appending `nvmf_pause_rpc.c` to the `C_SRCS` list (the multi-line assignment at lines 12–14, after `mdns_server.c`). Rationale identical to SPEC-60A §3.1: a new file's only conflict surface across upstream releases is the Makefile line; appending to `nvmf_rpc.c` (which churns every release) would create recurring conflicts. The file uses **only public headers** (`spdk/nvmf.h`, `spdk/rpc.h`, `spdk/thread.h`) — no nvmf internals — so it links cleanly; `SPDK_RPC_REGISTER` is constructor-based and works from any linked object.

### 3.2 Implementation overview

The implementation lives in **`lib/nvmf/nvmf_pause_rpc.c`**, delivered by `patches/0003-nvmf-add-subsystem-pause-resume-rpc.patch` (§3.4) — that file is the source of truth; this section records the design it must satisfy. It uses **only public headers** (`spdk/nvmf.h`, `spdk/rpc.h`, `spdk/thread.h`, `spdk/env.h`) — no nvmf internals — so it links cleanly, and `SPDK_RPC_REGISTER` is constructor-based (works from any linked object).

**Standing-pause registry.** A process-RAM `TAILQ`, one entry per paused subsystem, each holding: the subsystem handle, cached NQN and drained `nsid`, the last requested `ttl_ms`, a barrier `epoch`, a `state` (`PSTATE_PAUSED` or `PSTATE_RESUMING`), and a one-shot TTL poller. It is lock-free — every access is on the single app thread (`-m 0x1`); the first pause logs an error if more than one reactor is active (R4, §8).

**State machine.** Every transition resolves through the public `spdk_nvmf_subsystem_pause/resume` C API and **replies only from the completion callback** — a `200 OK` certifies the drain / the resume; replying earlier would re-introduce the in-flight-I/O race that makes `blob_freeze_io` insufficient (§1).

| Trigger | Behaviour |
|:--|:--|
| `pause`, no entry | dispatch pause → callback inserts the entry (`PSTATE_PAUSED`, fresh `epoch`), arms the TTL, replies `paused:true` + `token` |
| `pause`, entry `PSTATE_PAUSED` | idempotent: refresh the TTL and re-ack (no re-drain) |
| `pause`, entry `PSTATE_RESUMING` | `-EAGAIN` — the subsystem is leaving Paused; do not ack a false barrier |
| `resume`, entry `PSTATE_PAUSED` (token match / no token) | entry → `PSTATE_RESUMING`, **stays in the registry**, dispatch resume; callback frees it on success, or reverts to `PSTATE_PAUSED` + re-arms the TTL on failure |
| `resume`, no entry / `PSTATE_RESUMING` / token mismatch | `{resumed:false, barrier_intact:false}` no-op — never a second dispatch (this is what prevents a double-free of the entry) |
| TTL expiry | the poller drives the same resume path (internal callback), logs a `WARN`, and self-heals a leaked pause |

**Barrier token.** A per-process nonce `g_instance` plus a monotonic `g_epoch` (bumped on every new pause) form the opaque `"<instance>:<epoch>"` token returned by `pause`. `resume` compares the caller's token against the live entry to set `barrier_intact` (§2.2): a process restart changes the nonce and a TTL auto-resume + re-pause changes the epoch, so a stale token can never falsely validate a broken barrier (R1/R3, §8).

**Notes for the implementer**

- **Respond from the callback, never inline.** The whole value is that the `200 OK` *certifies* the drain (pause) / the resume. Returning before the callback would re-introduce the in-flight-I/O race that made `blob_freeze_io` insufficient (§1).
- **One-shot poller idiom.** SPDK has no one-shot timer; `SPDK_POLLER_REGISTER(fn, p, ttl_us)` fires every `ttl_us` — the handler unregisters itself on first fire. Period is **microseconds** (`ttl_ms * 1000`).
- **Single-reactor removes data races, not sequencing races.** The lock-free `g_paused` is safe because every access is on the single app thread (`-m 0x1`, `Dockerfile` `SPDK_CPU_MASK=0x1`); if a future build widens the mask, guard `g_paused` and the poller (un)register behind the subsystem's thread via `spdk_thread_send_msg` or a spinlock, and say so in the commit message. But completions are async, so an in-flight resume can overlap a later pause/resume RPC. The `PSTATE_RESUMING` state handles that on one thread: the entry stays in the registry while a resume is outstanding, a concurrent resume becomes a `{resumed:false}` no-op (never a second dispatch → no double-free), and a concurrent re-pause is refused with `-EAGAIN`. The entry is freed only on resume *success*; on *failure* it reverts to `PSTATE_PAUSED` and re-arms its TTL.
- **A wrong-state pause/resume fails via the callback, not the return value.** `spdk_nvmf_subsystem_pause/resume` return `< 0` only for `-EINVAL` (no SPDK thread) or `-ENOMEM`; concurrent state changes are *queued*, not rejected, so there is **no** `-EBUSY`/`-EAGAIN` from dispatch. An impossible transition (e.g. pausing an Inactive subsystem) is reported as `status = -1` in the completion callback — and **asserts/aborts in a debug build** (`subsystem.c:516,551,582`). Our callers only ever pause an Active replica, so this is a defensive path; `pause_done`/`resume_done` already surface a non-zero `status` as the error.
- The OOM-after-pause undo (`spdk_nvmf_subsystem_resume(ss, NULL, NULL)`) is best-effort; verify a NULL `cb_fn` is accepted in this checkout (it is for the internal callers) — otherwise pass a trivial no-op callback.

### 3.3 Threading & safety

- `spdk_nvmf_subsystem_pause/resume` are safe to *initiate* from the RPC thread — every existing nvmf RPC does exactly this (`nvmf_rpc.c:943,…`); SPDK messages the subsystem's own thread internally and calls back on completion. No manual thread hopping needed for the state change.
- The **TTL poller** runs on the thread that registered it (the app/RPC thread). Its only action is to call `spdk_nvmf_subsystem_resume`, again safe to initiate from there.
- **Crash safety:** the registry is process-RAM. An SPDK restart loses it and every subsystem reloads **Active** — no subsystem is ever stuck Paused after a target crash (§1 non-goal). The controller's restore/poll logic must therefore treat "subsystem unexpectedly Active" as a benign auto-clear, not an error (it re-issues the barrier if it still needs one). Mirrors the RAM-only reasoning of spdk-csi SPEC-66 H17 (`check_shallow_copy`).

### 3.4 Patch & build integration

```
patches/0003-nvmf-add-subsystem-pause-resume-rpc.patch
  ├─ A lib/nvmf/nvmf_pause_rpc.c   (new file, §3.2)
  └─ M lib/nvmf/Makefile           (append `nvmf_pause_rpc.c` to the C_SRCS list, lines 12–14)
```

No Dockerfile change: the build already applies every patch in a loop (`for p in /build/patches/*.patch; do … git apply "${p}"; done`, `images/spdk/Dockerfile:59`). Use `vendor/spdk` as the dev/test bed (`git apply patches/0003-*.patch` there) before an image build.

---

## 4. Tests

| Layer | Test |
|:------|:-----|
| **Compile/apply** | CI: patch applies cleanly on `${SPDK_VERSION}`; build-stage `spdk_tgt --version` smoke (already implied by the image build) |
| **RPC schema** | `rpc_get_methods` includes `nvmf_subsystem_pause` **and** `nvmf_subsystem_resume`; malformed params → `-32602`; unknown nqn → `-ENOENT`; unknown tgt → `-ENODEV` |
| **Drain certification** (`test/nvmf_pause_smoke.sh`, debug image) | create subsystem + malloc-backed ns over TCP; run `bdevperf`/fio from a host initiator; `nvmf_subsystem_pause` → assert the RPC returns **only after** outstanding I/O count hits 0 (`nvmf_get_stats`), then host I/O **blocks without error** (no `ctrl_loss_tmo` firing); `nvmf_subsystem_resume` → host I/O resumes, zero errors logged host-side |
| **TTL self-heal** | pause with `ttl_ms=2000`, **never** resume → after ≤ ~2.2 s the subsystem is Active again (`nvmf_get_subsystems`), a `WARN` is logged; host I/O resumes with zero errors |
| **Idempotency** | double `pause` → both succeed, single registry entry, TTL refreshed (second pause does not re-drain); `resume` twice → second returns `{resumed:false}`; `resume` of a never-paused subsystem → `{resumed:false}` success |
| **Barrier token** (R1/R3) | `pause` returns a `token`; `resume` with the matching token → `{resumed:true, barrier_intact:true}`. Force a TTL auto-resume (short `ttl_ms`) then `resume` with the now-stale token → `{barrier_intact:false}`, live state untouched. After `SIGKILL`+restart, `resume` with a pre-crash token → `{barrier_intact:false}` |
| **Host-timeout bound** (R2) | set the host `io_timeout` low; a pause longer than it makes the host abort/reset (real errors) — assert `ttl_ms` clamps at `PAUSE_TTL_MAX_MS` (60 s) and the controller keeps the effective pause < host timeout |
| **Multi-reactor guard** (R4) | start `spdk_tgt` with `-m 0x3`; the first `pause` emits the single-reactor `ERRLOG` (the registry is only correct on `-m 0x1`) |
| **`-EAGAIN` race** (R10) | re-`pause` while a `resume` is in flight → `-EAGAIN`; controller retries |
| **Crash safety** | pause, then `SIGKILL` + restart `spdk_tgt` → subsystem reloads Active, no stuck pause; controller poll treats it as benign |
| **Consumer contract** | mirrored in `spdk-csi` `Spdk.ContractTests` — request/response pinned to §2; capability probe (`IsMethodNotFound`) flips the controller from ANA-drain (H9) to true barrier (H10) |
| **End-to-end (spdk-csi QA)** | `QA_GroupSnapshot_CrossVolumeConsistent` on a cluster running this image: write a cross-volume invariant under load, group-snapshot via the pause barrier, restore, assert the invariant holds (the proof H9's best-effort path cannot give) |

---

## 5. Versioning & release

- Ships as a patch-level image bump (`v26.01.x`, scheme `v<upstream>.<patch>`, immutable tags, cosign-signed, SBOM-attested — unchanged repo conventions).
- **No lockstep with spdk-csi:** the controller probes `rpc_get_methods` / maps `IsMethodNotFound` to a per-node capability (spdk-csi SPEC-65 O7 pattern). Old image + new controller → graceful degradation to ANA-drain / honest refusal (SPEC-66 H9). New image + old controller → RPCs simply unused.
- The RPC names are the API contract: never change `pause`/`resume` semantics under the same name; additive params only (consumers ignore unknown result fields).

---

## 6. Upstreaming plan (SPEC-66 H10 "upstream-candidate")

1. Submit to SPDK Gerrit: `lib/nvmf/nvmf_pause_rpc.c` + Makefile line + `scripts/rpc.py` plugin subcommands + `doc/jsonrpc.md` entries + unit/functional test (`test/nvmf`), per upstream conventions. Expect debate on the registry/TTL ownership — be ready to move the registry into `struct spdk_nvmf_subsystem` if maintainers prefer.
2. Carry the patch locally regardless of review latency; on merge, drop `0003-*.patch` at the next `SPDK_VERSION` bump — consumer-invisible behind the capability probe.

---

## 7. Definition of Done

- Image `v26.01.x` exposes `nvmf_subsystem_pause` **and** `nvmf_subsystem_resume` per §2; schema + drain + TTL + idempotency + crash-safety tests green in CI.
- `spdk-csi` contract test green against the built image; the capability probe flips `GroupSnapshotSaga` from H9 (ANA-drain, opt-in) to H10 (true barrier, no opt-in needed).
- The controller consumes `barrier_intact`: a group snapshot whose `resume` reports `barrier_intact:false` for any member is **discarded and retried**, not accepted (closes R1/R3).
- `spdk-csi` QA `QA_GroupSnapshot_CrossVolumeConsistent` green on a cluster running this image — the end-to-end proof of cross-volume crash consistency (INV-41b moves from `Backlog` to `Enforced` in `InvariantRegistry.cs`).
- spdk-csi docs updated (these are companion edits, done in the spdk-csi PR that consumes the capability): SPEC-58 R1/§2.2/§2.8 already point here; `docs/snapshot-backup-prerequisites.md` fork-features table gains the `nvmf_subsystem_pause`/`resume` barrier row; README group-snapshot wording drops the "requires opt-in" caveat when the capability is present.

---

## 8. Risk analysis & mitigations

Severity = Likelihood × Impact, scored against the two things that matter: the **atomicity guarantee** (the reason this spec exists) and **availability** (a pause blocks the data path). "Mitigated" = addressed in this spec; "Accepted" = acknowledged, no action.

| # | Risk | L | I | Status / mitigation |
|:--|:--|:-:|:-:|:--|
| **R1** | TTL auto-resume mid-fan-out silently breaks atomicity — a replica un-freezes while the controller still snapshots others, and its `resume` looks identical to a clean one. | M | High | **Mitigated.** `pause` mints a `token`; `resume` returns `barrier_intact` (§2). A TTL break changes the token → `barrier_intact:false` → controller discards the snapshot. Also size `ttl_ms` > worst-case fan-out. |
| **R2** | `ttl_ms` outlasting the host I/O timeout → host aborts/resets despite the target queuing without error; the "zero host errors" property is conditional. | M-H | High | **Mitigated.** `PAUSE_TTL_MAX_MS` 600 s → 60 s; §2.1 requires the pause stay below `nvme_core.io_timeout` (Linux default 30 s); long-pause test added (§4). |
| **R3** | Target crash mid-barrier → replica reloads Active; a snapshot taken in that window is non-atomic but the controller may not notice. | L-M | High | **Mitigated.** Same `token`: the per-process nonce changes across restart, so a pre-crash token yields `barrier_intact:false`. Controller must verify the token (DoD §7). |
| **R4** | Single-reactor (`-m 0x1`) assumption silently broken by a Dockerfile CPU-mask change → real data races on the lock-free registry → memory corruption. | L | High | **Mitigated (detective).** First `pause` logs an `ERRLOG` if `spdk_env_get_core_count() > 1`; the real fix (lock / thread-confinement) is still required before widening the mask. |
| **R5** | `git apply` Makefile hunk is fuzz-free; two carried patches each have one → a version-bump can break the build. | M@bump | M (CI-caught) | **Accepted.** CI applies patches on `${SPDK_VERSION}` each PR (§4); the new `.c` file has no conflict surface. |
| **R6** | Mixed-image group during rollout → heterogeneous barrier (some replicas drained, some ANA best-effort). | M | M-H | **Accepted.** Controller responsibility (weakest-link: degrade the whole group if any member lacks the capability). |
| **R7** | RPC is a strong availability lever (freeze a replica up to 60 s) if the RPC socket is ever exposed. | L | M-H | **Accepted.** Inherits SPDK's trusted-socket model; the TTL bounds the worst case (auto-heal). |
| **R8** | Debug build `assert/abort` if a subsystem is paused while not Active. | L | High (debug only) | **Mitigated by discipline + docs.** Callers only pause Active replicas; SPDK serializes concurrent state changes; release builds return `status=-1` (§2.3, §3.2). |
| **R9** | `resumed:false` during `PSTATE_RESUMING`, then that resume fails → controller belief ≠ reality until the TTL. | L | L | **Accepted.** Self-heals via TTL; with a token the controller sees `barrier_intact:false` and retries. |
| **R10** | New `-EAGAIN` (re-pause during resume) unhandled by an old controller. | L | L | **Mitigated.** Pinned in the contract test (§4); controller treats `-EAGAIN` as retryable. |
| **R11** | Patch carried indefinitely (no upstreaming) → re-validation each SPDK bump. | Certain | L | **Accepted.** Maintenance cost; CI guards the apply/build. |
| **R12** | Unbounded registry / poller growth. | — | — | **Cleared (non-risk).** One idempotent entry per subsystem; bounded by subsystem count. |

**Design takeaway.** R1–R3 share one root cause — the barrier's integrity was not observable to the consumer. The `token` / `barrier_intact` round-trip is the single mechanism that closes all three: the controller no longer assumes "resume succeeded" means "the freeze held" — it is told.

