// Prolog integration: an optional init.pl can define custom commands and
// rebind keys via Prolog facts/rules, the way .emacs/init.vim extend
// Emacs/Vim -- see src/prolog.h for the engine and docs/init.pl.example for
// the scripting surface.
#ifndef TE_SCRIPT_H
#define TE_SCRIPT_H

#include <stdbool.h>

// Creates the Prolog engine, registers the `te_*` native predicates, and
// loads $XDG_CONFIG_HOME/te/init.pl (falling back to $HOME/.config/te/init.pl).
// A missing init file is not an error; a script error is echoed to the
// status line but does not stop the editor from starting.
void scriptInit(void);

// Same as scriptInit, but loads the given path instead of the default
// config location. For tests.
void scriptInitFromFile(const char *path);

void scriptShutdown(void);

// Solves key_binding(Key, Mod, Handler) fresh against this frame's input,
// re-deriving the match every call rather than consulting a cached table --
// a Handler can be a rule that itself consults live editor state. `cmd`/
// `shift` are the current modifier state, mirroring handleInput's own
// matching. Returns true if a binding matched and ran.
bool scriptHandleKey(bool cmd, bool shift);

// Same idea, for leader_binding/3 once the prefix is armed. Mirrors
// PREFIX_BINDINGS: Ctrl is optional, only Shift is checked.
bool scriptHandlePrefixKey(bool shift);

// Solves hook(Event, Handler) for this event name and runs every matching
// Handler, in registration order. A no-op if init.pl registered none.
// Errors are echoed, not fatal. `name` is a hyphenated C identifier (e.g.
// "post-save"); hyphens are translated to underscores before querying,
// since a bare Prolog atom can't contain one -- an init.pl file matches it
// as hook(post_save, ...).
void scriptRunHook(const char *name);

#endif // TE_SCRIPT_H
