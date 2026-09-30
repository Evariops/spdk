#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright (c) 2026 Evariops.
#
# unit-tests.sh <patches_dir> <map>
#
# Run from the root of an SPDK tree that is already built (the image's builder
# stage). Builds and runs the upstream unit-test suites the patch series owes:
# unit-tests.map says which suites a patched file names.
#
# The suites are compiled against the libraries of that build: SPDK is not
# built a second time. Each suite is built from its own leaf directory, because
# the parent Makefiles skip some suites with a mere warning (blob.c wants
# CUnit 2.1-3), and a skip must not read as a pass. A suite whose binary is
# missing after its build fails the stage, as does a suite that fails.
set -euo pipefail

patches_dir="${1:?usage: unit-tests.sh <patches_dir> <map>}"
map="${2:?usage: unit-tests.sh <patches_dir> <map>}"

# Plain bash 3 on purpose (no mapfile, no associative arrays): the selection is
# checkable on any workstation, not only in the builder.

# ── The files the series patches ──
touched=()
while IFS= read -r f; do
	touched+=("${f}")
done < <(sed -n 's|^+++ b/||p' "${patches_dir}"/[0-9]*.patch | sort -u)

# ── The map ──
prefixes=()
suites_of=()
while read -r prefix rest; do
	[[ -z "${prefix}" || "${prefix}" == \#* ]] && continue
	prefixes+=("${prefix}")
	suites_of+=("${rest}")
done < "${map}"

# ── Suites owed ──
owed=()
unmapped=()
for f in "${touched[@]}"; do
	case "${f}" in
		test/unit/*)
			# A patch's own tests: the leaf directory of the file it touches.
			owed+=("$(dirname "${f#test/unit/}")")
			continue
			;;
		lib/*.c | module/*.c | app/*.c) ;;
		*) continue ;;
	esac

	best=-1
	best_len=0
	for i in "${!prefixes[@]}"; do
		p="${prefixes[$i]}"
		if [[ "${f}" == "${p}"* && ${#p} -gt ${best_len} ]]; then
			best=${i}
			best_len=${#p}
		fi
	done
	if ((best < 0)); then
		unmapped+=("${f}")
		continue
	fi
	for s in ${suites_of[$best]}; do
		[[ "${s}" == "-" ]] || owed+=("${s}")
	done
done

if ((${#unmapped[@]} > 0)); then
	echo "FATAL: patched file(s) with no line in unit-tests.map (map them; \"-\" when upstream has no suite):" >&2
	printf '  %s\n' "${unmapped[@]}" >&2
	exit 1
fi

suites=()
while IFS= read -r s; do
	suites+=("${s}")
done < <(printf '%s\n' "${owed[@]}" | sort -u)
echo "Unit-test suites owed by the series (${#suites[@]}):"
printf '  %s\n' "${suites[@]}"

# ── Build ──
# The ut library is built with the tests only (lib/Makefile), and the image
# configures them off.
make -C lib/ut -j"$(nproc)"
for s in "${suites[@]}"; do
	if [[ ! -d "test/unit/${s}" ]]; then
		echo "FATAL: unit-tests.map names test/unit/${s}, which does not exist" >&2
		exit 1
	fi
	make -C "test/unit/${s}" -j"$(nproc)"
done

# ── Run ──
logs="$(mktemp -d)"
failed=()
for s in "${suites[@]}"; do
	src="$(find "test/unit/${s}" -maxdepth 1 -name '*_ut.c' | head -n 1)"
	if [[ -z "${src}" ]]; then
		echo "FATAL: test/unit/${s} holds no *_ut.c" >&2
		exit 1
	fi
	bin="${src%.c}"
	log="${logs}/${s//\//_}.log"
	if [[ ! -x "${bin}" ]]; then
		echo "FAIL ${s}: ${bin} was not built"
		failed+=("${s}")
		continue
	fi
	start=${SECONDS}
	if "${bin}" > "${log}" 2>&1; then
		echo "PASS ${s} ($((SECONDS - start)) s)"
		grep -E '^\s+(suites|tests|asserts)\s' "${log}" | sed 's/^/     /' || true
	else
		echo "FAIL ${s} ($((SECONDS - start)) s)"
		tail -n 80 "${log}" | sed 's/^/     /'
		failed+=("${s}")
	fi
done

if ((${#failed[@]} > 0)); then
	echo "FAILED: ${#failed[@]} of ${#suites[@]} suites: ${failed[*]}" >&2
	exit 1
fi
echo "All ${#suites[@]} suites passed."
