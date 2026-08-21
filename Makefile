# Build script for `te`. Linux-only: compiles src/*.c with cc and links
# against the system-installed SDL2 and libpcre2-8 (found via pkg-config).
#
#   make            # produces ./te in the project root
#   make run        # build, then run ./te
#   make test       # build, then run the test suite
#   make clean

CC ?= cc
CFLAGS := -std=c11 -Wall -Wextra -Ibuild -MMD -MP

SDL_CFLAGS   := $(shell pkg-config --cflags sdl2)
SDL_LIBS     := $(shell pkg-config --libs sdl2)
PCRE2_CFLAGS := $(shell pkg-config --cflags libpcre2-8)
PCRE2_LIBS   := $(shell pkg-config --libs libpcre2-8)

TE_CFLAGS := $(CFLAGS) $(SDL_CFLAGS) $(PCRE2_CFLAGS)
# -lm: stb_truetype's rasterizer uses floor/sqrt/etc; SDL2's pkg-config
# doesn't pull libm in on its own.
TE_LIBS   := $(SDL_LIBS) $(PCRE2_LIBS) -lm

.PHONY: all run test clean

all: te

build:
	mkdir -p build

# Generates build/<name>_pl.h from src/<name>.pl: a byte array
# (<NAME>_PL_SRC) plus its length, baking the engine's Prolog-defined
# standard library and te's own default bindings/undo history into the
# binary at build time -- unlike the bundled font, neither is a file `te`
# needs to find on disk next to the executable at runtime.
build/%_pl.h: src/%.pl tools/gen_pl_header.sh | build
	@echo "Generating $@"
	@symbol=$$(printf '%s' $* | tr 'a-z' 'A-Z')_PL_SRC; \
	./tools/gen_pl_header.sh $< $@ $$symbol

build/main.o: src/main.c | build
	$(CC) $(TE_CFLAGS) -c $< -o $@

build/glyphs.o: src/glyphs.c | build
	$(CC) $(TE_CFLAGS) -c $< -o $@

build/platform.o: src/platform.c | build
	$(CC) $(TE_CFLAGS) -c $< -o $@

# src/prolog.c #includes the generated build/bootstrap_pl.h.
build/prolog.o: src/prolog.c build/bootstrap_pl.h | build
	$(CC) $(TE_CFLAGS) -c $< -o $@

# src/script.c #includes the generated build/default_bindings_pl.h,
# build/undo_history_pl.h, and build/search_pl.h.
build/script.o: src/script.c build/default_bindings_pl.h build/undo_history_pl.h build/search_pl.h | build
	$(CC) $(TE_CFLAGS) -c $< -o $@

te: build/main.o build/glyphs.o build/platform.o build/script.o build/prolog.o
	$(CC) $^ -o $@ $(TE_LIBS)

# tests/unit_te.c #includes src/main.c directly to reach its static state
# and functions (see the comment at the top of that file), so it needs the
# same libraries as `te` itself, and must NOT link build/main.o (duplicate
# symbols).
build/unit_te.o: tests/unit_te.c | build
	$(CC) $(TE_CFLAGS) -c $< -o $@

build/unit_te: build/unit_te.o build/glyphs.o build/platform.o build/script.o build/prolog.o
	$(CC) $^ -o $@ $(TE_LIBS)

# tests/cli_te.c only drives the compiled `te` binary as a subprocess, so
# it needs no extra libraries.
build/cli_te.o: tests/cli_te.c | build
	$(CC) -std=c11 -Wall -Wextra -MMD -MP -c $< -o $@

build/cli_te: build/cli_te.o
	$(CC) $^ -o $@

run: te
	./te $(ARGS)

test: build/unit_te build/cli_te
	./build/unit_te
	./build/cli_te te

clean:
	rm -rf build te

-include $(wildcard build/*.d)
