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

#endif // TE_SCRIPT_H
