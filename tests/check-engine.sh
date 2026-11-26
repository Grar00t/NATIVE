#!/bin/sh
set -eu
engine=$(cd "$1" && pwd)
model=$(cd "$2" && pwd)
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
cd "$dir"
"$model/niyah-model"
printf 'قرأت النص.\nحفظت الملف.\n' >train.txt
"$model/niyah-train" train.txt 1 0.0003 0.00003 2026 >train.log
grep -qx 'training_steps=2' train.log
mv niyah_trained.bin reference.bin
printf '\r\nقرأت النص.\r\n \t \r\nحفظت الملف.' >train.txt
"$model/niyah-train" train.txt 1 0.0003 0.00003 2026 >train.log
grep -qx 'training_steps=2' train.log
cmp reference.bin niyah_trained.bin

awk 'BEGIN{for(i=0;i<5000;i++)printf "a";print ""}' >long.txt
"$model/niyah-train" long.txt 1 0.0003 0.00003 2026 >train.log
grep -qx 'training_steps=1' train.log
sha256sum niyah_trained.bin >before
printf ' \t\r\n\n' >blank.txt
printf 'a\000b\n' >nul.txt
for file in blank.txt nul.txt missing.txt; do
    if "$model/niyah-train" "$file" 1 >train.log 2>&1; then exit 1; fi
done
if "$model/niyah-train" train.txt x >train.log 2>&1; then exit 1; fi
sha256sum -c before

# Keep explicit deployment checks separate from the locally trained checkpoint.
if [ -n "${NIYAH_MMAP_MODEL_PATH+x}${NIYAH_MMAP_MODEL_SHA256+x}" ]; then
    : "${NIYAH_MMAP_MODEL_PATH:?missing model path}" "${NIYAH_MMAP_MODEL_SHA256:?missing model SHA-256}"
    external=1
elif [ -n "${NIYAH_CASPER_MODEL_PATH+x}${NIYAH_CASPER_MODEL_SHA256+x}" ]; then
    : "${NIYAH_CASPER_MODEL_PATH:?missing model path}" "${NIYAH_CASPER_MODEL_SHA256:?missing model SHA-256}"
    external=1
else
    external=0
fi
if [ "$external" -eq 1 ]; then
    ctest --test-dir "$engine" -C Release --output-on-failure
fi
NIYAH_MMAP_MODEL_PATH=$dir/reference.bin
NIYAH_MMAP_MODEL_SHA256=$(sha256sum reference.bin | cut -d' ' -f1)
export NIYAH_MMAP_MODEL_PATH NIYAH_MMAP_MODEL_SHA256
printf 'Local trainer checkpoint: %s bytes, SHA-256 %s\n' "$(wc -c <reference.bin)" "$NIYAH_MMAP_MODEL_SHA256"
if [ "$external" -eq 1 ]; then
    ctest --test-dir "$engine" -C Release -R 'model_mmap_(external|identity)' --output-on-failure
else
    ctest --test-dir "$engine" -C Release --output-on-failure
fi
