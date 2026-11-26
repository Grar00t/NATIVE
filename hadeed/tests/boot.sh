#!/bin/sh
set -eu
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
status=0
"${TIMEOUT:-timeout}" "${QEMU_BOOT_TIMEOUT:-90s}" "${QEMU:-qemu-system-x86_64}" \
    -machine pc,accel=tcg -m 64M -snapshot \
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
awk 'NR==1 && $0=="LM64" {next}
     NR==2 && /^USABLE_PAGES [1-9][0-9]*$/ {next}
     NR==3 && $0=="I8_GEMM 256" {next}
     NR==4 && $0=="M4 OK" {next}
     {bad=1} END {exit (bad || NR!=4)}' "$tmp/guest"
cat "$tmp/guest"
