// Key bindings -- map keys (and modifiers) to editor actions in one place.
// Edit the BINDINGS table to rebind; main.c dispatches each Action.
//
// Text typing, selection-with-Shift, and the mouse are handled directly in
// main.c (they aren't single key -> action mappings).
#ifndef TE_BINDING_H
#define TE_BINDING_H

#include <stddef.h>
#include "raylib.h"

// Things the editor can do in response to a key. main.c implements each.
typedef enum {
    ACTION_NEWLINE,
    ACTION_OPEN_LINE_BELOW,       // C-Enter: blank line below, cursor moves there
    ACTION_OPEN_LINE_ABOVE,       // C-Shift-Enter: blank line above, cursor moves there
    ACTION_INDENT,                // insert CFG_TAB
    ACTION_DELETE_BACK,
    ACTION_DELETE_FORWARD,
    ACTION_MOVE_LEFT,
    ACTION_MOVE_RIGHT,
    ACTION_MOVE_UP,
    ACTION_MOVE_DOWN,
    ACTION_MOVE_HOME,
    ACTION_MOVE_END,
    ACTION_MOVE_BUFFER_START,
    ACTION_MOVE_BUFFER_END,
    ACTION_MOVE_WORD_START_LEFT,
    ACTION_MOVE_WORD_START_RIGHT,
    ACTION_MOVE_WORD_END_RIGHT,
    ACTION_PAGE_UP,
    ACTION_PAGE_DOWN,
    ACTION_SELECT_ALL,
    ACTION_UNDO,
    ACTION_REDO,
    ACTION_COPY,
    ACTION_CUT,
    ACTION_PASTE,
    // whole-line operations
    ACTION_MOVE_LINE_LEFT,  // shift line left one space (outdent)
    ACTION_MOVE_LINE_RIGHT, // shift line right one space (indent)
    ACTION_MOVE_LINE_UP,    // swap with the line above
    ACTION_MOVE_LINE_DOWN,  // swap with the line below
    ACTION_CUT_LINE,
    ACTION_COPY_LINE,
    ACTION_PASTE_LINE,
    ACTION_SELECT_LINE,
    ACTION_SAVE,
    ACTION_SAVE_AS,               // prompt for a path in the minibuffer (C-x C-w in Emacs)
    ACTION_OPEN,                  // find-file via the minibuffer (C-x C-f)
    ACTION_SEARCH,                // search via the minibuffer (C-s)
    ACTION_SEARCH_REGEX,          // PCRE regex search (C-S-s)
    ACTION_SEARCH_REVERSE,        // backward search (command only)
    ACTION_SEARCH_REGEX_REVERSE,  // backward regex search (command only)
    ACTION_REPLACE,               // interactive query-replace (C-r)
    ACTION_REPLACE_REGEX,         // interactive regex query-replace (C-S-r)
    ACTION_REPLACE_ALL,           // replace every match at once (command only)
    ACTION_REPLACE_ALL_REGEX,     // replace every regex match at once (command only)
    ACTION_TOGGLE_WRAP,           // soft line wrapping on/off
    ACTION_QUIT,
    ACTION_COUNT,
} Action;

typedef enum {
    MOD_ANY,        // fires regardless of modifier state
    MOD_CTRL,       // requires Ctrl held (and not Shift)
    MOD_CTRL_SHIFT, // requires Ctrl+Shift held
} Mod;

// Shared by every table that matches a Binding's `mod` against live input:
// the native BINDINGS/PREFIX_BINDINGS loops in main.c, and their
// script-bound counterparts in script.c.
//
// modMatchesKey: a top-level key (handleInput/scriptHandleKey). `cmd` is
// Ctrl-or-modal-mode; MOD_CTRL/MOD_CTRL_SHIFT both require it held.
static inline bool modMatchesKey(Mod mod, bool cmd, bool shift) {
    switch (mod) {
        case MOD_ANY: return true;
        case MOD_CTRL: return cmd && !shift;
        case MOD_CTRL_SHIFT: return cmd && shift;
        default: return false;
    }
}
// modMatchesChord: a leader chord (handlePrefix/scriptHandlePrefixKey), once
// the prefix is armed -- Ctrl is optional there, so only Shift narrows it.
static inline bool modMatchesChord(Mod mod, bool shift) {
    switch (mod) {
        case MOD_ANY: return true;
        case MOD_CTRL: return !shift;
        case MOD_CTRL_SHIFT: return shift;
        default: return false;
    }
}

typedef struct {
    int key;
    Action action;
    Mod mod;
    bool repeat; // also fire while the key is held (keyboard autorepeat)
} Binding;

