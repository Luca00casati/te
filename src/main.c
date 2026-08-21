#define _GNU_SOURCE // memmem

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

#include "raylib.h"
#include "config.h"
#include "binding.h"
#include "glyphs.h"
#include "editor.h"
#include "script.h"

// Text buffer capacity (the buffer is a fixed array this big).
#define TEXT_CAP CFG_MAX_FILE_BYTES

// --- small value types -------------------------------------------------
typedef struct { size_t line, col; } LineCol;
typedef struct { uint32_t cp; size_t len; } Cp;
typedef struct { size_t start, end; } Span;
typedef struct { float char_w, line_h, text_x0; size_t visible, visible_cols; } Metrics;

// Undo / redo: each record can reverse one edit. `removed`/`inserted` are
// malloc-owned copies of the text that left and entered the buffer.
typedef struct {
    size_t pos;
    unsigned char *removed;
    size_t removed_len;
    unsigned char *inserted;
    size_t inserted_len;
    size_t cur_before, cur_after;
} Edit;

typedef enum { MB_NONE, MB_TEXT_PROMPT, MB_CHAR_QUERY, MB_REPLACE_QUERY } MbKind;
typedef enum { MBI_NONE, MBI_FIND_FILE, MBI_WRITE_FILE, MBI_SEARCH, MBI_REPLACE_FROM, MBI_REPLACE_TO, MBI_QUIT, MBI_COMMAND } MbIntent;
typedef enum { HELP_NONE, HELP_NAV, HELP_COMMANDS } HelpKind;

// --- font (loaded from disk at runtime, next to the executable) --------
static unsigned char *font_bytes = NULL;
static size_t font_bytes_len = 0;

// The editor renders entirely with UnifontEX. The common codepoints below are
// baked into a shared texture atlas for fast batched drawing; everything else
// (CJK, emoji, rarer scripts) is rasterized on demand by glyphs.c. Cover Basic
// Latin, Latin-1/Extended, Greek, and Cyrillic, plus common typographic
// punctuation (dashes, curly quotes, ellipsis...). inAtlas() must match this set.
#define FONT_CODEPOINTS_MAX 1200
static int font_codepoints[FONT_CODEPOINTS_MAX];
static size_t font_codepoints_count = 0;

// `--screenshot <frames> <path>`: after rendering `frames` frames, save a PNG
// to `path` and quit. Handy for headless verification. 0 = disabled.
static int shot_left = 0;
static char shot_path_buf[4096];
static char *shot_path = "";

// ---------------------------------------------------------------------------
// Text buffer: a flat byte array. `cursor` is the caret; `anchor` marks the
// other end of the selection (anchor == cursor means no selection). Columns
// are measured in bytes, which is exact for ASCII and good enough here.
// ---------------------------------------------------------------------------
static unsigned char text[TEXT_CAP];
static size_t len = 0;
static size_t cursor = 0;
static size_t anchor = 0;
static bool dirty = false;

static char filename_buf[4096];
static char *filename = "untitled.txt";
static bool has_file = false; // false until associated with a real path

// View / interaction state.
static size_t top_line = 0;
static size_t left_col = 0;
// Soft wrap: long logical lines continue on the next visual row instead of
// running off the right edge. When on, horizontal scrolling (left_col) is off.
static bool wrap = true;
static size_t page_lines = 1;
// Visible text columns, refreshed each frame; used for wrap-aware vertical moves.
static size_t view_cols = 1;
// Sticky/goal column for vertical movement: the on-screen column a run of
// up/down moves tries to keep. goal_col_set == false means "not set"; any
// horizontal motion clears it.
static bool goal_col_set = false;
static size_t goal_col_val = 0;
static bool shift = false;
// True only while a plain (Shift+navigation) move is extending the selection;
// false for chorded moves like Ctrl+Shift+L, which should just move.
static bool sel_extend = false;
// Mark (C-Space): while active, movement extends the selection, Emacs-style.
static bool mark_active = false;
// Repeat count from C-<digit>: the next action runs this many times (unset = 1).
static bool repeat_count_set = false;
static size_t repeat_count_val = 0;
// Modal mode (C-m): while on, keys behave as if Ctrl is held.
static bool modal = false;
static double blink_base = 0;
static bool quit_requested = false;
static bool running = true;

// ---------------------------------------------------------------------------
// Minibuffer (Emacs-style): the bottom line. Either a prompt the user types
// into, a single-key question, or a transient echo-area message.
// ---------------------------------------------------------------------------

// True after the leader (double-tap Ctrl) until the next key resolves it: a
// chord fires a shortcut directly, a printable key opens the command-name
// prompt.
static bool prefix_pending = false;

// Ctrl double/triple-tap detection. A "tap" is Ctrl pressed then released with
// no other key in between. Two taps arm the leader; three open the command
// prompt. No timeout: the count only resets when a non-Ctrl key is pressed.
static uint8_t ctrl_taps = 0;
static bool ctrl_clean = false; // current Ctrl hold has seen no other key yet

// Frames left to drop stray GetCharPressed() events after a chord/toggle key
// (e.g. leader-S for Save, C-m for modal) resolves an action rather than
// self-inserting. GLFW/X11 can deliver that keypress's character event a
// frame or two late -- observed with IBus and similar async input methods --
// by which point the action has already resolved and a same-frame drain
// finds nothing; this keeps draining for a few more frames to still catch it.
static int swallow_char_frames = 0;

// Help overlay: the leader + n lists the direct keybindings, the leader + h
// lists the named commands. While shown it covers the editor and any
// key/click closes it.
static HelpKind help = HELP_NONE;

static MbKind mb_kind = MB_NONE;
static MbIntent mb_intent = MBI_NONE;
static const char *mb_prompt = "";
static char mb_input[4096];
static size_t mb_len = 0;
static size_t mb_cursor = 0;

static char echo_buf[256];
static size_t echo_len = 0;
static double echo_time = -100;
static char last_search[256];
static size_t last_search_len = 0;
// Search state: literal vs regex, where the search began (for abort), and the
// collected matches of the active query.
static bool search_is_regex = false;
static bool search_reverse = false; // search backward from the origin
static size_t search_origin = 0;
static bool search_bad_regex = false;
// Search & replace: the pattern is captured in the first prompt, the replacement
// in the second. `replace_all_mode` distinguishes replace-all from the
// interactive query-replace (which then loops in the replace_query minibuffer).
static bool replace_is_regex = false;
static bool replace_all_mode = false;
static char replace_from_buf[4096];
static size_t replace_from_len = 0;
static char replace_to_buf[4096];
static size_t replace_to_len = 0;
static unsigned char one_rep_buf[8192]; // scratch for a single regex substitution
#define MAX_MATCHES 8192
static size_t match_starts[MAX_MATCHES];
static uint32_t match_lens[MAX_MATCHES];
static size_t match_count = 0;
static bool match_truncated = false;
static size_t search_index = 0; // which match is currently selected (for X/N)

// ---------------------------------------------------------------------------
// Undo / redo
// ---------------------------------------------------------------------------
static Edit undo_stack[CFG_UNDO_DEPTH];
static size_t undo_n = 0;
static Edit redo_stack[CFG_UNDO_DEPTH];
static size_t redo_n = 0;

// --- forward declarations (definitions follow, mirroring main.zig's layout) -
static void noteActivity(void);
static void echo(const char *msg);
static void echoFmt(const char *fmt, ...);
static void edit(size_t start, size_t end, const unsigned char *bytes, size_t bytes_len);
static void insertBytes(const unsigned char *bytes, size_t n);
static void deleteSelection(void);
static void deleteBack(void);
static void deleteForward(void);
static void clearRedo(void);
static void pushUndo(Edit e);
static void doUndo(void);
static void doRedo(void);
static void freeHistory(void);
static size_t lineStart(size_t pos);
static size_t lineEnd(size_t pos);
static void moveVisual(int delta);
static void moveVertical(int delta);
static size_t lineCount(void);
static size_t lineStartOfRow(size_t row);
static LineCol cursorLineCol(void);
static size_t visRows(size_t line_cols, size_t cols);
static Cp decodeCp(size_t i, size_t stop);
static bool inAtlas(uint32_t cp);
static size_t cpCells(uint32_t cp);
static size_t colsIn(size_t a, size_t b);
static size_t byteAtCol(size_t start, size_t stop, size_t col);
static void copyRange(size_t a, size_t b);
static void copySelection(void);
static Span currentLineSpan(void);
static void swapLine(bool down);
static void pasteLine(void);
static void pasteClipboard(void);
static void setFilename(const char *path, size_t path_len);
static bool readFileInto(const char *path);
static void openPath(const char *path);
static bool saveFile(void);
static void saveCurrent(void);
static void computeMatches(const unsigned char *query, size_t query_len);
static void gotoMatch(size_t idx);
static void searchUpdate(void);
static const unsigned char *replacementFor(const unsigned char *matchtext, size_t matchtext_len, size_t *out_len);
static void replaceAll(const unsigned char *pattern, size_t pattern_len, const unsigned char *replacement, size_t replacement_len, bool is_regex);
static void enterReplaceQuery(void);
static void replaceStep(bool forward);
static void replaceCurrentMatch(void);
static void searchStep(bool forward);
static void startSearch(bool is_regex, bool reverse, const char *prompt);
static void startReplace(bool is_regex, bool all_mode);
static void mbStartPrompt(MbIntent intent, const char *prompt, const unsigned char *prefill, size_t prefill_len);
static void mbStartQuery(MbIntent intent, const char *prompt);
static void mbClose(void);
static void mbConfirm(void);
static void handleMinibuffer(bool ctrl);
static void afterMove(void);
static void runAction(Action action);
static void applyAction(Action action);
static bool pressed(int key);
static size_t offsetFromMouse(Metrics m);
static bool grepMode(int argc, char **argv);
static void handleInput(bool ctrl, Metrics m);
static void detectCtrlTaps(void);
static void handlePrefix(bool ctrl);
static void drawMinibuffer(Font font, float char_w, float y, char *tmp, size_t tmp_cap);
static void drawHelp(Font font, float line_h, float win_w, float win_h, char *tmp, size_t tmp_cap);

