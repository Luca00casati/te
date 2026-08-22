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

#include "platform.h"
#include "config.h"
#include "binding.h"
#include "glyphs.h"
#include "editor.h"
#include "script.h"

// Text buffer capacity (the buffer is a fixed array this big).
#define TEXT_CAP CFG_MAX_FILE_BYTES

// --- small value types -------------------------------------------------
typedef struct { size_t line, col; } LineCol;
typedef struct { uint32_t cp; size_t nbytes; } Cp;
typedef struct { float char_w, line_h, text_x0; size_t visible, visible_cols; } Metrics;

typedef enum { MB_NONE, MB_TEXT_PROMPT, MB_CHAR_QUERY, MB_REPLACE_QUERY } MbKind;
typedef enum { MBI_NONE, MBI_FIND_FILE, MBI_WRITE_FILE, MBI_SEARCH, MBI_REPLACE_FROM, MBI_REPLACE_TO, MBI_QUIT, MBI_COMMAND } MbIntent;
typedef enum { HELP_NONE, HELP_NAV, HELP_COMMANDS } HelpKind;

// --- font (loaded from disk at runtime, next to the executable) --------
static unsigned char *font_bytes = NULL;
static size_t font_bytes_len = 0;

// `--screenshot <frames> <path>`: after rendering `frames` frames, save a PNG
// to `path` and quit. Handy for headless verification. 0 = disabled.
static int shot_left = 0;
static char shot_path_buf[4096];
static char *shot_path = "";

// ---------------------------------------------------------------------------
// Buffer: one open file's content, a flat byte array (heap-allocated, TEXT_CAP
// bytes) plus its own dirty/path identity. Columns are measured in bytes,
// which is exact for ASCII and good enough here. Multiple windows may show
// the same buffer; nothing here is per-window.
// ---------------------------------------------------------------------------
// Field names are `buf_`-prefixed, distinct from the bare text/len/dirty/...
// identifiers the macros below redirect -- every one of those macros
// expands to a plain textual substitution, so an *unprefixed* field of the
// same name would also get rewritten at every other `->` access anywhere in
// this file (not just the intended selected_window->buf->... spelling),
// breaking any future code that reaches into a Buffer/Window directly (e.g.
// a buffer-list walk touching `b->dirty` on some other buffer).
typedef struct Buffer {
    int id;
    unsigned char *buf_text;
    size_t buf_len;
    bool buf_dirty;
    char buf_filename_buf[4096];
    char *buf_filename;
    bool buf_has_file; // false until associated with a real path
    struct Buffer *next; // intrusive list, creation order
} Buffer;

// ---------------------------------------------------------------------------
// Window: one on-screen pane. A leaf shows a Buffer and owns that pane's own
// view/interaction state (scroll position, wrap, cursor, selection) -- real
// Emacs semantics: point and scroll position are per-window, not per-buffer,
// so the same buffer open in two panes scrolls and selects independently.
// A split node has no buffer of its own, just two children and how the
// parent rect divides between them. Splitting/closing panes and switching
// buffers are later commits; for now there is always exactly one leaf.
// ---------------------------------------------------------------------------
typedef enum { WIN_LEAF, WIN_SPLIT_RIGHT, WIN_SPLIT_BELOW } WindowKind;

typedef struct Window {
    int id;
    WindowKind kind;
    struct Window *parent;
    // WIN_LEAF only (see the Buffer comment above for why these are
    // `win_`-prefixed rather than bare text/cursor/... names):
    Buffer *buf;
    size_t win_cursor; // the caret
    size_t win_anchor; // other end of the selection (== cursor: no selection)
    size_t win_top_line, win_left_col;
    // Soft wrap: long logical lines continue on the next visual row instead
    // of running off the right edge. When on, horizontal scrolling
    // (win_left_col) is off.
    bool win_wrap;
    size_t win_page_lines;
    // Visible text columns, refreshed each frame; used for wrap-aware
    // vertical moves.
    size_t win_view_cols;
    // WIN_SPLIT_* only:
    struct Window *a, *b; // a = left/top child, b = right/bottom child
    float split_ratio;
} Window;

static Buffer *buffer_list = NULL;
static Buffer *buffer_list_tail = NULL; // so bufferCreate can append in O(1), keeping the list in creation order
static Window *root_window = NULL;
static Window *selected_window = NULL; // receives keys; the caret blinks here
static int next_buffer_id = 1;
static int next_window_id = 1;

// Every existing bit of buffer/view logic below (rendering, mouse hit-
// testing, minibuffer, save/open, line/column math, ...) refers to "the
// buffer" and "the view" by these bare names -- redirecting them through
// the selected window/buffer here means reassigning `selected_window`
// instantly reroutes everything, with no call-site changes needed anywhere
// below. (`Cp.nbytes`, renamed from `.len` in the previous commit, is the
// only identifier below that would otherwise have collided.)
#define text          (selected_window->buf->buf_text)
#define len           (selected_window->buf->buf_len)
#define dirty         (selected_window->buf->buf_dirty)
#define filename      (selected_window->buf->buf_filename)
#define filename_buf  (selected_window->buf->buf_filename_buf)
#define has_file      (selected_window->buf->buf_has_file)
#define cursor        (selected_window->win_cursor)
#define anchor        (selected_window->win_anchor)
#define top_line      (selected_window->win_top_line)
#define left_col      (selected_window->win_left_col)
#define wrap          (selected_window->win_wrap)
#define page_lines    (selected_window->win_page_lines)
#define view_cols     (selected_window->win_view_cols)

static Buffer *bufferCreate(void) {
    Buffer *b = malloc(sizeof(Buffer));
    b->id = next_buffer_id++;
    b->buf_text = malloc(TEXT_CAP);
    b->buf_len = 0;
    b->buf_dirty = false;
    b->buf_filename_buf[0] = 0;
    b->buf_filename = "untitled.txt";
    b->buf_has_file = false;
    b->next = NULL;
    if (buffer_list_tail) buffer_list_tail->next = b; else buffer_list = b;
    buffer_list_tail = b;
    return b;
}
static Buffer *bufferFindById(int id) {
    for (Buffer *b = buffer_list; b; b = b->next)
        if (b->id == id) return b;
    return NULL;
}
static Window *windowCreateLeaf(Buffer *buf) {
    Window *w = malloc(sizeof(Window));
    w->id = next_window_id++;
    w->kind = WIN_LEAF;
    w->parent = NULL;
    w->buf = buf;
    w->win_cursor = 0;
    w->win_anchor = 0;
    w->win_top_line = 0;
    w->win_left_col = 0;
    w->win_wrap = true;
    w->win_page_lines = 1;
    w->win_view_cols = 1;
    w->a = w->b = NULL;
    w->split_ratio = 0.5f;
    return w;
}
// Creates the one starting buffer/window pair. Must run before anything
// else touches the macros above -- including headless `--regex` mode, which
// reads/writes the buffer just like the interactive path does -- so main()
// calls this first, before even grepMode().
static void bootstrapEditor(void) {
    root_window = selected_window = windowCreateLeaf(bufferCreate());
}

// Sticky/goal column for vertical movement now lives in src/movement.pl
// (goal_col_set/1, goal_col_val/1) -- a run of up/down moves tries to keep
// the same on-screen column; any other action clears it (scriptClearGoalColumn).
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

