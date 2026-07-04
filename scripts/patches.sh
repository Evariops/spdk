#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Evariops.
#
# patches.sh — tooling for the Evariops SPDK patch series (UP2/U-3).
#
# The audit flagged manual hunk editing (`git apply` of by-hand-edited .patch
# files) as a real risk: `sed`/hand edits silently corrupt offsets and only
# surface at build time. This script makes the round trip mechanical:
#
#   patches.sh check   [<spdk_src>]   apply every patch with `git apply --check`
#                                      in order; report the first that fails.
#   patches.sh apply   <spdk_src>     git-apply the series into an SPDK checkout.
#   patches.sh regen   <spdk_worktree>   regenerate patches/*.patch from a
#                                      worktree whose commits (one per patch,
#                                      in order) sit on top of the pinned base.
#   patches.sh verify  [<spdk_src>]   clone the pinned upstream into a temp dir
#                                      and run `check` against it (no local SPDK
#                                      needed; requires network + git).
#
# The pinned upstream is read from images/spdk/Dockerfile (ARG SPDK_COMMIT_SHA),
# so the script and the container build can never disagree (W4).

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PATCH_DIR="$REPO_ROOT/patches"
DOCKERFILE="$REPO_ROOT/images/spdk/Dockerfile"

pinned_sha() { sed -n 's/^ARG SPDK_COMMIT_SHA=\([0-9a-f]\{7,\}\).*/\1/p' "$DOCKERFILE"; }
pinned_tag() { sed -n 's/^ARG SPDK_VERSION=\(v[^ ]*\).*/\1/p' "$DOCKERFILE"; }

patches() { ls "$PATCH_DIR"/[0-9]*.patch | sort; }

cmd_check() {
	local src="${1:?usage: patches.sh check <spdk_src>}"
	git -C "$src" reset --hard >/dev/null 2>&1 || true
	local p
	for p in $(patches); do
		if git -C "$src" apply --check "$p" 2>/tmp/patcherr; then
			git -C "$src" apply "$p"
			echo "OK   $(basename "$p")"
		else
			echo "FAIL $(basename "$p")"
			sed 's/^/     /' /tmp/patcherr
			return 1
		fi
	done
	echo "All $(patches | wc -l | tr -d ' ') patches apply cleanly."
}

cmd_apply() {
	local src="${1:?usage: patches.sh apply <spdk_src>}"
	local p
	for p in $(patches); do
		echo "Applying $(basename "$p")"
		git -C "$src" apply "$p"
	done
}

cmd_regen() {
	local wt="${1:?usage: patches.sh regen <spdk_worktree>}"
	local base
	base="$(pinned_sha)"
	local n
	n="$(patches | wc -l | tr -d ' ')"
	rm -f "$PATCH_DIR"/[0-9]*.patch
	git -C "$wt" format-patch --zero-commit "$base..HEAD" -o "$PATCH_DIR" >/dev/null
	echo "Regenerated $(patches | wc -l | tr -d ' ') patches from $wt ($base..HEAD); was $n."
}

cmd_verify() {
	local src="${1:-}"
	if [[ -z "$src" ]]; then
		src="$(mktemp -d)/spdk"
		echo "Cloning $(pinned_tag) into $src ..."
		git clone --branch "$(pinned_tag)" --depth 1 "https://github.com/spdk/spdk.git" "$src" >/dev/null 2>&1
		local actual
		actual="$(git -C "$src" rev-parse --short=7 HEAD)"
		if [[ "$actual" != "$(pinned_sha)" ]]; then
			echo "FATAL: $(pinned_tag) resolves to $actual, expected $(pinned_sha) (tag moved?)" >&2
			return 1
		fi
	fi
	cmd_check "$src"
}

case "${1:-}" in
	check)  shift; cmd_check "$@";;
	apply)  shift; cmd_apply "$@";;
	regen)  shift; cmd_regen "$@";;
	verify) shift; cmd_verify "$@";;
	*) sed -n '2,30p' "${BASH_SOURCE[0]}"; exit 1;;
esac
