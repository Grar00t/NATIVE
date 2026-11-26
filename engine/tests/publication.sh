#!/bin/sh
set -eu
driver=$1
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
printf original > "$d/dest"
ln "$d/dest" "$d/alias"
for name in dest alias ./dest; do
    if "$driver" publish "$d/$name" replaced none; then exit 1; fi
done
for fault in partial_write publish; do
    if "$driver" atomic "$d/dest" new-content "$fault"; then exit 1; fi
    test "$(cat "$d/dest")" = original
done
# Every child writes its own exit code. Verify the unique winner and payload.
for i in 1 2 3 4 5 6 7 8 9 10 11 12; do
    (rc=0; "$driver" publish "$d/winner" "$i" none > /dev/null || rc=$?
     printf '%s\n' "$rc" > "$d/rc.$i") &
done
wait
winner=$(cat "$d/winner")
test "$(cat "$d/rc.$winner")" = 0
test "$(grep -lx 0 "$d"/rc.* | wc -l)" -eq 1
test "$(grep -lx 1 "$d"/rc.* | wc -l)" -eq 11
test -z "$(find "$d" -name '*.niyah-stage-*' -print)"
