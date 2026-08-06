#include "glyphs.h"

#include <stdlib.h>
#include <string.h>

// Font bytes handed in by glyphs_init (owned by main.c; loaded from disk at
// startup instead of being embedded into the binary).
static const unsigned char *font_data = NULL;
static size_t font_data_len = 0;
static int raster_px = 16;

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
        if (table[i].occupied && table[i].val.has) UnloadTexture(table[i].val.tex);
    }
    free(table);
    table = NULL;
    table_cap = 0;
    table_count = 0;
}

void glyphs_init(const unsigned char *data, size_t data_len, int px) {
    font_data = data;
    font_data_len = data_len;
    raster_px = px;
}

void glyphs_deinit(void) {
    table_clear_unload();
}

void glyphs_reset(int px) {
    table_clear_unload();
    raster_px = px;
}

static Glyph rasterize(uint32_t cp) {
    int cps[1] = { (int)cp };
    int n = 0;
    // FONT_BITMAP: no anti-aliasing, so Unifont's pixels stay crisp.
    GlyphInfo *gi = LoadFontData(font_data, (int)font_data_len, raster_px, cps, 1, FONT_BITMAP, &n);
    Glyph g = { 0 };
    g.cells = 1;
    if (gi != NULL && n > 0) {
        GlyphInfo info = gi[0];
        // Unifont's advance is ~half the em for half-width glyphs, a full em
        // for full-width ones. Split at three-quarters to classify robustly.
        g.cells = (info.advanceX * 4 >= raster_px * 3) ? 2 : 1;
        if (info.image.width > 0 && info.image.height > 0 && info.image.data != NULL) {
            // LoadFontData returns GRAYSCALE (coverage in one channel), which
            // uploads as an opaque texture -- a black box behind the glyph.
            // Repack as GRAY_ALPHA (white, coverage-in-alpha), matching the
            // atlas, so the background is transparent and the tint colors
            // the glyph.
            size_t w = (size_t)info.image.width;
            size_t h = (size_t)info.image.height;
            const unsigned char *src = (const unsigned char *)info.image.data;
            unsigned char *ga = malloc(w * h * 2);
            if (ga != NULL) {
                for (size_t p = 0; p < w * h; p++) {
                    ga[p * 2] = 255;     // luminance
                    ga[p * 2 + 1] = src[p]; // alpha = coverage
                }
                Image img;
                img.data = ga;
                img.width = info.image.width;
                img.height = info.image.height;
                img.mipmaps = 1;
                img.format = PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA;
                g.tex = LoadTextureFromImage(img);
                SetTextureFilter(g.tex, TEXTURE_FILTER_POINT);
                g.has = true;
                g.ox = (float)info.offsetX;
                g.oy = (float)info.offsetY;
                free(ga);
            }
        }
        UnloadFontData(gi, n);
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
