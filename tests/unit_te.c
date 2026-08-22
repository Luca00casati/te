// Unit tests for te's core buffer/undo/search logic.
//
// main.c is one translation unit with every piece of state and every helper
// declared `static`, and no header exposes any of it. The only way to reach
// that logic without a large refactor is to #include the source directly, so
// its statics become ordinary file-scope symbols here. main() is renamed out
// of the way first so it is never linked in as `main` and never called --
// nothing in this file ever creates a GLFW window.
//
// platformTime()/platformKeyPressed() (called incidentally by edit()/echo())
// are safe to call pre-platformInit: every platform* query function is an
// explicit, documented no-op/safe-default before platformInit runs (see
// platform.h). Two things are NOT safe and are deliberately never exercised
// here: platformSetClipboardText/platformGetClipboardText (need a live GLFW
// window) and any codepoint outside inAtlas()'s ranges (glyphs_get() would
// try to create a GL texture). Tests below stick to ASCII/Latin/Greek/
// Cyrillic, which inAtlas() covers.
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
    wrap = true;
    view_cols = 80;
    search_bad_regex = false;
    match_count = 0;
    scriptClearSearch();
    scriptClearGoalColumn();
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

// deleteBack/deleteForward/deleteSelection's logic moved into src/editing.pl
// (delete_back/delete_forward, both routing through delete_selection when
// there's a selection) -- exercise it through runAction, same as
// test_word_movement below does for movement.pl.
static void test_deleteBack_ascii(void) {
    setText("abc");
    cursor = 3;
    anchor = 3;
    runAction(ACTION_DELETE_BACK);
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
    runAction(ACTION_DELETE_BACK);
    CHECK(textEquals("ab")); // the whole 2-byte sequence goes, not just its tail byte
    CHECK(cursor == 1);
}

static void test_deleteForward_ascii(void) {
    setText("abc");
    cursor = 0;
    anchor = 0;
    runAction(ACTION_DELETE_FORWARD);
    CHECK(textEquals("bc"));
    CHECK(cursor == 0);
}

static void test_deleteSelection(void) {
    setText("abcdef");
    anchor = 1;
    cursor = 4; // "bcd"
    runAction(ACTION_DELETE_BACK); // hasSel routes to delete_selection regardless of direction
    CHECK(textEquals("aef"));
    CHECK(cursor == 1);
}

// open_line_below/open_line_above (src/editing.pl): both insert a single
// newline, but land the cursor differently -- below, after it (on the new
// blank line, same as te_apply_replace's own default); above, an explicit
// te_set_cursor override puts it *before* the newline, onto the fresh blank
// line above instead.
static void test_openLineBelowAbove(void) {
    setText("abc\ndef");
    cursor = 1;
    anchor = 1; // inside "abc"
    runAction(ACTION_OPEN_LINE_BELOW);
    CHECK(textEquals("abc\n\ndef"));
    CHECK(cursor == 4);

    setText("abc\ndef");
    cursor = 1;
    anchor = 1;
    runAction(ACTION_OPEN_LINE_ABOVE);
    CHECK(textEquals("\nabc\ndef"));
    CHECK(cursor == 0);
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
    doUndo();
    CHECK(textEquals("")); // three keystrokes coalesced into one undo step
    CHECK(scriptUndoStackEmpty());
}

