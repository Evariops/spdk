# syntax=docker/dockerfile:1
# ============================================================================
# SPDK Data Engine — Production container (scratch-based, ~14 MB)
# ============================================================================
#
# Multi-stage build:
#   Stage 1 (builder)   — Compiles SPDK from source on Fedora 43
#   Stage 2 (collector) — Collects exact runtime closure (binary + shared libs)
#   Stage 3 (final)     — Scratch-based, no shell, no package manager
#
# Build:
#   docker build -t ghcr.io/evariops/spdk:v1.0.0 .
#
# Required K8s securityContext:
#   capabilities:
#     add:  [SYS_ADMIN, IPC_LOCK]
#     drop: [ALL]
#   readOnlyRootFilesystem: true
#   runAsNonRoot: true
#   runAsUser: 1000
#
# Required volume mounts:
#   - /dev/hugepages (hostPath)
#   - /var/tmp       (emptyDir — shared with sidecar for RPC socket)
#
# ============================================================================

# ============================================================================
# Stage 1: Build SPDK from source
# ============================================================================
FROM docker.io/library/fedora:44 AS builder

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

ARG SPDK_VERSION=v26.01
ARG TARGETARCH
ARG BUILD_TYPE=release

WORKDIR /build

# renovate: datasource=repology depName=fedora_43/git versioning=loose
RUN dnf install -y --setopt=install_weak_deps=False --nodocs git-2.53.0 && \
    dnf clean all && rm -rf /var/cache/dnf

RUN git clone --branch "${SPDK_VERSION}" --depth 1 --recurse-submodules \
        https://github.com/spdk/spdk.git

WORKDIR /build/spdk

RUN ./scripts/pkgdep.sh -d

# Ensure io_uring and AIO dependencies are present (pkgdep.sh may not install them)
RUN dnf install -y --setopt=install_weak_deps=False --nodocs liburing-devel libaio-devel && \
    dnf clean all && rm -rf /var/cache/dnf

# Configure: static SPDK libs + minimal module set.
#
# --without-shared      All SPDK code linked statically into spdk_tgt.
#                        Only system .so files needed at runtime.
#
# Modules KEPT (required by project features):
#   bdev_lvol   — thin provisioning, snapshots, clones
#   bdev_raid   — RAID-1 (Nexus), RAID-0 (striping), RAID-10
#   raid5f      — Erasure Coding (RAID-5f / RAID-6)
#   bdev_nvme   — NVMe-oF initiator for remote replicas
#   bdev_malloc — RAM disks for testing / fast scratch
#   bdev_uring  — io_uring bdev for modern kernels (>= 5.1), SATA SSDs
#   bdev_aio    — Linux AIO bdev fallback for all block devices
#   nvmf_tcp    — NVMe-oF TCP target
#
# Modules REMOVED (unused attack surface):
#   iscsi-initiator, vhost, virtio, fsdev (FUSE), daos, rbd (Ceph),
#   ublk (userspace block), dpdk-compressdev
RUN SPDK_ARCH=$([ "${TARGETARCH}" = "arm64" ] && echo "armv8-a" || echo "nehalem") && \
    DEBUG_FLAG=$([ "${BUILD_TYPE}" = "debug" ] && echo "--enable-debug" || true) && \
    ./configure \
        ${DEBUG_FLAG} \
        --disable-tests \
        --disable-unit-tests \
        --disable-examples \
        --without-shared \
        --with-uring \
        --with-raid5f \
        --target-arch="${SPDK_ARCH}" \
        --without-iscsi-initiator \
        --without-vhost \
        --without-virtio \
        --without-fsdev \
        --without-daos \
        --without-rbd \
        --without-ublk \
        --without-dpdk-compressdev

RUN make -j"$(nproc)"

# Strip debug symbols in release mode (~30-40% binary size reduction)
# Debug builds keep symbols for perf top, perf record, gdb attach
RUN if [ "${BUILD_TYPE}" != "debug" ]; then \
      strip --strip-all build/bin/spdk_tgt; \
    fi


# ============================================================================
# Stage 2: Collect the exact runtime closure
# ============================================================================
FROM builder AS collector

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

