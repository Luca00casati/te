#include "glyphs.h"

#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "../third_party/stb_truetype.h"

// Font bytes handed in by glyphs_init (owned by main.c; loaded from disk at
// startup instead of being embedded into the binary).
static const unsigned char *font_data = NULL;
static size_t font_data_len = 0;
static int raster_px = 16;
static SDL_Renderer *gl_renderer = NULL;

static stbtt_fontinfo stb_font;
static bool stb_font_ready = false;

// --- lazy glyph cache: open-addressing hash table, uint32 codepoint -> Glyph
typedef struct {
    uint32_t key;
    bool occupied;
    Glyph val;
} Slot;

static Slot *table = NULL;
static size_t table_cap = 0;
static size_t table_count = 0;

static uint32_t hash_u32(uint32_t x) {
    // murmur3-style finalizer
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}

static Slot *table_find_slot(Slot *t, size_t cap, uint32_t cp) {
    size_t mask = cap - 1;
    size_t i = hash_u32(cp) & mask;
    for (;;) {
        if (!t[i].occupied || t[i].key == cp) return &t[i];
        i = (i + 1) & mask;
    }
}

static void table_grow(void) {
    size_t new_cap = table_cap ? table_cap * 2 : 256;
    Slot *new_table = calloc(new_cap, sizeof(Slot));
    for (size_t i = 0; i < table_cap; i++) {
        if (table[i].occupied) {
            Slot *dst = table_find_slot(new_table, new_cap, table[i].key);
            *dst = table[i];
        }
    }
    free(table);
    table = new_table;
    table_cap = new_cap;
}

static void table_put(uint32_t cp, Glyph g) {
    // grow at ~70% load factor
    if (table_cap == 0 || (table_count + 1) * 10 >= table_cap * 7) table_grow();
    Slot *s = table_find_slot(table, table_cap, cp);
    if (!s->occupied) {
        s->occupied = true;
        table_count++;
    }
    s->key = cp;
    s->val = g;
}

static bool table_get(uint32_t cp, Glyph *out) {
    if (table_cap == 0) return false;
    Slot *s = table_find_slot(table, table_cap, cp);
    if (s->occupied) {
        *out = s->val;
        return true;
    }
    return false;
}

// Unload every cached texture and free the table (used by deinit and reset).
static void table_clear_unload(void) {
    for (size_t i = 0; i < table_cap; i++) {
        if (table[i].occupied && table[i].val.has) SDL_DestroyTexture(table[i].val.tex);
    }
    free(table);
    table = NULL;
    table_cap = 0;
    table_count = 0;
}

void glyphs_init(const unsigned char *data, size_t data_len, int px, SDL_Renderer *renderer) {
    font_data = data;
    font_data_len = data_len;
    raster_px = px;
    gl_renderer = renderer;
    stb_font_ready = false;
    if (font_data != NULL && font_data_len > 0) {
        int offset = stbtt_GetFontOffsetForIndex(font_data, 0);
        if (offset >= 0 && stbtt_InitFont(&stb_font, font_data, offset)) stb_font_ready = true;
    }
}

void glyphs_deinit(void) {
    table_clear_unload();
}

void glyphs_reset(int px) {
    table_clear_unload();
    raster_px = px;
}

static Glyph rasterize(uint32_t cp) {
    Glyph g = { 0 };
    g.cells = 1;
    if (!stb_font_ready) return g;

    float scale = stbtt_ScaleForPixelHeight(&stb_font, (float)raster_px);
    int advance_width = 0, left_bearing = 0;
    stbtt_GetCodepointHMetrics(&stb_font, (int)cp, &advance_width, &left_bearing);
    float advance_px = (float)advance_width * scale;
    // Unifont's advance is ~half the em for half-width glyphs, a full em
    // for full-width ones. Split at three-quarters to classify robustly.
    g.cells = (advance_px * 4 >= (float)raster_px * 3) ? 2 : 1;

    if (gl_renderer == NULL) return g;

    int w = 0, h = 0, xoff = 0, yoff = 0;
    // stbtt's own rasterizer: the same one raylib's FONT_BITMAP path already
    // used internally, so Unifont's pixel-crisp look at 16px-multiple sizes
    // carries over unchanged -- the AA coverage naturally lands on 0/255
    // when the glyph outline sits on pixel boundaries.
    unsigned char *bitmap = stbtt_GetCodepointBitmap(&stb_font, scale, scale, (int)cp, &w, &h, &xoff, &yoff);
    if (bitmap != NULL && w > 0 && h > 0) {
        // stb's bitmap is single-channel coverage. Repack as white RGB +
        // coverage-alpha so the background is transparent and platformDraw
        // Texture's color mod tints the glyph.
        size_t n = (size_t)w * (size_t)h;
        unsigned char *rgba = malloc(n * 4);
        if (rgba != NULL) {
            for (size_t p = 0; p < n; p++) {
                rgba[p * 4 + 0] = 255;
                rgba[p * 4 + 1] = 255;
                rgba[p * 4 + 2] = 255;
                rgba[p * 4 + 3] = bitmap[p];
            }
            SDL_Texture *tex = SDL_CreateTexture(gl_renderer, SDL_PIXELFORMAT_RGBA32,
                                                  SDL_TEXTUREACCESS_STATIC, w, h);
            if (tex != NULL) {
                SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
                SDL_UpdateTexture(tex, NULL, rgba, w * 4);
                g.tex = tex;
                g.has = true;
                g.ox = (float)xoff;
                g.oy = (float)yoff;
            }
            free(rgba);
        }
        stbtt_FreeBitmap(bitmap, NULL);
    }
    return g;
}

Glyph glyphs_get(uint32_t cp) {
    Glyph g;
    if (table_get(cp, &g)) return g;
    g = rasterize(cp);
    table_put(cp, g);
    return g;
}

uint8_t glyphs_cells(uint32_t cp) {
    return glyphs_get(cp).cells;
}

float glyphs_advance(uint32_t cp) {
    if (!stb_font_ready) return (float)raster_px / 2.0f;
    float scale = stbtt_ScaleForPixelHeight(&stb_font, (float)raster_px);
    int advance_width = 0, left_bearing = 0;
    stbtt_GetCodepointHMetrics(&stb_font, (int)cp, &advance_width, &left_bearing);
    return (float)advance_width * scale;
}
