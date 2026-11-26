.RECIPEPREFIX = >
CC     ?= gcc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -fPIC
LIB     = libdalw.a
OBJ     = dalw.o sha256.o intersect.o

all: $(LIB) dalw-cli

%.o: %.c dalw.h
> $(CC) $(CFLAGS) -c $< -o $@

dalw.o: ../native/niyah_utf8.h

$(LIB): $(OBJ)
> ar rcs $@ $^

dalw-cli: dalw_cli.c niyah_dalw.h niyah_dalw.o $(LIB)
> $(CC) $(CFLAGS) -I../native $< niyah_dalw.o $(LIB) -lm -o $@

test_dalw: test_dalw.c $(LIB)
> $(CC) $(CFLAGS) $< $(LIB) -lm -o $@

bench_isect: bench_isect.c $(LIB)
> $(CC) $(CFLAGS) $< $(LIB) -lm -o $@

test: test_dalw dalw-cli
> sh test_cli.sh "$(CURDIR)/dalw-cli"
> @set -e; dir=$$(mktemp -d); trap 'rm -rf "$$dir"' EXIT; cd "$$dir"; "$(CURDIR)/test_dalw"

clean:
> rm -f *.o $(LIB) dalw-cli test_dalw bench_isect test_niyah_dalw test_niyah_dalw_json

.PHONY: all test clean

STRICT = -D_POSIX_C_SOURCE=200809L -Wpedantic -Werror -Wconversion -Wshadow

niyah_dalw.o: niyah_dalw.c niyah_dalw.h dalw.h ../native/niyah_search.h
> $(CC) $(CFLAGS) $(STRICT) -I../native -c $< -o $@

test_niyah_dalw: test_niyah_dalw.c niyah_dalw.o $(LIB)
> $(CC) $(CFLAGS) -I../native $< niyah_dalw.o $(LIB) -lm -o $@

test-adapter: test_niyah_dalw
> @set -e; dir=$$(mktemp -d); trap 'rm -rf "$$dir"' EXIT; cd "$$dir"; "$(CURDIR)/test_niyah_dalw"

.PHONY: test-adapter

niyah_dalw_json.o: niyah_dalw_json.c niyah_dalw_json.h niyah_dalw.h ../native/niyah_search.h ../native/niyah_utf8.h
> $(CC) $(CFLAGS) $(STRICT) -I../native -c $< -o $@

test_niyah_dalw_json: test_niyah_dalw_json.c niyah_dalw_json.o niyah_dalw.o $(LIB)
> $(CC) $(CFLAGS) -I../native $< niyah_dalw_json.o niyah_dalw.o $(LIB) -lm -o $@

test-json: test_niyah_dalw_json
> @set -e; dir=$$(mktemp -d); trap 'rm -rf "$$dir"' EXIT; cd "$$dir"; "$(CURDIR)/test_niyah_dalw_json"

.PHONY: test-json

.PHONY: windows-dll
WIN_CC ?= x86_64-w64-mingw32-gcc
windows-dll: dist/niyah_dalw.dll
dist/niyah_dalw.dll: niyah_dalw.c niyah_dalw_json.c dalw.c sha256.c intersect.c dalw.h niyah_dalw.h niyah_dalw_json.h ../native/niyah_search.h ../native/niyah_utf8.h
> mkdir -p $(@D)
> $(WIN_CC) -std=c11 -O2 -I../native -shared -static-libgcc $(filter %.c,$^) -lm -o $@
