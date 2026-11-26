CC ?= cc
AR ?= ar
CPPFLAGS += -Iinclude -ICore_CPP -I../engine/include -I../engine/src -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS ?= -O2 -std=c11 -Wall -Wextra
LDFLAGS ?= -static
LDLIBS += -lm
OUT := build
LIB := $(OUT)/libniyah_model.a
CORE := niyah_core niyah_train_full hybrid_reasoner constraint_solver rule_parser rule_source_guard proof_generator khz_q_svd khz_q_ising casper_rag casper_chronicle chronicle_pool
SOURCES := $(addprefix Core_CPP/,$(addsuffix .c,$(CORE))) src/niyah_gguf_map.c tokenizer.c
ifneq ($(findstring mingw,$(shell $(CC) -dumpmachine)),)
SOURCES += src/casper_ffi.c
endif
OBJECTS := $(addprefix $(OUT)/,$(SOURCES:.c=.o)) $(OUT)/niyah_publish_internal.o
BINS := niyah-model niyah-train niyah-hybrid niyah-query niyah-chronicle
TESTS := test_raw_weights test_niyah_sampler test_proof_v1 test_rag_json test_gguf_infer test_chronicle_pool test_chronicle_api test_chronicle_stream
ifeq ($(findstring mingw,$(shell $(CC) -dumpmachine)),)
TESTS += test_local_http test_chronicle_alloc_fail test_chronicle_io test_chronicle_proof_stream_errors
endif
.PHONY: all check clean
.DELETE_ON_ERROR:
all: $(addprefix $(OUT)/,$(BINS))
$(OUT)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(OUT)/niyah_publish_internal.o: ../engine/src/niyah_publish_internal.c
	@mkdir -p $(@D)
	$(CC) $(CPPFLAGS) $(CFLAGS) -MMD -MP -c $< -o $@
$(LIB): $(OBJECTS)
	$(AR) rcs $@ $^
$(OUT)/niyah-model: $(OUT)/Core_CPP/niyah_main.o $(LIB)
$(OUT)/niyah-train: $(OUT)/Core_CPP/niyah_train.o $(LIB)
$(OUT)/niyah-hybrid: $(OUT)/Core_CPP/niyah_hybrid_main.o $(LIB)
$(OUT)/niyah-query: $(OUT)/Core_CPP/casper_cli.o $(LIB)
$(OUT)/niyah-chronicle: $(OUT)/Core_CPP/casper_chronicle_main.o $(LIB)
$(addprefix $(OUT)/,$(BINS)):
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@
$(OUT)/test_chronicle_alloc_fail: private override LDFLAGS += -Wl,--wrap=chr_malloc,--wrap=chr_calloc,--wrap=chr_realloc
$(OUT)/test_chronicle_io: private override LDFLAGS += -Wl,--wrap=fopen,--wrap=fread,--wrap=fgetc,--wrap=fclose
$(OUT)/test_chronicle_proof_stream_errors: private override LDFLAGS += -Wl,--wrap=fopen,--wrap=malloc
$(OUT)/test_chronicle_stream: private override LDFLAGS += -Wl,--wrap=fread
$(OUT)/test_%: $(OUT)/tests/test_%.o $(LIB)
	$(CC) $(LDFLAGS) $^ $(LDLIBS) -o $@
check: all $(addprefix $(OUT)/,$(TESTS))
	@set -e; dir=$$(mktemp -d); trap 'rm -rf "$$dir"' EXIT; mkdir -p "$$dir/build/release"; cd "$$dir"; for t in $(TESTS); do "$(CURDIR)/$(OUT)/$$t"; done
clean:
	rm -rf $(OUT)
-include $(wildcard $(OUT)/*.d $(OUT)/Core_CPP/*.d $(OUT)/src/*.d $(OUT)/tests/*.d)