// Frames left to drop stray platformCharPressed() events after a chord/toggle key
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
// Minibuffer-adjacent search/replace state that bridges the multi-step
// prompt flow (mbConfirm) -- the actual search/replace *decision* state
// (which match is selected, session is_regex/reverse/origin, ...) lives in
// src/search.pl now (see scriptStartSearch/scriptSearchUpdate/etc.).
static char last_search[256];
static size_t last_search_len = 0;
static bool replace_is_regex = false;
static bool replace_all_mode = false;
static char replace_from_buf[4096];
static size_t replace_from_len = 0;
static char replace_to_buf[4096];
static size_t replace_to_len = 0;
static unsigned char one_rep_buf[8192]; // scratch for a single regex substitution
// Scratch space for findMatches (shared by src/search.pl's te_find_matches
// native and grep()'s headless --regex path, so the PCRE2/memmem scanning
// isn't duplicated between the two -- see editorFindMatches/editorMatchGet).
#define MAX_MATCHES 8192
static size_t match_starts[MAX_MATCHES];
static uint32_t match_lens[MAX_MATCHES];
static size_t match_count = 0;
static bool match_truncated = false;
static bool search_bad_regex = false;

// ---------------------------------------------------------------------------
// Undo / redo: the history itself (src/undo_history.pl) and the raw buffer
// splice (replaceRange) are the only pieces left in C -- see edit()/doUndo()/
// doRedo() below.
// ---------------------------------------------------------------------------

// --- forward declarations (definitions follow, mirroring main.zig's layout) -
static void noteActivity(void);
static void echo(const char *msg);
static void echoFmt(const char *fmt, ...);
static void edit(size_t start, size_t end, const unsigned char *bytes, size_t bytes_len);
static void insertBytes(const unsigned char *bytes, size_t n);
static void doUndo(void);
static void doRedo(void);
static void freeHistory(void);
static size_t lineStart(size_t pos);
static size_t lineEnd(size_t pos);
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
static void setFilename(const char *path, size_t path_len);
static bool readFileInto(const char *path);
static void openPath(const char *path);
static bool saveFile(void);
static void saveCurrent(void);
static bool findMatches(const unsigned char *query, size_t query_len, bool is_regex);
static void searchUpdate(void);
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
static void drawMinibuffer(float char_w, float y, char *tmp, size_t tmp_cap);
static void drawHelp(float char_w, float line_h, float win_w, float win_h, char *tmp, size_t tmp_cap);

// --- small helpers -----------------------------------------------------
static bool isCont(unsigned char byte) { return (byte & 0xC0) == 0x80; }
static bool hasSel(void) { return anchor != cursor; }
static size_t selMin(void) { return anchor < cursor ? anchor : cursor; }
static size_t selMax(void) { return anchor > cursor ? anchor : cursor; }
static size_t clampz(size_t v, size_t lo, size_t hi) { return v < lo ? lo : (v > hi ? hi : v); }
static size_t satsub(size_t a, size_t b) { return a > b ? a - b : 0; }

static void noteActivity(void) { blink_base = platformTime(); }

static void echo(const char *msg) {
    size_t mlen = strlen(msg);
    size_t m = mlen < sizeof(echo_buf) ? mlen : sizeof(echo_buf);
    memcpy(echo_buf, msg, m);
    echo_len = m;
    echo_time = platformTime();
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
    echo_time = platformTime();
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

// Replace text[start..end] with bytes, recording it for undo. The removed
// bytes must be captured (via scriptRecordEdit, which copies them into its
// own Prolog term representation immediately) *before* replaceRange
// overwrites them -- no local malloc'd copy needed anymore, script.c's
// src/undo_history.pl owns the history bookkeeping (coalescing a run of
// typing, evicting the oldest entry past the depth cap, clearing redo).
static void edit(size_t start, size_t end, const unsigned char *bytes, size_t bytes_len) {
    if (len - (end - start) + bytes_len > TEXT_CAP) return;
    size_t cur_before = cursor;
    size_t cur_after = start + bytes_len;
    scriptRecordEdit(start, text + start, end - start, bytes, bytes_len, cur_before, cur_after);
    replaceRange(start, end, bytes, bytes_len);
    cursor = cur_after;
    anchor = cursor;
    dirty = true;
    noteActivity();
}

static void insertBytes(const unsigned char *bytes, size_t n) {
    if (hasSel()) edit(selMin(), selMax(), bytes, n);
    else edit(cursor, cursor, bytes, n);
}
// --- undo / redo -----------------------------------------------------------
// The history (coalescing, eviction, what undo/redo actually restore) lives
// in src/undo_history.pl now; these just keep the cursor/dirty/echo
// bookkeeping that isn't history storage, mirroring the old messages exactly.
static void doUndo(void) {
    if (!scriptUndo()) {
        echo("no more undo");
        return;
    }
    noteActivity();
    // Undone back to the start: the buffer matches where history began, so
    // treat the file as untouched again.
    if (scriptUndoStackEmpty()) {
        dirty = false;
        echo("no more undo");
    } else dirty = true;
}
static void doRedo(void) {
    if (!scriptRedo()) return;
    dirty = true;
    noteActivity();
}
static void freeHistory(void) { scriptClearHistory(); }

// --- cursor movement primitives (pure: position in, position out; the
// decision of what to do with the result -- collapse/extend selection,
// goal-column bookkeeping -- lives in src/movement.pl now) ------------------
static size_t stepLeft(size_t pos) {
    if (pos == 0) return pos;
    pos--;
    while (pos > 0 && isCont(text[pos])) pos--;
    return pos;
}
static size_t stepRight(size_t pos) {
    if (pos >= len) return pos;
    pos++;
    while (pos < len && isCont(text[pos])) pos++;
    return pos;
}
// Word constituent: ASCII alnum/underscore, or any non-ASCII byte (so
// multi-byte letters count as part of a word).
static bool isWordChar(unsigned char b) {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') ||
           (b >= '0' && b <= '9') || b == '_' || b >= 0x80;
}
// Start of the next word: finish the current word, then skip separators so
// the cursor lands on the first constituent of the following word.
static size_t wordStartRight(size_t pos) {
    while (pos < len && isWordChar(text[pos])) pos++;
    while (pos < len && !isWordChar(text[pos])) pos++;
    return pos;
}
// End of the next word: skip separators, then skip the word so the cursor
// lands just past its last constituent.
static size_t wordEndRight(size_t pos) {
    while (pos < len && !isWordChar(text[pos])) pos++;
    while (pos < len && isWordChar(text[pos])) pos++;
    return pos;
}
// Start of the previous word.
static size_t wordStartLeft(size_t pos) {
    while (pos > 0 && !isWordChar(text[pos - 1])) pos--;
    while (pos > 0 && isWordChar(text[pos - 1])) pos--;
    return pos;
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
            i += d.nbytes;
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
        i += d.nbytes;
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
        i += d.nbytes;
    }
    return i;
}

