// Unit tests for te's core buffer/undo/search logic.
//
// main.c is one translation unit with every piece of state and every helper
// declared `static`, and no header exposes any of it. The only way to reach
// that logic without a large refactor is to #include the source directly, so
// its statics become ordinary file-scope symbols here. main() is renamed out
// of the way first so it is never linked in as `main` and never called --
// nothing in this file ever creates a raylib window.
//
// GetTime()/IsKeyPressed() (called incidentally by edit()/echo()) are safe to
// call pre-InitWindow: raylib just reads its zeroed global state. Two things
// are NOT safe and are deliberately never exercised here: SetClipboardText/
// GetClipboardText (need a live GLFW window) and any codepoint outside
// inAtlas()'s ranges (glyphs_get() would try to upload a GPU texture). Tests
// below stick to ASCII/Latin/Greek/Cyrillic, which inAtlas() covers.
#define main te_disabled_main
#include "../src/main.c"
#undef main

#include <stdio.h>
#include <stdlib.h>

static int g_checks = 0;
static int g_failures = 0;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { \
        g_failures++; \
        fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define RUN(fn) do { fn(); } while (0)

// --- test helpers --------------------------------------------------------
static void reset(void) {
    freeHistory();
    len = 0;
    cursor = 0;
    anchor = 0;
    dirty = false;
    mark_active = false;
    goal_col_set = false;
    wrap = true;
    view_cols = 80;
    search_is_regex = false;
    search_reverse = false;
    search_bad_regex = false;
    match_count = 0;
}

static void setText(const char *s) {
    reset();
    size_t n = strlen(s);
    memcpy(text, s, n);
    len = n;
}

static bool textEquals(const char *s) {
    size_t n = strlen(s);
    return len == n && memcmp(text, s, n) == 0;
}

// --- UTF-8 -----------------------------------------------------------------
static void test_utf8_roundtrip(void) {
    unsigned char buf[4];

    size_t n = utf8Encode(0x41, buf); // 'A'
    CHECK(n == 1 && buf[0] == 0x41);

    n = utf8Encode(0xE9, buf); // 'e' with acute, U+00E9
    CHECK(n == 2);
    setText("");
    memcpy(text, buf, n);
    len = n;
    Cp d = decodeCp(0, len);
    CHECK(d.cp == 0xE9 && d.len == 2);

    n = utf8Encode(0x20AC, buf); // euro sign, U+20AC
    CHECK(n == 3);
    setText("");
    memcpy(text, buf, n);
    len = n;
    d = decodeCp(0, len);
    CHECK(d.cp == 0x20AC && d.len == 3);
}

static void test_utf8_malformed_falls_back(void) {
    unsigned char raw[] = { 0xC3, 0x28 }; // lead byte followed by a non-continuation
    setText("");
    memcpy(text, raw, sizeof(raw));
    len = sizeof(raw);
    Cp d = decodeCp(0, len);
    CHECK(d.cp == 0xC3 && d.len == 1); // falls back to a single raw byte
}

static void test_isCont(void) {
    CHECK(isCont(0x80) == true);
    CHECK(isCont(0xBF) == true);
    CHECK(isCont(0xC0) == false);
    CHECK(isCont('A') == false);
}

// --- buffer mutation ---------------------------------------------------
static void test_insert_into_empty(void) {
    setText("");
    insertBytes((const unsigned char *)"hello", 5);
    CHECK(textEquals("hello"));
    CHECK(cursor == 5);
    CHECK(anchor == 5);
    CHECK(dirty);
}

static void test_insert_at_cursor(void) {
    setText("abcdef");
    cursor = 3;
    anchor = 3;
    insertBytes((const unsigned char *)"XYZ", 3);
    CHECK(textEquals("abcXYZdef"));
    CHECK(cursor == 6);
}

static void test_insert_replaces_selection(void) {
    setText("abcdef");
    anchor = 1;
    cursor = 4; // selects "bcd"
    insertBytes((const unsigned char *)"Q", 1);
    CHECK(textEquals("aQef"));
    CHECK(cursor == 2);
    CHECK(anchor == 2);
}

static void test_deleteBack_ascii(void) {
    setText("abc");
    cursor = 3;
    anchor = 3;
    deleteBack();
    CHECK(textEquals("ab"));
    CHECK(cursor == 2);
}

static void test_deleteBack_utf8(void) {
    // "a" + U+00E9 + "b"
    unsigned char enc[4];
    size_t n = utf8Encode(0xE9, enc);
    setText("");
    text[0] = 'a';
    memcpy(text + 1, enc, n);
    text[1 + n] = 'b';
    len = 2 + n;
    cursor = 1 + n; // just after the accented char, before 'b'
    anchor = cursor;
    deleteBack();
    CHECK(textEquals("ab")); // the whole 2-byte sequence goes, not just its tail byte
    CHECK(cursor == 1);
}

static void test_deleteForward_ascii(void) {
    setText("abc");
    cursor = 0;
    anchor = 0;
    deleteForward();
    CHECK(textEquals("bc"));
    CHECK(cursor == 0);
}

static void test_deleteSelection(void) {
    setText("abcdef");
    anchor = 1;
    cursor = 4; // "bcd"
    deleteSelection();
    CHECK(textEquals("aef"));
    CHECK(cursor == 1);
}

// --- undo / redo -------------------------------------------------------
static void test_undo_redo_roundtrip(void) {
    setText("");
    insertBytes((const unsigned char *)"hello", 5);
    CHECK(textEquals("hello"));
    CHECK(dirty);
    doUndo();
    CHECK(textEquals(""));
    CHECK(!dirty); // undoing back to the start clears dirty
    doRedo();
    CHECK(textEquals("hello"));
    CHECK(dirty);
}

static void test_undo_coalesces_single_char_typing(void) {
    setText("");
    insertBytes((const unsigned char *)"a", 1);
    insertBytes((const unsigned char *)"b", 1);
    insertBytes((const unsigned char *)"c", 1);
    CHECK(textEquals("abc"));
    CHECK(undo_n == 1); // three keystrokes, one undo step
    doUndo();
    CHECK(textEquals(""));
    CHECK(undo_n == 0);
}

static void test_undo_does_not_coalesce_across_newline(void) {
    setText("");
    insertBytes((const unsigned char *)"a", 1);
    insertBytes((const unsigned char *)"\n", 1);
    CHECK(undo_n == 2); // newline breaks the coalescing run
    doUndo();
    CHECK(textEquals("a"));
    doUndo();
    CHECK(textEquals(""));
}

static void test_undo_multistep(void) {
    setText("abc");
    cursor = 3;
    anchor = 3;
    edit(3, 3, (const unsigned char *)"DEF", 3); // "abcDEF"
    edit(0, 0, (const unsigned char *)"XY", 2);  // "XYabcDEF"
    CHECK(textEquals("XYabcDEF"));
    doUndo();
    CHECK(textEquals("abcDEF"));
    doUndo();
    CHECK(textEquals("abc"));
    CHECK(undo_n == 0);
    CHECK(!dirty);
}

// --- lines ---------------------------------------------------------------
static void test_line_boundaries(void) {
    setText("abc\ndef\nghi");
    CHECK(lineCount() == 3);
    CHECK(lineStart(0) == 0);
    CHECK(lineEnd(0) == 3);
    CHECK(lineStart(5) == 4); // byte 5 is inside "def", which starts at 4
    CHECK(lineEnd(5) == 7);
    CHECK(lineStartOfRow(0) == 0);
    CHECK(lineStartOfRow(1) == 4);
    CHECK(lineStartOfRow(2) == 8);
}

static void test_currentLineSpan(void) {
    setText("abc\ndef\nghi");
    cursor = 5; // inside "def"
    Span s = currentLineSpan();
    CHECK(s.start == 4 && s.end == 8); // includes the trailing newline
    cursor = 9; // inside the last line, no trailing newline
    s = currentLineSpan();
    CHECK(s.start == 8 && s.end == 11);
}

static void test_cursorLineCol(void) {
    setText("abc\nde");
    cursor = 5; // "abc\nd|e" -- after 'd' on the second line
    LineCol lc = cursorLineCol();
    CHECK(lc.line == 1 && lc.col == 1);
}

static void test_swapLine_down_and_up(void) {
    setText("one\ntwo\nthree");
    cursor = 1; // inside "one"
    anchor = 1;
    swapLine(true); // swap with "two"
    CHECK(textEquals("two\none\nthree"));
    swapLine(false); // swap back
    CHECK(textEquals("one\ntwo\nthree"));
}

// --- word movement ---------------------------------------------------------
static void test_word_movement(void) {
    setText("foo bar_baz  qux");
    cursor = 0;
    moveWordStartRight();
    CHECK(cursor == 4); // start of "bar_baz" ('_' is a word char)
    moveWordStartRight();
    CHECK(cursor == 13); // start of "qux", past the double space

    setText("foo bar");
    cursor = 0;
    moveWordEndRight();
    CHECK(cursor == 3); // end of "foo"

    setText("foo bar");
    cursor = 7; // end of buffer
    moveWordStartLeft();
    CHECK(cursor == 4); // start of "bar"
}

static void test_moveLeft_moveRight_utf8(void) {
    unsigned char enc[4];
    size_t n = utf8Encode(0xE9, enc); // U+00E9, 2 bytes
    setText("");
    text[0] = 'a';
    memcpy(text + 1, enc, n);
    text[1 + n] = 'b';
    len = 2 + n;
    cursor = 0;
    moveRight(); // over 'a'
    CHECK(cursor == 1);
    moveRight(); // over the whole 2-byte codepoint, not just one byte
    CHECK(cursor == 1 + n);
    moveLeft(); // back over the codepoint
    CHECK(cursor == 1);
}

// --- search & replace --------------------------------------------------
static void test_computeMatches_literal(void) {
    setText("the cat sat on the mat");
    search_is_regex = false;
    computeMatches((const unsigned char *)"at", 2);
    CHECK(match_count == 3); // c[at], s[at], m[at]
    CHECK(match_starts[0] == 5);
}

static void test_computeMatches_regex(void) {
    setText("foo1 bar22 baz333");
    search_is_regex = true;
    computeMatches((const unsigned char *)"[0-9]+", 6);
    CHECK(!search_bad_regex);
    CHECK(match_count == 3);
    CHECK(match_lens[2] == 3); // "333"
}

static void test_computeMatches_bad_regex(void) {
    setText("anything");
    search_is_regex = true;
    computeMatches((const unsigned char *)"(unterminated", 14);
    CHECK(search_bad_regex);
    CHECK(match_count == 0);
}

static void test_gotoMatch(void) {
    setText("aXaXa");
    search_is_regex = false;
    computeMatches((const unsigned char *)"X", 1);
    CHECK(match_count == 2);
    gotoMatch(1);
    CHECK(anchor == match_starts[1]);
    CHECK(cursor == match_starts[1] + match_lens[1]);
}

static void test_replaceAll_literal(void) {
    setText("cat cat cat");
    replaceAll((const unsigned char *)"cat", 3, (const unsigned char *)"dog", 3, false);
    CHECK(textEquals("dog dog dog"));
}

static void test_replaceAll_regex_backref(void) {
    setText("2024-01-02 then 2025-12-31");
    replace_is_regex = true;
    const char *pat = "(\\d+)-(\\d+)-(\\d+)";
    const char *rep = "$3/$2/$1";
    replace_from_len = strlen(pat);
    memcpy(replace_from_buf, pat, replace_from_len);
    replace_to_len = strlen(rep);
    memcpy(replace_to_buf, rep, replace_to_len);
    replaceAll((const unsigned char *)replace_from_buf, replace_from_len,
               (const unsigned char *)replace_to_buf, replace_to_len, true);
    CHECK(textEquals("02/01/2024 then 31/12/2025"));
}

static void test_replaceAll_no_match(void) {
    setText("hello world");
    replaceAll((const unsigned char *)"zzz", 3, (const unsigned char *)"q", 1, false);
    CHECK(textEquals("hello world")); // untouched
}

// --- misc helpers ------------------------------------------------------
static void test_selection_helpers(void) {
    setText("abcdef");
    anchor = 4;
    cursor = 2;
    CHECK(hasSel());
    CHECK(selMin() == 2);
    CHECK(selMax() == 4);
    anchor = cursor;
    CHECK(!hasSel());
}

static void test_clampz_satsub(void) {
    CHECK(clampz(5, 0, 10) == 5);
    CHECK(clampz(15, 0, 10) == 10);
    CHECK(clampz(0, 2, 10) == 2);
    CHECK(satsub(5, 3) == 2);
    CHECK(satsub(3, 5) == 0);
}

// --- file I/O ------------------------------------------------------------
static void test_save_and_read_roundtrip(void) {
    char path[] = "/tmp/te_unit_test_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);

    setText("saved through the real file path\nwith two lines\n");
    setFilename(path, strlen(path));
    bool saved = saveFile();
    CHECK(saved);
    CHECK(!dirty);

    setText(""); // clobber in-memory state
    bool existed = readFileInto(path);
    CHECK(existed);
    CHECK(textEquals("saved through the real file path\nwith two lines\n"));

    unlink(path);
}

static void test_read_missing_file(void) {
    setText("unchanged marker");
    bool existed = readFileInto("/tmp/te_unit_test_does_not_exist_hopefully");
    CHECK(!existed);
    CHECK(len == 0); // readFileInto resets len on failure
}

// --- Lua scripting (src/script.c) ---------------------------------------
static void test_script_loads_and_runs_api(void) {
    reset();
    scriptInitFromFile("tests/fixtures/init_test.lua");
    // The fixture's own assert()s drive most of the checking; a failure
    // there would have replaced this echo with a "lua error: ..." message
    // instead of leaving the buffer/echo state below.
    CHECK(textEquals(""));
    CHECK(echo_len == strlen("hello from init.lua"));
    CHECK(memcmp(echo_buf, "hello from init.lua", echo_len) == 0);
    scriptShutdown();
}

static void test_script_error_is_echoed_not_fatal(void) {
    reset();
    scriptInitFromFile("tests/fixtures/init_test_bad.lua");
    CHECK(echo_len >= 9);
    CHECK(memcmp(echo_buf, "lua error", 9) == 0);
    scriptShutdown();
}

static void test_script_hooks_fire_on_save_and_open(void) {
    reset();
    scriptInitFromFile("tests/fixtures/init_hooks.lua");

    char path[] = "/tmp/te_unit_test_hooks_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);

    setText("hook test");
    setFilename(path, strlen(path));
    CHECK(saveFile());
    CHECK(echo_len == strlen("post-save fired"));
    CHECK(memcmp(echo_buf, "post-save fired", echo_len) == 0);

    openPath(path); // openPath's own "Opened ..." echo is overwritten by the hook
    CHECK(echo_len == strlen("post-open fired"));
    CHECK(memcmp(echo_buf, "post-open fired", echo_len) == 0);

    unlink(path);
    scriptShutdown();
}

int main(void) {
    RUN(test_utf8_roundtrip);
    RUN(test_utf8_malformed_falls_back);
    RUN(test_isCont);

    RUN(test_insert_into_empty);
    RUN(test_insert_at_cursor);
    RUN(test_insert_replaces_selection);
    RUN(test_deleteBack_ascii);
    RUN(test_deleteBack_utf8);
    RUN(test_deleteForward_ascii);
    RUN(test_deleteSelection);

    RUN(test_undo_redo_roundtrip);
    RUN(test_undo_coalesces_single_char_typing);
    RUN(test_undo_does_not_coalesce_across_newline);
    RUN(test_undo_multistep);

    RUN(test_line_boundaries);
    RUN(test_currentLineSpan);
    RUN(test_cursorLineCol);
    RUN(test_swapLine_down_and_up);

    RUN(test_word_movement);
    RUN(test_moveLeft_moveRight_utf8);

    RUN(test_computeMatches_literal);
    RUN(test_computeMatches_regex);
    RUN(test_computeMatches_bad_regex);
    RUN(test_gotoMatch);
    RUN(test_replaceAll_literal);
    RUN(test_replaceAll_regex_backref);
    RUN(test_replaceAll_no_match);

    RUN(test_selection_helpers);
    RUN(test_clampz_satsub);

    RUN(test_save_and_read_roundtrip);
    RUN(test_read_missing_file);

    RUN(test_script_loads_and_runs_api);
    RUN(test_script_error_is_echoed_not_fatal);
    RUN(test_script_hooks_fire_on_save_and_open);

    fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
