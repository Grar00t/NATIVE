#!/bin/sh
set -eu
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

build/tensor-oracle input > "$tmp/input"
build/tensor-oracle expected > "$tmp/expected"
test "$(wc -c < "$tmp/expected")" -eq 1024
for mode in scalar auto; do
    asm/build/tensor/project "$mode" < "$tmp/input" > "$tmp/result"
    cmp "$tmp/expected" "$tmp/result"
done
head -c 2047 "$tmp/input" > "$tmp/short"
cat "$tmp/input" "$tmp/input" > "$tmp/long"
for malformed in short long; do
    status=0
    asm/build/tensor/project < "$tmp/$malformed" > "$tmp/result" || status=$?
    test "$status" -eq 65 && test ! -s "$tmp/result" || exit 1
done
printf '%s\n' 'GEMM: 256 int32 outputs matched Engine FP32 in scalar and auto modes'
printf 'مصفوفة الإدخال\000\377' > "$tmp/source"
asm/build/morph-tokenizer encode < "$tmp/source" > "$tmp/tokens"
asm/build/morph-tokenizer decode < "$tmp/tokens" > "$tmp/text"
cmp "$tmp/text" "$tmp/source"
head -c 63 "$tmp/tokens" > "$tmp/bad"
status=0
asm/build/morph-tokenizer decode < "$tmp/bad" > "$tmp/text" || status=$?
test "$status" -eq 2 && test ! -s "$tmp/text" || exit 1
# A bad final record must reject the entire stream before writing to stdout.
head -c -2 "$tmp/tokens" > "$tmp/bad-tail"
printf '\377\377' >> "$tmp/bad-tail"
status=0
asm/build/morph-tokenizer decode < "$tmp/bad-tail" > "$tmp/text" || status=$?
test "$status" -eq 2 && test ! -s "$tmp/text" || exit 1

# Verify both inclusive size limits, including byte ID zero.
for size in 0 1048576; do
    head -c "$size" /dev/zero > "$tmp/boundary"
    asm/build/morph-tokenizer encode < "$tmp/boundary" > "$tmp/encoded"
    asm/build/morph-tokenizer decode < "$tmp/encoded" > "$tmp/text"
    cmp "$tmp/boundary" "$tmp/text"
done
test "$(asm/build/exactcalc div 5 7 -2 9)" = '-45/14'
test "$(asm/build/nexus_audit add 1 10 2 10 3/10)" = 'VERIFIED 3/10'
status=0
asm/build/nexus_audit add 1 10 2 10 6/20 > "$tmp/mismatch" || status=$?
test "$status" -eq 1 && grep -qx 'MISMATCH' "$tmp/mismatch" || exit 1
test "$(lang/niyahrun tests/recursive-assignment.niy)" = 'RESULT=80'

# Run the examples in the public build instructions against this checkout.
test "$(lang/niyahrun lang/examples/factorial.niy)" = 'RESULT=720'
test "$(build/engine/niyah calc '10/4')" = '5/2'
sh retrieval/index-docs.sh "$tmp/manual.dalw"
retrieval/lowlevel/dalw-cli verify "$tmp/manual.dalw" > "$tmp/verified"
retrieval/lowlevel/dalw-cli search "$tmp/manual.dalw" 5 checkpoint > "$tmp/doc-results"
test "$(wc -l < "$tmp/doc-results")" -eq 5
grep -F 'engine/docs/' "$tmp/doc-results" >/dev/null

count=0
for exe in build/engine/niyah build/engine/niyah-train build/engine/niyah_probe \
           lang/niyahlex lang/niyahparse lang/niyahrun lang/niyahc lang/niyahc-tile \
           asm/build/trigram asm/build/attention_lm asm/build/exactcalc asm/build/nexus_audit \
           asm/build/continual_lora asm/build/tile_probe asm/build/tile_allocator \
           asm/build/morph-tokenizer asm/build/tensor/project hadeed/build/kernel.elf \
           model/build/niyah-model model/build/niyah-train model/build/niyah-hybrid \
           model/build/niyah-query model/build/niyah-chronicle retrieval/lowlevel/dalw-cli \
           build/search/test_niyah_url build/search/test_niyah_index build/search/test_html_extract \
           build/search/test_index_retry; do
    readelf -lW "$exe" > "$tmp/headers"
    readelf -dW "$exe" > "$tmp/dynamic"
    if grep -q INTERP "$tmp/headers" || grep -q NEEDED "$tmp/dynamic"; then
        echo "external runtime dependency: $exe" >&2; exit 1
    fi
    count=$((count + 1))
done
printf 'ELF files without PT_INTERP or DT_NEEDED: %d\n' "$count"
