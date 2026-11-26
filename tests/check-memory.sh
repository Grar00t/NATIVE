#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
jobs=$1
flags='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer'
link="-no-pie $flags"
export ASAN_OPTIONS=detect_leaks=1:halt_on_error=1
export UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1

"${CC:-cc}" -std=c11 $flags -DTHINK_WITH_AGENTS -c "$root/tools/think.c" -o "$work/think.o"
"${CXX:-c++}" -std=c++17 $link "$root/tools/agents.cpp" "$root/tools/agent_connectors.cpp" "$work/think.o" -o "$work/think"
sh "$root/tests/agents.sh" "$work/think"

"${CC:-cc}" -std=c11 -Wall -Wextra -Wpedantic -Werror $link -I"$root/hadeed/kernel" \
    "$root/hadeed/tests/memory_test.c" "$root/hadeed/kernel/memory.c" -o "$work/guest-memory-test"
"$work/guest-memory-test"

mkdir -p "$work/ribat"
cp -R "$root/ribat/src" "$root/ribat/tests" "$root/ribat/Makefile" "$work/ribat/"
make -C "$work/ribat" -j"$jobs" "CFLAGS=-std=c11 -Wall -Wextra -Wpedantic -Werror $link" test

for component in engine retrieval/search; do
    build="$work/build/$component"
    cmake -S "$root/$component" -B "$build" -DCMAKE_BUILD_TYPE=Debug \
        "-DCMAKE_C_FLAGS=$flags" "-DCMAKE_CXX_FLAGS=$flags" "-DCMAKE_EXE_LINKER_FLAGS=$link"
    cmake --build "$build" --parallel "$jobs"
    if [ "$component" = retrieval/search ]; then ctest --test-dir "$build" --output-on-failure; fi
done

make -C "$root/asm" -j"$jobs" "QKV_BUILD=$work/qkv" "CFLAGS=$flags" \
    "QKV_LDFLAGS=$link" test-qkv-gradients

# The Make-based components place binaries next to their sources.
mkdir -p "$work/model" "$work/retrieval/lowlevel" "$work/retrieval/native" "$work/lang/tests"
mkdir -p "$work/engine/src" "$work/engine/include/niyah"
cp "$root/engine/src/niyah_publish_internal."? "$work/engine/src/"
cp "$root/engine/include/niyah/status.h" "$work/engine/include/niyah/"
cp -R "$root/model/Core_CPP" "$root/model/include" "$root/model/src" "$root/model/tests" "$work/model/"
cp "$root/model/"*.[ch] "$root/model/Makefile" "$work/model/"
make -C "$work/model" -j"$jobs" "CFLAGS=-std=c11 -Wall -Wextra $flags" "LDFLAGS=$link" check

sh "$root/tests/check-engine.sh" "$work/build/engine" "$work/model/build"

for module in native lowlevel; do
    src="$root/retrieval/$module"
    cp "$src/"*.[ch] "$src/Makefile" "$work/retrieval/$module/"
done
cp "$root/retrieval/lowlevel/test_cli.sh" "$work/retrieval/lowlevel/"
make -C "$work/retrieval/native" -j"$jobs" "CC=${CC:-cc} -no-pie" \
    "CFLAGS=-std=c11 -Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow $flags" "LDLIBS=$flags -lm" test
make -C "$work/retrieval/lowlevel" -j"$jobs" "CC=${CC:-cc} -no-pie" \
    "CFLAGS=-std=c11 -Wall -Wextra -fPIC $flags" test test-adapter test-json

cp -R "$root/lang/src" "$root/lang/examples" "$work/lang/"
cp "$root/lang/Makefile" "$root/lang/Makefile.tensor" "$work/lang/"
cp "$root/lang/tests/"*.c "$work/lang/tests/"
cd "$work/lang"
units='tests/lexer_test tests/lexer_recovery_test tests/morph_test tests/parser_test tests/check_test tests/eval_test tests/depth_test'
make -j"$jobs" "HOST_CFLAGS=-std=c11 -Wall -Wextra -Wpedantic -Werror $flags" \
    "HOST_LDFLAGS=$link" niyahrun $units
for unit in $units; do "./$unit"; done
test "$(./niyahrun "$root/tests/recursive-assignment.niy")" = RESULT=80
printf '%s\n' 'Native memory checks passed'
