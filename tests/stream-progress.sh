#!/bin/sh
set -eu
meter=$1
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
printf '\000\032\r\n\377العربية' > "$tmp/input"
dd if=/dev/zero bs=65536 count=2 >> "$tmp/input" 2>/dev/null
"$meter" --count < "$tmp/input" > "$tmp/copy" 2> "$tmp/count"
cmp "$tmp/input" "$tmp/copy"
test "$(cat "$tmp/count")" = "$(wc -c < "$tmp/input") bytes"
"$meter" </dev/null > "$tmp/copy" 2> "$tmp/count"
test ! -s "$tmp/copy" && test ! -s "$tmp/count"
"$meter" --demo > "$tmp/copy" 2> "$tmp/count"
test ! -s "$tmp/copy" && grep -Fxq 'thinking...' "$tmp/count"
if "$meter" < "$tmp/input" > /dev/full 2>/dev/null; then exit 1; fi