// Draws one glyph at pixel (x, y) tinted `color`, returning x advanced by
// its cell width -- the one primitive drawCells/drawStr both walk with.
// Every printable codepoint goes through glyphs_get (no separate atlas
// texture/batched string draw -- platformDrawTexture has no equivalent to
// build one from, and the lazy cache already does exactly this for CJK/
// emoji today).
static float drawCp(uint32_t cp, float x, float y, float cw, Color color) {
    if (cp < 0x20) return x + cw;
    Glyph g = glyphs_get(cp);
    if (g.has) platformDrawTexture(g.tex, x + g.ox, y + g.oy, color);
    return x + (float)g.cells * cw;
}
// Draw text[s..e] from pixel (x0, y), one glyph per cell.
static void drawCells(float cw, float x0, float y, size_t s, size_t e, Color color) {
    float x = x0;
    size_t i = s;
    while (i < e) {
        Cp d = decodeCp(i, e);
        x = drawCp(d.cp, x, y, cw, color);
        i += d.nbytes;
    }
}
// Decodes one UTF-8 codepoint from a plain C string at b[i] (same leniency
// as decodeCp, just over an arbitrary buffer instead of the text[] buffer).
static Cp decodeCpStr(const unsigned char *b, size_t i, size_t n) {
    size_t seqlen = utf8SeqLen(b[i]);
    uint32_t cp = b[i];
    if (seqlen >= 2 && i + seqlen <= n) {
        cp = (seqlen == 2) ? (b[i] & 0x1F) : (seqlen == 3) ? (b[i] & 0x0F) : (b[i] & 0x07);
        bool ok = true;
        for (size_t k = 1; k < seqlen; k++) {
            if ((b[i + k] & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (b[i + k] & 0x3F);
        }
        if (!ok) { cp = b[i]; seqlen = 1; }
    } else seqlen = 1;
    Cp out = { cp, seqlen };
    return out;
}
// Draw a NUL-terminated UTF-8 C string (minibuffer/status/help labels --
// fixed text, not a text[] buffer range) from pixel (x0, y).
static void drawStr(const char *s, float cw, float x0, float y, Color color) {
    float x = x0;
    const unsigned char *b = (const unsigned char *)s;
    size_t i = 0, n = strlen(s);
    while (i < n) {
        Cp d = decodeCpStr(b, i, n);
        x = drawCp(d.cp, x, y, cw, color);
        i += d.nbytes;
    }
}
// Pixel width of a plain C string, same cell-width model as drawStr but
// without drawing -- replaces the one MeasureTextEx(font, mb_prompt, ...)
// call for positioning text after the (fixed, ASCII) minibuffer prompt.
static float strWidth(const char *s, float cw) {
    const unsigned char *b = (const unsigned char *)s;
    size_t i = 0, n = strlen(s);
    float w = 0;
    while (i < n) {
        Cp d = decodeCpStr(b, i, n);
        w += (d.cp < 0x20) ? cw : (float)cpCells(d.cp) * cw;
        i += d.nbytes;
    }
    return w;
}

// --- clipboard -------------------------------------------------------------
static void copyRange(size_t a, size_t b) {
    if (b <= a) return;
    size_t n = b - a;
    char *buf = malloc(n + 1);
    if (!buf) return;
    memcpy(buf, text + a, n);
    buf[n] = 0;
    platformSetClipboardText(buf);
    free(buf);
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
    scriptClearSearch(); // a stale match set/last-search from another file shouldn't survive
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
// is_regex, in which case query is a PCRE2 pattern. Shared by editorFindMatches
// (src/search.pl's te_find_matches/4) and grep()'s headless --regex path, so
// the PCRE2/memmem scanning logic isn't duplicated between the two. Returns
// false if the pattern failed to compile (regex only).
static bool findMatches(const unsigned char *query, size_t query_len, bool is_regex) {
    match_count = 0;
    match_truncated = false;
    search_bad_regex = false;
    if (query_len == 0) return true;
    if (is_regex) {
        int errcode = 0;
        PCRE2_SIZE erroff = 0;
        pcre2_code *re = pcre2_compile(query, query_len, 0, &errcode, &erroff, NULL);
        if (!re) {
            search_bad_regex = true;
            return false;
        }
        pcre2_match_data *md = pcre2_match_data_create_from_pattern(re, NULL);
        if (!md) {
            pcre2_code_free(re);
            return true;
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
    return true;
}
// The query currently being searched: the live prompt text, else the last
// one -- minibuffer-adjacent (mb_input/last_search), so this stays here
// rather than moving into src/search.pl.
static const unsigned char *activeQueryPtr(size_t *out_len) {
    if (mb_kind == MB_TEXT_PROMPT && mb_intent == MBI_SEARCH && mb_len > 0) {
        *out_len = mb_len;
        return (const unsigned char *)mb_input;
    }
    *out_len = last_search_len;
    return (const unsigned char *)last_search;
}
// Echoes the live "Match i/N[+]" status (or why there isn't one) from
// src/search.pl's current state -- shared by every function below that
// jumps to a match.
static void echoMatchStatus(void) {
    size_t index, count; bool truncated, bad_regex;
    scriptSearchStatus(&index, &count, &truncated, &bad_regex);
    if (bad_regex) echo("Invalid regex");
    else if (count == 0) echo("No matches");
    else echoFmt("Match %zu/%zu%s", index + 1, count, truncated ? "+" : "");
}
// Re-run the search after the query changed and jump to the first match at or
// after where the search began (wrapping). Used for incremental search.
static void searchUpdate(void) {
    scriptSearchUpdate((const unsigned char *)mb_input, mb_len);
}
// Replace every match of `pattern` with `replacement` in one undo step per
// match, via PCRE2 (literal pattern/replacement unless is_regex).
static void replaceAll(const unsigned char *pattern, size_t pattern_len, const unsigned char *replacement, size_t replacement_len, bool is_regex) {
    if (pattern_len == 0) return;
    size_t count = scriptReplaceAll(pattern, pattern_len, replacement, replacement_len, is_regex);
    size_t index, mcount; bool truncated, bad_regex;
    scriptSearchStatus(&index, &mcount, &truncated, &bad_regex);
    if (bad_regex) echo("Invalid pattern");
    else if (count == 0) echo("No matches");
    else echoFmt("Replaced %zu", count);
}
// Interactive query-replace: after the pattern and replacement are entered,
// loop over matches. Enter replaces the current one and advances; n/p
// navigate; Esc quits. Enters the replace_query minibuffer, or closes it if
// nothing matches.
static void enterReplaceQuery(void) {
    bool ok = scriptEnterReplaceQuery((const unsigned char *)replace_from_buf, replace_from_len,
                                       (const unsigned char *)replace_to_buf, replace_to_len,
                                       replace_is_regex);
    if (!ok) {
        size_t index, count; bool truncated, bad_regex;
        scriptSearchStatus(&index, &count, &truncated, &bad_regex);
        echo(bad_regex ? "Invalid pattern" : "No matches");
        mbClose();
        return;
    }
    echoMatchStatus();
    mb_kind = MB_REPLACE_QUERY;
}
// Move to the next/previous match without replacing (n/p in replace mode).
static void replaceStep(bool forward) {
    scriptReplaceStep(forward);
    echoMatchStatus();
}
// Replace the current match, then advance to the next one (past the insertion
// so the replacement text is never re-matched). Ends replace mode when none
// remain.
static void replaceCurrentMatch(void) {
    ReplaceStepResult r = scriptReplaceCurrentMatch();
    switch (r) {
        case REPLACE_STEP_OK: echoMatchStatus(); break;
        case REPLACE_STEP_FAILED: echo("Replace failed"); break;
        case REPLACE_STEP_DONE: echo("Replace done"); mbClose(); break;
    }
}
// Jump to the next/previous match of the active query (wrapping).
static void searchStep(bool forward) {
    size_t qlen;
    const unsigned char *q = activeQueryPtr(&qlen);
    scriptSearchStep(q, qlen, forward);
    echoMatchStatus();
}
// Open a search prompt. If a single-line selection exists, prefill it as the
// query (search the "word" under the selection) and jump straight to the next
// (or previous, for reverse) occurrence past it.
static void startSearch(bool is_regex, bool reverse, const char *prompt) {
    const unsigned char *prefill = NULL;
    size_t prefill_len = 0;
    size_t origin;
    if (hasSel()) {
        size_t a = selMin(), b = selMax();
        if (b - a < 256 && memchr(text + a, '\n', b - a) == NULL) {
            prefill = text + a;
            prefill_len = b - a;
            // Look past the current selection: before it when reversing, after
            // it otherwise, so we land on the *next* match, not this one.
            origin = reverse ? a : b;
        } else origin = cursor;
    } else origin = cursor;
    scriptStartSearch(is_regex, reverse, origin);
    mbStartPrompt(MBI_SEARCH, prompt, prefill, prefill_len);
    if (prefill_len > 0) searchUpdate();
}
// Open the replace pattern prompt. The pattern entry highlights matches like a
// search (incremental + C-n/C-p); after the replacement is given it either
// replaces all at once or enters the interactive query-replace loop.
static void startReplace(bool is_regex, bool all_mode) {
    replace_is_regex = is_regex;
    replace_all_mode = all_mode;
    scriptStartReplace(is_regex, all_mode, cursor); // is_regex also drives the incremental highlight
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
    size_t command_count = scriptCommandCount();
    for (size_t ci = 0; ci < command_count; ci++) {
        const char *name;
        if (!scriptCommandGet(ci, &name)) continue;
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
                searchStep(!scriptSearchReverse()); // empty input repeats the search
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
            bool matched = scriptRunCommand(name);
            mbClose();
            if (!matched) echoFmt("No command: %s", name);
            return;
        }
        default: break;
    }
    mbClose();
}

static void handleMinibuffer(bool ctrl) {
    if (mb_kind == MB_CHAR_QUERY) {
        // currently only the quit question
        if (platformKeyPressed(GLFW_KEY_Y) || platformKeyPressed(GLFW_KEY_S)) {
            if (saveFile()) {
                running = false;
            } else {
                echo("Save failed");
                mbClose();
            }
        } else if (platformKeyPressed(GLFW_KEY_N)) {
            running = false;
        } else if (platformKeyPressed(GLFW_KEY_C) || platformKeyPressed(GLFW_KEY_ESCAPE) ||
                   (ctrl && platformKeyPressed(GLFW_KEY_G))) {
            mbClose();
            quit_requested = false;
        }
        return;
    }

    if (mb_kind == MB_REPLACE_QUERY) {
        // Interactive query-replace: Enter replaces & advances, n/p navigate.
        if (platformKeyPressed(GLFW_KEY_ESCAPE) || (ctrl && platformKeyPressed(GLFW_KEY_G))) {
            echo("Replace done");
            mbClose();
        } else if (platformKeyPressed(GLFW_KEY_ENTER) || platformKeyPressed(GLFW_KEY_KP_ENTER)) {
            replaceCurrentMatch();
        } else if (pressed(GLFW_KEY_N)) {
            replaceStep(true);
        } else if (pressed(GLFW_KEY_P)) {
            replaceStep(false);
        }
        return;
    }

    // text_prompt
    bool is_search = mb_intent == MBI_SEARCH || mb_intent == MBI_REPLACE_FROM;
    if (platformKeyPressed(GLFW_KEY_ESCAPE) || (ctrl && platformKeyPressed(GLFW_KEY_G))) {
        if (is_search) { // abort returns to where the search began
            size_t origin = scriptSearchOrigin();
            anchor = origin;
            cursor = origin;
        }
        echo("Aborted");
        mbClose();
        return;
    }
    if (platformKeyPressed(GLFW_KEY_ENTER) || platformKeyPressed(GLFW_KEY_KP_ENTER)) {
        mbConfirm();
        return;
    }
    if (platformKeyPressed(GLFW_KEY_TAB)) {
        mbComplete();
        return;
    }
    // In the search/replace-pattern prompt, C-n / C-p jump between matches.
    if (is_search && ctrl) {
        if (pressed(GLFW_KEY_N)) {
            searchStep(true);
            return;
        }
        if (pressed(GLFW_KEY_P)) {
            searchStep(false);
            return;
        }
    }
    bool changed = false;
    if (!ctrl) {
        int cp = platformCharPressed();
        while (cp > 0) {
            unsigned char enc[4];
            size_t n = utf8Encode((uint32_t)cp, enc);
            if (n > 0) {
                mbInsert(enc, n);
                changed = true;
            }
            cp = platformCharPressed();
        }
    }
    if (pressed(GLFW_KEY_BACKSPACE)) {
        mbBackspace();
        changed = true;
    }
    if (pressed(GLFW_KEY_LEFT) && mb_cursor > 0) {
        mb_cursor--;
        while (mb_cursor > 0 && isCont((unsigned char)mb_input[mb_cursor])) mb_cursor--;
    }
    if (pressed(GLFW_KEY_RIGHT) && mb_cursor < mb_len) {
        mb_cursor++;
        while (mb_cursor < mb_len && isCont((unsigned char)mb_input[mb_cursor])) mb_cursor++;
    }
    if (pressed(GLFW_KEY_HOME)) mb_cursor = 0;
    if (pressed(GLFW_KEY_END)) mb_cursor = mb_len;
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
            scriptClearGoalColumn();
            break;
    }
    // Echo the committed action to the minibuffer. Actions that set their own
    // message (e.g. wrap on/off) or open a prompt run below and override this.
    echo(ACTION_LABELS[action]);
    switch (action) {
        case ACTION_NEWLINE: scriptNewline(); break;
        case ACTION_OPEN_LINE_BELOW: scriptOpenLineBelow(); break;
        case ACTION_OPEN_LINE_ABOVE: scriptOpenLineAbove(); break;
        case ACTION_INDENT: scriptIndent(); break;
        case ACTION_DELETE_BACK: scriptDeleteBack(); break;
        case ACTION_DELETE_FORWARD: scriptDeleteForward(); break;
        case ACTION_MOVE_LEFT: scriptMoveLeft(); afterMove(); break;
        case ACTION_MOVE_RIGHT: scriptMoveRight(); afterMove(); break;
        case ACTION_MOVE_UP: scriptMoveVertical(-1, wrap, view_cols); afterMove(); break;
        case ACTION_MOVE_DOWN: scriptMoveVertical(1, wrap, view_cols); afterMove(); break;
        case ACTION_MOVE_HOME: scriptMoveHome(); afterMove(); break;
        case ACTION_MOVE_END: scriptMoveEnd(); afterMove(); break;
        case ACTION_MOVE_BUFFER_START: scriptMoveBufferStart(); afterMove(); break;
        case ACTION_MOVE_BUFFER_END: scriptMoveBufferEnd(); afterMove(); break;
        case ACTION_MOVE_WORD_START_LEFT: scriptMoveWordStartLeft(); afterMove(); break;
        case ACTION_MOVE_WORD_START_RIGHT: scriptMoveWordStartRight(); afterMove(); break;
        case ACTION_MOVE_WORD_END_RIGHT: scriptMoveWordEndRight(); afterMove(); break;
        case ACTION_PAGE_UP: scriptPageUp(wrap, view_cols, page_lines); afterMove(); break;
        case ACTION_PAGE_DOWN: scriptPageDown(wrap, view_cols, page_lines); afterMove(); break;
        case ACTION_SELECT_ALL:
            scriptSelectAll();
            noteActivity();
            break;
        case ACTION_UNDO: doUndo(); break;
        case ACTION_REDO: doRedo(); break;
        case ACTION_COPY: scriptCopy(); break;
        case ACTION_CUT: scriptCut(); break;
        case ACTION_PASTE: scriptPaste(); break;
        case ACTION_MOVE_LINE_LEFT: scriptMoveLineLeft(); break;
        case ACTION_MOVE_LINE_RIGHT: scriptMoveLineRight(); break;
        case ACTION_MOVE_LINE_UP: scriptMoveLineUp(); break;
        case ACTION_MOVE_LINE_DOWN: scriptMoveLineDown(); break;
        case ACTION_CUT_LINE: scriptCutLine(); break;
        case ACTION_COPY_LINE: scriptCopyLine(); break;
        case ACTION_PASTE_LINE: scriptPasteLine(); break;
        case ACTION_SELECT_LINE: scriptSelectLine(); break;
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
static bool pressed(int key) { return platformKeyPressed(key) || platformKeyPressedRepeat(key); }

// --- pixel <-> text mapping ------------------------------------------------
static size_t offsetFromMouse(Metrics m) {
    float mx, my;
    platformMousePos(&mx, &my);
    float rf = (my - CFG_MARGIN_Y) / m.line_h;
    if (rf < 0) rf = 0;
    float cf = (mx - m.text_x0) / m.char_w + 0.5f;
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
    findMatches((const unsigned char *)pattern, strlen(pattern), /*is_regex=*/true);
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
    if (platformKeyPressed(GLFW_KEY_ESCAPE)) {
        anchor = cursor;
        mark_active = false;
        repeat_count_set = false;
        modal = false; // Esc also leaves modal mode
        noteActivity();
    }
    if (!cmd) {
        int cp = platformCharPressed();
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
                scriptClearGoalColumn();
                mark_active = false; // self-insert ends the mark (Emacs-style)
            }
            cp = platformCharPressed();
        }
    }
    // C-Enter opens a blank line below; C-Shift-Enter opens one above. Handled
    // here (not via the table) so the plain-Enter binding doesn't also fire.
    if (ctrl && (platformKeyPressed(GLFW_KEY_ENTER) || platformKeyPressed(GLFW_KEY_KP_ENTER))) {
        runAction(shift ? ACTION_OPEN_LINE_ABOVE : ACTION_OPEN_LINE_BELOW);
        return;
    }
    // C-Space (or bare Space in modal) toggles the mark; movement then extends.
    if (cmd && platformKeyPressed(GLFW_KEY_SPACE)) {
        mark_active = !mark_active;
        anchor = cursor;
        echo(mark_active ? "Mark set" : "Mark deactivated");
        noteActivity();
        return;
    }
    // C-<digit> (or bare digit in modal) accumulates a repeat count.
    // GLFW_KEY_1..9/KP_1..9 are sequential but GLFW_KEY_0/KP_0 comes
    // *after* 9, not before -- unlike raylib's KEY_ZERO..KEY_NINE, so d==0
    // needs its own scancode rather than d-based arithmetic from a zero base.
    if (cmd && !shift) {
        for (int d = 0; d <= 9; d++) {
            int digit_key = (d == 0) ? GLFW_KEY_0 : GLFW_KEY_1 + (d - 1);
            int kp_key = (d == 0) ? GLFW_KEY_KP_0 : GLFW_KEY_KP_1 + (d - 1);
            if (platformKeyPressed(digit_key) || platformKeyPressed(kp_key)) {
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
    // key_binding/3 + key_binding_once/3 (src/default_bindings.pl, plus
    // whatever a user init.pl added or overrode) are the sole dispatch path
    // now -- modal-mode's navigation-only restriction and the mark/Shift
    // selection-extend computation both moved into scriptHandleKey (see
    // script.c's matchAndRun), via the editorIsNavAction/editorGetMarkActive/
    // editorSetSelExtend calls it makes back into this file.
    scriptHandleKey(ctrl, shift, modal);
    float wheel = platformMouseWheel();
    if (wheel != 0) {
        // With wrap, lines vary in height, so just stop at the last line.
        size_t max_top = wrap ? (lineCount() - 1) : (lineCount() > m.visible ? lineCount() - m.visible : 0);
        long long nt = (long long)top_line - (long long)wheel * CFG_SCROLL_SPEED;
        if (nt < 0) nt = 0;
        if (nt > (long long)max_top) nt = (long long)max_top;
        top_line = (size_t)nt;
    }
    if (platformMouseLeftPressed()) {
        cursor = offsetFromMouse(m);
        if (!shift) anchor = cursor;
        scriptClearGoalColumn();
        noteActivity();
    } else if (platformMouseLeftDown()) {
        cursor = offsetFromMouse(m);
        scriptClearGoalColumn();
        noteActivity();
    }
}

// Detect Ctrl double/triple taps. A tap is Ctrl pressed then released with no
// other key pressed while it was held. Two taps arm the leader; a third opens
// the command prompt. There is no timeout -- the count resets only when a
// non-Ctrl key is pressed -- so a pause between taps still gets you there.
// Runs only in normal editing mode. Draining the key-pressed queue here is
// safe: bindings use platformKeyPressed and text uses platformCharPressed,
// both independent of it.
static void detectCtrlTaps(void) {
    int lc = GLFW_KEY_LEFT_CONTROL, rc = GLFW_KEY_RIGHT_CONTROL;
    if (platformKeyPressed(lc) || platformKeyPressed(rc)) ctrl_clean = true;
    int k = platformKeyPressedQueue();
    while (k != 0) {
        if (k != lc && k != rc) {
            ctrl_clean = false; // a real key was pressed with Ctrl: not a tap
            ctrl_taps = 0;      // and it breaks any tap run in progress
        }
        k = platformKeyPressedQueue();
    }
    if (platformKeyReleased(lc) || platformKeyReleased(rc)) {
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
    if (platformKeyPressed(GLFW_KEY_ESCAPE) || (ctrl && platformKeyPressed(GLFW_KEY_G))) {
        prefix_pending = false;
        echo("Quit");
        return;
    }
    // leader_binding/3 (src/default_bindings.pl, plus whatever a user
    // init.pl added or overrode) is the sole chord dispatch path now.
    if (scriptHandlePrefixKey(shift)) {
        prefix_pending = false;
        // See the matching drain below: swallow a same-key char event GLFW
        // may still have queued (or deliver a frame or two late under async
        // IME) so it isn't typed into the buffer or a prompt afterward.
        while (platformCharPressed() != 0) {}
        swallow_char_frames = 3;
        return;
    }
    // leader n -> key/navigation help overlay
    if (platformKeyPressed(GLFW_KEY_N)) {
        prefix_pending = false;
        help = HELP_NAV;
        return;
    }
    // leader h -> commands help overlay
    if (platformKeyPressed(GLFW_KEY_H)) {
        prefix_pending = false;
        help = HELP_COMMANDS;
        return;
    }
    // any other printable key is an undefined chord: cancel
    if (platformCharPressed() > 0) {
        prefix_pending = false;
        echo("Quit");
    }
}

static void drawMinibuffer(float char_w, float y, char *tmp, size_t tmp_cap) {
    if (mb_kind == MB_NONE) {
        if (prefix_pending) {
            drawStr("Ctrlx2-", char_w, CFG_MARGIN_X, y + 2, CFG_COLOR_FG);
            return;
        }
        // echo area
        if (echo_len > 0 && platformTime() - echo_time < 4.0) {
            size_t n = echo_len < tmp_cap - 1 ? echo_len : tmp_cap - 1;
            memcpy(tmp, echo_buf, n);
            tmp[n] = 0;
            drawStr(tmp, char_w, CFG_MARGIN_X, y + 2, CFG_COLOR_STATUS_FG);
        }
        return;
    }
    if (mb_kind == MB_REPLACE_QUERY) {
        size_t index, count; bool truncated, bad_regex;
        scriptSearchStatus(&index, &count, &truncated, &bad_regex);
        char buf[128];
        int n = snprintf(buf, sizeof(buf), "Replace? Enter=yes  n/p=skip  Esc=done  (%zu/%zu)", index + 1, count);
        drawStr(n > 0 ? buf : "Replace?", char_w, CFG_MARGIN_X, y + 2, CFG_COLOR_FG);
        return;
    }
    // prompt
    drawStr(mb_prompt, char_w, CFG_MARGIN_X, y + 2, CFG_COLOR_FG);
    float prompt_w = strWidth(mb_prompt, char_w);
    if (mb_kind == MB_TEXT_PROMPT) {
        size_t n = mb_len < tmp_cap - 1 ? mb_len : tmp_cap - 1;
        memcpy(tmp, mb_input, n);
        tmp[n] = 0;
        float x0 = CFG_MARGIN_X + prompt_w;
        drawStr(tmp, char_w, x0, y + 2, CFG_COLOR_FG);
        // minibuffer caret (solid)
        float cx = x0 + (float)mb_cursor * char_w;
        platformDrawRect(cx, y + 2, 2, CFG_FONT_SIZE, CFG_COLOR_CURSOR);
        // search / replace-pattern prompt: show the live match count after query
        if ((mb_intent == MBI_SEARCH || mb_intent == MBI_REPLACE_FROM) && mb_len > 0) {
            size_t index, count; bool truncated, bad_regex;
            scriptSearchStatus(&index, &count, &truncated, &bad_regex);
            char st[64];
            if (bad_regex) snprintf(st, sizeof(st), "(bad regex)");
            else if (count == 0) snprintf(st, sizeof(st), "(no match)");
            else snprintf(st, sizeof(st), "(%zu/%zu%s)", index + 1, count, truncated ? "+" : "");
            float sx = x0 + (float)mb_len * char_w + 2 * char_w;
            drawStr(st, char_w, sx, y + 2, CFG_COLOR_GUTTER);
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
            drawStr(hint, char_w, hx, y + 2, CFG_COLOR_GUTTER);
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
    if (key == GLFW_KEY_LEFT) return "Left";
    if (key == GLFW_KEY_RIGHT) return "Right";
    if (key == GLFW_KEY_UP) return "Up";
    if (key == GLFW_KEY_DOWN) return "Down";
    if (key == GLFW_KEY_HOME) return "Home";
    if (key == GLFW_KEY_END) return "End";
    if (key == GLFW_KEY_PAGE_UP) return "PgUp";
    if (key == GLFW_KEY_PAGE_DOWN) return "PgDn";
    if (key == GLFW_KEY_ENTER) return "Enter";
    if (key == GLFW_KEY_KP_ENTER) return "KpEnter";
    if (key == GLFW_KEY_TAB) return "Tab";
    if (key == GLFW_KEY_BACKSPACE) return "Backspace";
    if (key == GLFW_KEY_DELETE) return "Delete";
    if (key == GLFW_KEY_SPACE) return "Space";
    return "";
}
// Human-readable chord like "C-S-Left" or "B" into buf; returns its length.
static size_t comboName(char *buf, int key, Mod mod) {
    size_t n = 0;
    const char *pfx = modPrefix(mod);
    size_t pfx_len = strlen(pfx);
    memcpy(buf + n, pfx, pfx_len);
    n += pfx_len;
    const char *named = keyLabel(key);
    size_t named_len = strlen(named);
    if (named_len > 0) {
        memcpy(buf + n, named, named_len);
        n += named_len;
    } else if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z) {
        buf[n++] = (char)('A' + (key - GLFW_KEY_A));
    } else {
        buf[n++] = '?';
    }
    return n;
}
// Distinct labels across the direct keybindings (one help row each) -- a
// linear "seen" scan rather than an Action-indexed array, since a binding's
// label (scriptTopBindingGet) is now a string, not necessarily one of
// ACTION_LABELS (a user init.pl's custom handler gets its own predicate
// name as a fallback label).
static size_t navRowCount(void) {
    size_t total = scriptTopBindingCount();
    const char *seen[ACTION_COUNT + 64];
    size_t seenCount = 0, rows = 0;
    for (size_t i = 0; i < total; i++) {
        int key; Mod mod; const char *label;
        if (!scriptTopBindingGet(i, &key, &mod, &label)) continue;
        bool dup = false;
        for (size_t s = 0; s < seenCount; s++) if (strcmp(seen[s], label) == 0) { dup = true; break; }
        if (dup) continue;
        if (seenCount < sizeof(seen) / sizeof(seen[0])) seen[seenCount++] = label;
        rows++;
    }
    return rows;
}
// Emacs-style: the help grows upward from the echo/status area as a panel of
// lines, rather than a full-screen overlay.
static void drawHelp(float char_w, float line_h, float win_w, float win_h, char *tmp, size_t tmp_cap) {
    float status_y = win_h - 2 * line_h; // top of the status line
    size_t content = (help == HELP_NAV) ? navRowCount() : (scriptCommandCount() + 1);
    float pad = line_h * 0.5f;
    float block_h = (float)(content + 2) * line_h + pad * 2;
    float top = status_y - block_h;
    if (top < 0) top = 0;
    platformDrawRect(0, top, win_w, status_y - top, CFG_COLOR_STATUS_BG);
    platformDrawRect(0, top, win_w, 1, CFG_COLOR_GUTTER);

    float x = CFG_MARGIN_X + 8;
    float y = status_y - block_h + pad;

    if (help == HELP_NAV) {
        drawStr("Navigation & editing keys  (Ctrlx2 n)", char_w, x, y, CFG_COLOR_CURSOR);
        y += line_h;
        size_t total = scriptTopBindingCount();
        const char *shown[ACTION_COUNT + 64];
        size_t shownCount = 0;
        for (size_t bi = 0; bi < total; bi++) {
            int key; Mod mod; const char *label;
            if (!scriptTopBindingGet(bi, &key, &mod, &label)) continue;
            bool dup = false;
            for (size_t s = 0; s < shownCount; s++) if (strcmp(shown[s], label) == 0) { dup = true; break; }
            if (dup) continue;
            if (shownCount < sizeof(shown) / sizeof(shown[0])) shown[shownCount++] = label;
            char cbuf[96];
            size_t clen = 0;
            bool first = true;
            for (size_t bj = 0; bj < total; bj++) {
                int key2; Mod mod2; const char *label2;
                if (!scriptTopBindingGet(bj, &key2, &mod2, &label2)) continue;
                if (strcmp(label2, label) != 0) continue;
                if (!first) {
                    cbuf[clen] = ',';
                    cbuf[clen + 1] = ' ';
                    clen += 2;
                }
                first = false;
                char one[24];
                size_t cs = comboName(one, key2, mod2);
                memcpy(cbuf + clen, one, cs);
                clen += cs;
            }
            int n = snprintf(tmp, tmp_cap, "%-22.*s%s", (int)clen, cbuf, label);
            if (n < 0) continue;
            drawStr(tmp, char_w, x, y, CFG_COLOR_FG);
            y += line_h;
        }
    } else {
        drawStr("Commands  (Ctrlx2 = double-tap Ctrl, then h)  --  then name, or chord", char_w, x, y, CFG_COLOR_CURSOR);
        y += line_h;
        size_t command_count = scriptCommandCount();
        size_t leader_count = scriptLeaderBindingCount();
        for (size_t ci = 0; ci < command_count; ci++) {
            const char *name;
            if (!scriptCommandGet(ci, &name)) continue;
            char chord[24] = "";
            for (size_t pi = 0; pi < leader_count; pi++) {
                int key; Mod mod; const char *label;
                if (!scriptLeaderBindingGet(pi, &key, &mod, &label)) continue;
                if (strcmp(label, name) != 0) continue;
                char one[16];
                size_t cs = comboName(one, key, mod);
                snprintf(chord, sizeof(chord), "Ctrlx2 %.*s", (int)cs, one);
                break;
            }
            int n = snprintf(tmp, tmp_cap, "%-16s%s", name, chord);
            if (n < 0) continue;
            drawStr(tmp, char_w, x, y, CFG_COLOR_FG);
            y += line_h;
        }
        drawStr("Ctrlx3 (triple-tap Ctrl) : type a command", char_w, x, y, CFG_COLOR_GUTTER);
        y += line_h;
    }
    drawStr("Press any key to close", char_w, x, y, CFG_COLOR_GUTTER);
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

// Resolve UnifontExMono.ttf next to the running executable and read it whole
// into a heap buffer kept for the program's lifetime (glyphs.c's FreeType
// rasterizer is handed the raw bytes via FT_New_Memory_Face, not a path).
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
bool editorIsNavAction(Action action) { return isNavAction(action); }
bool editorGetMarkActive(void) { return mark_active; }
void editorSetSelExtend(bool extend) { sel_extend = extend; }
void editorReplaceRange(size_t start, size_t end, const unsigned char *bytes, size_t bytes_len) {
    replaceRange(start, end, bytes, bytes_len);
}
size_t editorUndoDepth(void) { return CFG_UNDO_DEPTH; }
size_t editorGetAnchor(void) { return anchor; }
void editorSetSelection(size_t anchor_pos, size_t cursor_pos) {
    if (anchor_pos > len) anchor_pos = len;
    if (cursor_pos > len) cursor_pos = len;
    anchor = anchor_pos;
    cursor = cursor_pos;
    noteActivity();
}
void editorApplyReplace(size_t start, size_t end, const unsigned char *bytes, size_t bytes_len) {
    edit(start, end, bytes, bytes_len);
}
void editorCopyRange(size_t start, size_t end) { copyRange(start, end); }
bool editorFindMatches(const unsigned char *query, size_t query_len, bool is_regex) {
    return findMatches(query, query_len, is_regex);
}
size_t editorMatchCount(void) { return match_count; }
bool editorMatchTruncated(void) { return match_truncated; }
void editorMatchGet(size_t i, size_t *start, size_t *end) {
    *start = match_starts[i];
    *end = match_starts[i] + match_lens[i];
}
bool editorRegexSubstitute(const unsigned char *pattern, size_t pattern_len,
                           size_t match_start, size_t match_end,
                           const unsigned char *replacement, size_t replacement_len,
                           const unsigned char **out, size_t *out_len) {
    int ec = 0;
    PCRE2_SIZE eo = 0;
    pcre2_code *re = pcre2_compile(pattern, pattern_len, 0, &ec, &eo, NULL);
    if (!re) return false;
    PCRE2_SIZE outlen = sizeof(one_rep_buf);
    int rc = pcre2_substitute(re, text + match_start, match_end - match_start, 0,
                               PCRE2_SUBSTITUTE_OVERFLOW_LENGTH, NULL, NULL,
                               replacement, replacement_len, one_rep_buf, &outlen);
    pcre2_code_free(re);
    if (rc < 0) return false;
    *out = one_rep_buf;
    *out_len = outlen;
    return true;
}
size_t editorLineStart(size_t pos) { return lineStart(pos); }
size_t editorLineEnd(size_t pos) { return lineEnd(pos); }
size_t editorColsIn(size_t start, size_t end) { return colsIn(start, end); }
size_t editorByteAtCol(size_t start, size_t stop, size_t col) { return byteAtCol(start, stop, col); }
size_t editorVisRows(size_t line_cols, size_t cols) { return visRows(line_cols, cols); }
size_t editorStepLeft(size_t pos) { return stepLeft(pos); }
size_t editorStepRight(size_t pos) { return stepRight(pos); }
size_t editorWordStartLeft(size_t pos) { return wordStartLeft(pos); }
size_t editorWordStartRight(size_t pos) { return wordStartRight(pos); }
size_t editorWordEndRight(size_t pos) { return wordEndRight(pos); }
size_t editorViewCols(void) { return view_cols; }
size_t editorPageLines(void) { return page_lines; }
size_t editorBufferLen(void) { return len; }
int editorCurrentBufferId(void) { return selected_window->buf->id; }

// A window's own id -- today there's only ever root_window (a lone leaf),
// so this is a trivial lookup; once splitting lands this needs a real tree
// walk over leaves (a split node has no buffer of its own to switch and no
// meaningful "select" target).
static Window *windowFindById(int id) {
    return (root_window && root_window->id == id) ? root_window : NULL;
}

int editorBufferCreate(void) { return bufferCreate()->id; }
// Populates a *new* buffer from `path` (openPath's existing open-or-new-file
// logic, reused verbatim) without disturbing whatever's currently on
// screen -- temporarily points the selected window at the new buffer so
// openPath's macro-based text/len/filename/... writes land on it, restores
// the previous buffer before returning. Whether/when to actually switch to
// it is a policy decision left to the caller (src/buffers.pl's open_file/1).
int editorBufferOpenFile(const char *path) {
    Buffer *b = bufferCreate();
    Buffer *saved = selected_window->buf;
    selected_window->buf = b;
    openPath(path);
    selected_window->buf = saved;
    return b->id;
}
int editorBufferFindByPath(const char *path) {
    for (Buffer *b = buffer_list; b; b = b->next)
        if (b->buf_has_file && strcmp(b->buf_filename, path) == 0) return b->id;
    return -1;
}
// Repoints any window currently showing the killed buffer -- today that's
// only ever selected_window (no other windows exist yet; the window-split
// commit will need to walk the whole tree here). Auto-creates a fresh
// scratch buffer if this was the last one, so there's always at least one.
bool editorBufferKill(int id) {
    Buffer *target = bufferFindById(id);
    if (!target) return false;
    Buffer *prev = NULL;
    for (Buffer *b = buffer_list; b; prev = b, b = b->next)
        if (b == target) break;
    if (prev) prev->next = target->next; else buffer_list = target->next;
    if (buffer_list_tail == target) buffer_list_tail = prev;
    if (selected_window->buf == target) {
        selected_window->buf = buffer_list ? buffer_list : bufferCreate();
        cursor = 0; anchor = 0; top_line = 0; left_col = 0;
    }
    free(target->buf_text);
    free(target);
    return true;
}
size_t editorBufferCount(void) {
    size_t n = 0;
    for (Buffer *b = buffer_list; b; b = b->next) n++;
    return n;
}
// 0-based, creation order (bufferCreate appends).
int editorBufferIdAt(size_t index) {
    size_t i = 0;
    for (Buffer *b = buffer_list; b; b = b->next, i++)
        if (i == index) return b->id;
    return -1;
}
// Display name: the filename's basename, or the literal "untitled.txt"
// default for a buffer never associated with a real path -- derived on
// demand rather than stored, since it's always exactly this computation.
const char *editorBufferName(int id) {
    Buffer *b = bufferFindById(id);
    if (!b) return NULL;
    const char *slash = strrchr(b->buf_filename, '/');
    return slash ? slash + 1 : b->buf_filename;
}
bool editorBufferFilename(int id, const char **out_path, bool *out_has_file) {
    Buffer *b = bufferFindById(id);
    if (!b) return false;
    *out_path = b->buf_filename;
    *out_has_file = b->buf_has_file;
    return true;
}
bool editorBufferDirty(int id, bool *out) {
    Buffer *b = bufferFindById(id);
    if (!b) return false;
    *out = b->buf_dirty;
    return true;
}
// saveFile() operates on selected_window->buf via the macros -- temporarily
// point the selected window at the target buffer (whether or not it's the
// one currently on screen) so an arbitrary buffer can be saved by id.
bool editorBufferSave(int id) {
    Buffer *b = bufferFindById(id);
    if (!b) return false;
    Buffer *saved = selected_window->buf;
    selected_window->buf = b;
    bool ok = saveFile();
    selected_window->buf = saved;
    return ok;
}

int editorSelectedWindowId(void) { return selected_window->id; }
bool editorSelectWindow(int id) {
    Window *w = windowFindById(id);
    if (!w) return false;
    selected_window = w;
    return true;
}
int editorWindowBufferId(int win_id) {
    Window *w = windowFindById(win_id);
    return w ? w->buf->id : -1;
}
// Switches `win_id`'s buffer and resets that window's own view state (a
// fresh buffer means the old scroll position/cursor are meaningless) --
// does not touch which window is selected.
bool editorWindowSetBuffer(int win_id, int buf_id) {
    Window *w = windowFindById(win_id);
    Buffer *b = bufferFindById(buf_id);
    if (!w || !b) return false;
    w->buf = b;
    w->win_cursor = 0;
    w->win_anchor = 0;
    w->win_top_line = 0;
    w->win_left_col = 0;
    return true;
}

int main(int argc, char **argv) {
    bootstrapEditor(); // must run before anything touches the buffer/window macros
    grepMode(argc, argv); // `te --regex <pattern> <file>` prints matches and exits

    if (!loadFontFile()) {
        fprintf(stderr, "te: cannot load UnifontExMono.ttf (expected next to the executable)\n");
        return 1;
    }

    // platformInit opens a hidden placeholder window, sizes it to half the
    // primary display, then reveals it -- the user never sees the
    // placeholder size.
    platformInit(CFG_WINDOW_TITLE, CFG_TARGET_FPS);

    float font_size = CFG_FONT_SIZE; // mutable: Ctrl +/- zooms it
    glyphs_init(font_bytes, font_bytes_len, (int)font_size, platformGLContext());

    float char_w = glyphs_advance('M');
    float line_h = font_size + CFG_FONT_LINE_GAP;
    float margin_x = CFG_MARGIN_X;
    float margin_y = CFG_MARGIN_Y;
    blink_base = platformTime();

    // Loaded before the command-line file (if any) is opened, so an
    // init.pl hook(post_open, ...) also fires for it.
    if (!scriptInit()) {
        fprintf(stderr, "te: cannot load src/bootstrap.pl (expected next to the executable, or ./src when run from the repo)\n");
        glyphs_deinit();
        platformShutdown();
        free(font_bytes);
        return 1;
    }

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
        platformPollEvents();
        shift = platformKeyDown(GLFW_KEY_LEFT_SHIFT) || platformKeyDown(GLFW_KEY_RIGHT_SHIFT);
        bool ctrl = platformKeyDown(GLFW_KEY_LEFT_CONTROL) || platformKeyDown(GLFW_KEY_RIGHT_CONTROL);
        if (swallow_char_frames > 0) {
            swallow_char_frames--;
            while (platformCharPressed() != 0) {}
        }
        // C-m toggles modal (command) mode: bare keys run the Ctrl-key actions
        // and typing is suppressed (see handleInput). Ignored while a minibuffer
        // prompt is open so 'm' types normally there; swallow the toggling key so
        // the bare 'm' that exits modal isn't inserted as text.
        if (platformKeyPressed(GLFW_KEY_M) && (ctrl || (modal && mb_kind == MB_NONE))) {
            modal = !modal;
            echo(modal ? "Modal ON (m/Esc to exit)" : "Modal OFF");
            while (platformCharPressed() != 0) {}
            swallow_char_frames = 3;
        }

        // ---- layout metrics (two bottom lines reserved: status + minibuffer) ----
        float win_w, win_h;
        platformScreenSize(&win_w, &win_h);
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
            if (platformAnyKeyPressed() || platformMouseLeftPressed()) help = HELP_NONE;
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
            if (mb_kind == MB_NONE && (platformWindowShouldClose() || quit_requested)) {
                if (dirty) mbStartQuery(MBI_QUIT, "Save changes? (y) yes  (n) no  (c) cancel");
                else running = false;
            }
        }

        // ---- Ctrl +/- : zoom the font in 16 px steps (multiples of 16 keep
        // UnifontEX pixel-crisp). Reset the glyph cache to re-rasterize at
        // the new size.
        if (help == HELP_NONE && mb_kind == MB_NONE) {
            bool zin = ctrl && (platformKeyPressed(GLFW_KEY_EQUAL) || platformKeyPressed(GLFW_KEY_KP_ADD));
            bool zout = ctrl && (platformKeyPressed(GLFW_KEY_MINUS) || platformKeyPressed(GLFW_KEY_KP_SUBTRACT));
            if (zin || zout) {
                float step = zin ? 16.0f : -16.0f;
                float ns = font_size + step;
                if (ns < 16) ns = 16;
                if (ns > 64) ns = 64;
                if (ns != font_size) {
                    font_size = ns;
                    glyphs_reset((int)font_size);
                    char_w = glyphs_advance('M');
                    line_h = font_size + CFG_FONT_LINE_GAP;
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
        platformBeginDrawing();
        platformClearBackground(CFG_COLOR_BG);

        size_t sel_a = selMin();
        size_t sel_b = selMax();
        LineCol cp = cursorLineCol();
        bool blink_on = !CFG_CURSOR_BLINK ||
                        fmod(platformTime() - blink_base, CFG_CURSOR_BLINK_PERIOD * 2) < CFG_CURSOR_BLINK_PERIOD;
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
                        if (b > a) platformDrawRect(
                            text_x0 + (float)colsIn(seg_s, a) * char_w,
                            y,
                            (float)colsIn(a, b) * char_w,
                            line_h,
                            CFG_COLOR_SELECTION);
                    }

                    if (seg == 0) {
                        int np = snprintf(num_tmp, sizeof(num_tmp), "%zu", li + 1);
                        size_t npu = np > 0 ? (size_t)np : 0;
                        float nx = margin_x + (float)(npu < digits ? digits - npu : 0) * char_w;
                        drawStr(num_tmp, char_w, nx, y, CFG_COLOR_GUTTER);
                    }

                    drawCells(char_w, text_x0, y, seg_s, seg_e, CFG_COLOR_FG);
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
                platformDrawRect(cx, caret_y, 2, line_h, CFG_COLOR_CURSOR);
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
                            if (cb > ca) platformDrawRect(
                                text_x0 + (float)ca * char_w,
                                y,
                                (float)(cb - ca) * char_w,
                                line_h,
                                CFG_COLOR_SELECTION);
                        }
                    }

                    int np = snprintf(num_tmp, sizeof(num_tmp), "%zu", li + 1);
                    size_t npu = np > 0 ? (size_t)np : 0;
                    float nx = margin_x + (float)(npu < digits ? digits - npu : 0) * char_w;
                    drawStr(num_tmp, char_w, nx, y, CFG_COLOR_GUTTER);

                    size_t vis_start = byteAtCol(s, e, left_col);
                    drawCells(char_w, text_x0, y, vis_start, e, CFG_COLOR_FG);
                }
                li++;
                if (e >= len) break;
                s = e + 1;
            }

            if (show_caret && cp.line >= top_line && cp.line < top_line + visible && cp.col >= left_col) {
                size_t row = cp.line - top_line;
                float cx = text_x0 + (float)(cp.col - left_col) * char_w;
                float cy = margin_y + (float)row * line_h;
                platformDrawRect(cx, cy, 2, line_h, CFG_COLOR_CURSOR);
            }
        }

        // status (mode) line
        platformDrawRect(0, status_y, win_w, line_h, CFG_COLOR_STATUS_BG);
        int slen = snprintf(status_tmp, sizeof(status_tmp), "%s%s%s  |  Ln %zu, Col %zu  |  %zu bytes",
                             modal ? "[MODAL]  " : "", filename, dirty ? " *" : "", cp.line + 1, cp.col + 1, len);
        drawStr(slen > 0 ? status_tmp : "te", char_w, margin_x, status_y + 2, CFG_COLOR_STATUS_FG);

        drawMinibuffer(char_w, mb_y, line_tmp, sizeof(line_tmp));

        if (help != HELP_NONE) drawHelp(char_w, line_h, win_w, win_h, line_tmp, sizeof(line_tmp));

        if (shot_left > 0) {
            shot_left--;
            if (shot_left == 0) {
                platformScreenshot(shot_path);
                running = false;
            }
        }
        platformEndDrawing();
    }

    scriptShutdown();
    freeHistory();
    glyphs_deinit();
    platformShutdown();
    free(font_bytes);
    return 0;
}
