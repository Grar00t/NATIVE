#!/bin/sh
set -eu
if [ "$#" -ne 1 ]; then
    printf '%s\n' 'usage: sh retrieval/index-docs.sh OUTPUT.dalw' >&2
    exit 2
fi
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
cli="$root/retrieval/lowlevel/dalw-cli"
if [ ! -x "$cli" ]; then
    printf '%s\n' 'Build dalw-cli with make retrieval first.' >&2
    exit 1
fi
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
for path in README.md docs/architecture.md docs/verification.md engine/README.md \
    engine/docs/CLI.md engine/docs/PERSISTENCE_PROTOCOL.md engine/docs/REPETITION_GENERATION.md \
    lang/docs/architecture.md lang/docs/language.md lang/docs/tokenizer.md; do
    file="$root/$path"
    title=$(sed -n '1s/^# //p' "$file")
    bytes=$(wc -c <"$file")
    digest=$(sha256sum <"$file")
    printf '%s\t%s\t%s\t0\t%s\t%s\t' "$path" "$title" "$path" "$bytes" "${digest%% *}"
    tr '\t\r\n' '   ' <"$file"
    printf '\n'
done >"$tmp"
"$cli" index "$1" "$tmp"
