# SPDK Container Image

Hardened, minimal SPDK container image — built from source, multi-arch, signed, and SBOM-attested.

- **Built from source** — no pre-built binaries from third parties
- **Multi-arch** — native `amd64` and `arm64` builds
- **`scratch`-based** — no shell, no package manager, minimal attack surface
- **Signed** — Sigstore cosign (keyless) with full provenance
- **SBOM-attested** — SPDX and CycloneDX attached as OCI attestations

---

## Available images

| Image | What it does | Upstream | Final size |
|-------|-------------|----------|------------|
| **[spdk]** | NVMe-oF TCP/RDMA storage engine | [spdk/spdk](https://github.com/spdk/spdk) | ~14 MB |
| **[spdk-debug]** | Debug variant with gdb, strace, perf, RPC scripts | [spdk/spdk](https://github.com/spdk/spdk) | ~300 MB |

[spdk]: https://ghcr.io/evariops/spdk
[spdk-debug]: https://ghcr.io/evariops/spdk-debug

### Pull an image

```bash
docker pull ghcr.io/evariops/spdk:<tag>

# Debug variant
docker pull ghcr.io/evariops/spdk-debug:<tag>
```

---

## How tags work

There is no `latest` tag. All exact tags are **immutable**.

```
ghcr.io/evariops/spdk:v26.01.0   ← exact version, never changes
ghcr.io/evariops/spdk:v26.01     ← floating, follows the latest patch
```

The version scheme is **`v<upstream>.<patch>`** where the patch number tracks our rebuilds (Dockerfile changes, dependency bumps) of the same upstream release.

> Git tags follow the convention `spdk/v26.01.0`.

---

## Required K8s securityContext

```yaml
securityContext:
  capabilities:
    add: [SYS_ADMIN, IPC_LOCK]
    drop: [ALL]
  readOnlyRootFilesystem: true
  runAsNonRoot: true
  runAsUser: 1000
```

Required volume mounts:
- `/dev/hugepages` (hostPath)
- `/var/tmp` (emptyDir — shared with sidecar for RPC socket)

---

## Verify a signature

All images are signed with [Sigstore cosign](https://docs.sigstore.dev/) (keyless — no keys to manage).

```bash
cosign verify \
  --certificate-identity-regexp="https://github.com/Evariops/spdk/" \
  --certificate-oidc-issuer="https://token.actions.githubusercontent.com" \
  ghcr.io/evariops/spdk:<tag>
```

---

## Inspect the SBOM

Both SPDX and CycloneDX SBOMs are attached to each image.

```bash
# View SPDX SBOM
cosign verify-attestation --type spdxjson \
  --certificate-identity-regexp="https://github.com/Evariops/spdk/" \
  --certificate-oidc-issuer="https://token.actions.githubusercontent.com" \
  ghcr.io/evariops/spdk:<tag> 2>/dev/null | jq -r '.payload' | base64 -d | jq .
```

Replace `spdxjson` with `cyclonedx` for the CycloneDX format.

---

## License

[Apache-2.0](LICENSE)
