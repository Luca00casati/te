// Key bindings live as Prolog facts now (src/default_bindings.pl,
// key_binding/3, key_binding_once/3, leader_binding/3), not as static
// tables here -- see src/script.c for the dispatch. This header keeps what
// both native C code and the Prolog integration still need: the `Action`
// enum every key ultimately resolves to (main.c's applyAction/runAction
// switch implements each one), the `Mod` enum + matching helpers, and the
// human-readable labels used for echoing/help text.
#ifndef TE_BINDING_H
#define TE_BINDING_H

#include <stdbool.h>
#include <stddef.h>

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

// Shared by anything that matches a live key/chord against a `mod` value:
// the script dispatch in src/script.c (key_binding/leader_binding today;
// formerly also the native BINDINGS/PREFIX_BINDINGS tables this replaced).
//
// modMatchesKey: a top-level key. `cmd` is Ctrl-or-modal-mode; MOD_CTRL/
// MOD_CTRL_SHIFT both require it held.
static inline bool modMatchesKey(Mod mod, bool cmd, bool shift) {
    switch (mod) {
        case MOD_ANY: return true;
        case MOD_CTRL: return cmd && !shift;
        case MOD_CTRL_SHIFT: return cmd && shift;
        default: return false;
    }
}
// modMatchesChord: a leader chord, once the prefix is armed -- Ctrl is
// optional there, so only Shift narrows it.
static inline bool modMatchesChord(Mod mod, bool shift) {
    switch (mod) {
        case MOD_ANY: return true;
        case MOD_CTRL: return !shift;
        case MOD_CTRL_SHIFT: return shift;
        default: return false;
    }
}

// A name usable with te_action(Name) (script.c's lookupAction scans this),
// and/or typeable as a named command (leader, then the name + Enter) --
// those are two different things that happen to share this one table:
// *every* action has a name here (so any key_binding/leader_binding fact in
// src/default_bindings.pl, or a user's init.pl, can reach any action via
// te_action), but only a curated subset is also asserted as a command/2
// fact in default_bindings.pl (the same subset COMMANDS covered before this
// table existed) -- so what's typeable at the command prompt is unchanged.
// Each name is the action's own tag (underscores as hyphens) so there's no
// drift between an action and how it's referred to.
typedef struct {
    const char *name;
    Action action;
} Command;

static const Command COMMANDS[] = {
    { "newline", ACTION_NEWLINE },
    { "open-line-below", ACTION_OPEN_LINE_BELOW },
    { "open-line-above", ACTION_OPEN_LINE_ABOVE },
    { "indent", ACTION_INDENT },
    { "delete-back", ACTION_DELETE_BACK },
    { "delete-forward", ACTION_DELETE_FORWARD },
    { "move-left", ACTION_MOVE_LEFT },
    { "move-right", ACTION_MOVE_RIGHT },
    { "move-up", ACTION_MOVE_UP },
    { "move-down", ACTION_MOVE_DOWN },
    { "move-home", ACTION_MOVE_HOME },
    { "move-end", ACTION_MOVE_END },
    { "move-buffer-start", ACTION_MOVE_BUFFER_START },
    { "move-buffer-end", ACTION_MOVE_BUFFER_END },
    { "move-word-start-left", ACTION_MOVE_WORD_START_LEFT },
    { "move-word-start-right", ACTION_MOVE_WORD_START_RIGHT },
    { "move-word-end-right", ACTION_MOVE_WORD_END_RIGHT },
    { "page-up", ACTION_PAGE_UP },
    { "page-down", ACTION_PAGE_DOWN },
    { "select-all", ACTION_SELECT_ALL },
    { "undo", ACTION_UNDO },
    { "redo", ACTION_REDO },
    { "copy", ACTION_COPY },
    { "cut", ACTION_CUT },
    { "paste", ACTION_PASTE },
    { "move-line-left", ACTION_MOVE_LINE_LEFT },
    { "move-line-right", ACTION_MOVE_LINE_RIGHT },
    { "move-line-up", ACTION_MOVE_LINE_UP },
    { "move-line-down", ACTION_MOVE_LINE_DOWN },
    { "cut-line", ACTION_CUT_LINE },
    { "copy-line", ACTION_COPY_LINE },
    { "paste-line", ACTION_PASTE_LINE },
    { "select-line", ACTION_SELECT_LINE },
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
