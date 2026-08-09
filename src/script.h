// Lua integration: an optional init.lua can define custom commands and
// rebind keys to Lua functions, the way .emacs/init.vim extend Emacs/Vim.
#ifndef TE_SCRIPT_H
#define TE_SCRIPT_H

#include <stdbool.h>

// Creates the Lua state, registers the `te` API table, and loads
// $XDG_CONFIG_HOME/te/init.lua (falling back to $HOME/.config/te/init.lua).
// A missing init file is not an error; a script error is echoed to the
// status line but does not stop the editor from starting.
void scriptInit(void);

// Same as scriptInit, but loads the given path instead of the default
// config location. For tests.
void scriptInitFromFile(const char *path);

void scriptShutdown(void);

// Checks script-registered key bindings (te.bind) against this frame's
// input and runs the first match. `cmd` mirrors handleInput's ctrl-or-modal
// flag; `shift` is the current Shift state. Returns true if a binding
// matched and ran, so the caller can skip the built-in BINDINGS table.
bool scriptHandleKey(bool cmd, bool shift);

// Same idea, for leader-chord bindings (te.bind_leader) once the prefix is
// armed. Mirrors PREFIX_BINDINGS: Ctrl is optional, only Shift is checked.
bool scriptHandlePrefixKey(bool shift);

// Runs every Lua function registered via te.on(name, fn) for this event
// name, in registration order. A no-op if init.lua registered none. Errors
// are echoed, not fatal. Current events: "post-save", "post-open".
void scriptRunHook(const char *name);

#endif // TE_SCRIPT_H
