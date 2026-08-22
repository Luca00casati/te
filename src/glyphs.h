// Lazy per-codepoint glyph cache backed by UnifontEX.
//
// The main atlas font (also UnifontEX) only bakes a common set of codepoints.
// Anything outside that set -- CJK, emoji, rarer scripts and symbols -- is
// rasterized here one glyph at a time and cached as a GPU texture, so the
// editor can display essentially all of Unicode without a giant atlas.
//
// UnifontEX is a single TrueType file covering every Unicode plane. Glyphs are
// monochrome and either half-width (one cell) or full-width (two cells).
#ifndef TE_GLYPHS_H
#define TE_GLYPHS_H

#include <stddef.h>
#include <stdint.h>
#include "platform.h"

typedef struct {
    Texture tex;
    bool has;      // a real glyph was rasterized (vs. a blank/missing one)
    uint8_t cells; // display width: 1 = half-width, 2 = full-width
    float ox, oy;  // glyph bearing: pixel offset from the pen position to
                   // the bitmap's top-left corner
} Glyph;

// `data`/`data_len` must stay valid for as long as the cache is used (glyphs
// are rasterized lazily on demand, not all up front). `gl_ctx` is
// platformGLContext() -- pass NULL (e.g. in tests, which never open a
// window) to keep the cache safely inert: glyphs_get/glyphs_cells still
// work, they just never rasterize a real bitmap or touch GL.
void glyphs_init(const unsigned char *data, size_t data_len, int px, void *gl_ctx);
void glyphs_deinit(void);

// Drop every cached glyph and re-target a new rasterization size (used when
// the font is zoomed). Cached textures were baked at the old size, so they
// must be re-rasterized on next use.
void glyphs_reset(int px);

// Cached glyph for `cp`, rasterizing (and caching) on first use.
Glyph glyphs_get(uint32_t cp);

// Display width of `cp` in cells (1 or 2). Rasterizes lazily to read the
// advance, then serves from cache.
uint8_t glyphs_cells(uint32_t cp);

// Scaled pixel advance width of `cp` -- metrics only, no bitmap/texture.
// Used once at startup (and on zoom) to measure the monospace cell width.
float glyphs_advance(uint32_t cp);

#endif // TE_GLYPHS_H
