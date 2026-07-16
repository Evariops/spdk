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
#   patches.sh check   <spdk_src>     verify the whole series applies in order
#                                      (non-destructive: checks against a scratch
#                                      index, never touches the caller's tree).
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

# NUL-delimited list so a patch path with spaces/globs is never word-split.
patches() { find "$PATCH_DIR" -maxdepth 1 -name '[0-9]*.patch' -print0 | sort -z; }
patches_count() { patches | tr -cd '\0' | wc -c | tr -d ' '; }

# Non-destructive: `git apply --check` verifies the WHOLE series applies in order
# without touching the caller's tree. --check alone stops at the first patch, so
# each subsequent patch is checked against a scratch index (git apply --cached
# into a temporary index copied from HEAD), leaving the working tree untouched.
cmd_check() {
	local src="${1:?usage: patches.sh check <spdk_src>}"
	local tmpidx err p rc=0
	tmpidx="$(mktemp)"
	err="$(mktemp)"
	# Seed the scratch index from HEAD so --cached checks stack in order.
	GIT_INDEX_FILE="$tmpidx" git -C "$src" read-tree HEAD
	while IFS= read -r -d '' p; do
		if GIT_INDEX_FILE="$tmpidx" git -C "$src" apply --cached --check "$p" 2>"$err"; then
			GIT_INDEX_FILE="$tmpidx" git -C "$src" apply --cached "$p"
			echo "OK   $(basename "$p")"
		else
			echo "FAIL $(basename "$p")"
			sed 's/^/     /' "$err"
			rc=1
			break
		fi
	done < <(patches)
	rm -f "$tmpidx" "$err"
	[ "$rc" -eq 0 ] && echo "All $(patches_count) patches apply cleanly."
	return "$rc"
}

cmd_apply() {
	local src="${1:?usage: patches.sh apply <spdk_src>}"
	local p
	while IFS= read -r -d '' p; do
		echo "Applying $(basename "$p")"
		git -C "$src" apply "$p"
	done < <(patches)
}

# Regenerate patches/*.patch as RAW git diffs, one per commit on top of the
# pinned base, in order. The commit SUBJECT is the patch filename stem and
# must match NNNN-name. Raw diffs (no mail header) are the series format:
# `git apply` consumes them (Dockerfile + check/apply); `git am` is NOT part
# of the contract (it chokes on raw diffs — build the worktree with
# `git apply` + one commit per patch, subject = filename stem).
cmd_regen() {
	local wt="${1:?usage: patches.sh regen <spdk_worktree>}"
	local base commit subject n=0
	base="$(pinned_sha)"
	rm -f "$PATCH_DIR"/[0-9]*.patch
	while IFS= read -r commit; do
		subject="$(git -C "$wt" log -1 --format=%s "$commit")"
		case "$subject" in
			[0-9][0-9][0-9][0-9]-*) ;;
			*) echo "FATAL: commit $commit subject '$subject' is not NNNN-name (the subject IS the patch filename)" >&2
			   return 1;;
		esac
		git -C "$wt" diff "$commit^..$commit" > "$PATCH_DIR/$subject.patch"
		n=$((n + 1))
	done < <(git -C "$wt" rev-list --reverse "$base..HEAD")
	echo "Regenerated $n raw-diff patches from $wt ($base..HEAD)."
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
