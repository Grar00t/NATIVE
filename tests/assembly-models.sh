#!/bin/sh
# Exercise the byte models, raw attention checkpoint and its adapter with local
# fixtures. Work in a private directory so concurrent checks do not share weights.
set -eu
root=$(pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir "$tmp/data" "$tmp/models"
cp asm/data/corpus.txt asm/data/heldout.txt "$tmp/data/"
cp asm/src/attention_lm.s "$tmp/"
cd "$tmp"
"$root/asm/build/trigram" > trigram.a
"$root/asm/build/trigram" > trigram.b
cmp trigram.a trigram.b
grep -aq '^--- sample ---$' trigram.a

"${CC:-cc}" -static -nostdlib -no-pie -Wl,-z,noexecstack \
    -Wa,--defsym,EXPORT_WEIGHTS=1,--defsym,EPOCHS=4 -o export-attention attention_lm.s
./export-attention > trained
# Export mode appends exactly 9,616 FP32 parameters after the metrics.
test "$(wc -c < trained)" -gt 38464
head -c -38464 trained > metrics
grep -Eq '^heldout_loss_x1000 [0-9]+$' metrics
tail -c 38464 trained > models/attention_weights.f32
"${CC:-cc}" -static -nostdlib -no-pie -Wl,-z,noexecstack \
    -Wa,--defsym,INFERENCE=1 -o reload attention_lm.s
./reload > generated
grep -aq '^--- attention sample ---$' generated

adapter="$root/asm/build/continual_lora"
"$adapter" init models/attention_weights.f32 frozen.lora
head -c 64 /dev/zero > x.f32
"$adapter" predict models/attention_weights.f32 frozen.lora x.f32 > prediction.f32
cmp prediction.f32 x.f32
sha256sum frozen.lora > before
if "$adapter" init models/attention_weights.f32 frozen.lora; then exit 1; fi
sha256sum -c before