// --- small helpers -----------------------------------------------------
static bool isCont(unsigned char byte) { return (byte & 0xC0) == 0x80; }
static bool hasSel(void) { return anchor != cursor; }
static size_t selMin(void) { return anchor < cursor ? anchor : cursor; }
static size_t selMax(void) { return anchor > cursor ? anchor : cursor; }
static size_t clampz(size_t v, size_t lo, size_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
static size_t satsub(size_t a, size_t b) { return a > b ? a - b : 0; }

static void noteActivity(void) { blink_base = GetTime(); }

static void echo(const char *msg) {
    size_t mlen = strlen(msg);
    size_t m = mlen < sizeof(echo_buf) ? mlen : sizeof(echo_buf);
    memcpy(echo_buf, msg, m);
    echo_len = m;
    echo_time = GetTime();
}
static void echoFmt(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(echo_buf, sizeof(echo_buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        echo_len = 0;
        return;
    }
    echo_len = (size_t)n < sizeof(echo_buf) ? (size_t)n : sizeof(echo_buf) - 1;
    echo_time = GetTime();
}

// --- buffer mutation -----------------------------------------------------
// Replace text[start..end] with bytes[0..bytes_len) (no undo bookkeeping).
static void replaceRange(size_t start, size_t end, const unsigned char *bytes, size_t bytes_len) {
    size_t tail_len = len - end;
    size_t new_end = start + bytes_len;
    memmove(text + new_end, text + end, tail_len);
    memcpy(text + start, bytes, bytes_len);
    len = new_end + tail_len;
}

static unsigned char *dupeBytes(const unsigned char *src, size_t n) {
    unsigned char *p = malloc(n > 0 ? n : 1);
    if (!p) return NULL;
    if (n > 0) memcpy(p, src, n);
    return p;
}

// Replace text[start..end] with bytes, recording it for undo.
static void edit(size_t start, size_t end, const unsigned char *bytes, size_t bytes_len) {
    if (len - (end - start) + bytes_len > TEXT_CAP) return;
    unsigned char *removed = dupeBytes(text + start, end - start);
    if (!removed) return;
    unsigned char *inserted = dupeBytes(bytes, bytes_len);
    if (!inserted) {
        free(removed);
        return;
    }
    size_t cur_before = cursor;
    replaceRange(start, end, bytes, bytes_len);
    cursor = start + bytes_len;
    anchor = cursor;
    dirty = true;
    clearRedo();
    Edit e = { start, removed, end - start, inserted, bytes_len, cur_before, cursor };
    pushUndo(e);
    noteActivity();
}

static void insertBytes(const unsigned char *bytes, size_t n) {
    if (hasSel()) edit(selMin(), selMax(), bytes, n);
    else edit(cursor, cursor, bytes, n);
}
static void deleteSelection(void) {
    if (hasSel()) edit(selMin(), selMax(), NULL, 0);
}
static void deleteBack(void) {
    if (hasSel()) {
        deleteSelection();
        return;
    }
    if (cursor == 0) return;
    size_t start = cursor - 1;
    while (start > 0 && isCont(text[start])) start--;
    edit(start, cursor, NULL, 0);
}
static void deleteForward(void) {
    if (hasSel()) {
        deleteSelection();
        return;
    }
    if (cursor >= len) return;
    size_t end = cursor + 1;
    while (end < len && isCont(text[end])) end++;
    edit(cursor, end, NULL, 0);
}

// --- undo / redo -----------------------------------------------------------
static void freeEdit(Edit *e) {
    free(e->removed);
    free(e->inserted);
}
static void evictOldest(Edit *stack, size_t *n) {
    freeEdit(&stack[0]);
    memmove(&stack[0], &stack[1], (*n - 1) * sizeof(Edit));
    (*n)--;
}
static void pushUndo(Edit e) {
    // Coalesce a run of single-character typing into one undo step.
    if (undo_n > 0) {
        Edit *top = &undo_stack[undo_n - 1];
        if (top->removed_len == 0 && e.removed_len == 0 &&
            e.inserted_len == 1 && e.inserted[0] != '\n' &&
            e.pos == top->pos + top->inserted_len) {
            unsigned char *grown = realloc(top->inserted, top->inserted_len + 1);
            if (grown) {
                grown[top->inserted_len] = e.inserted[0];
                top->inserted = grown;
                top->inserted_len += 1;
                top->cur_after = e.cur_after;
                free(e.removed);
                free(e.inserted);
                return;
            }
        }
    }
    if (undo_n == CFG_UNDO_DEPTH) evictOldest(undo_stack, &undo_n);
    undo_stack[undo_n] = e;
    undo_n++;
}
static void pushRaw(Edit *stack, size_t *n, Edit e) {
    if (*n == CFG_UNDO_DEPTH) evictOldest(stack, n);
    stack[*n] = e;
    (*n)++;
}
static void clearRedo(void) {
    for (size_t i = 0; i < redo_n; i++) freeEdit(&redo_stack[i]);
    redo_n = 0;
}
static void doUndo(void) {
    if (undo_n == 0) {
        echo("no more undo");
        return;
    }
    undo_n--;
    Edit e = undo_stack[undo_n];
    replaceRange(e.pos, e.pos + e.inserted_len, e.removed, e.removed_len);
    cursor = e.cur_before;
    anchor = cursor;
    pushRaw(redo_stack, &redo_n, e);
    noteActivity();
    // Undone back to the start: the buffer matches where history began, so
    // treat the file as untouched again.
    if (undo_n == 0) {
        dirty = false;
        echo("no more undo");
    } else dirty = true;
}
static void doRedo(void) {
    if (redo_n == 0) return;
    redo_n--;
    Edit e = redo_stack[redo_n];
    replaceRange(e.pos, e.pos + e.removed_len, e.inserted, e.inserted_len);
    cursor = e.cur_after;
    anchor = cursor;
    dirty = true;
    pushRaw(undo_stack, &undo_n, e);
    noteActivity();
}
static void freeHistory(void) {
    for (size_t i = 0; i < undo_n; i++) freeEdit(&undo_stack[i]);
    undo_n = 0;
    clearRedo();
}

// --- cursor movement (pure caret; selection handled by caller) -------------
static void moveLeft(void) {
    if (cursor == 0) return;
    cursor--;
    while (cursor > 0 && isCont(text[cursor])) cursor--;
}
static void moveRight(void) {
    if (cursor >= len) return;
    cursor++;
    while (cursor < len && isCont(text[cursor])) cursor++;
}
// Word constituent: ASCII alnum/underscore, or any non-ASCII byte (so
// multi-byte letters count as part of a word).
static bool isWordChar(unsigned char b) {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
           (b >= '0' && b <= '9') || b == '_' || b >= 0x80;
}
// Start of the next word: finish the current word, then skip separators so
// the cursor lands on the first constituent of the following word.
static void moveWordStartRight(void) {
    while (cursor < len && isWordChar(text[cursor])) cursor++;
    while (cursor < len && !isWordChar(text[cursor])) cursor++;
}
// End of the next word: skip separators, then skip the word so the cursor
// lands just past its last constituent.
static void moveWordEndRight(void) {
    while (cursor < len && !isWordChar(text[cursor])) cursor++;
    while (cursor < len && isWordChar(text[cursor])) cursor++;
}
// Start of the previous word.
static void moveWordStartLeft(void) {
    while (cursor > 0 && !isWordChar(text[cursor - 1])) cursor--;
    while (cursor > 0 && isWordChar(text[cursor - 1])) cursor--;
}
static size_t lineStart(size_t pos) {
    size_t i = pos;
    while (i > 0 && text[i - 1] != '\n') i--;
    return i;
}
static size_t lineEnd(size_t pos) {
    size_t i = pos;
    while (i < len && text[i] != '\n') i++;
    return i;
}
static void moveHome(void) { cursor = lineStart(cursor); }
static void moveEnd(void) { cursor = lineEnd(cursor); }
static void moveVertical(int delta) {
    if (wrap) {
        moveVisual(delta);
        return;
    }
    size_t ls = lineStart(cursor);
    size_t col = goal_col_set ? goal_col_val : colsIn(ls, cursor);
    goal_col_set = true;
    goal_col_val = col;
    if (delta < 0) {
        if (ls == 0) return;
        size_t prev_start = lineStart(ls - 1);
        cursor = byteAtCol(prev_start, ls - 1, col);
    } else {
        size_t le = lineEnd(cursor);
        if (le >= len) return;
        size_t next_start = le + 1;
        cursor = byteAtCol(next_start, lineEnd(next_start), col);
    }
}
// Move one visual (wrapped) row, keeping the same on-screen column. Within a
// long logical line this steps between its segments; at a segment edge it
// crosses to the adjacent logical line's nearest row.
static void moveVisual(int delta) {
    size_t cols = view_cols > 1 ? view_cols : 1;
    size_t ls = lineStart(cursor);
    size_t le = lineEnd(cursor);
    size_t col = colsIn(ls, cursor); // display column within this logical line
    size_t sub = col / cols;         // which visual row within this logical line
    if (!goal_col_set) {
        goal_col_set = true;
        goal_col_val = col % cols;
    }
    size_t vcol = goal_col_val < cols - 1 ? goal_col_val : cols - 1; // on-screen column to preserve
    if (delta < 0) {
        if (sub > 0) {
            cursor = byteAtCol(ls, le, (sub - 1) * cols + vcol);
        } else {
            if (ls == 0) return;
            size_t prev_start = lineStart(ls - 1);
            size_t prev_cols = colsIn(prev_start, ls - 1);
            size_t last_sub = visRows(prev_cols, cols) - 1;
            cursor = byteAtCol(prev_start, ls - 1, last_sub * cols + vcol);
        }
    } else {
        size_t llen = colsIn(ls, le);
        size_t last_sub = visRows(llen, cols) - 1;
        if (sub < last_sub) {
            cursor = byteAtCol(ls, le, (sub + 1) * cols + vcol);
        } else {
            if (le >= len) return;
            size_t next_start = le + 1;
            cursor = byteAtCol(next_start, lineEnd(next_start), vcol);
        }
    }
}
static size_t lineCount(void) {
    size_t c = 1;
    for (size_t i = 0; i < len; i++)
        if (text[i] == '\n') c++;
    return c;
}
static size_t lineStartOfRow(size_t row) {
    size_t i = 0, r = 0;
    while (i < len && r < row) {
        if (text[i] == '\n') r++;
        i++;
    }
    return i;
}
static LineCol cursorLineCol(void) {
    size_t line = 0, col = 0, i = 0;
    while (i < cursor) {
        if (text[i] == '\n') {
            line++;
            col = 0;
            i++;
        } else {
            Cp d = decodeCp(i, cursor);
            col += cpCells(d.cp);
            i += d.len;
        }
    }
    LineCol lc = { line, col };
    return lc;
}
// --- soft wrap -------------------------------------------------------------
// Number of visual rows a logical line of `line_cols` display columns occupies
// at `cols` columns (>= 1, so an empty line still takes one row).
static size_t visRows(size_t line_cols, size_t cols) {
    size_t v = (line_cols + cols - 1) / cols;
    return v > 1 ? v : 1;
}
// Decode one UTF-8 codepoint at text[i]; on malformed bytes fall back to a
// single byte so the editor never gets stuck mid-buffer.
static size_t utf8SeqLen(unsigned char b) {
    if ((b & 0x80) == 0) return 1;
    if ((b & 0xE0) == 0xC0) return 2;
    if ((b & 0xF0) == 0xE0) return 3;
    if ((b & 0xF8) == 0xF0) return 4;
    return 0; // invalid leading byte
}
static Cp decodeCp(size_t i, size_t stop) {
    static const uint32_t min_for_len[5] = { 0, 0, 0x80, 0x800, 0x10000 };
    unsigned char b = text[i];
    Cp fallback = { b, 1 };
    if (b < 0x80) return fallback;
    size_t n = utf8SeqLen(b);
    if (n < 2 || i + n > stop) return fallback;
    uint32_t cp = (n == 2) ? (b & 0x1F) : (n == 3) ? (b & 0x0F) : (b & 0x07);
    for (size_t k = 1; k < n; k++) {
        unsigned char c = text[i + k];
        if ((c & 0xC0) != 0x80) return fallback;
        cp = (cp << 6) | (c & 0x3F);
    }
    if (cp < min_for_len[n] || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) return fallback;
    Cp out = { cp, n };
    return out;
}
static size_t utf8Encode(uint32_t cp, unsigned char *out) {
    if (cp <= 0x7F) {
        out[0] = (unsigned char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        out[0] = (unsigned char)(0xC0 | (cp >> 6));
        out[1] = (unsigned char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;
        out[0] = (unsigned char)(0xE0 | (cp >> 12));
        out[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (unsigned char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        out[0] = (unsigned char)(0xF0 | (cp >> 18));
        out[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (unsigned char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}
// Whether `cp` is baked into the shared atlas (fast batched path). Must match
// the ranges built by buildFontCodepoints().
static bool inAtlas(uint32_t cp) {
    return (cp >= 0x20 && cp <= 0x24F) ||
           (cp >= 0x370 && cp <= 0x3FF) ||
           (cp >= 0x400 && cp <= 0x4FF) ||
           (cp >= 0x2010 && cp <= 0x2026) ||
           cp == 0x2030 || cp == 0x2039 || cp == 0x203A || cp == 0x20AC || cp == 0x2122;
}
// Display width of `cp` in cells. Atlas glyphs are half-width; anything else
// asks Unifont (full-width CJK/emoji occupy two cells).
static size_t cpCells(uint32_t cp) {
    if (cp < 0x20) return 1;
    if (inAtlas(cp)) return 1;
    return glyphs_cells(cp);
}
// Sum of display columns in text[a..b].
static size_t colsIn(size_t a, size_t b) {
    size_t n = 0, i = a;
    while (i < b) {
        Cp d = decodeCp(i, b);
        n += cpCells(d.cp);
        i += d.len;
    }
    return n;
}
// Byte offset of the `col`-th display column within [start, stop). A
// full-width glyph that would straddle `col` is not split: the offset lands
// just before it.
static size_t byteAtCol(size_t start, size_t stop, size_t col) {
    size_t i = start, c = 0;
    while (i < stop && c < col) {
        Cp d = decodeCp(i, stop);
        size_t w = cpCells(d.cp);
        if (c + w > col) break;
        c += w;
        i += d.len;
    }
    return i;
}

// Draw text[s..e] from pixel (x0, y): consecutive atlas codepoints are batched
// into one DrawTextEx run, while each fall-back glyph (CJK, emoji, rarer
// scripts) is blitted from the lazy Unifont cache into its cell(s).
static unsigned char draw_tmp[8192];
static void flushRun(Font font, float fsize, float sp, size_t a, size_t b, float x, float y, Color color) {
    if (b <= a) return;
    size_t n = (b - a) < (sizeof(draw_tmp) - 1) ? (b - a) : (sizeof(draw_tmp) - 1);
    memcpy(draw_tmp, text + a, n);
    draw_tmp[n] = 0;
    DrawTextEx(font, (const char *)draw_tmp, (Vector2){ x, y }, fsize, sp, color);
}
static void drawCells(Font font, float cw, float fsize, float sp, float x0, float y, size_t s, size_t e, Color color) {
    float x = x0;
    size_t i = s, run_start = s;
    float run_x = x0;
    while (i < e) {
        Cp d = decodeCp(i, e);
        if (d.cp < 0x20 || inAtlas(d.cp)) {
            x += cw;
            i += d.len;
            continue;
        }
        flushRun(font, fsize, sp, run_start, i, run_x, y, color);
        Glyph g = glyphs_get(d.cp);
        if (g.has) DrawTextureV(g.tex, (Vector2){ x + g.ox, y + g.oy }, color);
        x += (float)g.cells * cw;
        i += d.len;
        run_start = i;
        run_x = x;
    }
    flushRun(font, fsize, sp, run_start, i, run_x, y, color);
}

// --- clipboard -------------------------------------------------------------
static void copyRange(size_t a, size_t b) {
    if (b <= a) return;
    size_t n = b - a;
    char *buf = malloc(n + 1);
    if (!buf) return;
    memcpy(buf, text + a, n);
    buf[n] = 0;
    SetClipboardText(buf);
    free(buf);
}
static void copySelection(void) {
    if (hasSel()) copyRange(selMin(), selMax());
}

// --- whole-line operations -------------------------------------------------
// Byte range of the current line including its trailing newline (if any).
static Span currentLineSpan(void) {
    size_t ls = lineStart(cursor);
    size_t le = lineEnd(cursor);
    Span s = { ls, le < len ? le + 1 : le };
    return s;
}
// Swap the current line with an adjacent one. `down` picks the line below,
// else above. The cursor rides along, keeping its column.
static void swapLine(bool down) {
    size_t ls = lineStart(cursor);
    size_t le = lineEnd(cursor);
    size_t col = cursor - ls;
    if (down) {
        if (le >= len) return; // last line: nothing below
        size_t ns = le + 1;
        size_t ne = lineEnd(ns);
        size_t a = le - ls; // current line length
        size_t b = ne - ns; // next line length
        unsigned char *buf = malloc(ne - ls);
        if (!buf) return;
        memcpy(buf, text + ns, b);
        buf[b] = '\n';
        memcpy(buf + b + 1, text + ls, a);
        edit(ls, ne, buf, ne - ls);
        free(buf);
        cursor = ls + b + 1 + (col < a ? col : a);
    } else {
        if (ls == 0) return; // first line: nothing above
        size_t ps = lineStart(ls - 1);
        size_t pe = ls - 1; // previous line end (the newline before us)
        size_t a = pe - ps; // previous line length
        size_t b = le - ls; // current line length
        unsigned char *buf = malloc(le - ps);
        if (!buf) return;
        memcpy(buf, text + ls, b);
        buf[b] = '\n';
        memcpy(buf + b + 1, text + ps, a);
        edit(ps, le, buf, le - ps);
        free(buf);
        cursor = ps + (col < b ? col : b);
    }
    anchor = cursor;
}
// Paste the clipboard as whole line(s) above the current line.
static void pasteLine(void) {
    const char *c = GetClipboardText();
    if (c == NULL) return;
    size_t slen = strlen(c);
    if (slen == 0) return;
    size_t ls = lineStart(cursor);
    if (c[slen - 1] == '\n') {
        edit(ls, ls, (const unsigned char *)c, slen);
    } else {
        unsigned char *buf = malloc(slen + 1);
        if (!buf) return;
        memcpy(buf, c, slen);
        buf[slen] = '\n';
        edit(ls, ls, buf, slen + 1);
        free(buf);
    }
}
static void pasteClipboard(void) {
    const char *c = GetClipboardText();
    if (c == NULL) return;
    size_t slen = strlen(c);
    if (slen > 0) insertBytes((const unsigned char *)c, slen);
}

// --- file I/O --------------------------------------------------------------
static void setFilename(const char *path, size_t path_len) {
    size_t m = path_len < sizeof(filename_buf) - 1 ? path_len : sizeof(filename_buf) - 1;
    memcpy(filename_buf, path, m);
    filename_buf[m] = 0;
    filename = filename_buf;
}
static bool readFileInto(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        len = 0;
        return false;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        len = 0;
        return false;
    }
    long size = ftell(f);
    if (size < 0 || (size_t)size > TEXT_CAP) {
        fclose(f);
        len = 0;
        return false;
    }
    fseek(f, 0, SEEK_SET);
    size_t n = fread(text, 1, (size_t)size, f);
    fclose(f);
    len = n;
    return true;
}
static void openPath(const char *path) {
    freeHistory();
    setFilename(path, strlen(path));
    bool existed = readFileInto(filename);
    cursor = 0;
    anchor = 0;
    top_line = 0;
    left_col = 0;
    dirty = false;
    has_file = true;
    if (existed) echoFmt("Opened %s", filename);
    else echoFmt("(New file) %s", filename);
    scriptRunHook("post-open");
}
static bool saveFile(void) {
    FILE *f = fopen(filename, "wb");
    if (!f) return false;
    size_t written = fwrite(text, 1, len, f);
    fclose(f);
    if (written != len) return false;
    freeHistory(); // the saved buffer is the new baseline; drop undo/redo history
    dirty = false;
    has_file = true;
    scriptRunHook("post-save");
    return true;
}
static void saveCurrent(void) {
    if (saveFile()) echoFmt("Saved %s (%zu bytes)", filename, len);
    else echo("Save failed");
}

// --- search ----------------------------------------------------------------
// Collect every match of `query` into match_starts/match_lens. Literal unless
// search_is_regex, in which case query is a PCRE2 pattern.
static void computeMatches(const unsigned char *query, size_t query_len) {
    match_count = 0;
    match_truncated = false;
    search_bad_regex = false;
    if (query_len == 0) return;
    if (search_is_regex) {
        int errcode = 0;
        PCRE2_SIZE erroff = 0;
        pcre2_code *re = pcre2_compile(query, query_len, 0, &errcode, &erroff, NULL);
        if (!re) {
            search_bad_regex = true;
            return;
        }
        pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
        if (!md) {
            pcre2_code_free(re);
            return;
        }
        size_t start = 0;
        while (start <= len) {
            int rc = pcre2_match(re, text, len, start, 0, md, NULL);
            if (rc < 0) break;
            PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
            size_t ms = ov[0], me = ov[1];
            if (match_count >= MAX_MATCHES) {
                match_truncated = true;
                break;
            }
            match_starts[match_count] = ms;
            match_lens[match_count] = (uint32_t)(me - ms);
            match_count++;
            start = (me > ms) ? me : me + 1; // step past empty matches
        }
        pcre2_match_data_free(md);
        pcre2_code_free(re);
    } else {
        size_t from = 0;
        while (from <= len) {
            void *found = memmem(text + from, len - from, query, query_len);
            if (!found) break;
            size_t idx = (size_t)((unsigned char *)found - text);
            if (match_count >= MAX_MATCHES) {
                match_truncated = true;
                break;
            }
            match_starts[match_count] = idx;
            match_lens[match_count] = (uint32_t)query_len;
            match_count++;
            from = idx + query_len;
        }
    }
}
// Select match `idx`. The count is shown inline in the search prompt (the echo
// area is hidden while the prompt is open), and echoed for after it closes.
static void gotoMatch(size_t idx) {
    search_index = idx;
    anchor = match_starts[idx];
    cursor = anchor + match_lens[idx];
    goal_col_set = false;
    noteActivity();
    echoFmt("Match %zu/%zu%s", idx + 1, match_count, match_truncated ? "+" : "");
}
// The query currently being searched: the live prompt text, else the last one.
static const unsigned char *activeQueryPtr(size_t *out_len) {
    if (mb_kind == MB_TEXT_PROMPT && mb_intent == MBI_SEARCH && mb_len > 0) {
        *out_len = mb_len;
        return (const unsigned char *)mb_input;
    }
    *out_len = last_search_len;
    return (const unsigned char *)last_search;
}
// Re-run the search after the query changed and jump to the first match at or
// after where the search began (wrapping). Used for incremental search.
static void searchUpdate(void) {
    computeMatches((const unsigned char *)mb_input, mb_len);
    // No (or invalid) query, or nothing found: don't leave the caret on a stale
    // partial match -- return to where the search began. The prompt shows why.
    if (mb_len == 0 || search_bad_regex || match_count == 0) {
        anchor = search_origin;
        cursor = search_origin;
        return;
    }
    size_t idx = 0;
    if (search_reverse) {
        idx = match_count - 1; // wrap to the last match
        size_t j = match_count;
        while (j > 0) {
            j--;
            if (match_starts[j] < search_origin) {
                idx = j;
                break;
            }
        }
    } else {
        for (size_t i = 0; i < match_count; i++) {
            if (match_starts[i] >= search_origin) {
                idx = i;
                break;
            }
        }
    }
    gotoMatch(idx);
}
// The replacement text for one match. For literal replace it's the replacement
// string verbatim; for regex it's replace_to applied to the matched text (so
// $1, $0 etc. expand). Returns NULL on failure.
static const unsigned char *replacementFor(const unsigned char *matchtext, size_t matchtext_len, size_t *out_len) {
    if (!replace_is_regex) {
        *out_len = replace_to_len;
        return (const unsigned char *)replace_to_buf;
    }
    int ec = 0;
    PCRE2_SIZE eo = 0;
    pcre2_code *re = pcre2_compile((const unsigned char *)replace_from_buf, replace_from_len, 0, &ec, &eo, NULL);
    if (!re) return NULL;
    PCRE2_SIZE outlen = sizeof(one_rep_buf);
    int rc = pcre2_substitute(re, matchtext, matchtext_len, 0, PCRE2_SUBSTITUTE_OVERFLOW_LENGTH, NULL, NULL,
                               (const unsigned char *)replace_to_buf, replace_to_len,
                               one_rep_buf, &outlen);
    pcre2_code_free(re);
    if (rc < 0) return NULL;
    *out_len = outlen;
    return one_rep_buf;
}
// Replace every match of `pattern` with `replacement` in one undo step, via
// PCRE2 (literal pattern/replacement unless is_regex). Reports the count.
static void replaceAll(const unsigned char *pattern, size_t pattern_len, const unsigned char *replacement, size_t replacement_len, bool is_regex) {
    if (pattern_len == 0) return;
    search_is_regex = is_regex;
    computeMatches(pattern, pattern_len);
    if (search_bad_regex) {
        echo("Invalid pattern");
        return;
    }
    if (match_count == 0) {
        echo("No matches");
        return;
    }
    size_t count = match_count;
    size_t saved = cursor;
    // Apply from the last match to the first so earlier offsets stay valid, and
    // so each replacement is its own undo step (undo stops at each one, not the
    // whole batch).
    size_t i = match_count;
    while (i > 0) {
        i--;
        size_t ms = match_starts[i];
        size_t me = ms + match_lens[i];
        const unsigned char *rep;
        size_t rep_len;
        if (is_regex) {
            rep = replacementFor(text + ms, me - ms, &rep_len);
            if (!rep) continue;
        } else {
            rep = replacement;
            rep_len = replacement_len;
        }
        edit(ms, me, rep, rep_len);
    }
    cursor = saved < len ? saved : len;
    anchor = cursor;
    echoFmt("Replaced %zu", count);
}
// Interactive query-replace: after the pattern and replacement are entered,
// loop over matches. Enter replaces the current one and advances; n/p
// navigate; Esc quits. Enters the replace_query minibuffer, or closes it if
// nothing matches.
static void enterReplaceQuery(void) {
    search_is_regex = replace_is_regex;
    computeMatches((const unsigned char *)replace_from_buf, replace_from_len);
    if (search_bad_regex) {
        echo("Invalid pattern");
        mbClose();
        return;
    }
    if (match_count == 0) {
        echo("No matches");
        mbClose();
        return;
    }
    size_t idx = 0;
    for (size_t i = 0; i < match_count; i++) {
        if (match_starts[i] >= search_origin) {
            idx = i;
            break;
        }
    }
    gotoMatch(idx);
    mb_kind = MB_REPLACE_QUERY;
}
// Move to the next/previous match without replacing (n/p in replace mode).
static void replaceStep(bool forward) {
    if (match_count == 0) return;
    size_t idx = forward ? (search_index + 1) % match_count : (search_index + match_count - 1) % match_count;
    gotoMatch(idx);
}
// Replace the current match, then advance to the next one (past the insertion
// so the replacement text is never re-matched). Ends replace mode when none
// remain.
static void replaceCurrentMatch(void) {
    if (match_count == 0) {
        echo("Replace done");
        mbClose();
        return;
    }
    size_t ms = match_starts[search_index];
    size_t me = ms + match_lens[search_index];
    size_t rep_len;
    const unsigned char *rep = replacementFor(text + ms, me - ms, &rep_len);
    if (!rep) {
        echo("Replace failed");
        return;
    }
    edit(ms, me, rep, rep_len);
    size_t from = ms + rep_len;
    computeMatches((const unsigned char *)replace_from_buf, replace_from_len);
    for (size_t i = 0; i < match_count; i++) {
        if (match_starts[i] >= from) {
            gotoMatch(i);
            return;
        }
    }
    echo("Replace done");
    mbClose();
}
// Jump to the next/previous match of the active query (wrapping).
static void searchStep(bool forward) {
    size_t qlen;
    const unsigned char *q = activeQueryPtr(&qlen);
    computeMatches(q, qlen);
    if (search_bad_regex) {
        echo("Invalid regex");
        return;
    }
    if (match_count == 0) {
        echo("No matches");
        return;
    }
    bool have_cur = false;
    size_t cur = 0;
    for (size_t i = 0; i < match_count; i++) {
        if (match_starts[i] == anchor && cursor == match_starts[i] + match_lens[i]) {
            cur = i;
            have_cur = true;
            break;
        }
    }
    size_t idx;
    if (have_cur) {
        idx = forward ? (cur + 1) % match_count : (cur + match_count - 1) % match_count;
    } else if (forward) {
        idx = 0;
        for (size_t i = 0; i < match_count; i++) {
            if (match_starts[i] > cursor) {
                idx = i;
                break;
            }
        }
    } else {
        idx = match_count - 1;
        size_t j = match_count;
        while (j > 0) {
            j--;
            if (match_starts[j] < cursor) {
                idx = j;
                break;
            }
        }
    }
    gotoMatch(idx);
}
// Open a search prompt. If a single-line selection exists, prefill it as the
// query (search the "word" under the selection) and jump straight to the next
// (or previous, for reverse) occurrence past it.
static void startSearch(bool is_regex, bool reverse, const char *prompt) {
    search_is_regex = is_regex;
    search_reverse = reverse;
    const unsigned char *prefill = NULL;
    size_t prefill_len = 0;
    if (hasSel()) {
        size_t a = selMin(), b = selMax();
        if (b - a < 256 && memchr(text + a, '\n', b - a) == NULL) {
            prefill = text + a;
            prefill_len = b - a;
            // Look past the current selection: before it when reversing, after
            // it otherwise, so we land on the *next* match, not this one.
            search_origin = reverse ? a : b;
        } else search_origin = cursor;
    } else search_origin = cursor;
    mbStartPrompt(MBI_SEARCH, prompt, prefill, prefill_len);
    if (prefill_len > 0) searchUpdate();
}
// Open the replace pattern prompt. The pattern entry highlights matches like a
// search (incremental + C-n/C-p); after the replacement is given it either
// replaces all at once or enters the interactive query-replace loop.
static void startReplace(bool is_regex, bool all_mode) {
    replace_is_regex = is_regex;
    replace_all_mode = all_mode;
    search_is_regex = is_regex; // drives the incremental highlight
    search_reverse = false;
    search_origin = cursor;
    const char *label = all_mode
                             ? (is_regex ? "Replace all (regex): " : "Replace all: ")
                             : (is_regex ? "Replace (regex): " : "Replace: ");
    mbStartPrompt(MBI_REPLACE_FROM, label, NULL, 0);
}

// --- minibuffer ------------------------------------------------------------
static void mbStartPrompt(MbIntent intent, const char *prompt, const unsigned char *prefill, size_t prefill_len) {
    mb_kind = MB_TEXT_PROMPT;
    mb_intent = intent;
    mb_prompt = prompt;
    size_t m = prefill_len < sizeof(mb_input) - 1 ? prefill_len : sizeof(mb_input) - 1;
    if (prefill && m > 0) memcpy(mb_input, prefill, m);
    mb_len = m;
    mb_cursor = m;
    mb_input[mb_len] = 0;
}
static void mbStartQuery(MbIntent intent, const char *prompt) {
    mb_kind = MB_CHAR_QUERY;
    mb_intent = intent;
    mb_prompt = prompt;
}
static void mbClose(void) {
    mb_kind = MB_NONE;
    mb_intent = MBI_NONE;
    mb_len = 0;
    mb_cursor = 0;
}
static void mbInsert(const unsigned char *bytes, size_t n) {
    if (mb_len + n >= sizeof(mb_input)) return;
    memmove(mb_input + mb_cursor + n, mb_input + mb_cursor, mb_len - mb_cursor);
    memcpy(mb_input + mb_cursor, bytes, n);
    mb_len += n;
    mb_cursor += n;
    mb_input[mb_len] = 0;
}
static void mbBackspace(void) {
    if (mb_cursor == 0) return;
    size_t start = mb_cursor - 1;
    while (start > 0 && isCont((unsigned char)mb_input[start])) start--;
    size_t n = mb_cursor - start;
    memmove(mb_input + start, mb_input + mb_cursor, mb_len - mb_cursor);
    mb_len -= n;
    mb_cursor = start;
    mb_input[mb_len] = 0;
}
// Tab completion for the command prompt: fill in the longest common prefix
// of the matching command names (the full name when only one matches).
static void mbComplete(void) {
    if (mb_intent != MBI_COMMAND) return;
    size_t prefix_len = mb_len;
    size_t matches = 0;
    const char *lcp = "";
    size_t lcp_len = 0;
    for (size_t ci = 0; ci < COMMANDS_COUNT; ci++) {
        const char *name = COMMANDS[ci].name;
        size_t name_len = strlen(name);
        if (name_len < prefix_len || memcmp(name, mb_input, prefix_len) != 0) continue;
        if (matches == 0) {
            lcp = name;
            lcp_len = name_len;
        } else {
            size_t i = 0, m = lcp_len < name_len ? lcp_len : name_len;
            while (i < m && lcp[i] == name[i]) i++;
            lcp_len = i;
        }
        matches++;
    }
    if (matches == 0) {
        echo("No match");
        return;
    }
    size_t m = lcp_len < sizeof(mb_input) - 1 ? lcp_len : sizeof(mb_input) - 1;
    memcpy(mb_input, lcp, m);
    mb_len = m;
    mb_cursor = m;
    mb_input[mb_len] = 0;
}
static void mbConfirm(void) {
    switch (mb_intent) {
        case MBI_FIND_FILE:
            if (mb_len > 0) openPath(mb_input);
            else echo("Aborted");
            break;
        case MBI_WRITE_FILE:
            if (mb_len > 0) {
                setFilename(mb_input, mb_len);
                saveCurrent();
            } else echo("Aborted");
            break;
        case MBI_SEARCH:
            if (mb_len > 0) {
                size_t n = mb_len < sizeof(last_search) ? mb_len : sizeof(last_search);
                memcpy(last_search, mb_input, n);
                last_search_len = n;
                // Incremental search already sits on a match; keep it.
            } else if (last_search_len > 0) {
                searchStep(!search_reverse); // empty input repeats the search
            }
            break;
        case MBI_REPLACE_FROM: {
            size_t n = mb_len < sizeof(replace_from_buf) ? mb_len : sizeof(replace_from_buf);
            memcpy(replace_from_buf, mb_input, n);
            replace_from_len = n;
            mbStartPrompt(MBI_REPLACE_TO, "With: ", NULL, 0); // second step
            return;                                           // keep the minibuffer open for the replacement
        }
        case MBI_REPLACE_TO: {
            size_t n = mb_len < sizeof(replace_to_buf) ? mb_len : sizeof(replace_to_buf);
            memcpy(replace_to_buf, mb_input, n);
            replace_to_len = n;
            if (replace_all_mode) {
                replaceAll((const unsigned char *)replace_from_buf, replace_from_len,
                           (const unsigned char *)replace_to_buf, replace_to_len, replace_is_regex);
            } else {
                enterReplaceQuery(); // sets its own minibuffer state
                return;
            }
            break;
        }
        case MBI_COMMAND: {
            // Resolve the name before mbClose() so an action that opens its own
            // prompt (e.g. save -> "Write file:") isn't immediately closed.
            size_t s = 0, e = mb_len;
            while (s < e && mb_input[s] == ' ') s++;
            while (e > s && mb_input[e - 1] == ' ') e--;
            char name[4096];
            size_t nlen = e - s;
            size_t m = nlen < sizeof(name) - 1 ? nlen : sizeof(name) - 1;
            memcpy(name, mb_input + s, m);
            name[m] = 0;
            bool matched = false;
            Action act = ACTION_QUIT;
            for (size_t i = 0; i < COMMANDS_COUNT; i++) {
                if (strcmp(COMMANDS[i].name, name) == 0) {
                    act = COMMANDS[i].action;
                    matched = true;
                    break;
                }
            }
            mbClose();
            if (matched) applyAction(act);
            else echoFmt("No command: %s", name);
            return;
        }
        default: break;
    }
    mbClose();
}

static void handleMinibuffer(bool ctrl) {
    if (mb_kind == MB_CHAR_QUERY) {
        // currently only the quit question
        if (IsKeyPressed(KEY_Y) || IsKeyPressed(KEY_S)) {
            if (saveFile()) {
                running = false;
            } else {
                echo("Save failed");
                mbClose();
            }
        } else if (IsKeyPressed(KEY_N)) {
            running = false;
        } else if (IsKeyPressed(KEY_C) || IsKeyPressed(KEY_ESCAPE) ||
                   (ctrl && IsKeyPressed(KEY_G))) {
            mbClose();
            quit_requested = false;
        }
        return;
    }

    if (mb_kind == MB_REPLACE_QUERY) {
        // Interactive query-replace: Enter replaces & advances, n/p navigate.
        if (IsKeyPressed(KEY_ESCAPE) || (ctrl && IsKeyPressed(KEY_G))) {
            echo("Replace done");
            mbClose();
        } else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
            replaceCurrentMatch();
        } else if (pressed(KEY_N)) {
            replaceStep(true);
        } else if (pressed(KEY_P)) {
            replaceStep(false);
        }
        return;
    }

    // text_prompt
    bool is_search = mb_intent == MBI_SEARCH || mb_intent == MBI_REPLACE_FROM;
    if (IsKeyPressed(KEY_ESCAPE) || (ctrl && IsKeyPressed(KEY_G))) {
        if (is_search) { // abort returns to where the search began
            anchor = search_origin;
            cursor = search_origin;
        }
        echo("Aborted");
        mbClose();
        return;
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        mbConfirm();
        return;
    }
    if (IsKeyPressed(KEY_TAB)) {
        mbComplete();
        return;
    }
    // In the search/replace-pattern prompt, C-n / C-p jump between matches.
    if (is_search && ctrl) {
        if (pressed(KEY_N)) {
            searchStep(true);
            return;
        }
        if (pressed(KEY_P)) {
            searchStep(false);
            return;
        }
    }
    bool changed = false;
    if (!ctrl) {
        int cp = GetCharPressed();
        while (cp > 0) {
            unsigned char enc[4];
            size_t n = utf8Encode((uint32_t)cp, enc);
            if (n > 0) {
                mbInsert(enc, n);
                changed = true;
            }
            cp = GetCharPressed();
        }
    }
    if (pressed(KEY_BACKSPACE)) {
        mbBackspace();
        changed = true;
    }
    if (pressed(KEY_LEFT) && mb_cursor > 0) {
        mb_cursor--;
        while (mb_cursor > 0 && isCont((unsigned char)mb_input[mb_cursor])) mb_cursor--;
    }
    if (pressed(KEY_RIGHT) && mb_cursor < mb_len) {
        mb_cursor++;
        while (mb_cursor < mb_len && isCont((unsigned char)mb_input[mb_cursor])) mb_cursor++;
    }
    if (pressed(KEY_HOME)) mb_cursor = 0;
    if (pressed(KEY_END)) mb_cursor = mb_len;
    // Incremental search: re-run and jump to the nearest match as you type.
    if (changed && is_search) searchUpdate();
}

// --- action dispatch -------------------------------------------------------
static void afterMove(void) {
    if (!sel_extend) anchor = cursor;
    noteActivity();
}
// Run an action, honoring a pending C-<digit> repeat count: the action fires
// repeat_count times and the echo area reports it (e.g. "move left x3").
static void runAction(Action action) {
    size_t n = 1;
    if (repeat_count_set) n = repeat_count_val < 9999 ? repeat_count_val : 9999;
    repeat_count_set = false;
    for (size_t i = 0; i < n; i++) applyAction(action);
    if (n > 1) echoFmt("%s x%zu", ACTION_LABELS[action], n);
}
static void applyAction(Action action) {
    // Only vertical moves preserve the sticky column; everything else drops it.
    switch (action) {
        case ACTION_MOVE_UP:
        case ACTION_MOVE_DOWN:
        case ACTION_PAGE_UP:
        case ACTION_PAGE_DOWN:
            break;
        default:
            goal_col_set = false;
            break;
    }
    // Echo the committed action to the minibuffer. Actions that set their own
    // message (e.g. wrap on/off) or open a prompt run below and override this.
    echo(ACTION_LABELS[action]);
    switch (action) {
        case ACTION_NEWLINE:
            insertBytes((const unsigned char *)"\n", 1);
            break;
        case ACTION_OPEN_LINE_BELOW: {
            size_t pos = lineEnd(cursor);
            edit(pos, pos, (const unsigned char *)"\n", 1); // cursor lands at the start of the new line
            break;
        }
        case ACTION_OPEN_LINE_ABOVE: {
            size_t ls = lineStart(cursor);
            edit(ls, ls, (const unsigned char *)"\n", 1);
            cursor = ls; // move onto the fresh blank line above
            anchor = ls;
            break;
        }
        case ACTION_INDENT:
            insertBytes((const unsigned char *)CFG_TAB, strlen(CFG_TAB));
            break;
        case ACTION_DELETE_BACK: deleteBack(); break;
        case ACTION_DELETE_FORWARD: deleteForward(); break;
        case ACTION_MOVE_LEFT: moveLeft(); afterMove(); break;
        case ACTION_MOVE_RIGHT: moveRight(); afterMove(); break;
        case ACTION_MOVE_UP: moveVertical(-1); afterMove(); break;
        case ACTION_MOVE_DOWN: moveVertical(1); afterMove(); break;
        case ACTION_MOVE_HOME: moveHome(); afterMove(); break;
        case ACTION_MOVE_END: moveEnd(); afterMove(); break;
        case ACTION_MOVE_BUFFER_START: cursor = 0; afterMove(); break;
        case ACTION_MOVE_BUFFER_END: cursor = len; afterMove(); break;
        case ACTION_MOVE_WORD_START_LEFT: moveWordStartLeft(); afterMove(); break;
        case ACTION_MOVE_WORD_START_RIGHT: moveWordStartRight(); afterMove(); break;
        case ACTION_MOVE_WORD_END_RIGHT: moveWordEndRight(); afterMove(); break;
        case ACTION_PAGE_UP:
            for (size_t i = 0; i < page_lines; i++) moveVertical(-1);
            afterMove();
            break;
        case ACTION_PAGE_DOWN:
            for (size_t i = 0; i < page_lines; i++) moveVertical(1);
            afterMove();
            break;
        case ACTION_SELECT_ALL:
            anchor = 0;
            cursor = len;
            noteActivity();
            break;
        case ACTION_UNDO: doUndo(); break;
        case ACTION_REDO: doRedo(); break;
        case ACTION_COPY: copySelection(); break;
        case ACTION_CUT:
            copySelection();
            deleteSelection();
            break;
        case ACTION_PASTE: pasteClipboard(); break;
        case ACTION_MOVE_LINE_LEFT: {
            size_t ls = lineStart(cursor);
            if (ls < len && (text[ls] == ' ' || text[ls] == '\t')) {
                size_t c = cursor;
                edit(ls, ls + 1, NULL, 0);
                cursor = c > ls ? c - 1 : ls;
                anchor = cursor;
            }
            break;
        }
        case ACTION_MOVE_LINE_RIGHT: {
            size_t ls = lineStart(cursor);
            size_t c = cursor;
            edit(ls, ls, (const unsigned char *)" ", 1);
            cursor = c + 1;
            anchor = cursor;
            break;
        }
        case ACTION_MOVE_LINE_UP: swapLine(false); break;
        case ACTION_MOVE_LINE_DOWN: swapLine(true); break;
        case ACTION_CUT_LINE: {
            Span s = currentLineSpan();
            copyRange(s.start, s.end);
            edit(s.start, s.end, NULL, 0);
            break;
        }
        case ACTION_COPY_LINE: {
            Span s = currentLineSpan();
            copyRange(s.start, s.end);
            break;
        }
        case ACTION_PASTE_LINE: pasteLine(); break;
        case ACTION_SELECT_LINE: {
            Span s = currentLineSpan();
            anchor = s.start;
            cursor = s.end;
            noteActivity();
            break;
        }
        case ACTION_SAVE:
            if (has_file) saveCurrent();
            else mbStartPrompt(MBI_WRITE_FILE, "Write file: ", (const unsigned char *)filename, strlen(filename));
            break;
        case ACTION_SAVE_AS:
            mbStartPrompt(MBI_WRITE_FILE, "Write file: ", (const unsigned char *)filename, strlen(filename));
            break;
        case ACTION_OPEN: mbStartPrompt(MBI_FIND_FILE, "Find file: ", NULL, 0); break;
        case ACTION_SEARCH: startSearch(false, false, "Search: "); break;
        case ACTION_SEARCH_REGEX: startSearch(true, false, "Regex: "); break;
        case ACTION_SEARCH_REVERSE: startSearch(false, true, "Reverse search: "); break;
        case ACTION_SEARCH_REGEX_REVERSE: startSearch(true, true, "Reverse regex: "); break;
        case ACTION_REPLACE: startReplace(false, false); break;
        case ACTION_REPLACE_REGEX: startReplace(true, false); break;
        case ACTION_REPLACE_ALL: startReplace(false, true); break;
        case ACTION_REPLACE_ALL_REGEX: startReplace(true, true); break;
        case ACTION_TOGGLE_WRAP:
            wrap = !wrap;
            left_col = 0;
            echo(wrap ? "Wrap on" : "Wrap off");
            break;
        case ACTION_QUIT: quit_requested = true; break;
        default: break;
    }
}

// True on initial press and on key autorepeat.
static bool pressed(int key) { return IsKeyPressed(key) || IsKeyPressedRepeat(key); }

// --- pixel <-> text mapping ------------------------------------------------
static size_t offsetFromMouse(Metrics m) {
    Vector2 mp = GetMousePosition();
    float rf = (mp.y - CFG_MARGIN_Y) / m.line_h;
    if (rf < 0) rf = 0;
    float cf = (mp.x - m.text_x0) / m.char_w + 0.5f;
    if (cf < 0) cf = 0;
    size_t click_col = (size_t)cf;
    if (wrap) {
        // Find the logical line + segment under the clicked visual row.
        size_t target = (size_t)rf;
        size_t vrow = 0;
        size_t s = lineStartOfRow(top_line);
        for (;;) {
            size_t e = lineEnd(s);
            size_t segs = visRows(colsIn(s, e), m.visible_cols);
            if (target < vrow + segs) {
                size_t seg = target - vrow;
                size_t seg_s = byteAtCol(s, e, seg * m.visible_cols);
                size_t seg_e = byteAtCol(s, e, (seg + 1) * m.visible_cols);
                return byteAtCol(seg_s, seg_e, click_col);
            }
            vrow += segs;
            if (e >= len) return e;
            s = e + 1;
        }
    }
    size_t row = top_line + (size_t)rf;
    size_t lc = lineCount();
    size_t rrow = row >= lc ? lc - 1 : row;
    size_t start = lineStartOfRow(rrow);
    return byteAtCol(start, lineEnd(start), left_col + click_col);
}

static void outWrite(const unsigned char *b, size_t n) {
    ssize_t r = write(1, b, n);
    (void)r;
}
static void errWrite(const char *s) {
    ssize_t r = write(2, s, strlen(s));
    (void)r;
}
static void bufApp(unsigned char *buf, size_t buf_cap, size_t *n, const unsigned char *s, size_t s_len) {
    size_t room = buf_cap - *n;
    size_t m = s_len < room ? s_len : room;
    memcpy(buf + *n, s, m);
    *n += m;
}
// Read all of stdin into the text buffer (up to TEXT_CAP bytes).
static bool readStdin(void) {
    size_t total = 0;
    while (total < TEXT_CAP) {
        ssize_t n = read(0, text + total, TEXT_CAP - total);
        if (n < 0) return false;
        if (n == 0) break; // EOF
        total += (size_t)n;
    }
    len = total;
    return true;
}
// Headless grep: `te --regex <pattern> [file] [out]`. Reads `file` (or stdin
// when omitted), writes each matching line ("lineno:text") to `out` (or
// stdout), and exits. Exit 0 if any matched, 1 if none, 2 on error.
static int grep(const char *pattern, const char *input, const char *output) {
    if (!pattern) {
        errWrite("te: --regex needs a pattern\n");
        return 2;
    }
    if (input) {
        if (!readFileInto(input)) {
            errWrite("te: cannot read file\n");
            return 2;
        }
    } else if (!readStdin()) {
        errWrite("te: cannot read stdin\n");
        return 2;
    }
    search_is_regex = true;
    computeMatches((const unsigned char *)pattern, strlen(pattern));
    if (search_bad_regex) {
        errWrite("te: invalid regex\n");
        return 2;
    }
    // Collect one line per matching line. Matches are position-sorted, so a
    // running line number only moves forward.
    size_t buf_cap = 2 * TEXT_CAP;
    unsigned char *buf = malloc(buf_cap);
    if (!buf) return 2;
    size_t w = 0;
    size_t run_line = 1, run_pos = 0, last = 0;
    for (size_t mi = 0; mi < match_count; mi++) {
        size_t ms = match_starts[mi];
        while (run_pos < ms) {
            if (text[run_pos] == '\n') run_line++;
            run_pos++;
        }
        if (run_line == last) continue;
        last = run_line;
        char pre[24];
        int pn = snprintf(pre, sizeof(pre), "%zu:", run_line);
        bufApp(buf, buf_cap, &w, (const unsigned char *)pre, pn > 0 ? (size_t)pn : 0);
        size_t ls = lineStart(ms), le = lineEnd(ms);
        bufApp(buf, buf_cap, &w, text + ls, le - ls);
        bufApp(buf, buf_cap, &w, (const unsigned char *)"\n", 1);
    }
    if (output) {
        FILE *f = fopen(output, "wb");
        if (!f || fwrite(buf, 1, w, f) != w) {
            if (f) fclose(f);
            errWrite("te: cannot write output\n");
            free(buf);
            return 2;
        }
        fclose(f);
    } else {
        outWrite(buf, w);
    }
    int result = (match_count == 0) ? 1 : 0;
    free(buf);
    return result;
}
// `te --regex <pattern> <file>` prints matches and exits. Returns false (and
// does nothing) when --regex was not given, so the caller falls through to
// GUI mode.
static bool grepMode(int argc, char **argv) {
    bool saw_regex = false;
    const char *pattern = NULL;
    const char *pos[2] = { NULL, NULL };
    size_t npos = 0;
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--regex") == 0) {
            saw_regex = true;
            i++;
            pattern = (i < argc) ? argv[i] : NULL;
        } else if (strcmp(arg, "--screenshot") == 0) {
            i += 2;
        } else if (arg[0] != 0 && arg[0] != '-') {
            if (npos < 2) pos[npos++] = arg;
        }
    }
    if (!saw_regex) return false;
    exit(grep(pattern, pos[0], pos[1]));
}

// Pure caret movement -- the only actions enabled by bare keys in modal mode.
static bool isNavAction(Action a) {
    switch (a) {
        case ACTION_MOVE_LEFT:
        case ACTION_MOVE_RIGHT:
        case ACTION_MOVE_UP:
        case ACTION_MOVE_DOWN:
        case ACTION_MOVE_HOME:
        case ACTION_MOVE_END:
        case ACTION_MOVE_BUFFER_START:
        case ACTION_MOVE_BUFFER_END:
        case ACTION_MOVE_WORD_START_LEFT:
        case ACTION_MOVE_WORD_START_RIGHT:
        case ACTION_MOVE_WORD_END_RIGHT:
        case ACTION_PAGE_UP:
        case ACTION_PAGE_DOWN:
            return true;
        default:
            return false;
    }
}

static void handleInput(bool ctrl, Metrics m) {
    // Esc clears the selection, mark, and any pending repeat count (the
    // minibuffer, when open, handles Esc itself).
    // In modal mode a bare key acts like its Ctrl-chord.
    bool cmd = ctrl || modal;
    if (IsKeyPressed(KEY_ESCAPE)) {
        anchor = cursor;
        mark_active = false;
        repeat_count_set = false;
        modal = false; // Esc also leaves modal mode
        noteActivity();
    }
    if (!cmd) {
        int cp = GetCharPressed();
        while (cp > 0) {
            unsigned char enc[4];
            size_t n = utf8Encode((uint32_t)cp, enc);
            if (n > 0) {
                // A pending C-<digit> count repeats this character (C-3 w -> www).
                size_t reps = 1;
                if (repeat_count_set) reps = repeat_count_val < 9999 ? repeat_count_val : 9999;
                repeat_count_set = false;
                for (size_t i = 0; i < reps; i++) insertBytes(enc, n);
                if (reps > 1) echoFmt("insert '%.*s' x%zu", (int)n, (const char *)enc, reps);
                goal_col_set = false;
                mark_active = false; // self-insert ends the mark (Emacs-style)
            }
            cp = GetCharPressed();
        }
    }
    // C-Enter opens a blank line below; C-Shift-Enter opens one above. Handled
    // here (not via the table) so the plain-Enter binding doesn't also fire.
    if (ctrl && (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER))) {
        runAction(shift ? ACTION_OPEN_LINE_ABOVE : ACTION_OPEN_LINE_BELOW);
        return;
    }
    // C-Space (or bare Space in modal) toggles the mark; movement then extends.
    if (cmd && IsKeyPressed(KEY_SPACE)) {
        mark_active = !mark_active;
        anchor = cursor;
        echo(mark_active ? "Mark set" : "Mark deactivated");
        noteActivity();
        return;
    }
    // C-<digit> (or bare digit in modal) accumulates a repeat count.
    if (cmd && !shift) {
        for (int d = 0; d <= 9; d++) {
            if (IsKeyPressed(KEY_ZERO + d) || IsKeyPressed(KEY_KP_0 + d)) {
                size_t cur = repeat_count_set ? repeat_count_val : 0;
                size_t nv = cur * 10 + (size_t)d;
                repeat_count_val = nv < 9999 ? nv : 9999;
                repeat_count_set = true;
                echoFmt("Repeat: %zu", repeat_count_val);
                noteActivity();
                return;
            }
        }
    }
    // Script-registered bindings (key_binding/3, from init.pl) get first look, so
    // a user script can override a built-in; if none match, fall through to
    // the built-in BINDINGS table exactly as before.
    if (!scriptHandleKey(cmd, shift)) {
        for (size_t bi = 0; bi < BINDINGS_COUNT; bi++) {
            const Binding *b = &BINDINGS[bi];
            if (!modMatchesKey(b->mod, cmd, shift)) continue;
            // Modal mode is navigation-only: bare keys move the caret and nothing
            // else, so getting around stays fluid without exposing editing or
            // destructive commands. Real Ctrl still triggers everything.
            if (modal && !ctrl && !isNavAction(b->action)) continue;
            bool hit = b->repeat ? pressed(b->key) : IsKeyPressed(b->key);
            if (hit) {
                // Extend the selection when the mark is active, or on a plain
                // Shift+navigation key (not when Shift is part of a chord).
                sel_extend = mark_active || (shift && b->mod == MOD_ANY);
                runAction(b->action);
            }
        }
    }
    float wheel = GetMouseWheelMove();
    if (wheel != 0) {
        // With wrap, lines vary in height, so just stop at the last line.
        size_t max_top = wrap ? (lineCount() - 1) : (lineCount() > m.visible ? lineCount() - m.visible : 0);
        long long nt = (long long)top_line - (long long)wheel * CFG_SCROLL_SPEED;
        if (nt < 0) nt = 0;
        if (nt > (long long)max_top) nt = (long long)max_top;
        top_line = (size_t)nt;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        cursor = offsetFromMouse(m);
        if (!shift) anchor = cursor;
        goal_col_set = false;
        noteActivity();
    } else if (IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        cursor = offsetFromMouse(m);
        goal_col_set = false;
        noteActivity();
    }
}

// Detect Ctrl double/triple taps. A tap is Ctrl pressed then released with no
// other key pressed while it was held. Two taps arm the leader; a third opens
// the command prompt. There is no timeout -- the count resets only when a
// non-Ctrl key is pressed -- so a pause between taps still gets you there.
// Runs only in normal editing mode. Draining the key-pressed queue here is
// safe: bindings use IsKeyPressed and text uses GetCharPressed, both
// independent of it.
static void detectCtrlTaps(void) {
    int lc = KEY_LEFT_CONTROL, rc = KEY_RIGHT_CONTROL;
    if (IsKeyPressed(lc) || IsKeyPressed(rc)) ctrl_clean = true;
    int k = GetKeyPressed();
    while (k != 0) {
        if (k != lc && k != rc) {
            ctrl_clean = false; // a real key was pressed with Ctrl: not a tap
            ctrl_taps = 0;      // and it breaks any tap run in progress
        }
        k = GetKeyPressed();
    }
    if (IsKeyReleased(lc) || IsKeyReleased(rc)) {
        if (ctrl_clean) {
            ctrl_taps++;
            if (ctrl_taps == 2) {
                prefix_pending = true;
                noteActivity();
            } else if (ctrl_taps >= 3) {
                ctrl_taps = 0;
                prefix_pending = false;
                mbStartPrompt(MBI_COMMAND, "Command: ", NULL, 0);
            }
        }
        ctrl_clean = false;
    }
}

// Prefix is armed: resolve the next key. Esc/C-g cancels; otherwise a chord
// fires a shortcut directly. Chords match with Ctrl optional (leader s ==
// leader C-s); Shift selects shifted variants.
static void handlePrefix(bool ctrl) {
    if (IsKeyPressed(KEY_ESCAPE) || (ctrl && IsKeyPressed(KEY_G))) {
        prefix_pending = false;
        echo("Quit");
        return;
    }
    // Script-registered leader chords (leader_binding/3, from init.pl) get
    // first look, so a user script can override a built-in chord or even the
    // help overlays below; if none match, fall through to the built-ins.
    if (scriptHandlePrefixKey(shift)) {
        prefix_pending = false;
        // See the matching drain below: swallow a same-key char event GLFW
        // may still have queued (or deliver a frame or two late under async
        // IME) so it isn't typed into the buffer or a prompt afterward.
        while (GetCharPressed() != 0) {}
        swallow_char_frames = 3;
        return;
    }
    // leader n -> key/navigation help overlay
    if (IsKeyPressed(KEY_N)) {
        prefix_pending = false;
        help = HELP_NAV;
        return;
    }
    // leader h -> commands help overlay
    if (IsKeyPressed(KEY_H)) {
        prefix_pending = false;
        help = HELP_COMMANDS;
        return;
    }
    // chord path: leader <key>, with or without Ctrl
    for (size_t i = 0; i < PREFIX_BINDINGS_COUNT; i++) {
        const Binding *b = &PREFIX_BINDINGS[i];
        if (!modMatchesChord(b->mod, shift)) continue;
        if (IsKeyPressed(b->key)) {
            prefix_pending = false;
            applyAction(b->action);
            // Some actions (e.g. quit) don't resolve until a later frame's
            // handleInput()/handleMinibuffer(), by which point GLFW may still
            // have queued a char event for this same keypress; swallow it so
            // it isn't typed into the buffer or a prompt on that later frame.
            // That event can also arrive a frame or two late in the first
            // place (async IME commit), after this drain already ran and
            // found nothing -- swallow_char_frames keeps catching it.
            while (GetCharPressed() != 0) {}
            swallow_char_frames = 3;
            return;
        }
    }
    // any other printable key is an undefined chord: cancel
    if (GetCharPressed() > 0) {
        prefix_pending = false;
        echo("Quit");
    }
}

static void drawMinibuffer(Font font, float char_w, float y, char *tmp, size_t tmp_cap) {
    float fs = CFG_FONT_SIZE, sp = CFG_FONT_SPACING;
    if (mb_kind == MB_NONE) {
        if (prefix_pending) {
            DrawTextEx(font, "Ctrlx2-", (Vector2){ CFG_MARGIN_X, y + 2 }, fs, sp, CFG_COLOR_FG);
            return;
        }
        // echo area
        if (echo_len > 0 && GetTime() - echo_time < 4.0) {
            size_t n = echo_len < tmp_cap - 1 ? echo_len : tmp_cap - 1;
            memcpy(tmp, echo_buf, n);
            tmp[n] = 0;
            DrawTextEx(font, tmp, (Vector2){ CFG_MARGIN_X, y + 2 }, fs, sp, CFG_COLOR_STATUS_FG);
        }
        return;
    }
    if (mb_kind == MB_REPLACE_QUERY) {
        char buf[128];
        int n = snprintf(buf, sizeof(buf), "Replace? Enter=yes  n/p=skip  Esc=done  (%zu/%zu)", search_index + 1, match_count);
        DrawTextEx(font, n > 0 ? buf : "Replace?", (Vector2){ CFG_MARGIN_X, y + 2 }, fs, sp, CFG_COLOR_FG);
        return;
    }
    // prompt
    DrawTextEx(font, mb_prompt, (Vector2){ CFG_MARGIN_X, y + 2 }, fs, sp, CFG_COLOR_FG);
    float prompt_w = MeasureTextEx(font, mb_prompt, fs, sp).x;
    if (mb_kind == MB_TEXT_PROMPT) {
        size_t n = mb_len < tmp_cap - 1 ? mb_len : tmp_cap - 1;
        memcpy(tmp, mb_input, n);
        tmp[n] = 0;
        float x0 = CFG_MARGIN_X + prompt_w;
        DrawTextEx(font, tmp, (Vector2){ x0, y + 2 }, fs, sp, CFG_COLOR_FG);
        // minibuffer caret (solid)
        float cx = x0 + (float)mb_cursor * char_w;
        DrawRectangleV((Vector2){ cx, y + 2 }, (Vector2){ 2, CFG_FONT_SIZE }, CFG_COLOR_CURSOR);
        // search / replace-pattern prompt: show the live match count after query
        if ((mb_intent == MBI_SEARCH || mb_intent == MBI_REPLACE_FROM) && mb_len > 0) {
            char st[64];
            if (search_bad_regex) snprintf(st, sizeof(st), "(bad regex)");
            else if (match_count == 0) snprintf(st, sizeof(st), "(no match)");
            else snprintf(st, sizeof(st), "(%zu/%zu%s)", search_index + 1, match_count, match_truncated ? "+" : "");
            float sx = x0 + (float)mb_len * char_w + 2 * char_w;
            DrawTextEx(font, st, (Vector2){ sx, y + 2 }, fs, sp, CFG_COLOR_GUTTER);
        }
        // command prompt: dim list of matching completions (Tab to fill in)
        if (mb_intent == MBI_COMMAND) {
            char hint[256];
            size_t hl = 0;
            for (size_t i = 0; i < COMMANDS_COUNT; i++) {
                const char *name = COMMANDS[i].name;
                size_t name_len = strlen(name);
                if (name_len < mb_len || memcmp(name, mb_input, mb_len) != 0) continue;
                if (hl != 0 && hl < sizeof(hint) - 1) hint[hl++] = ' ';
                size_t take = name_len < (sizeof(hint) - 1 - hl) ? name_len : (sizeof(hint) - 1 - hl);
                memcpy(hint + hl, name, take);
                hl += take;
                if (hl >= sizeof(hint) - 1) break;
            }
            hint[hl] = 0;
            float hx = x0 + (float)mb_len * char_w + 2 * char_w;
            DrawTextEx(font, hint, (Vector2){ hx, y + 2 }, fs, sp, CFG_COLOR_GUTTER);
        }
    }
}

// --- help overlay ------------------------------------------------------
static const char *modPrefix(Mod m) {
    switch (m) {
        case MOD_CTRL: return "C-";
        case MOD_CTRL_SHIFT: return "C-S-";
        default: return "";
    }
}
static const char *keyLabel(int key) {
    if (key == KEY_LEFT) return "Left";
    if (key == KEY_RIGHT) return "Right";
    if (key == KEY_UP) return "Up";
    if (key == KEY_DOWN) return "Down";
    if (key == KEY_HOME) return "Home";
    if (key == KEY_END) return "End";
    if (key == KEY_PAGE_UP) return "PgUp";
    if (key == KEY_PAGE_DOWN) return "PgDn";
    if (key == KEY_ENTER) return "Enter";
    if (key == KEY_KP_ENTER) return "KpEnter";
    if (key == KEY_TAB) return "Tab";
    if (key == KEY_BACKSPACE) return "Backspace";
    if (key == KEY_DELETE) return "Delete";
    if (key == KEY_SPACE) return "Space";
    return "";
}
// Human-readable chord like "C-S-Left" or "B" into buf; returns its length.
static size_t comboName(char *buf, const Binding *b) {
    size_t n = 0;
    const char *pfx = modPrefix(b->mod);
    size_t pfx_len = strlen(pfx);
    memcpy(buf + n, pfx, pfx_len);
    n += pfx_len;
    const char *named = keyLabel(b->key);
    size_t named_len = strlen(named);
    if (named_len > 0) {
        memcpy(buf + n, named, named_len);
        n += named_len;
    } else if (b->key >= 'A' && b->key <= 'Z') {
        buf[n++] = (char)b->key;
    } else {
        buf[n++] = '?';
    }
    return n;
}
// Distinct actions across the direct keybindings (one help row each).
static size_t navRowCount(void) {
    bool shown[ACTION_COUNT] = { 0 };
    size_t n = 0;
    for (size_t i = 0; i < BINDINGS_COUNT; i++) {
        Action a = BINDINGS[i].action;
        if (shown[a]) continue;
        shown[a] = true;
        n++;
    }
    return n;
}
// Emacs-style: the help grows upward from the echo/status area as a panel of
// lines, rather than a full-screen overlay.
static void drawHelp(Font font, float line_h, float win_w, float win_h, char *tmp, size_t tmp_cap) {
    float status_y = win_h - 2 * line_h; // top of the status line
    size_t content = (help == HELP_NAV) ? navRowCount() : (COMMANDS_COUNT + 1);
    float pad = line_h * 0.5f;
    float block_h = (float)(content + 2) * line_h + pad * 2;
    float top = status_y - block_h;
    if (top < 0) top = 0;
    DrawRectangle(0, (int)top, (int)win_w, (int)(status_y - top), CFG_COLOR_STATUS_BG);
    DrawRectangle(0, (int)top, (int)win_w, 1, CFG_COLOR_GUTTER);

    float x = CFG_MARGIN_X + 8;
    float y = status_y - block_h + pad;

    if (help == HELP_NAV) {
        DrawTextEx(font, "Navigation & editing keys  (Ctrlx2 n)", (Vector2){ x, y }, CFG_FONT_SIZE, CFG_FONT_SPACING, CFG_COLOR_CURSOR);
        y += line_h;
        bool shown[ACTION_COUNT] = { 0 };
        for (size_t bi = 0; bi < BINDINGS_COUNT; bi++) {
            Action a = BINDINGS[bi].action;
            if (shown[a]) continue;
            shown[a] = true;
            char cbuf[96];
            size_t clen = 0;
            bool first = true;
            for (size_t bj = 0; bj < BINDINGS_COUNT; bj++) {
                if (BINDINGS[bj].action != a) continue;
                if (!first) {
                    cbuf[clen] = ',';
                    cbuf[clen + 1] = ' ';
                    clen += 2;
                }
                first = false;
                char one[24];
                size_t cs = comboName(one, &BINDINGS[bj]);
                memcpy(cbuf + clen, one, cs);
                clen += cs;
            }
            const char *lab = ACTION_LABELS[a];
            int n = snprintf(tmp, tmp_cap, "%-22.*s%s", (int)clen, cbuf, lab);
            if (n < 0) continue;
            DrawTextEx(font, tmp, (Vector2){ x, y }, CFG_FONT_SIZE, CFG_FONT_SPACING, CFG_COLOR_FG);
            y += line_h;
        }
    } else {
        DrawTextEx(font, "Commands  (Ctrlx2 = double-tap Ctrl, then h)  --  then name, or chord", (Vector2){ x, y }, CFG_FONT_SIZE, CFG_FONT_SPACING, CFG_COLOR_CURSOR);
        y += line_h;
        for (size_t ci = 0; ci < COMMANDS_COUNT; ci++) {
            char chord[24] = "";
            for (size_t pi = 0; pi < PREFIX_BINDINGS_COUNT; pi++) {
                if (PREFIX_BINDINGS[pi].action != COMMANDS[ci].action) continue;
                char one[16];
                size_t cs = comboName(one, &PREFIX_BINDINGS[pi]);
                snprintf(chord, sizeof(chord), "Ctrlx2 %.*s", (int)cs, one);
                break;
            }
            int n = snprintf(tmp, tmp_cap, "%-16s%s", COMMANDS[ci].name, chord);
            if (n < 0) continue;
            DrawTextEx(font, tmp, (Vector2){ x, y }, CFG_FONT_SIZE, CFG_FONT_SPACING, CFG_COLOR_FG);
            y += line_h;
        }
        DrawTextEx(font, "Ctrlx3 (triple-tap Ctrl) : type a command", (Vector2){ x, y }, CFG_FONT_SIZE, CFG_FONT_SPACING, CFG_COLOR_GUTTER);
        y += line_h;
    }
    DrawTextEx(font, "Press any key to close", (Vector2){ x, y }, CFG_FONT_SIZE, CFG_FONT_SPACING, CFG_COLOR_GUTTER);
}

// Floor a float to size_t, treating <= 0 as 0.
static size_t floorToUsize(float v) {
    if (v <= 0) return 0;
    return (size_t)v;
}
static size_t digitCount(size_t n) {
    size_t d = 1;
    while (n >= 10) {
        n /= 10;
        d++;
    }
    return d;
}

static void buildFontCodepoints(void) {
    static const int ranges[3][2] = {
        { 0x20, 0x24F }, // Basic Latin, Latin-1 Supplement, Latin Extended-A & B
        { 0x370, 0x3FF }, // Greek and Coptic
        { 0x400, 0x4FF }, // Cyrillic
    };
    static const int extra[] = {
        0x2010, 0x2011, 0x2012, 0x2013, 0x2014, 0x2015, // hyphens & dashes
        0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, // curly quotes
        0x2020, 0x2021, 0x2022, 0x2026,                 // dagger, double dagger, bullet, ellipsis
        0x2030, 0x2039, 0x203A,                         // per mille, angle quotes
        0x20AC, 0x2122,                                 // euro, trademark
    };
    // Permanently-unassigned Greek-block slots: no font has glyphs for these, so
    // requesting them makes LoadFontData warn ("glyphs found: 972/981"). Skip them.
    static const int skip[] = { 0x378, 0x379, 0x380, 0x381, 0x382, 0x383, 0x38B, 0x38D, 0x3A2 };
    size_t n = 0;
    for (size_t r = 0; r < 3; r++) {
        for (int c = ranges[r][0]; c <= ranges[r][1]; c++) {
            bool skipit = false;
            for (size_t s = 0; s < sizeof(skip) / sizeof(skip[0]); s++) {
                if (skip[s] == c) {
                    skipit = true;
                    break;
                }
            }
            if (!skipit) font_codepoints[n++] = c;
        }
    }
    for (size_t e = 0; e < sizeof(extra) / sizeof(extra[0]); e++) font_codepoints[n++] = extra[e];
    font_codepoints_count = n;
}

// Build the shared atlas from UnifontEX at `size` px. We assemble it by hand
// (rather than LoadFontFromMemory) so we can rasterize with FONT_BITMAP -- no
// anti-aliasing -- and pair it with point filtering, keeping Unifont's pixels
// crisp instead of blurred.
static Font buildAtlasFont(int size) {
    Font f = { 0 };
    f.baseSize = size;
    f.glyphPadding = 1;
    int count = 0;
    f.glyphs = LoadFontData(font_bytes, (int)font_bytes_len, size, font_codepoints, (int)font_codepoints_count, FONT_BITMAP, &count);
    if (f.glyphs == NULL) return f;
    f.glyphCount = count;
    Rectangle *recs = NULL;
    Image atlas = GenImageFontAtlas(f.glyphs, &recs, count, size, f.glyphPadding, 0);
    f.recs = recs;
    f.texture = LoadTextureFromImage(atlas);
    UnloadImage(atlas);
    SetTextureFilter(f.texture, TEXTURE_FILTER_POINT);
    return f;
}

// Resolve UnifontExMono.ttf next to the running executable and read it whole
// into a heap buffer kept for the program's lifetime (LoadFontData/glyphs.c
// need the raw bytes, not a path).
static bool loadFontFile(void) {
    char exe_path[4096];
    ssize_t n = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);
    char font_path[4160];
    if (n > 0) {
        exe_path[n] = 0;
        char *slash = strrchr(exe_path, '/');
        if (slash) {
            size_t dir_len = (size_t)(slash - exe_path);
            memcpy(font_path, exe_path, dir_len);
            snprintf(font_path + dir_len, sizeof(font_path) - dir_len, "/UnifontExMono.ttf");
        } else {
            snprintf(font_path, sizeof(font_path), "UnifontExMono.ttf");
        }
    } else {
        snprintf(font_path, sizeof(font_path), "UnifontExMono.ttf");
    }
    FILE *f = fopen(font_path, "rb");
    if (!f) return false;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return false;
    }
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)size);
    if (!buf) {
        fclose(f);
        return false;
    }
    size_t rd = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (rd != (size_t)size) {
        free(buf);
        return false;
    }
    font_bytes = buf;
    font_bytes_len = rd;
    return true;
}

