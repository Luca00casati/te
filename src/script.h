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

#endif // TE_SCRIPT_H
