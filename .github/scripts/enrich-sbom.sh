#!/usr/bin/env bash
# ──────────────────────────────────────────────────────────────────
# Enrich syft-generated SBOMs with source-package metadata.
#
# Usage:
#   enrich-sbom.sh \
#     --name spdk \
#     --version 26.01 \
#     --supplier "SPDK Project (Linux Foundation)" \
#     --license BSD-3-Clause \
#     --purl "pkg:github/spdk/spdk@v26.01" \
#     --download "https://github.com/spdk/spdk/archive/refs/tags/v26.01.tar.gz" \
#     --vcs "https://github.com/spdk/spdk" \
#     --spdx sbom.spdx.json \
#     --cdx  sbom.cdx.json
# ──────────────────────────────────────────────────────────────────
set -euo pipefail

# ── Parse arguments ──────────────────────────────────────────────
while [[ $# -gt 0 ]]; do
  case "$1" in
    --name)       NAME="$2";       shift 2 ;;
    --version)    VERSION="$2";    shift 2 ;;
    --supplier)   SUPPLIER="$2";   shift 2 ;;
    --license)    LICENSE="$2";    shift 2 ;;
    --purl)       PURL="$2";       shift 2 ;;
    --download)   DOWNLOAD="$2";   shift 2 ;;
    --vcs)        VCS="$2";        shift 2 ;;
    --spdx)       SPDX="$2";       shift 2 ;;
    --cdx)        CDX="$2";        shift 2 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

for var in NAME VERSION SUPPLIER LICENSE PURL DOWNLOAD VCS SPDX CDX; do
  if [[ -z "${!var:-}" ]]; then
    echo "Missing required argument: --$(echo "$var" | tr '[:upper:]' '[:lower:]')" >&2
    exit 1
  fi
done

# ── SPDX enrichment ─────────────────────────────────────────────
jq \
  --arg name     "$NAME" \
  --arg ver      "$VERSION" \
  --arg supplier "$SUPPLIER" \
  --arg license  "$LICENSE" \
  --arg purl     "$PURL" \
  --arg download "$DOWNLOAD" \
'
  .predicate.packages += [{
    "SPDXID":                ("SPDXRef-Package-source-" + $name),
    "name":                  $name,
    "versionInfo":           $ver,
    "supplier":              ("Organization: " + $supplier),
    "downloadLocation":      $download,
    "filesAnalyzed":         false,
    "primaryPackagePurpose": "SOURCE",
    "licenseConcluded":      $license,
    "licenseDeclared":       $license,
    "copyrightText":         "NOASSERTION",
    "externalRefs": [{
      "referenceCategory": "PACKAGE-MANAGER",
      "referenceType":     "purl",
      "referenceLocator":  $purl
    }]
  }] |
  .predicate.relationships += [{
    "spdxElementId":    ("SPDXRef-Package-source-" + $name),
    "relatedSpdxElement": .predicate.packages[0].SPDXID,
    "relationshipType": "BUILD_TOOL_OF"
  }]
' "$SPDX" > "${SPDX}.tmp" && mv "${SPDX}.tmp" "$SPDX"

echo "SPDX enriched: added ${NAME}@${VERSION} to ${SPDX}"

# ── CycloneDX enrichment ────────────────────────────────────────
jq \
  --arg name     "$NAME" \
  --arg ver      "$VERSION" \
  --arg supplier "$SUPPLIER" \
  --arg license  "$LICENSE" \
  --arg purl     "$PURL" \
  --arg vcs      "$VCS" \
'
  .components += [{
    "type":     "library",
    "name":     $name,
    "version":  $ver,
    "purl":     $purl,
    "supplier": { "name": $supplier },
    "licenses": [{ "license": { "id": $license } }],
    "externalReferences": [{
      "type": "vcs",
      "url":  $vcs
    }]
  }]
' "$CDX" > "${CDX}.tmp" && mv "${CDX}.tmp" "$CDX"

echo "CycloneDX enriched: added ${NAME}@${VERSION} to ${CDX}"
