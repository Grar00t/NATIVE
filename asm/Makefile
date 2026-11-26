CC ?= cc
BIN := build
NATIVE_FLAGS := -nostdlib -static -no-pie -Wl,--build-id=none,-z,noexecstack
FREESTANDING := -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -ffreestanding -fno-builtin -fno-stack-protector -fno-tree-loop-distribute-patterns
LANG_SRC := ../lang/src
MORPH := src/morph_tokenizer
IR := $(BIN)/tensor
ASM_TOOLS := trigram attention_lm continual_lora exactcalc tile_allocator nexus_audit
.PHONY: all clean
.DELETE_ON_ERROR:
all: $(addprefix $(BIN)/,$(ASM_TOOLS)) $(BIN)/tile_probe $(BIN)/morph-tokenizer $(IR)/project

$(BIN) $(IR):
	mkdir -p $@

$(BIN)/%: src/%.s | $(BIN)
	$(CC) $(NATIVE_FLAGS) -o $@ $<

$(BIN)/trigram: data/corpus.txt
$(BIN)/attention_lm: data/corpus.txt data/heldout.txt
$(BIN)/nexus_audit: $(BIN)/exactcalc

$(BIN)/tile_probe: src/tile_probe.s src/tile_kernels.s | $(BIN)
	$(CC) $(NATIVE_FLAGS) -o $@ $^

$(BIN)/morph-tokenizer: $(MORPH)/start.s $(MORPH)/tokenizer.c $(MORPH)/vocab.h $(LANG_SRC)/morph.c $(LANG_SRC)/morph.h $(LANG_SRC)/lexer.h | $(BIN)
	$(CC) $(FREESTANDING) $(NATIVE_FLAGS) -I$(LANG_SRC) -o $@ $(MORPH)/start.s $(MORPH)/tokenizer.c $(LANG_SRC)/morph.c

$(IR)/project.nir: examples/tensor_arabic.niyah ../lang/niyahc-tile | $(IR)
	../lang/niyahc-tile --ir $< > $@

$(IR)/intervals: $(IR)/project.nir
	../lang/niyahc-tile --intervals $< > $@

$(IR)/assignment: $(IR)/intervals $(BIN)/tile_allocator
	$(BIN)/tile_allocator < $< > $@

$(IR)/project.s: $(IR)/project.nir $(IR)/assignment
	../lang/niyahc-tile --asm $^ > $@

$(IR)/project: $(IR)/project.s src/tensor_runtime.s src/tensor_dispatch.s src/gemm_i8.s
	$(CC) $(NATIVE_FLAGS) -o $@ $^

clean:
	rm -rf $(BIN)

$(BIN)/i8-gemm-check: tests/i8_gemm_harness.c tests/i8_dot.s src/niyah_i8_gemm.s | $(BIN)
	$(CC) -O2 -std=c11 -Wall -Wextra -static -Wl,-z,noexecstack -o $@ $^ -lm

.PHONY: check-i8
check-i8: $(BIN)/i8-gemm-check
	@rc=0; $(BIN)/i8-gemm-check || rc=$$?; test $$rc -eq 0 || test $$rc -eq 77

QKV_BUILD ?= build/qkv
QKV_LDFLAGS ?= -static
QKV_EXPORT_FLAGS := -nostdlib -static -no-pie -Wl,-z,noexecstack -Wa,--defsym,SAMPLE_LEN=0,--defsym,EXPORT_WEIGHTS=1
QKV_LIVE := $(addprefix $(QKV_BUILD)/current/,initial step)
QKV_FIXED := $(addprefix $(QKV_BUILD)/reference/,initial step trained)
QKV_MODELS := $(QKV_LIVE) $(QKV_FIXED)

$(filter %/initial,$(QKV_MODELS)): private QKV_EPOCHS := -Wa,--defsym,EPOCHS=0
$(filter %/step,$(QKV_MODELS)): private QKV_EPOCHS := -Wa,--defsym,EPOCHS=1,--defsym,ONE_STEP=1
$(QKV_BUILD)/reference/trained: private QKV_EPOCHS := -Wa,--defsym,EPOCHS=240

$(QKV_LIVE): private QKV_DIR := .
$(QKV_FIXED): private QKV_DIR := tests/qkv
$(QKV_LIVE): data/corpus.txt data/heldout.txt
$(QKV_FIXED): tests/qkv/data/corpus.txt tests/qkv/data/heldout.txt
$(QKV_MODELS): src/attention_lm.s
	@mkdir -p "$(@D)"
	cd "$(QKV_DIR)" && $(CC) $(QKV_EXPORT_FLAGS) $(QKV_EPOCHS) -o "$(abspath $@)" "$(abspath src/attention_lm.s)"

$(QKV_BUILD)/check: tests/test_qkv_gradients.c
	@mkdir -p "$(@D)"
	$(CC) $(CPPFLAGS) $(CFLAGS) -std=c11 -Wall -Wextra -Wpedantic $(LDFLAGS) $(QKV_LDFLAGS) -o $@ $< -lm

.PHONY: test-qkv-gradients
test-qkv-gradients: $(QKV_BUILD)/check $(QKV_MODELS)
	@set -eu; tmp=$$(mktemp -d); trap 'rm -rf "$$tmp"' EXIT; \
	  for run in current reference; do \
	    corpus=data/corpus.txt; if [ "$$run" = reference ]; then corpus=tests/qkv/data/corpus.txt; fi; \
	    printf 'QKV corpus %s\n' "$$corpus"; \
	    timeout 60 "$(QKV_BUILD)/$$run/initial" > "$$tmp/initial"; \
	    timeout 60 "$(QKV_BUILD)/$$run/step" > "$$tmp/step"; \
	    "$(QKV_BUILD)/check" "$$corpus" "$$tmp/initial" "$$tmp/step"; \
	  done; \
	  timeout 60 "$(QKV_BUILD)/reference/trained" > "$$tmp/trained"; \
	  "$(QKV_BUILD)/check" --trained "$$tmp/trained"
