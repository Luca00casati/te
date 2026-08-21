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
// both anchor and cursor, clears the remembered goal column (up/down
// navigation shouldn't snap back to a column from before the jump), and
// resets the cursor-blink clock -- exactly what the old static gotoMatch did.
// Distinct from editorSetCursor, which doesn't touch anchor or goal_col_set.
void editorSetSelection(size_t anchor_pos, size_t cursor_pos);
// Applies a replacement through the real edit() primitive (undo-recording,
// same as typing), for an arbitrary [start, end) range rather than the
// current cursor/selection -- src/search.pl's replace-all/query-replace use
// this so every applied match gets its own undo step for free, the same way
// ordinary typing does. Distinct from editorReplaceRange, which is
// undo-free and reserved for undo_step/redo_step themselves.
void editorApplyReplace(size_t start, size_t end, const unsigned char *bytes, size_t bytes_len);

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

#endif // TE_EDITOR_H
