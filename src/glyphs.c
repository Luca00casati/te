#include "glyphs.h"

#include <stdlib.h>
#include <string.h>

#include <GL/gl.h>

#include <ft2build.h>
#include FT_FREETYPE_H

// Font bytes handed in by glyphs_init (owned by main.c; loaded from disk at
// startup instead of being embedded into the binary).
static const unsigned char *font_data = NULL;
static size_t font_data_len = 0;
static int raster_px = 16;
static void *gl_ctx = NULL;

static FT_Library ft_library;
static bool ft_library_ready = false;
static FT_Face ft_face;
static bool ft_face_ready = false;

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
        if (table[i].occupied && table[i].val.has) glDeleteTextures(1, &table[i].val.tex.id);
    }
    free(table);
    table = NULL;
    table_cap = 0;
    table_count = 0;
}

void glyphs_init(const unsigned char *data, size_t data_len, int px, void *ctx) {
    font_data = data;
    font_data_len = data_len;
    raster_px = px;
    gl_ctx = ctx;
    ft_face_ready = false;
    if (!ft_library_ready) ft_library_ready = FT_Init_FreeType(&ft_library) == 0;
    if (ft_library_ready && font_data != NULL && font_data_len > 0) {
        if (FT_New_Memory_Face(ft_library, font_data, (FT_Long)font_data_len, 0, &ft_face) == 0) {
            if (FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)raster_px) == 0) ft_face_ready = true;
            else FT_Done_Face(ft_face);
        }
    }
}

void glyphs_deinit(void) {
    table_clear_unload();
    if (ft_face_ready) {
        FT_Done_Face(ft_face);
        ft_face_ready = false;
    }
    if (ft_library_ready) {
        FT_Done_FreeType(ft_library);
        ft_library_ready = false;
    }
}

void glyphs_reset(int px) {
    table_clear_unload();
    raster_px = px;
    if (ft_face_ready) FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)raster_px);
}

static Glyph rasterize(uint32_t cp) {
    Glyph g = { 0 };
    g.cells = 1;
    if (!ft_face_ready) return g;

    // FT_LOAD_RENDER rasterizes straight into an 8-bit coverage bitmap,
    // which keeps Unifont's pixel-crisp look at 16px-multiple sizes: the
    // coverage naturally lands on 0/255 when the glyph outline sits on
    // pixel boundaries. A missing codepoint maps to glyph index 0 (.notdef).
    FT_UInt gi = FT_Get_Char_Index(ft_face, cp);
    if (FT_Load_Glyph(ft_face, gi, FT_LOAD_RENDER) != 0) return g;

    FT_GlyphSlot slot = ft_face->glyph;
    float advance_px = (float)slot->advance.x / 64.0f; // 26.6 fixed point
    // Unifont's advance is ~half the em for half-width glyphs, a full em
    // for full-width ones. Split at three-quarters to classify robustly.
    g.cells = (advance_px * 4 >= (float)raster_px * 3) ? 2 : 1;

    if (gl_ctx == NULL) return g;

    FT_Bitmap *bmp = &slot->bitmap;
    int w = (int)bmp->width, h = (int)bmp->rows;
    if (w > 0 && h > 0 && bmp->pixel_mode == FT_PIXEL_MODE_GRAY) {
        // FT's bitmap is single-channel coverage, one row per bmp->pitch
        // bytes (may include row padding). Repack as white RGB + coverage-
        // alpha so the background is transparent and platformDrawTexture's
        // color mod tints the glyph.
        size_t n = (size_t)w * (size_t)h;
        unsigned char *rgba = malloc(n * 4);
        if (rgba != NULL) {
            for (int row = 0; row < h; row++) {
                const unsigned char *src = bmp->buffer + (ptrdiff_t)row * bmp->pitch;
                for (int col = 0; col < w; col++) {
                    size_t p = (size_t)row * (size_t)w + (size_t)col;
                    rgba[p * 4 + 0] = 255;
                    rgba[p * 4 + 1] = 255;
                    rgba[p * 4 + 2] = 255;
                    rgba[p * 4 + 3] = src[col];
                }
            }
            GLuint id = 0;
            glGenTextures(1, &id);
            if (id != 0) {
                glBindTexture(GL_TEXTURE_2D, id);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                g.tex.id = id;
                g.tex.w = w;
                g.tex.h = h;
                g.has = true;
                // bitmap_left/bitmap_top are pen-relative (baseline origin,
                // up positive); flip bitmap_top to match the down-positive
                // pixel coordinates platformDrawTexture draws in.
                g.ox = (float)slot->bitmap_left;
                g.oy = -(float)slot->bitmap_top;
            }
            free(rgba);
        }
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
    if (!ft_face_ready) return (float)raster_px / 2.0f;
    FT_UInt gi = FT_Get_Char_Index(ft_face, cp);
    if (FT_Load_Glyph(ft_face, gi, FT_LOAD_DEFAULT) != 0) return (float)raster_px / 2.0f;
    return (float)ft_face->glyph->advance.x / 64.0f;
}