// A named command, reachable via the leader then typing its name.
typedef struct {
    const char *name;
    Action action;
} Command;

// The prefix (leader) is armed by double-tapping Ctrl (see detectCtrlTaps in
// main.c). Once armed, either type a command name (see COMMANDS) and Enter,
// or press one of the PREFIX_BINDINGS chords for a direct shortcut. A third
// Ctrl tap opens the command-name prompt directly.

// Chords reachable after the leader (e.g. double-tap Ctrl, then C-s -> save).
// The second key carries its own modifier, so these are matched while the
// prefix is pending.
static const Binding PREFIX_BINDINGS[] = {
    { KEY_S, ACTION_SAVE, MOD_ANY, true },
    { KEY_W, ACTION_SAVE_AS, MOD_ANY, true },
    { KEY_O, ACTION_OPEN, MOD_ANY, true },
    { KEY_A, ACTION_SELECT_ALL, MOD_ANY, true },
    { KEY_SPACE, ACTION_SELECT_LINE, MOD_ANY, true },
    { KEY_Q, ACTION_QUIT, MOD_ANY, true },
};
static const size_t PREFIX_BINDINGS_COUNT = sizeof(PREFIX_BINDINGS) / sizeof(PREFIX_BINDINGS[0]);

static const Binding BINDINGS[] = {
    // editing / navigation
    { KEY_ENTER, ACTION_NEWLINE, MOD_ANY, true },
    { KEY_KP_ENTER, ACTION_NEWLINE, MOD_ANY, true },
    { KEY_TAB, ACTION_INDENT, MOD_ANY, true },
    { KEY_BACKSPACE, ACTION_DELETE_BACK, MOD_ANY, true },
    { KEY_BACKSPACE, ACTION_DELETE_FORWARD, MOD_CTRL, true },
    { KEY_DELETE, ACTION_DELETE_FORWARD, MOD_ANY, true },
    { KEY_H, ACTION_MOVE_LEFT, MOD_CTRL, true },
    { KEY_L, ACTION_MOVE_RIGHT, MOD_CTRL, true },
    { KEY_K, ACTION_MOVE_UP, MOD_CTRL, true },
    { KEY_J, ACTION_MOVE_DOWN, MOD_CTRL, true },
    { KEY_A, ACTION_MOVE_HOME, MOD_CTRL, true },
    { KEY_E, ACTION_MOVE_END, MOD_CTRL, true },
    { KEY_A, ACTION_MOVE_BUFFER_START, MOD_CTRL_SHIFT, true },
    { KEY_E, ACTION_MOVE_BUFFER_END, MOD_CTRL_SHIFT, true },
    { KEY_W, ACTION_MOVE_WORD_START_RIGHT, MOD_CTRL, true },
    { KEY_W, ACTION_MOVE_WORD_START_LEFT, MOD_CTRL_SHIFT, true },
    { KEY_D, ACTION_MOVE_WORD_END_RIGHT, MOD_CTRL, true },
    // whole-line moves: Ctrl+Shift+ f/b shift the line, n/p reorder it
    { KEY_H, ACTION_MOVE_LINE_LEFT, MOD_CTRL_SHIFT, true },
    { KEY_L, ACTION_MOVE_LINE_RIGHT, MOD_CTRL_SHIFT, true },
    { KEY_J, ACTION_MOVE_LINE_DOWN, MOD_CTRL_SHIFT, true },
    { KEY_K, ACTION_MOVE_LINE_UP, MOD_CTRL_SHIFT, true },
    { KEY_F, ACTION_PAGE_UP, MOD_CTRL, true },
    { KEY_F, ACTION_PAGE_DOWN, MOD_CTRL_SHIFT, true },
    // shortcuts
    { KEY_Z, ACTION_UNDO, MOD_CTRL, true },
    { KEY_Z, ACTION_REDO, MOD_CTRL_SHIFT, true },
    { KEY_C, ACTION_COPY, MOD_CTRL, false },
    { KEY_X, ACTION_CUT, MOD_CTRL, false },
    { KEY_V, ACTION_PASTE, MOD_CTRL, false },
    // Ctrl+Shift+ x/c/v: cut/copy/paste the whole current line
    { KEY_X, ACTION_CUT_LINE, MOD_CTRL_SHIFT, false },
    { KEY_C, ACTION_COPY_LINE, MOD_CTRL_SHIFT, false },
    { KEY_V, ACTION_PASTE_LINE, MOD_CTRL_SHIFT, false },
    { KEY_S, ACTION_SEARCH, MOD_CTRL, false },
    { KEY_S, ACTION_SEARCH_REGEX, MOD_CTRL_SHIFT, false },
    { KEY_R, ACTION_REPLACE, MOD_CTRL, false },
    { KEY_R, ACTION_REPLACE_REGEX, MOD_CTRL_SHIFT, false },
    { KEY_Q, ACTION_QUIT, MOD_CTRL, false },
};
static const size_t BINDINGS_COUNT = sizeof(BINDINGS) / sizeof(BINDINGS[0]);