RUN set -euo pipefail && \
    mkdir -p /rt/usr/local/bin /rt/etc /rt/var/tmp /rt/tmp && \
    \
    # ── Binary ──
    cp build/bin/spdk_tgt /rt/usr/local/bin/ && \
    \
    # ── Shared libs: copy ONLY the .so files ldd reports ──
    ldd build/bin/spdk_tgt | awk '/=>/{print $3}' | sort -u > /tmp/libs.txt && \
    while read -r lib; do \
        [ -f "${lib}" ] || continue; \
        dir="/rt$(dirname "${lib}")"; \
        mkdir -p "${dir}"; \
        cp --dereference "${lib}" "${dir}/"; \
    done < /tmp/libs.txt && \
    \
    # ── Dynamic linker (path hard-coded in ELF PT_INTERP) ──
    interp=$(readelf -l build/bin/spdk_tgt \
             | awk '/interpreter:/{gsub(/[\[\]]/, ""); print $NF}') && \
    mkdir -p "/rt$(dirname "${interp}")" && \
    cp --dereference "${interp}" "/rt${interp}" && \
    \
    # ── Minimal /etc so glibc works without NSS module lookups ──
    printf 'spdk:x:1000:1000::/:\n'            > /rt/etc/passwd        && \
    printf 'spdk:x:1000:\n'                    > /rt/etc/group         && \
    printf 'passwd: files\ngroup: files\n'     > /rt/etc/nsswitch.conf && \
    \
    # ── OpenSSL config (DPDK/SPDK init requires openssl.cnf) ──
    mkdir -p /rt/etc/pki/tls                                           && \
    printf '%s\n' \
        '# Minimal OpenSSL config for SPDK scratch image' \
        'openssl_conf = openssl_init' \
        '' \
        '[openssl_init]' \
        'ssl_conf = ssl_module' \
        '' \
        '[ssl_module]' \
        'system_default = system_default_sect' \
        '' \
        '[system_default_sect]' \
        'MinProtocol = TLSv1.2' \
        'CipherString = DEFAULT:@SECLEVEL=2' \
        '' \
        '[default_sect]' \
        > /rt/etc/pki/tls/openssl.cnf


# ============================================================================
# Stage 3: Debug image (Fedora-based — shell, gdb, strace, perf, RPC scripts)
# ============================================================================
# Built only when explicitly targeted:
#   docker build --target debug --build-arg BUILD_TYPE=debug .
# ============================================================================
FROM docker.io/library/fedora:44 AS debug

SHELL ["/bin/bash", "-o", "pipefail", "-c"]

# Install runtime libs (same Fedora as builder — ABI-compatible) + debug tooling
RUN dnf install -y --setopt=install_weak_deps=False --nodocs \
        liburing \
        libaio \
        libuuid \
        openssl-libs \
        numactl-libs \
        libiscsi \
        gdb \
        strace \
        perf \
        lsof \
        procps-ng \
        iproute \
        nvme-cli \
        python3 \
        jq \
    && dnf clean all && rm -rf /var/cache/dnf

# Binary only (runtime .so provided by Fedora packages above)
COPY --from=builder /build/spdk/build/bin/spdk_tgt /usr/local/bin/spdk_tgt

# SPDK RPC scripts (useful for live debugging: rpc.py, spdkcli, etc.)
COPY --from=builder /build/spdk/scripts/ /usr/local/share/spdk/scripts/

LABEL org.opencontainers.image.title="SPDK Data Engine (debug)" \
      org.opencontainers.image.description="Fedora-based SPDK debug image with gdb, strace, perf, and RPC scripts" \
      org.opencontainers.image.vendor="Evariops" \
      org.opencontainers.image.source="https://github.com/Evariops/containers" \
      org.opencontainers.image.licenses="Apache-2.0" \
      io.k8s.security.capabilities.required="SYS_ADMIN,IPC_LOCK"

ENV PATH="/usr/local/bin:/usr/local/share/spdk/scripts:/usr/local/sbin:/usr/sbin:/usr/bin:/sbin:/bin" \
    LD_LIBRARY_PATH="/lib64:/lib:/usr/lib64:/usr/lib" \
    SPDK_RPC_SOCKET="/var/tmp/spdk.sock" \
    SPDK_CPU_MASK="0x1"

EXPOSE 4420

USER 1000:1000

ENTRYPOINT ["/usr/local/bin/spdk_tgt"]
CMD ["-r", "/var/tmp/spdk.sock", "-m", "0x1", "--wait-for-rpc"]


# ============================================================================
# Stage 4: Production image (scratch — nothing but spdk_tgt + its libs)
# ============================================================================
FROM scratch

LABEL org.opencontainers.image.title="SPDK Data Engine" \
      org.opencontainers.image.description="Ultra-minimal hardened SPDK for NVMe-oF TCP/RDMA" \
      org.opencontainers.image.vendor="Evariops" \
      org.opencontainers.image.source="https://github.com/Evariops/containers" \
      org.opencontainers.image.licenses="Apache-2.0" \
      io.k8s.security.capabilities.required="SYS_ADMIN,IPC_LOCK"

COPY --from=collector /rt/ /

ENV PATH="/usr/local/bin" \
    LD_LIBRARY_PATH="/lib64:/lib:/usr/lib64:/usr/lib" \
    SPDK_RPC_SOCKET="/var/tmp/spdk.sock" \
    SPDK_CPU_MASK="0x1"

EXPOSE 4420

# Non-root. Pod spec must grant capabilities:
#   add: [SYS_ADMIN, IPC_LOCK]   drop: [ALL]
USER 1000:1000

ENTRYPOINT ["/usr/local/bin/spdk_tgt"]
CMD ["-r", "/var/tmp/spdk.sock", "-m", "0x1", "--wait-for-rpc"]
