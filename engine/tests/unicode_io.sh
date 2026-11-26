#!/bin/sh
set -eu
cli=$1
train=$2
probe=$3
d=$(mktemp -d)
trap 'rm -rf "$d"' EXIT
cd "$d"
mkdir 'تجربة-Ω'
cd 'تجربة-Ω'
printf 'ابج abc abc\n' > نص
prepare() {
    "$cli" prepare --corpus نص --tokenizer-out رموز --shard-out بيانات \
        --target-vocab 258 --min-pair-frequency 2 --sequence-length 4
}
prepare
sha256sum رموز بيانات > before
if prepare; then exit 1; fi
sha256sum -c before
"$train" new --tokenizer رموز --shard بيانات --checkpoint-out نموذج --cursor-out مؤشر \
    --updates 1 --batch-size 1 --accumulation-steps 1 --model-seed 42 --data-seed 7 \
    --context-length 4 --embedding-dim 8 --layers 1 --heads 2 --kv-heads 1 \
    --ffn-hidden-dim 16 --rms-norm-eps 0.00001 --tie-word-embeddings 1 \
    --learning-rate 0.001 --beta1 0.9 --beta2 0.999 --epsilon 0.00000001 \
    --weight-decay 0 --max-grad-norm 1
run() {
    "$cli" run --tokenizer رموز --checkpoint نموذج --prompt a --max-new-tokens 1 --temperature 0
}
rank() {
    "$probe" --tokenizer رموز --shard بيانات --checkpoint نموذج --prompt a --topk 1 --trace-steps 1
}
run > generated
rank > ranking
for operation in run rank; do
    if "$operation" > /dev/full 2> diagnostic; then exit 1; fi
    grep -q stdout_write diagnostic
done
"$cli" eval --tokenizer رموز --checkpoint نموذج --heldout بيانات --format shard
sha256sum نموذج مؤشر > pair
"$train" resume --tokenizer رموز --shard بيانات --checkpoint-in نموذج --cursor-in مؤشر \
    --checkpoint-out 'تال' --cursor-out 'موضع' --updates 1 --batch-size 1 --accumulation-steps 1 > resume.log
grep 'optimizer_step=2' resume.log
sha256sum -c pair
