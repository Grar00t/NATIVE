#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
# Hash the bytes used in this revision. Original Git and SHA-256 identifiers
# are recorded separately for comparisons against each pinned upstream tree.
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
awk -F '\t' 'NR>1 {if(NF!=6 || length($6)!=64) exit 1; print $6 "  " $5}
              END {if(NR<2) exit 1}' docs/imports.tsv > "$tmp"
sha256sum --quiet -c "$tmp"
