// Editor configuration -- all the knobs in one place. Tweak and rebuild.
#ifndef TE_CONFIG_H
#define TE_CONFIG_H

#include "platform.h"


#define CFG_TARGET_FPS 60

#define CFG_WINDOW_TITLE "te"

// Glyph height in pixels. UnifontEX is a 16 px bitmap design, so multiples
// of 16 (16, 32, ...) stay pixel-crisp; other sizes rasterize with uneven,
// "wobbly" stems.
#define CFG_FONT_SIZE 16.0f
// Extra vertical space between lines (line height = size + line_gap).
#define CFG_FONT_LINE_GAP 4.0f

#define CFG_MARGIN_X 8.0f
#define CFG_MARGIN_Y 6.0f
// Text inserted when Tab is pressed.
#define CFG_TAB "    "

// Maximum editable file size (the text buffer is a fixed array this big).
#define CFG_MAX_FILE_BYTES (1 << 20)

// How many lines the mouse wheel scrolls per notch.
#define CFG_SCROLL_SPEED 3

// Cursor blinking.
#define CFG_CURSOR_BLINK true
#define CFG_CURSOR_BLINK_PERIOD 0.5

// How many undo steps to keep.
#define CFG_UNDO_DEPTH 4096

static inline Color cfg_rgb(unsigned char r, unsigned char g, unsigned char b) {
    Color c = { r, g, b, 255 };
    return c;
}
static inline Color cfg_rgba(unsigned char r, unsigned char g, unsigned char b, unsigned char a) {
    Color c = { r, g, b, a };
    return c;
}

#define CFG_COLOR_BG        ((Color){ 30, 30, 38, 255 })
#define CFG_COLOR_FG        ((Color){ 220, 220, 230, 255 })
#define CFG_COLOR_CURSOR    ((Color){ 120, 200, 255, 255 })
#define CFG_COLOR_SELECTION ((Color){ 58, 78, 110, 255 })
#define CFG_COLOR_GUTTER    ((Color){ 95, 95, 120, 255 })
#define CFG_COLOR_STATUS_BG ((Color){ 50, 50, 64, 255 })
#define CFG_COLOR_STATUS_FG ((Color){ 180, 200, 220, 255 })
// Dim overlay drawn behind the unsaved-changes dialog.
#define CFG_COLOR_OVERLAY   ((Color){ 0, 0, 0, 160 })

#endif // TE_CONFIG_H
