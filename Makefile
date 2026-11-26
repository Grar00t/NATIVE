JOBS ?= 2
ENGINE_DIR := build/engine
.DEFAULT_GOAL := all
.PHONY: all engine lang asm hadeed check check-host check-boot clean

all: engine lang asm hadeed

engine:
	cmake -S engine -B $(ENGINE_DIR) -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXE_LINKER_FLAGS=-static -DNIYAH_BUILD_TESTS=ON
	cmake --build $(ENGINE_DIR) --parallel $(JOBS)

lang:
	$(MAKE) -C lang all tensor-backend

asm: lang
	$(MAKE) -C asm

hadeed:
	$(MAKE) -C hadeed

check-host: all
	sh tests/check-engine.sh "$(ENGINE_DIR)" model/build
	$(MAKE) -C lang test
	$(CC) -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -Iengine/include -static -o build/tensor-oracle tests/tensor_oracle.c $(ENGINE_DIR)/libniyah.a -lm
	sh tests/integration.sh
	sh tests/assembly-models.sh
	$(MAKE) -C hadeed static-test m4-trace-verify

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

check-host: check-imports

.PHONY: model retrieval
all: model retrieval
model:
	$(MAKE) -C model all
retrieval:
	$(MAKE) -C retrieval/lowlevel CC="$(CC) -static" all
	cmake -S retrieval/search -B build/search -DCMAKE_EXE_LINKER_FLAGS=-static
	cmake --build build/search --parallel $(JOBS)

.PHONY: check-components clean-components
check-host: check-components
check-components: model retrieval asm
	$(MAKE) -C model check
	$(MAKE) -C asm check-i8
	$(MAKE) -C retrieval/native LDLIBS="-static -lm" test
	$(MAKE) -C retrieval/lowlevel CC="$(CC) -static" test test-adapter test-json
	ctest --test-dir build/search --output-on-failure
clean: clean-components
clean-components:
	$(MAKE) -C model clean
	$(MAKE) -C retrieval/native clean
	$(MAKE) -C retrieval/lowlevel clean