static void test_undo_does_not_coalesce_across_newline(void) {
    setText("");
    insertBytes((const unsigned char *)"a", 1);
    insertBytes((const unsigned char *)"\n", 1);
    doUndo();
    CHECK(textEquals("a")); // newline breaks the coalescing run: one undo only removes it
    CHECK(!scriptUndoStackEmpty());
    doUndo();
    CHECK(textEquals(""));
    CHECK(scriptUndoStackEmpty());
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
    CHECK(scriptUndoStackEmpty());
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

// currentLineSpan's logic moved into src/editing.pl (current_line_span,
// used by select_line/cut_line/copy_line) -- exercise it through
// ACTION_SELECT_LINE, which sets anchor/cursor to exactly that span.
static void test_selectLine(void) {
    setText("abc\ndef\nghi");
    cursor = 5; // inside "def"
    runAction(ACTION_SELECT_LINE);
    CHECK(anchor == 4 && cursor == 8); // includes the trailing newline
    cursor = 9; // inside the last line, no trailing newline
    runAction(ACTION_SELECT_LINE);
    CHECK(anchor == 8 && cursor == 11);
}

static void test_cursorLineCol(void) {
    setText("abc\nde");
    cursor = 5; // "abc\nd|e" -- after 'd' on the second line
    LineCol lc = cursorLineCol();
    CHECK(lc.line == 1 && lc.col == 1);
}

// swapLine's logic moved into src/editing.pl (swap_line_down/swap_line_up,
// via move_line_down/move_line_up) -- exercise it through runAction.
static void test_swapLine_down_and_up(void) {
    setText("one\ntwo\nthree");
    cursor = 1; // inside "one"
    anchor = 1;
    runAction(ACTION_MOVE_LINE_DOWN); // swap with "two"
    CHECK(textEquals("two\none\nthree"));
    runAction(ACTION_MOVE_LINE_UP); // swap back
    CHECK(textEquals("one\ntwo\nthree"));
}

// move_line_left/move_line_right (src/editing.pl): outdent removes one
// leading space/tab (a no-op if the line doesn't start with one), indent
// always adds one leading space; both keep the cursor's position relative
// to the rest of the line rather than te_apply_replace's own default.
static void test_moveLineLeftRight(void) {
    setText("  abc");
    cursor = 4;
    anchor = 4;
    runAction(ACTION_MOVE_LINE_LEFT);
    CHECK(textEquals(" abc"));
    CHECK(cursor == 3);
    runAction(ACTION_MOVE_LINE_RIGHT);
    CHECK(textEquals("  abc"));
    CHECK(cursor == 4);

    setText("abc"); // no leading space/tab: outdent is a no-op
    cursor = 1;
    anchor = 1;
    runAction(ACTION_MOVE_LINE_LEFT);
    CHECK(textEquals("abc"));
    CHECK(cursor == 1);
}

// --- word movement -----------------------------------------------------
// moveWordStartRight/moveWordEndRight/moveWordStartLeft's logic moved into
// src/movement.pl (move_word_start_right/move_word_end_right/
// move_word_start_left) -- exercise it through the public runAction entry
// point, same as production code does.
static void test_word_movement(void) {
    setText("foo bar_baz  qux");
    cursor = 0;
    runAction(ACTION_MOVE_WORD_START_RIGHT);
    CHECK(cursor == 4); // start of "bar_baz" ('_' is a word char)
    runAction(ACTION_MOVE_WORD_START_RIGHT);
    CHECK(cursor == 13); // start of "qux", past the double space

    setText("foo bar");
    cursor = 0;
    runAction(ACTION_MOVE_WORD_END_RIGHT);
    CHECK(cursor == 3); // end of "foo"

    setText("foo bar");
    cursor = 7; // end of buffer
    runAction(ACTION_MOVE_WORD_START_LEFT);
    CHECK(cursor == 4); // start of "bar"
}

// moveLeft/moveRight's logic moved into src/movement.pl (move_left/
// move_right) -- exercise it through runAction, same as above.
static void test_moveLeft_moveRight_utf8(void) {
    unsigned char enc[4];
    size_t n = utf8Encode(0xE9, enc); // U+00E9, 2 bytes
    setText("");
    text[0] = 'a';
    memcpy(text + 1, enc, n);
    text[1 + n] = 'b';
    len = 2 + n;
    cursor = 0;
    runAction(ACTION_MOVE_RIGHT); // over 'a'
    CHECK(cursor == 1);
    runAction(ACTION_MOVE_RIGHT); // over the whole 2-byte codepoint, not just one byte
    CHECK(cursor == 1 + n);
    runAction(ACTION_MOVE_LEFT); // back over the codepoint
    CHECK(cursor == 1);
}

// --- search & replace --------------------------------------------------
static void test_findMatches_literal(void) {
    setText("the cat sat on the mat");
    findMatches((const unsigned char *)"at", 2, false);
    CHECK(match_count == 3); // c[at], s[at], m[at]
    CHECK(match_starts[0] == 5);
}

static void test_findMatches_regex(void) {
    setText("foo1 bar22 baz333");
    findMatches((const unsigned char *)"[0-9]+", 6, true);
    CHECK(!search_bad_regex);
    CHECK(match_count == 3);
    CHECK(match_lens[2] == 3); // "333"
}

static void test_findMatches_bad_regex(void) {
    setText("anything");
    findMatches((const unsigned char *)"(unterminated", 14, true);
    CHECK(search_bad_regex);
    CHECK(match_count == 0);
}

// goto_match itself lives in src/search.pl now (reached via
// scriptSearchUpdate/scriptSearchStep) -- exercise it end to end via the
// public anchor/cursor state, the same thing the old gotoMatch set.
static void test_gotoMatch(void) {
    setText("aXaXa");
    scriptStartSearch(false, false, 0);
    scriptSearchUpdate((const unsigned char *)"X", 1);
    CHECK(anchor == 1 && cursor == 2); // first X, byte 1
    scriptSearchStep((const unsigned char *)"X", 1, true);
    CHECK(anchor == 3 && cursor == 4); // second X, byte 3
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

// --- Prolog scripting (src/script.c) -------------------------------------
static void test_script_loads_and_runs_api(void) {
    reset();
    scriptInitFromFile("tests/fixtures/init_test.pl");
    // The fixture's own checks (throw/1 on failure) drive most of the
    // verification; a failure there would have replaced this echo with a
    // "prolog error: ..." message instead of leaving the buffer/echo state
    // below.
    CHECK(textEquals(""));
    CHECK(echo_len == strlen("hello from init.pl"));
    CHECK(memcmp(echo_buf, "hello from init.pl", echo_len) == 0);
    scriptShutdown();
}

static void test_script_error_is_echoed_not_fatal(void) {
    reset();
    scriptInitFromFile("tests/fixtures/init_test_bad.pl");
    CHECK(echo_len >= 12);
    CHECK(memcmp(echo_buf, "prolog error", 12) == 0);
    scriptShutdown();
}

static void test_script_hooks_fire_on_save_and_open(void) {
    reset();
    scriptInitFromFile("tests/fixtures/init_hooks.pl");

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
    // Undo/redo now goes through the Prolog engine (src/undo_history.pl),
    // so it needs one alive -- a nonexistent init.pl still loads
    // bootstrap.pl/default_bindings.pl/undo_history.pl, which is all any
    // test below the script-specific ones needs. Torn down again right
    // before those (each sets up its own fixture-specific engine).
    scriptInitFromFile("tests/fixtures/does_not_exist.pl");

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
    RUN(test_openLineBelowAbove);

    RUN(test_undo_redo_roundtrip);
    RUN(test_undo_coalesces_single_char_typing);
    RUN(test_undo_does_not_coalesce_across_newline);
    RUN(test_undo_multistep);

    RUN(test_line_boundaries);
    RUN(test_selectLine);
    RUN(test_cursorLineCol);
    RUN(test_swapLine_down_and_up);
    RUN(test_moveLineLeftRight);

    RUN(test_word_movement);
    RUN(test_moveLeft_moveRight_utf8);

    RUN(test_findMatches_literal);
    RUN(test_findMatches_regex);
    RUN(test_findMatches_bad_regex);
    RUN(test_gotoMatch);
    RUN(test_replaceAll_literal);
    RUN(test_replaceAll_regex_backref);
    RUN(test_replaceAll_no_match);

    RUN(test_selection_helpers);
    RUN(test_clampz_satsub);

    RUN(test_save_and_read_roundtrip);
    RUN(test_read_missing_file);

    scriptShutdown(); // the script-specific tests below set up their own engine each
    RUN(test_script_loads_and_runs_api);
    RUN(test_script_error_is_echoed_not_fatal);
    RUN(test_script_hooks_fire_on_save_and_open);

    fprintf(stderr, "%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
