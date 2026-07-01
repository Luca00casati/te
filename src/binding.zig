//! Key bindings — map keys (and modifiers) to editor actions in one place.
//! Edit the `bindings` table to rebind; `main.zig` dispatches each `Action`.
//!
//! Text typing, selection-with-Shift, and the mouse are handled directly in
//! `main.zig` (they aren't single key → action mappings).

const rl = @import("rl");

/// Things the editor can do in response to a key. `main.zig` implements each.
pub const Action = enum {
    newline,
    indent, // insert config.layout.tab
    delete_back,
    delete_forward,
    move_left,
    move_right,
    move_up,
    move_down,
    move_home,
    move_end,
    move_word_start_left,
    move_word_start_right,
    move_word_end_right,
    page_up,
    page_down,
    select_all,
    undo,
    redo,
    copy,
    cut,
    paste,
    save,
    save_as, // prompt for a path in the minibuffer (C-x C-w in Emacs)
    open, // find-file via the minibuffer (C-x C-f)
    find, // search via the minibuffer (C-s)
    toggle_wrap, // soft line wrapping on/off
    quit,
};

pub const Mod = enum {
    /// Fires regardless of modifier state.
    any,
    /// Requires Ctrl held (and not Shift).
    ctrl,
    /// Requires Ctrl+Shift held.
    ctrl_shift,
};

pub const Binding = struct {
    key: c_int,
    action: Action,
    mod: Mod = .any,
    /// Also fire while the key is held (keyboard autorepeat).
    repeat: bool = true,
};

/// The prefix (leader) key, Emacs-style. Press Ctrl + this key to start a
/// command: then either type a command name (see `commands`) and Enter, or
/// press one of the `prefix_bindings` chords for a direct shortcut.
pub const prefix_key = rl.KEY_T;

/// A named command, reachable via `prefix_key` then typing its name.
pub const Command = struct {
    name: []const u8,
    action: Action,
};

/// Names typed after the prefix (e.g. C-t then "save" Enter). Short names.
pub const commands = [_]Command{
    .{ .name = "save", .action = .save },
    .{ .name = "save-as", .action = .save_as },
    .{ .name = "open", .action = .open },
    .{ .name = "find", .action = .find },
    .{ .name = "undo", .action = .undo },
    .{ .name = "redo", .action = .redo },
    .{ .name = "copy", .action = .copy },
    .{ .name = "cut", .action = .cut },
    .{ .name = "paste", .action = .paste },
    .{ .name = "select-all", .action = .select_all },
    .{ .name = "wrap", .action = .toggle_wrap },
    .{ .name = "quit", .action = .quit },
};

/// Chords reachable after the prefix (e.g. C-t C-s -> save). The second key
/// carries its own modifier, so these are matched while the prefix is pending.
pub const prefix_bindings = [_]Binding{
    .{ .key = rl.KEY_S, .action = .save },
    .{ .key = rl.KEY_W, .action = .save_as },
    .{ .key = rl.KEY_O, .action = .open },
    .{ .key = rl.KEY_A, .action = .select_all },
};

pub const bindings = [_]Binding{
    // editing / navigation
    .{ .key = rl.KEY_ENTER, .action = .newline },
    .{ .key = rl.KEY_KP_ENTER, .action = .newline },
    .{ .key = rl.KEY_TAB, .action = .indent },
    .{ .key = rl.KEY_BACKSPACE, .action = .delete_back },
    .{ .key = rl.KEY_BACKSPACE, .mod = .ctrl, .action = .delete_forward },
    .{ .key = rl.KEY_DELETE, .action = .delete_forward },
    .{ .key = rl.KEY_LEFT, .action = .move_left },
    .{ .key = rl.KEY_B, .mod = .ctrl, .action = .move_left },
    .{ .key = rl.KEY_RIGHT, .action = .move_right },
    .{ .key = rl.KEY_F, .mod = .ctrl, .action = .move_right },
    .{ .key = rl.KEY_UP, .action = .move_up },
    .{ .key = rl.KEY_P, .mod = .ctrl, .action = .move_up },
    .{ .key = rl.KEY_DOWN, .action = .move_down },
    .{ .key = rl.KEY_N, .mod = .ctrl, .action = .move_down },
    .{ .key = rl.KEY_HOME, .action = .move_home },
    .{ .key = rl.KEY_A, .mod = .ctrl_shift, .action = .move_home },
    .{ .key = rl.KEY_END, .action = .move_end },
    .{ .key = rl.KEY_E, .mod = .ctrl_shift , .action = .move_end },
    .{ .key = rl.KEY_W, .mod = .ctrl, .action = .move_word_start_right },
    .{ .key = rl.KEY_D, .mod = .ctrl, .action = .move_word_end_right },
    .{ .key = rl.KEY_R, .mod = .ctrl, .action = .move_word_start_left },
    .{ .key = rl.KEY_PAGE_UP, .action = .page_up },
    .{ .key = rl.KEY_J, .mod = .ctrl, .action = .page_up },
    .{ .key = rl.KEY_PAGE_DOWN, .action = .page_down },
    .{ .key = rl.KEY_J, .mod = .ctrl_shift, .action = .page_down },
    // shortcuts
    .{ .key = rl.KEY_Z, .action = .undo, .mod = .ctrl },
    .{ .key = rl.KEY_Z, .action = .redo, .mod = .ctrl_shift },
    .{ .key = rl.KEY_C, .action = .copy, .mod = .ctrl, .repeat = false },
    .{ .key = rl.KEY_X, .action = .cut, .mod = .ctrl, .repeat = false },
    .{ .key = rl.KEY_V, .action = .paste, .mod = .ctrl, .repeat = false },
    .{ .key = rl.KEY_S, .action = .find, .mod = .ctrl, .repeat = false },
    .{ .key = rl.KEY_Q, .action = .quit, .mod = .ctrl, .repeat = false },
};
