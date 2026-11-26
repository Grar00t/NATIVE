JOBS ?= 2
CMAKE_JOBS = $(if $(filter --jobserver-auth=% --jobserver-fds=%,$(MAKEFLAGS)),,--parallel $(JOBS))
ENGINE_DIR := build/engine
.DEFAULT_GOAL := all
.PHONY: all engine lang asm hadeed check check-host check-boot clean

all: engine lang asm hadeed

engine:
	cmake -S engine -B $(ENGINE_DIR) -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS=-static -DNIYAH_BUILD_TESTS=ON
	+cmake --build $(ENGINE_DIR) $(CMAKE_JOBS)

lang:
	$(MAKE) -C lang all tensor-backend

asm: lang
	$(MAKE) -C asm

hadeed:
	$(MAKE) -C hadeed

check-host: all
	sh tests/stream-progress.sh build/think
	sh tests/check-engine.sh "$(ENGINE_DIR)" model/build
	$(MAKE) -C lang test
	$(CC) -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -Iengine/include -static -o build/tensor-oracle tests/tensor_oracle.c $(ENGINE_DIR)/libniyah.a -lm
	sh tests/integration.sh
	sh tests/assembly-models.sh
	$(MAKE) -C hadeed static-test memory-test trace-verify m4-trace-verify

check-boot: hadeed
	$(MAKE) -C hadeed runtime-test

.PHONY: check-memory
check-memory:
	CC="$(CC)" sh tests/check-memory.sh "$(JOBS)"

check: check-host
	$(MAKE) check-boot

clean:
	$(MAKE) -C asm clean
	$(MAKE) -C lang clean
	$(MAKE) -C hadeed clean
	rm -rf build

.PHONY: check-imports
check-imports:
	sh tests/check-imports.sh

check-host: check-imports check-local-only

.PHONY: check-local-only
check-local-only:
	sh tests/check-local-only.sh

.PHONY: model retrieval
all: model retrieval
model:
	$(MAKE) -C model all
retrieval:
	$(MAKE) -C retrieval/lowlevel CC="$(CC) -static" all
	cmake -S retrieval/search -B build/search -DCMAKE_EXE_LINKER_FLAGS=-static
	+cmake --build build/search $(CMAKE_JOBS)

.PHONY: check-components clean-components test-qkv-gradients
test-qkv-gradients:
	$(MAKE) -C asm test-qkv-gradients

check-host: check-components
check-components: model retrieval asm test-qkv-gradients
	$(MAKE) -C model check
	$(MAKE) -C asm check-i8
	$(MAKE) -C retrieval/native LDLIBS="-static -lm" test
	$(MAKE) -C retrieval/lowlevel CC="$(CC) -static" test test-adapter test-json
	ctest --test-dir build/search --output-on-failure
clean: clean-components
clean-components:
	$(MAKE) -C ribat clean
	$(MAKE) -C model clean
	$(MAKE) -C retrieval/native clean
	$(MAKE) -C retrieval/lowlevel clean

.PHONY: bench-search
SEARCH_SOURCES = retrieval/search/niyah_index.c retrieval/lowlevel/dalw.c retrieval/lowlevel/sha256.c retrieval/lowlevel/intersect.c
build/search-bench: tests/search_bench.c $(SEARCH_SOURCES) retrieval/search/niyah_index.h retrieval/lowlevel/dalw.h retrieval/lowlevel/dalw_analysis.h retrieval/native/niyah_utf8.h
	mkdir -p build
	$(CC) -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -Iretrieval/search -Iretrieval/lowlevel -o $@ tests/search_bench.c $(SEARCH_SOURCES) -lm

bench-search: build/search-bench
	./build/search-bench

THINK_CPP = tools/agents.cpp tools/agent_connectors.cpp
THINK_HEADERS = tools/think.h tools/agent_connectors.hpp retrieval/native/niyah_utf8.h

build/think-core.o: tools/think.c tools/think.h Makefile
	mkdir -p $(@D)
	$(CC) -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -DTHINK_WITH_AGENTS -c $< -o $@

build/think: build/think-core.o $(THINK_CPP) $(THINK_HEADERS) Makefile
	$(CXX) -std=c++17 -O2 -Wall -Wextra -Wpedantic -Werror -static $(THINK_CPP) $< -o $@

.PHONY: check-agents
check-agents: build/think
	sh tests/agents.sh build/think

all: build/think
check-host: check-agents

.PHONY: check-ribat check-al-kimiyya
check-components: check-ribat check-al-kimiyya
check-ribat:
	$(MAKE) -C ribat test

build/al-kimiyya: experiments/al-kimiyya/vigenere.c
	mkdir -p $(@D)
	$(CC) -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror $< -o $@

check-al-kimiyya: build/al-kimiyya
	test "$$($< encrypt LEMON ATTACKATDAWN)" = LXFOPVEFRNHR
	test "$$($< decrypt LEMON LXFOPVEFRNHR)" = ATTACKATDAWN
	test "$$($< encrypt A '')" = ''
	! $< encrypt '' ABC
	! $< encrypt A 'A B'
