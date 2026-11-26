CC ?= cc
CFLAGS := -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror -ffreestanding
HASH = src/sha256.c src/object_id.c
HEADERS = src/sha256.h src/object_id.h src/capability.h
TESTS = ribat-hash-test ribat-capability-test ribat-width-test

all: $(TESTS)

ribat-hash-test: $(HASH) $(HEADERS) tests/test_sha256.c
	$(CC) $(CFLAGS) -Isrc -o $@ $(HASH) tests/test_sha256.c

ribat-capability-test: src/capability.c $(HEADERS) tests/test_capability.c
	$(CC) $(CFLAGS) -Isrc -o $@ src/capability.c tests/test_capability.c

ribat-width-test: $(HASH) src/capability.c $(HEADERS) tests/test_width.c
	$(CC) $(CFLAGS) -Isrc -o $@ $(HASH) src/capability.c tests/test_width.c

test: all
	./ribat-hash-test
	./ribat-capability-test
	./ribat-width-test

clean:
	rm -f $(TESTS)

.PHONY: all test clean
