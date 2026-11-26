#!/bin/sh
set -eu
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
ram=${QEMU_MEMORY_MB:-64}
case "$ram" in ''|*[!0-9]*|0) echo "Invalid RAM size: $ram" >&2; exit 2;; esac
status=0
"${TIMEOUT:-timeout}" "${QEMU_BOOT_TIMEOUT:-90s}" "${QEMU:-qemu-system-x86_64}" \
    -machine pc,accel=tcg -nic none -m "${ram}M" -snapshot \
    -drive "format=raw,file=$1,if=ide,index=0,media=disk" -boot c \
    -display none -serial none -monitor none -no-reboot \
    -device isa-debug-exit,iobase=0xf4,iosize=4 \
    -debugcon "file:$tmp/guest" -global isa-debugcon.iobase=0xe9 \
    -d int,cpu_reset -D "$tmp/cpu" || status=$?
if [ "$status" -ne 33 ]; then
    [ ! -f "$tmp/guest" ] || cat "$tmp/guest" >&2
    echo "QEMU exited with $status (expected guest success exit 33)" >&2
    exit 1
fi
test -s "$tmp/cpu"
if grep -iq 'triple fault' "$tmp/cpu"; then exit 1; fi
printf 'RAM %s MiB, QEMU exit %s\n' "$ram" "$status"
awk -v mb="$ram" '
     NR==1 && $0=="LM64" {next}
     NR==2 && /^PIT_TICKS [1-9][0-9]*$/ && $2+0 >= 3 &&
         (length($2)<20 || (length($2)==20 && ("x" $2)<="x18446744073709551615")) {next}
     NR==3 && /^USABLE_PAGES [1-9][0-9]*$/ {
         cap = (mb > 256 ? 256 : mb) * 256 - 512
         if ($2 > cap) bad=1
         next
     }
     NR==4 && $0=="PAGE_MAP OK" {next}
     NR==5 && $0=="I8_GEMM 256" {next}
     NR==6 && $0=="M4 OK" {next}
     {bad=1} END {exit (bad || NR!=6)}' "$tmp/guest" || { cat "$tmp/guest" >&2; exit 1; }
cat "$tmp/guest"
