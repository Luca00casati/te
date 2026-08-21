// Prolog integration: an optional init.pl can define custom commands and
// rebind keys via Prolog facts/rules, the way .emacs/init.vim extend
// Emacs/Vim -- see src/prolog.h for the engine and docs/init.pl.example for
// the scripting surface. This is also the *sole* dispatch path now: te's
// own default bindings/commands (src/default_bindings.pl) are facts too,
// loaded after the user's init.pl so a user override at the same key/mod
// wins (findall tries clauses in assertion order; matchAndRun fires on the
// first match).
#ifndef TE_SCRIPT_H
#define TE_SCRIPT_H

#include <stdbool.h>
#include <stddef.h>
#include "binding.h"

// Creates the Prolog engine, registers the `te_*` native predicates, loads
// $XDG_CONFIG_HOME/te/init.pl (falling back to $HOME/.config/te/init.pl),
// then loads the built-in src/default_bindings.pl (baked into the binary --
// see build/default_bindings_pl.h). A missing init.pl is not an error; a
// script error is echoed to the status line but does not stop the editor
// from starting.
void scriptInit(void);

// Same as scriptInit, but loads the given path instead of the default
// config location, still followed by default_bindings.pl. For tests.
void scriptInitFromFile(const char *path);

void scriptShutdown(void);

// Solves key_binding(Key, Mod, Handler) (auto-repeat while held) and, if
// nothing matched, key_binding_once(Key, Mod, Handler) (fires once per
// press) fresh against this frame's input -- re-deriving the match every
// call rather than consulting a cached table, so a Handler can be a rule
// that itself consults live editor state. `ctrl`/`shift` are the raw
// modifier state; `modal` is modal (command) mode, which restricts a bare
// key (ctrl not actually held) to a te_action(Name) handler whose Name is a
// pure navigation action -- a custom (non-te_action) handler has no way to
// self-report that, so it's blocked outright in modal mode, same as a
// non-nav built-in always was. Returns true if a binding matched and ran.
bool scriptHandleKey(bool ctrl, bool shift, bool modal);

// Same idea, for leader_binding/3 once the prefix is armed. Ctrl is
// optional on the chord itself, only Shift narrows it; a chord never
// auto-repeats and modal mode doesn't affect it (arming the leader is
// already an explicit gesture).
bool scriptHandlePrefixKey(bool shift);

// Solves hook(Event, Handler) for this event name and runs every matching
// Handler, in registration order. A no-op if init.pl registered none.
// Errors are echoed, not fatal. `name` is a hyphenated C identifier (e.g.
// "post-save"); hyphens are translated to underscores before querying,
// since a bare Prolog atom can't contain one -- an init.pl file matches it
// as hook(post_save, ...).
void scriptRunHook(const char *name);

// Solves command(Name, Handler) for a name typed at the command prompt
// (leader/triple-tap-Ctrl, then a name + Enter) and runs Handler if found.
// Returns false (no-op) if there's no match, so the caller can report "no
// such command" itself.
bool scriptRunCommand(const char *name);

// Introspection for main.c's help overlay and command-prompt completion --
// not part of the scripting surface itself. Each *Count call re-queries and
// refreshes that kind's cache; the matching *Get calls then read from it,
// so call Count once before looping Get(0), Get(1), .... `label` is the
// te_action/1 name when the handler is te_action(Name)-shaped, else a
// best-effort fallback (the handler goal's own functor name).
size_t scriptTopBindingCount(void);
bool scriptTopBindingGet(size_t i, int *key, Mod *mod, const char **label);
size_t scriptLeaderBindingCount(void);
bool scriptLeaderBindingGet(size_t i, int *key, Mod *mod, const char **label);
size_t scriptCommandCount(void);
bool scriptCommandGet(size_t i, const char **name);

// --- undo/redo history (src/undo_history.pl) ------------------------------
// The bookkeeping -- what to remember, coalescing a run of typing into one
// step, evicting the oldest entry past editorUndoDepth() -- lives in
// Prolog; the actual buffer splice is native (te_replace_range/3, called
// from undo_step/redo_step). main.c's edit() calls scriptRecordEdit right
// after computing the same removed/inserted/cursor snapshot its old Edit
// struct held; doUndo/doRedo call scriptUndo/scriptRedo and keep only
// their cursor/dirty/echo bookkeeping.

// Records one edit for undo (pos, the bytes that left and entered the
// buffer, and the cursor before/after) -- also clears the redo history and
// evicts the oldest entry past the depth cap, same as the old pushUndo did.
void scriptRecordEdit(size_t pos, const unsigned char *removed, size_t removed_len,
                      const unsigned char *inserted, size_t inserted_len,
                      size_t cur_before, size_t cur_after);
// Undoes the most recent recorded edit (restoring its pre-edit cursor) and
// moves it to the redo history. Returns false if there's nothing to undo.
bool scriptUndo(void);
// Re-applies the most recently undone edit (restoring its post-edit cursor)
// and moves it back to the undo history. Returns false if there's nothing
// to redo.
bool scriptRedo(void);
// True if the undo history is now empty -- main.c checks this right after
// a successful scriptUndo() to decide whether to clear the dirty flag and
// echo "no more undo" again, matching the old undo_n==0 check.
bool scriptUndoStackEmpty(void);
// Resets both histories to empty, e.g. after opening a file or saving (the
// buffer on disk is the new baseline, so old undo/redo steps no longer apply).
void scriptClearHistory(void);