// --- editor.h: the surface exposed to src/script.c (Prolog integration) ---
void editorRunAction(Action action) { runAction(action); }
void editorApplyAction(Action action) { applyAction(action); }
void editorEcho(const char *msg) { echo(msg); }
void editorInsertText(const unsigned char *bytes, size_t n) { insertBytes(bytes, n); }
const unsigned char *editorGetText(size_t *out_len) { *out_len = len; return text; }
size_t editorGetCursor(void) { return cursor; }
void editorSetCursor(size_t pos) {
    if (pos > len) pos = len;
    cursor = pos;
    anchor = pos;
}

int main(int argc, char **argv) {
    grepMode(argc, argv); // `te --regex <pattern> <file>` prints matches and exits

    if (!loadFontFile()) {
        fprintf(stderr, "te: cannot load UnifontExMono.ttf (expected next to the executable)\n");
        return 1;
    }
    buildFontCodepoints();

    // A window has to exist before the monitor can be queried, so open a
    // hidden placeholder, size it to half the monitor's resolution, then
    // reveal it -- the user never sees the placeholder size.
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIDDEN);
    InitWindow(1, 1, CFG_WINDOW_TITLE);
    int monitor = GetCurrentMonitor();
    SetWindowSize(GetMonitorWidth(monitor) / 2, GetMonitorHeight(monitor) / 2);
    ClearWindowState(FLAG_WINDOW_HIDDEN);
    SetTargetFPS(CFG_TARGET_FPS);
    SetExitKey(0); // ESC is "cancel", not "quit"

    float font_size = CFG_FONT_SIZE; // mutable: Ctrl +/- zooms it
    float spacing = CFG_FONT_SPACING;
    Font font = buildAtlasFont((int)font_size);
    if (font.texture.id == 0) font = GetFontDefault();
    glyphs_init(font_bytes, font_bytes_len, (int)font_size);

    float char_w = MeasureTextEx(font, "M", font_size, spacing).x;
    float line_h = font_size + CFG_FONT_LINE_GAP;
    float margin_x = CFG_MARGIN_X;
    float margin_y = CFG_MARGIN_Y;
    blink_base = GetTime();

    // Loaded before the command-line file (if any) is opened, so an
    // init.pl hook(post_open, ...) also fires for it.
    scriptInit();

    // Arguments (parsed after window init so echo works): an optional file to
    // open, and `--screenshot <frames> <path>`. (--regex is handled headlessly
    // before the window opens -- see grepMode.)
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (strcmp(arg, "--screenshot") == 0) {
            if (i + 2 >= argc) break;
            const char *frames = argv[++i];
            const char *path = argv[++i];
            shot_left = atoi(frames);
            if (shot_left < 1) shot_left = 1;
            size_t m = strlen(path);
            if (m > sizeof(shot_path_buf) - 1) m = sizeof(shot_path_buf) - 1;
            memcpy(shot_path_buf, path, m);
            shot_path_buf[m] = 0;
            shot_path = shot_path_buf;
        } else if (arg[0] != 0) {
            openPath(arg);
        }
    }

    static char line_tmp[8192];
    static char status_tmp[256];
    static char num_tmp[16];
    size_t prev_cursor = 1; // force first ensure-visible

    while (running) {
        shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
        bool ctrl = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
        if (swallow_char_frames > 0) {
            swallow_char_frames--;
            while (GetCharPressed() != 0) {}
        }
        // C-m toggles modal (command) mode: bare keys run the Ctrl-key actions
        // and typing is suppressed (see handleInput). Ignored while a minibuffer
        // prompt is open so 'm' types normally there; swallow the toggling key so
        // the bare 'm' that exits modal isn't inserted as text.
        if (IsKeyPressed(KEY_M) && (ctrl || (modal && mb_kind == MB_NONE))) {
            modal = !modal;
            echo(modal ? "Modal ON (m/Esc to exit)" : "Modal OFF");
            while (GetCharPressed() != 0) {}
            swallow_char_frames = 3;
        }

        // ---- layout metrics (two bottom lines reserved: status + minibuffer) ----
        float win_w = (float)GetScreenWidth();
        float win_h = (float)GetScreenHeight();
        float status_y = win_h - 2 * line_h;
        float mb_y = win_h - line_h;
        size_t total_lines = lineCount();
        size_t digits = digitCount(total_lines);
        if (digits < 2) digits = 2;
        float gutter_w = (float)(digits + 1) * char_w;
        float text_x0 = margin_x + gutter_w;
        size_t visible = floorToUsize((win_h - margin_y - 2 * line_h) / line_h);
        if (visible < 1) visible = 1;
        size_t visible_cols = floorToUsize((win_w - text_x0) / char_w);
        if (visible_cols < 1) visible_cols = 1;
        page_lines = visible;
        view_cols = visible_cols;
        Metrics metrics = { char_w, line_h, text_x0, visible, visible_cols };

        // ---- input ----
        if (help != HELP_NONE) {
            if (GetKeyPressed() != 0 || IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) help = HELP_NONE;
        } else if (mb_kind != MB_NONE) {
            handleMinibuffer(ctrl);
        } else {
            detectCtrlTaps(); // may arm the leader or open the command prompt
            if (mb_kind != MB_NONE) {
                // command prompt just opened by a triple Ctrl tap
            } else if (prefix_pending) {
                handlePrefix(ctrl); // may itself request quit (e.g. leader Q)
            } else {
                handleInput(ctrl, metrics);
            }
            // Checked after either handler, not just handleInput(): a prefix
            // chord (leader Q) can set quit_requested too, and this must fire
            // the same frame it's requested -- otherwise the loop runs another
            // frame with handleInput() active (cmd back to false), and a
            // same-key char event GLFW queues while the key is still physically
            // held (initial or OS auto-repeat) gets typed into the buffer
            // before the quit is ever noticed.
            if (mb_kind == MB_NONE && (WindowShouldClose() || quit_requested)) {
                if (dirty) mbStartQuery(MBI_QUIT, "Save changes? (y) yes  (n) no  (c) cancel");
                else running = false;
            }
        }

        // ---- Ctrl +/- : zoom the font in 16 px steps (multiples of 16 keep
        // UnifontEX pixel-crisp). Rebuild the atlas and reset the glyph cache.
        if (help == HELP_NONE && mb_kind == MB_NONE) {
            bool zin = ctrl && (IsKeyPressed(KEY_EQUAL) || IsKeyPressed(KEY_KP_ADD));
            bool zout = ctrl && (IsKeyPressed(KEY_MINUS) || IsKeyPressed(KEY_KP_SUBTRACT));
            if (zin || zout) {
                float step = zin ? 16.0f : -16.0f;
                float ns = font_size + step;
                if (ns < 16) ns = 16;
                if (ns > 64) ns = 64;
                if (ns != font_size) {
                    font_size = ns;
                    UnloadFont(font);
                    font = buildAtlasFont((int)font_size);
                    char_w = MeasureTextEx(font, "M", font_size, spacing).x;
                    line_h = font_size + CFG_FONT_LINE_GAP;
                    glyphs_reset((int)font_size);
                    echoFmt("Font %dpx", (int)font_size);
                }
            }
        }

        // ---- scroll: follow caret only when it actually moved ----
        if (cursor != prev_cursor) {
            LineCol cp = cursorLineCol();
            if (wrap) {
                left_col = 0;
                if (cp.line < top_line) top_line = cp.line;
                // Visual rows from top_line down to the caret's own row. Walk the
                // lines once, then scroll down a line at a time (reusing the byte
                // offset) until the caret fits -- no repeated scans from the start.
                size_t s = lineStartOfRow(top_line);
                size_t used = cp.col / visible_cols;
                {
                    size_t ss = s, l = top_line;
                    while (l < cp.line) {
                        size_t e = lineEnd(ss);
                        used += visRows(colsIn(ss, e), visible_cols);
                        ss = e + 1;
                        l++;
                    }
                }
                while (used >= visible && top_line < cp.line) {
                    size_t e = lineEnd(s);
                    used -= visRows(colsIn(s, e), visible_cols);
                    s = e + 1;
                    top_line++;
                }
            } else {
                if (cp.line < top_line) top_line = cp.line;
                if (cp.line >= top_line + visible) top_line = cp.line - visible + 1;
                if (cp.col < left_col) left_col = cp.col;
                if (cp.col >= left_col + visible_cols) left_col = cp.col - visible_cols + 1;
            }
            prev_cursor = cursor;
        }

        // ---- draw ----
        BeginDrawing();
        ClearBackground(CFG_COLOR_BG);

        size_t sel_a = selMin();
        size_t sel_b = selMax();
        LineCol cp = cursorLineCol();
        bool blink_on = !CFG_CURSOR_BLINK ||
                        fmod(GetTime() - blink_base, CFG_CURSOR_BLINK_PERIOD * 2) < CFG_CURSOR_BLINK_PERIOD;
        bool show_caret = mb_kind == MB_NONE && blink_on;

        if (wrap) {
            // Walk logical lines from top_line, laying each out across one or
            // more visual rows of visible_cols columns. The caret position is
            // captured during this walk so we don't rescan the buffer for it.
            size_t caret_seg = cp.col / visible_cols;
            float caret_y = -1;
            size_t row = 0;
            size_t li = top_line;
            size_t s = lineStartOfRow(top_line);
            while (row < visible) {
                size_t e = lineEnd(s);
                size_t segs = visRows(colsIn(s, e), visible_cols);
                size_t seg = 0;
                bool stop_outer = false;
                while (seg < segs) {
                    if (row >= visible) {
                        stop_outer = true;
                        break;
                    }
                    float y = margin_y + (float)row * line_h;
                    size_t seg_s = byteAtCol(s, e, seg * visible_cols);
                    size_t seg_e = byteAtCol(s, e, (seg + 1) * visible_cols);
                    if (li == cp.line && seg == caret_seg) caret_y = y;

                    if (sel_b > sel_a) {
                        size_t a = clampz(sel_a, seg_s, seg_e);
                        size_t b = clampz(sel_b, seg_s, seg_e);
                        if (b > a) DrawRectangle(
                            (int)(text_x0 + (float)colsIn(seg_s, a) * char_w),
                            (int)y,
                            (int)((float)colsIn(a, b) * char_w),
                            (int)line_h,
                            CFG_COLOR_SELECTION);
                    }

                    if (seg == 0) {
                        int np = snprintf(num_tmp, sizeof(num_tmp), "%zu", li + 1);
                        size_t npu = np > 0 ? (size_t)np : 0;
                        float nx = margin_x + (float)(npu < digits ? digits - npu : 0) * char_w;
                        DrawTextEx(font, num_tmp, (Vector2){ nx, y }, font_size, spacing, CFG_COLOR_GUTTER);
                    }

                    drawCells(font, char_w, font_size, spacing, text_x0, y, seg_s, seg_e, CFG_COLOR_FG);
                    row++;
                    seg++;
                }
                if (stop_outer) break;
                li++;
                if (e >= len) break;
                s = e + 1;
            }

            if (show_caret && caret_y >= 0) {
                float cx = text_x0 + (float)(cp.col % visible_cols) * char_w;
                DrawRectangleV((Vector2){ cx, caret_y }, (Vector2){ 2, line_h }, CFG_COLOR_CURSOR);
            }
        } else {
            size_t li = 0, s = 0;
            for (;;) {
                size_t e = lineEnd(s);
                if (li >= top_line && li < top_line + visible) {
                    size_t row = li - top_line;
                    float y = margin_y + (float)row * line_h;

                    if (sel_b > sel_a) {
                        size_t a = clampz(sel_a, s, e);
                        size_t b = clampz(sel_b, s, e);
                        if (b > a) {
                            size_t ca = satsub(colsIn(s, a), left_col);
                            size_t cb = satsub(colsIn(s, b), left_col);
                            if (cb > ca) DrawRectangle(
                                (int)(text_x0 + (float)ca * char_w),
                                (int)y,
                                (int)((float)(cb - ca) * char_w),
                                (int)line_h,
                                CFG_COLOR_SELECTION);
                        }
                    }

                    int np = snprintf(num_tmp, sizeof(num_tmp), "%zu", li + 1);
                    size_t npu = np > 0 ? (size_t)np : 0;
                    float nx = margin_x + (float)(npu < digits ? digits - npu : 0) * char_w;
                    DrawTextEx(font, num_tmp, (Vector2){ nx, y }, font_size, spacing, CFG_COLOR_GUTTER);

                    size_t vis_start = byteAtCol(s, e, left_col);
                    drawCells(font, char_w, font_size, spacing, text_x0, y, vis_start, e, CFG_COLOR_FG);
                }
                li++;
                if (e >= len) break;
                s = e + 1;
            }

            if (show_caret && cp.line >= top_line && cp.line < top_line + visible && cp.col >= left_col) {
                size_t row = cp.line - top_line;
                float cx = text_x0 + (float)(cp.col - left_col) * char_w;
                float cy = margin_y + (float)row * line_h;
                DrawRectangleV((Vector2){ cx, cy }, (Vector2){ 2, line_h }, CFG_COLOR_CURSOR);
            }
        }

        // status (mode) line
        DrawRectangle(0, (int)status_y, (int)win_w, (int)line_h, CFG_COLOR_STATUS_BG);
        int slen = snprintf(status_tmp, sizeof(status_tmp), "%s%s%s  |  Ln %zu, Col %zu  |  %zu bytes",
                             modal ? "[MODAL]  " : "", filename, dirty ? " *" : "", cp.line + 1, cp.col + 1, len);
        DrawTextEx(font, slen > 0 ? status_tmp : "te", (Vector2){ margin_x, status_y + 2 }, font_size, spacing, CFG_COLOR_STATUS_FG);

        drawMinibuffer(font, char_w, mb_y, line_tmp, sizeof(line_tmp));

        if (help != HELP_NONE) drawHelp(font, line_h, win_w, win_h, line_tmp, sizeof(line_tmp));

        EndDrawing();
        if (shot_left > 0) {
            shot_left--;
            if (shot_left == 0) {
                TakeScreenshot(shot_path);
                running = false;
            }
        }
    }

    scriptShutdown();
    freeHistory();
    glyphs_deinit();
    UnloadFont(font);
    CloseWindow();
    free(font_bytes);
    return 0;
}