// Actions reachable as typed commands (leader, then the name + Enter). Each
// name matches its action (underscores as hyphens) so there's no drift
// between an action and how you type it.
static const Command COMMANDS[] = {
    { "save", ACTION_SAVE },
    { "save-as", ACTION_SAVE_AS },
    { "open", ACTION_OPEN },
    { "search", ACTION_SEARCH },
    { "search-regex", ACTION_SEARCH_REGEX },
    { "search-reverse", ACTION_SEARCH_REVERSE },
    { "search-regex-reverse", ACTION_SEARCH_REGEX_REVERSE },
    { "replace", ACTION_REPLACE },
    { "replace-regex", ACTION_REPLACE_REGEX },
    { "replace-all", ACTION_REPLACE_ALL },
    { "replace-all-regex", ACTION_REPLACE_ALL_REGEX },
    { "undo", ACTION_UNDO },
    { "redo", ACTION_REDO },
    { "copy", ACTION_COPY },
    { "cut", ACTION_CUT },
    { "paste", ACTION_PASTE },
    { "select-all", ACTION_SELECT_ALL },
    { "toggle-wrap", ACTION_TOGGLE_WRAP },
    { "quit", ACTION_QUIT },
};
static const size_t COMMANDS_COUNT = sizeof(COMMANDS) / sizeof(COMMANDS[0]);

// Human-readable label per action (tag with '_' -> ' '), used for the echo
// area and the help overlay. Indexed by Action.
static const char *const ACTION_LABELS[ACTION_COUNT] = {
    [ACTION_NEWLINE] = "newline",
    [ACTION_OPEN_LINE_BELOW] = "open line below",
    [ACTION_OPEN_LINE_ABOVE] = "open line above",
    [ACTION_INDENT] = "indent",
    [ACTION_DELETE_BACK] = "delete back",
    [ACTION_DELETE_FORWARD] = "delete forward",
    [ACTION_MOVE_LEFT] = "move left",
    [ACTION_MOVE_RIGHT] = "move right",
    [ACTION_MOVE_UP] = "move up",
    [ACTION_MOVE_DOWN] = "move down",
    [ACTION_MOVE_HOME] = "move home",
    [ACTION_MOVE_END] = "move end",
    [ACTION_MOVE_BUFFER_START] = "move buffer start",
    [ACTION_MOVE_BUFFER_END] = "move buffer end",
    [ACTION_MOVE_WORD_START_LEFT] = "move word start left",
    [ACTION_MOVE_WORD_START_RIGHT] = "move word start right",
    [ACTION_MOVE_WORD_END_RIGHT] = "move word end right",
    [ACTION_PAGE_UP] = "page up",
    [ACTION_PAGE_DOWN] = "page down",
    [ACTION_SELECT_ALL] = "select all",
    [ACTION_UNDO] = "undo",
    [ACTION_REDO] = "redo",
    [ACTION_COPY] = "copy",
    [ACTION_CUT] = "cut",
    [ACTION_PASTE] = "paste",
    [ACTION_MOVE_LINE_LEFT] = "move line left",
    [ACTION_MOVE_LINE_RIGHT] = "move line right",
    [ACTION_MOVE_LINE_UP] = "move line up",
    [ACTION_MOVE_LINE_DOWN] = "move line down",
    [ACTION_CUT_LINE] = "cut line",
    [ACTION_COPY_LINE] = "copy line",
    [ACTION_PASTE_LINE] = "paste line",
    [ACTION_SELECT_LINE] = "select line",
    [ACTION_SAVE] = "save",
    [ACTION_SAVE_AS] = "save as",
    [ACTION_OPEN] = "open",
    [ACTION_SEARCH] = "search",
    [ACTION_SEARCH_REGEX] = "search regex",
    [ACTION_SEARCH_REVERSE] = "search reverse",
    [ACTION_SEARCH_REGEX_REVERSE] = "search regex reverse",
    [ACTION_REPLACE] = "replace",
    [ACTION_REPLACE_REGEX] = "replace regex",
    [ACTION_REPLACE_ALL] = "replace all",
    [ACTION_REPLACE_ALL_REGEX] = "replace all regex",
    [ACTION_TOGGLE_WRAP] = "toggle wrap",
    [ACTION_QUIT] = "quit",
};

#endif // TE_BINDING_H