// --- search/replace (src/search.pl) ---------------------------------------
// PCRE2/memmem stay native (te_find_matches/3, te_regex_substitute/5 in
// script.c, wrapping main.c's findMatches/editorRegexSubstitute); this is
// the decision logic on top -- which match is selected, next/prev/replace,
// wrap-around, replace-all's ordering. main.c still owns the minibuffer
// widget itself (typing the query, Tab-complete, the modal shell) and
// resolving "what's the active query" (live prompt text vs. the last
// committed search), passing the resolved bytes in here as an argument.

void scriptStartSearch(bool is_regex, bool reverse, size_t origin);
void scriptStartReplace(bool is_regex, bool all_mode, size_t origin);
// Where the current session began -- main.c reads this back to restore the
// caret when a search/replace-pattern prompt is aborted (Esc).
size_t scriptSearchOrigin(void);
// Whether the current session searches backward -- main.c reads this back
// for empty-Enter repeat-search (which reverses direction each time).
bool scriptSearchReverse(void);
// Re-runs the search for `query` and jumps to the first match at/after (or,
// reversed, before) the session's origin, wrapping -- or back to the origin
// itself if the query is empty/invalid/matchless. Never fails.
void scriptSearchUpdate(const unsigned char *query, size_t len);
// Jumps to the next/previous match of `query` (wrapping). `query` is
// whatever main.c's activeQueryPtr()-equivalent resolved to: the live
// prompt text, or the last committed search if the prompt is empty.
void scriptSearchStep(const unsigned char *query, size_t len, bool forward);

// Enters the query-replace loop: records pattern/replacement/is_regex and
// jumps to the first match at/after the origin scriptStartReplace recorded
// (no wraparound). Returns false on a bad pattern or zero matches --
// scriptSearchStatus's bad_regex flag tells the caller which message to
// echo; either way the caller closes the minibuffer itself (no loop was
// entered on failure).
bool scriptEnterReplaceQuery(const unsigned char *pattern, size_t pattern_len,
                             const unsigned char *replacement, size_t replacement_len,
                             bool is_regex);
// n/p in the query-replace loop: step without replacing.
void scriptReplaceStep(bool forward);

typedef enum { REPLACE_STEP_OK, REPLACE_STEP_DONE, REPLACE_STEP_FAILED } ReplaceStepResult;
// Applies the current match, then advances to the next remaining one (past
// the insertion, so the replacement text is never re-matched). OK: applied
// and another match is now selected (read scriptSearchStatus for "Match
// i/N"). DONE: nothing left -- caller echoes "Replace done" and closes the
// loop. FAILED: substitution failed for this match -- caller echoes
// "Replace failed" and leaves the loop open (matches the old asymmetry).
ReplaceStepResult scriptReplaceCurrentMatch(void);
// Replaces every match of `pattern` with `replacement`, one native call per
// match (each gets its own undo step, same as ordinary typing). Returns the
// match count (0 if bad regex or no matches -- scriptSearchStatus's
// bad_regex flag disambiguates which). Caller should skip this call
// entirely for an empty pattern (silent no-op, matching the old code).
size_t scriptReplaceAll(const unsigned char *pattern, size_t pattern_len,
                        const unsigned char *replacement, size_t replacement_len,
                        bool is_regex);

// Read-back for the minibuffer draw code / echo messages -- re-queried
// fresh each call, same as scriptTopBindingCount and friends.
void scriptSearchStatus(size_t *index, size_t *count, bool *truncated, bool *bad_regex);
// Resets match/search/replace state to empty -- called when a new file is
// opened, alongside the existing scriptClearHistory() call.
void scriptClearSearch(void);

// --- cursor movement (src/movement.pl) ------------------------------------
// The raw UTF-8/line/column math stays native (see src/movement.pl's header
// comment); this is the decision logic on top -- which direction, how far,
// and the goal-column bookkeeping a run of up/down presses needs. main.c's
// applyAction calls these instead of the old static moveLeft/moveVertical/
// etc., keeping its switch statement's shape unchanged.

// Clears the goal column -- called by every non-vertical action (typing,
// deleting, jumping...) so a later up/down recomputes it from the new
// position rather than keeping a stale one.
void scriptClearGoalColumn(void);
void scriptMoveLeft(void);
void scriptMoveRight(void);
void scriptMoveWordStartLeft(void);
void scriptMoveWordStartRight(void);
void scriptMoveWordEndRight(void);
void scriptMoveHome(void);
void scriptMoveEnd(void);
void scriptMoveBufferStart(void);
void scriptMoveBufferEnd(void);
void scriptSelectAll(void);
// delta: -1 (up) or 1 (down). wrap/cols are main.c's `wrap` config and
// current view_cols -- Prolog doesn't track viewport state itself.
void scriptMoveVertical(int delta, bool wrap, size_t cols);
void scriptPageUp(bool wrap, size_t cols, size_t lines);
void scriptPageDown(bool wrap, size_t cols, size_t lines);

#endif // TE_SCRIPT_H
