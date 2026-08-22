// The surface main.c exposes to src/script.c (the Prolog integration), so
// scripts can run built-in actions and touch the buffer without script.c
// reaching into main.c's static state directly.
#ifndef TE_EDITOR_H
#define TE_EDITOR_H

#include <stddef.h>
#include "binding.h"

void editorRunAction(Action action);   // repeat-count aware, like a typed key
void editorApplyAction(Action action); // runs once, like a leader chord
void editorEcho(const char *msg);
void editorInsertText(const unsigned char *bytes, size_t n);
const unsigned char *editorGetText(size_t *out_len);
size_t editorGetCursor(void);
void editorSetCursor(size_t pos);
// The other end of the current selection (== cursor when there's no
// selection) -- src/search.pl reads this alongside the cursor to tell
// whether the current selection *is* a particular match, for stepping
// next/prev relative to "the match currently selected" vs. "nothing
// selected yet, jump from the raw cursor position" (see editorSetSelection,
// which is the only way to set this from script.c).
size_t editorGetAnchor(void);

// Modal mode (bare keys act like their Ctrl-chord) restricts bare keys to
// pure navigation, same as the old native BINDINGS table's modal check did
// -- script.c consults this so a te_action(Name)-shaped key_binding still
// gets that restriction. A custom (non-te_action) handler has no comparable
// way to self-report "this is just navigation", so script.c blocks those
// outright in modal mode rather than guessing.
bool editorIsNavAction(Action action);
// Mark (Ctrl-Space): while active, movement extends the selection.
bool editorGetMarkActive(void);
// Whether the action script.c is about to run should extend the current
// selection (mark active, or a plain Shift+navigation key) -- script.c
// computes this from the matched binding's Mod and calls it right before
// running a te_action(Name) handler, same computation the old native
// BINDINGS loop did inline.
void editorSetSelExtend(bool extend);

// Raw byte-level buffer splice, no undo bookkeeping -- src/undo_history.pl
// is the only caller (via te_replace_range/3), for undo_step/redo_step
// actually applying a stored edit record. Mirrors the existing static
// replaceRange in main.c exactly.
void editorReplaceRange(size_t start, size_t end, const unsigned char *bytes, size_t bytes_len);
// CFG_UNDO_DEPTH (src/config.h), exposed so src/undo_history.pl's eviction
// cap has one source of truth instead of a second copy of the number.
size_t editorUndoDepth(void);

// Selects [anchorPos, cursorPos) as the current match/selection span: sets
// both anchor and cursor and resets the cursor-blink clock -- exactly what
// the old static gotoMatch did (the goal-column reset it also did is now
// src/search.pl's goto_match/1 calling src/movement.pl's clear_goal_col
// directly, since goal-column state lives in Prolog facts now). Distinct
// from editorSetCursor, which doesn't touch anchor.
void editorSetSelection(size_t anchor_pos, size_t cursor_pos);
// Applies a replacement through the real edit() primitive (undo-recording,
// same as typing), for an arbitrary [start, end) range rather than the
// current cursor/selection -- src/search.pl's replace-all/query-replace use
// this so every applied match gets its own undo step for free, the same way
// ordinary typing does. Distinct from editorReplaceRange, which is
// undo-free and reserved for undo_step/redo_step themselves.
void editorApplyReplace(size_t start, size_t end, const unsigned char *bytes, size_t bytes_len);
// Copies text[start,end) to the system clipboard, no-op if end <= start --
// mirrors the old static copyRange exactly. Kept native (rather than routing
// a substring through Prolog as a code list, the way src/editing.pl's
// swap_line does for single-line-sized reads) since a selection can span the
// whole buffer (e.g. select-all then copy).
void editorCopyRange(size_t start, size_t end);

// --- search (src/search.pl) -------------------------------------------------
// PCRE2 (regex) and memmem (literal) are C-only, so the actual pattern
// matching stays a native primitive; src/search.pl is the decision logic on
// top (which match is selected, next/prev, replace-all's ordering). Also
// used directly by main.c's headless `--regex` grep path, which runs before
// any Prolog engine exists -- editorFindMatches must not depend on one.

// Recomputes the match set for `query` against the whole buffer into
// internal scratch storage read back via editorMatchCount/Truncated/Get.
// Regex via PCRE2 if `is_regex`, else a literal memmem scan. Returns false
// if the pattern failed to compile (regex only) -- editorMatchCount() is 0
// in that case too, but the two aren't the same thing (a valid pattern can
// legitimately have zero matches).
bool editorFindMatches(const unsigned char *query, size_t query_len, bool is_regex);
size_t editorMatchCount(void);
bool editorMatchTruncated(void); // true if there were more than the cap allows
void editorMatchGet(size_t i, size_t *start, size_t *end);

