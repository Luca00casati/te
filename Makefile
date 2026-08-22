# Build script for `te`. Linux-only: compiles src/*.c with cc and links
# against the system-installed GLFW3, OpenGL, libpcre2-8, FreeType2, and
# libpng (found via pkg-config).
#
#   make            # produces ./te in the project root
#   make run        # build, then run ./te
#   make test       # build, then run the test suite
#   make clean

CC ?= cc
CFLAGS := -std=c11 -Wall -Wextra -MMD -MP

GLFW_CFLAGS  := $(shell pkg-config --cflags glfw3)
GLFW_LIBS    := $(shell pkg-config --libs glfw3) -lGL
PCRE2_CFLAGS := $(shell pkg-config --cflags libpcre2-8)
PCRE2_LIBS   := $(shell pkg-config --libs libpcre2-8)
FT_CFLAGS    := $(shell pkg-config --cflags freetype2)
FT_LIBS      := $(shell pkg-config --libs freetype2)
PNG_CFLAGS   := $(shell pkg-config --cflags libpng)
PNG_LIBS     := $(shell pkg-config --libs libpng)

TE_CFLAGS := $(CFLAGS) $(GLFW_CFLAGS) $(PCRE2_CFLAGS) $(FT_CFLAGS) $(PNG_CFLAGS)
TE_LIBS   := $(GLFW_LIBS) $(PCRE2_LIBS) $(FT_LIBS) $(PNG_LIBS) -lm

.PHONY: all run test clean

all: te

build:
	mkdir -p build

# te's own src/*.pl files (bootstrap.pl, default_bindings.pl, and whatever
# else is under src/) are read from disk at startup, not baked into the
# binary -- see script.c's resolvePlDir/scriptSetup. Nothing to generate
# here; they just need to exist on disk next to the built `te` (or ./src
# relative to the working directory, which is what the test binary in
# build/ falls back to).

build/main.o: src/main.c | build
	$(CC) $(TE_CFLAGS) -c $< -o $@

build/glyphs.o: src/glyphs.c | build
	$(CC) $(TE_CFLAGS) -c $< -o $@

build/platform.o: src/platform.c | build
	$(CC) $(TE_CFLAGS) -c $< -o $@

build/prolog.o: src/prolog.c | build
	$(CC) $(TE_CFLAGS) -c $< -o $@

build/script.o: src/script.c | build
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

# tests/unit_prolog.c exercises src/prolog.c/.h directly (it's not editor-
# specific, so unlike unit_te.c it doesn't need to #include main.c or link
# against GLFW/PCRE2/FreeType/libpng at all).
build/unit_prolog.o: tests/unit_prolog.c | build
	$(CC) -std=c11 -Wall -Wextra -MMD -MP -c $< -o $@

build/unit_prolog: build/unit_prolog.o build/prolog.o
	$(CC) $^ -o $@ -lm

run: te
	./te $(ARGS)

test: build/unit_te build/cli_te build/unit_prolog
	./build/unit_te
	./build/cli_te te
	./build/unit_prolog

clean:
	rm -rf build te

-include $(wildcard build/*.d)