// Regex substitution of `replacement` (which may contain PCRE2 backreferences
// like $1/$0) against one already-matched span [start,end) of the buffer --
// used for a regex replace's per-match expansion. `out` receives a pointer
// valid until the next call (internal scratch buffer); returns false on a
// bad pattern or an oversized expansion.
bool editorRegexSubstitute(const unsigned char *pattern, size_t pattern_len,
                           size_t match_start, size_t match_end,
                           const unsigned char *replacement, size_t replacement_len,
                           const unsigned char **out, size_t *out_len);

// --- movement (src/movement.pl) -------------------------------------------
// lineStart/lineEnd/colsIn/byteAtCol/visRows are also called every *frame*
// by rendering/scrolling (wrapped-line layout, scroll-to-cursor, mouse
// click->text mapping) -- these wrappers don't change that, they just give
// src/movement.pl a way to call the same pure functions once per keypress,
// the same dual-use pattern editorFindMatches/findMatches already has.

// Byte offset of the start/end of the logical line containing `pos`.
size_t editorLineStart(size_t pos);
size_t editorLineEnd(size_t pos);
// Sum of display columns (UTF-8/full-width-aware) in [start, end).
size_t editorColsIn(size_t start, size_t end);
// Byte offset of the `col`-th display column within [start, stop) -- never
// splits a full-width glyph.
size_t editorByteAtCol(size_t start, size_t stop, size_t col);
// Visual rows a logical line of `line_cols` display columns occupies at
// `cols` columns wide (>=1, so an empty line still takes one row).
size_t editorVisRows(size_t line_cols, size_t cols);

// One codepoint left/right of `pos`, skipping UTF-8 continuation bytes --
// pure position in/out versions of the old moveLeft/moveRight (which
// mutated the cursor directly); src/movement.pl decides what to do with
// the result.
size_t editorStepLeft(size_t pos);
size_t editorStepRight(size_t pos);
// Start of the previous word / start of the next word / end of the next
// word from `pos` -- pure versions of the old moveWordStartLeft/Right and
// moveWordEndRight.
size_t editorWordStartLeft(size_t pos);
size_t editorWordStartRight(size_t pos);
size_t editorWordEndRight(size_t pos);

// Current viewport metrics (recomputed every frame from the window size) --
// so page-up/down and wrap-aware vertical movement know the viewport
// without main.c threading it through every call.
size_t editorViewCols(void);
size_t editorPageLines(void);
// Total buffer length -- cheaper than editorGetText when only the length
// is needed (e.g. move-buffer-end, select-all).
size_t editorBufferLen(void);

// --- buffers (src/buffers.pl) ----------------------------------------------
// The selected window's buffer's id -- stable for the buffer's lifetime,
// Prolog-facing identity for buffer-local variables (src/buffers.pl's
// blocal_get/2, blocal_set/2) and everything below that needs to name a
// specific buffer.
int editorCurrentBufferId(void);

// Structural buffer operations -- memory-safety-bearing (create/free a
// Buffer, keep every Window's `buf` pointer valid), so these stay native
// rather than Prolog; src/buffers.pl is the policy on top (switch_buffer/1,
// open_file/1's reuse-if-already-open, next_buffer/0, kill_buffer/1).
int editorBufferCreate(void); // a blank scratch buffer, no file
// Reads `path` into a *new* buffer (like the old single-buffer openPath: a
// missing file is fine, becomes an empty new-file buffer, not an error)
// without touching whatever's currently on screen. Returns the new id.
int editorBufferOpenFile(const char *path);
// The id of a live buffer already showing `path`, or -1 if none does.
int editorBufferFindByPath(const char *path);
// Frees the buffer, repointing any window that showed it (auto-creating a
// fresh scratch buffer if it was the last one). False if `id` isn't live.
bool editorBufferKill(int id);
size_t editorBufferCount(void);
int editorBufferIdAt(size_t index); // 0-based, creation order; -1 out of range
const char *editorBufferName(int id); // NULL if `id` isn't live
bool editorBufferFilename(int id, const char **out_path, bool *out_has_file);
bool editorBufferDirty(int id, bool *out);
bool editorBufferSave(int id);

// --- windows (src/windows.pl) -----------------------------------------------
// Only the addressing surface switch_buffer/1 needs exists yet (there's
// only ever one window, root_window, until a later commit adds real
// splitting) -- split/close/list/delete-others land alongside that.
int editorSelectedWindowId(void);
bool editorSelectWindow(int id);
int editorWindowBufferId(int win_id);
bool editorWindowSetBuffer(int win_id, int buf_id);

#endif // TE_EDITOR_H
