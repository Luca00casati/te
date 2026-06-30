const std = @import("std");

const rl = @import("rl");
const cfg = @import("config.zig");
const binding = @import("binding.zig");

const Dir = std.Io.Dir;

// Monospace font embedded at compile time (Roboto Mono Regular).
const font_ttf = @embedFile("font.ttf");

// Runtime services handed to us via the Init parameter to main().
var io: std.Io = undefined;
var gpa: std.mem.Allocator = undefined;

// ---------------------------------------------------------------------------
// Text buffer: a flat byte array. `cursor` is the caret; `anchor` marks the
// other end of the selection (anchor == cursor means no selection). Columns
// are measured in bytes, which is exact for ASCII and good enough here.
// ---------------------------------------------------------------------------
const MAX = cfg.max_file_bytes;
var text: [MAX]u8 = undefined;
var len: usize = 0;
var cursor: usize = 0;
var anchor: usize = 0;
var dirty: bool = false;

var filename_buf: [4096]u8 = undefined;
var filename: [:0]const u8 = "untitled.txt";
var has_file: bool = false; // false until associated with a real path

// View / interaction state.
var top_line: usize = 0;
var left_col: usize = 0;
// Soft wrap: long logical lines continue on the next visual row instead of
// running off the right edge. When on, horizontal scrolling (left_col) is off.
var wrap: bool = true;
var page_lines: usize = 1;
// Visible text columns, refreshed each frame; used for wrap-aware vertical moves.
var view_cols: usize = 1;
// Sticky/goal column for vertical movement: the on-screen column a run of
// up/down moves tries to keep. null = not set; any horizontal motion clears it.
var goal_col: ?usize = null;
var shift: bool = false;
// True only while a plain (Shift+navigation) move is extending the selection;
// false for chorded moves like Ctrl+Shift+L, which should just move.
var sel_extend: bool = false;
var blink_base: f64 = 0;
var quit_requested: bool = false;
var running: bool = true;

// ---------------------------------------------------------------------------
// Minibuffer (Emacs-style): the bottom line. Either a prompt the user types
// into, a single-key question, or a transient echo-area message.
// ---------------------------------------------------------------------------
const MbKind = enum { none, text_prompt, char_query };
const MbIntent = enum { none, find_file, write_file, search, quit, command };

// True after the prefix key (Ctrl-T) until the next key resolves it: a chord
// fires a shortcut directly, a printable key opens the command-name prompt.
var prefix_pending: bool = false;

// Help overlay: C-h lists the direct keybindings, C-t h lists the named
// commands. While shown it covers the editor and any key/click closes it.
const HelpKind = enum { none, nav, commands };
var help: HelpKind = .none;

var mb_kind: MbKind = .none;
var mb_intent: MbIntent = .none;
var mb_prompt: [:0]const u8 = "";
var mb_input: [4096]u8 = undefined;
var mb_len: usize = 0;
var mb_cursor: usize = 0;

var echo_buf: [256]u8 = undefined;
var echo_len: usize = 0;
var echo_time: f64 = -100;
var last_search: [256]u8 = undefined;
var last_search_len: usize = 0;

// ---------------------------------------------------------------------------
// Undo / redo: each record can reverse one edit. `removed`/`inserted` are
// gpa-owned copies of the text that left and entered the buffer.
// ---------------------------------------------------------------------------
const Edit = struct {
    pos: usize,
    removed: []u8,
    inserted: []u8,
    cur_before: usize,
    cur_after: usize,
};
var undo_stack: [cfg.undo_depth]Edit = undefined;
var undo_n: usize = 0;
var redo_stack: [cfg.undo_depth]Edit = undefined;
var redo_n: usize = 0;

// --- small helpers ---------------------------------------------------------
fn isCont(byte: u8) bool {
    return (byte & 0xC0) == 0x80;
}
fn hasSel() bool {
    return anchor != cursor;
}
fn selMin() usize {
    return @min(anchor, cursor);
}
fn selMax() usize {
    return @max(anchor, cursor);
}
fn noteActivity() void {
    blink_base = rl.GetTime();
}
fn echo(msg: []const u8) void {
    const m = @min(msg.len, echo_buf.len);
    @memcpy(echo_buf[0..m], msg[0..m]);
    echo_len = m;
    echo_time = rl.GetTime();
}
fn echoFmt(comptime fmt: []const u8, args: anytype) void {
    const s = std.fmt.bufPrint(&echo_buf, fmt, args) catch {
        echo_len = 0;
        return;
    };
    echo_len = s.len;
    echo_time = rl.GetTime();
}

// --- buffer mutation -------------------------------------------------------
/// Replace text[start..end] with `bytes` (no undo bookkeeping).
fn replaceRange(start: usize, end: usize, bytes: []const u8) void {
    const tail_len = len - end;
    const new_end = start + bytes.len;
    if (new_end < end) {
        std.mem.copyForwards(u8, text[new_end .. new_end + tail_len], text[end .. end + tail_len]);
    } else if (new_end > end) {
        std.mem.copyBackwards(u8, text[new_end .. new_end + tail_len], text[end .. end + tail_len]);
    }
    @memcpy(text[start .. start + bytes.len], bytes);
    len = new_end + tail_len;
}

/// Replace text[start..end] with `bytes`, recording it for undo.
fn edit(start: usize, end: usize, bytes: []const u8) void {
    if (len - (end - start) + bytes.len > MAX) return;
    const removed = gpa.dupe(u8, text[start..end]) catch return;
    const inserted = gpa.dupe(u8, bytes) catch {
        gpa.free(removed);
        return;
    };
    const cur_before = cursor;
    replaceRange(start, end, bytes);
    cursor = start + bytes.len;
    anchor = cursor;
    dirty = true;
    clearRedo();
    pushUndo(.{
        .pos = start,
        .removed = removed,
        .inserted = inserted,
        .cur_before = cur_before,
        .cur_after = cursor,
    });
    noteActivity();
}

fn insertBytes(bytes: []const u8) void {
    if (hasSel()) edit(selMin(), selMax(), bytes) else edit(cursor, cursor, bytes);
}
fn deleteSelection() void {
    if (hasSel()) edit(selMin(), selMax(), "");
}
fn deleteBack() void {
    if (hasSel()) return deleteSelection();
    if (cursor == 0) return;
    var start = cursor - 1;
    while (start > 0 and isCont(text[start])) start -= 1;
    edit(start, cursor, "");
}
fn deleteForward() void {
    if (hasSel()) return deleteSelection();
    if (cursor >= len) return;
    var end = cursor + 1;
    while (end < len and isCont(text[end])) end += 1;
    edit(cursor, end, "");
}

// --- undo / redo -----------------------------------------------------------
fn evictOldest(stack: []Edit, n: *usize) void {
    gpa.free(stack[0].removed);
    gpa.free(stack[0].inserted);
    std.mem.copyForwards(Edit, stack[0 .. n.* - 1], stack[1..n.*]);
    n.* -= 1;
}
fn pushUndo(e: Edit) void {
    // Coalesce a run of single-character typing into one undo step.
    if (undo_n > 0) {
        const top = &undo_stack[undo_n - 1];
        if (top.removed.len == 0 and e.removed.len == 0 and
            e.inserted.len == 1 and e.inserted[0] != '\n' and
            e.pos == top.pos + top.inserted.len)
        {
            if (gpa.realloc(top.inserted, top.inserted.len + 1)) |grown| {
                grown[grown.len - 1] = e.inserted[0];
                top.inserted = grown;
                top.cur_after = e.cur_after;
                gpa.free(e.removed);
                gpa.free(e.inserted);
                return;
            } else |_| {}
        }
    }
    if (undo_n == undo_stack.len) evictOldest(&undo_stack, &undo_n);
    undo_stack[undo_n] = e;
    undo_n += 1;
}
fn pushRaw(stack: []Edit, n: *usize, e: Edit) void {
    if (n.* == stack.len) evictOldest(stack, n);
    stack[n.*] = e;
    n.* += 1;
}
fn clearRedo() void {
    var i: usize = 0;
    while (i < redo_n) : (i += 1) {
        gpa.free(redo_stack[i].removed);
        gpa.free(redo_stack[i].inserted);
    }
    redo_n = 0;
}
fn undo() void {
    if (undo_n == 0) return;
    undo_n -= 1;
    const e = undo_stack[undo_n];
    replaceRange(e.pos, e.pos + e.inserted.len, e.removed);
    cursor = e.cur_before;
    anchor = cursor;
    dirty = true;
    pushRaw(&redo_stack, &redo_n, e);
    noteActivity();
}
fn redo() void {
    if (redo_n == 0) return;
    redo_n -= 1;
    const e = redo_stack[redo_n];
    replaceRange(e.pos, e.pos + e.removed.len, e.inserted);
    cursor = e.cur_after;
    anchor = cursor;
    dirty = true;
    pushRaw(&undo_stack, &undo_n, e);
    noteActivity();
}
fn freeHistory() void {
    var i: usize = 0;
    while (i < undo_n) : (i += 1) {
        gpa.free(undo_stack[i].removed);
        gpa.free(undo_stack[i].inserted);
    }
    undo_n = 0;
    clearRedo();
}

// --- cursor movement (pure caret; selection handled by caller) -------------
fn moveLeft() void {
    if (cursor == 0) return;
    cursor -= 1;
    while (cursor > 0 and isCont(text[cursor])) cursor -= 1;
}
fn moveRight() void {
    if (cursor >= len) return;
    cursor += 1;
    while (cursor < len and isCont(text[cursor])) cursor += 1;
}
fn lineStart(pos: usize) usize {
    var i = pos;
    while (i > 0 and text[i - 1] != '\n') i -= 1;
    return i;
}
fn lineEnd(pos: usize) usize {
    var i = pos;
    while (i < len and text[i] != '\n') i += 1;
    return i;
}
fn moveHome() void {
    cursor = lineStart(cursor);
}
fn moveEnd() void {
    cursor = lineEnd(cursor);
}
fn moveVertical(delta: i32) void {
    if (wrap) return moveVisual(delta);
    const ls = lineStart(cursor);
    const col = goal_col orelse (cursor - ls);
    goal_col = col;
    if (delta < 0) {
        if (ls == 0) return;
        const prev_start = lineStart(ls - 1);
        const prev_len = ls - 1 - prev_start;
        cursor = prev_start + @min(col, prev_len);
    } else {
        const le = lineEnd(cursor);
        if (le >= len) return;
        const next_start = le + 1;
        const next_len = lineEnd(next_start) - next_start;
        cursor = next_start + @min(col, next_len);
    }
}
// Move one visual (wrapped) row, keeping the same on-screen column. Within a
// long logical line this steps between its segments; at a segment edge it
// crosses to the adjacent logical line's nearest row.
fn moveVisual(delta: i32) void {
    const cols = @max(@as(usize, 1), view_cols);
    const ls = lineStart(cursor);
    const col = cursor - ls;
    const sub = col / cols; // which visual row within this logical line
    goal_col = goal_col orelse (col % cols);
    const vcol = @min(goal_col.?, cols - 1); // on-screen column to preserve
    if (delta < 0) {
        if (sub > 0) {
            cursor = ls + (sub - 1) * cols + vcol;
        } else {
            if (ls == 0) return;
            const prev_start = lineStart(ls - 1);
            const prev_len = ls - 1 - prev_start;
            const last_sub = visRows(prev_len, cols) - 1;
            cursor = prev_start + @min(last_sub * cols + vcol, prev_len);
        }
    } else {
        const le = lineEnd(cursor);
        const llen = le - ls;
        const last_sub = visRows(llen, cols) - 1;
        if (sub < last_sub) {
            cursor = ls + @min((sub + 1) * cols + vcol, llen);
        } else {
            if (le >= len) return;
            const next_start = le + 1;
            const next_len = lineEnd(next_start) - next_start;
            cursor = next_start + @min(vcol, next_len);
        }
    }
}
fn lineCount() usize {
    var c: usize = 1;
    for (text[0..len]) |ch| {
        if (ch == '\n') c += 1;
    }
    return c;
}
fn lineStartOfRow(row: usize) usize {
    var i: usize = 0;
    var r: usize = 0;
    while (i < len and r < row) : (i += 1) {
        if (text[i] == '\n') r += 1;
    }
    return i;
}
fn cursorLineCol() struct { line: usize, col: usize } {
    var line: usize = 0;
    var col: usize = 0;
    var i: usize = 0;
    while (i < cursor) : (i += 1) {
        if (text[i] == '\n') {
            line += 1;
            col = 0;
        } else col += 1;
    }
    return .{ .line = line, .col = col };
}
// --- soft wrap -------------------------------------------------------------
// Number of visual rows a logical line of `line_len` bytes occupies at `cols`
// columns (>= 1, so an empty line still takes one row).
fn visRows(line_len: usize, cols: usize) usize {
    return @max(1, (line_len + cols - 1) / cols);
}

// --- clipboard -------------------------------------------------------------
fn copySelection() void {
    if (!hasSel()) return;
    const a = selMin();
    const b = selMax();
    const buf = gpa.allocSentinel(u8, b - a, 0) catch return;
    defer gpa.free(buf);
    @memcpy(buf[0 .. b - a], text[a..b]);
    rl.SetClipboardText(buf.ptr);
}
fn pasteClipboard() void {
    const c = rl.GetClipboardText();
    if (c == null) return;
    const s = std.mem.span(@as([*:0]const u8, @ptrCast(c)));
    if (s.len > 0) insertBytes(s);
}

// --- file I/O --------------------------------------------------------------
fn setFilename(path: []const u8) void {
    const m = @min(path.len, filename_buf.len - 1);
    @memcpy(filename_buf[0..m], path[0..m]);
    filename_buf[m] = 0;
    filename = filename_buf[0..m :0];
}
fn readFileInto(path: []const u8) bool {
    if (Dir.cwd().readFileAlloc(io, path, gpa, .limited(MAX))) |data| {
        defer gpa.free(data);
        @memcpy(text[0..data.len], data);
        len = data.len;
        return true;
    } else |_| {
        len = 0;
        return false;
    }
}
fn openPath(path: []const u8) void {
    freeHistory();
    setFilename(path);
    const existed = readFileInto(filename);
    cursor = 0;
    anchor = 0;
    top_line = 0;
    left_col = 0;
    dirty = false;
    has_file = true;
    if (existed) echoFmt("Opened {s}", .{filename}) else echoFmt("(New file) {s}", .{filename});
}
fn saveFile() bool {
    Dir.cwd().writeFile(io, .{ .sub_path = filename, .data = text[0..len] }) catch return false;
    dirty = false;
    has_file = true;
    return true;
}
fn saveCurrent() void {
    if (saveFile()) echoFmt("Saved {s} ({d} bytes)", .{ filename, len }) else echo("Save failed");
}

// --- search ----------------------------------------------------------------
fn searchNext(query: []const u8) void {
    if (query.len == 0) return;
    const from = if (cursor < len) cursor + 1 else 0;
    const hit = std.mem.indexOfPos(u8, text[0..len], from, query) orelse
        std.mem.indexOf(u8, text[0..len], query); // wrap to top
    if (hit) |idx| {
        anchor = idx;
        cursor = idx + query.len;
        goal_col = null;
        noteActivity();
        echoFmt("Found: {s}", .{query});
    } else {
        echoFmt("Failing search: {s}", .{query});
    }
}

// --- minibuffer ------------------------------------------------------------
fn mbStartPrompt(intent: MbIntent, prompt: [:0]const u8, prefill: []const u8) void {
    mb_kind = .text_prompt;
    mb_intent = intent;
    mb_prompt = prompt;
    const m = @min(prefill.len, mb_input.len);
    @memcpy(mb_input[0..m], prefill[0..m]);
    mb_len = m;
    mb_cursor = m;
}
fn mbStartQuery(intent: MbIntent, prompt: [:0]const u8) void {
    mb_kind = .char_query;
    mb_intent = intent;
    mb_prompt = prompt;
}
fn mbClose() void {
    mb_kind = .none;
    mb_intent = .none;
    mb_len = 0;
    mb_cursor = 0;
}
fn mbInsert(bytes: []const u8) void {
    if (mb_len + bytes.len > mb_input.len) return;
    std.mem.copyBackwards(u8, mb_input[mb_cursor + bytes.len .. mb_len + bytes.len], mb_input[mb_cursor..mb_len]);
    @memcpy(mb_input[mb_cursor .. mb_cursor + bytes.len], bytes);
    mb_len += bytes.len;
    mb_cursor += bytes.len;
}
fn mbBackspace() void {
    if (mb_cursor == 0) return;
    var start = mb_cursor - 1;
    while (start > 0 and isCont(mb_input[start])) start -= 1;
    const n = mb_cursor - start;
    std.mem.copyForwards(u8, mb_input[start .. mb_len - n], mb_input[mb_cursor..mb_len]);
    mb_len -= n;
    mb_cursor = start;
}
fn mbConfirm() void {
    const input = mb_input[0..mb_len];
    switch (mb_intent) {
        .find_file => if (input.len > 0) openPath(input) else echo("Aborted"),
        .write_file => {
            if (input.len > 0) {
                setFilename(input);
                saveCurrent();
            } else echo("Aborted");
        },
        .search => {
            // empty input repeats the previous search
            if (input.len > 0) {
                const n = @min(input.len, last_search.len);
                @memcpy(last_search[0..n], input[0..n]);
                last_search_len = n;
            }
            if (last_search_len > 0) searchNext(last_search[0..last_search_len]);
        },
        .command => {
            // Resolve the name before mbClose() so an action that opens its own
            // prompt (e.g. save -> "Write file:") isn't immediately closed.
            const name = std.mem.trim(u8, input, " ");
            var matched: ?binding.Action = null;
            for (binding.commands) |c| {
                if (std.mem.eql(u8, c.name, name)) {
                    matched = c.action;
                    break;
                }
            }
            mbClose();
            if (matched) |a| applyAction(a) else echoFmt("No command: {s}", .{name});
            return;
        },
        else => {},
    }
    mbClose();
}

fn handleMinibuffer(ctrl: bool) void {
    if (mb_kind == .char_query) {
        // currently only the quit question
        if (rl.IsKeyPressed(rl.KEY_Y) or rl.IsKeyPressed(rl.KEY_S)) {
            if (saveFile()) {
                running = false;
            } else {
                echo("Save failed");
                mbClose();
            }
        } else if (rl.IsKeyPressed(rl.KEY_N)) {
            running = false;
        } else if (rl.IsKeyPressed(rl.KEY_C) or rl.IsKeyPressed(rl.KEY_ESCAPE) or
            (ctrl and rl.IsKeyPressed(rl.KEY_G)))
        {
            mbClose();
            quit_requested = false;
        }
        return;
    }

    // text_prompt
    if (rl.IsKeyPressed(rl.KEY_ESCAPE) or (ctrl and rl.IsKeyPressed(rl.KEY_G))) {
        echo("Aborted");
        mbClose();
        return;
    }
    if (rl.IsKeyPressed(rl.KEY_ENTER) or rl.IsKeyPressed(rl.KEY_KP_ENTER)) {
        mbConfirm();
        return;
    }
    if (!ctrl) {
        var cp = rl.GetCharPressed();
        while (cp > 0) : (cp = rl.GetCharPressed()) {
            var enc: [4]u8 = undefined;
            const n = std.unicode.utf8Encode(@intCast(cp), &enc) catch 0;
            if (n > 0) mbInsert(enc[0..n]);
        }
    }
    if (pressed(rl.KEY_BACKSPACE)) mbBackspace();
    if (pressed(rl.KEY_LEFT) and mb_cursor > 0) {
        mb_cursor -= 1;
        while (mb_cursor > 0 and isCont(mb_input[mb_cursor])) mb_cursor -= 1;
    }
    if (pressed(rl.KEY_RIGHT) and mb_cursor < mb_len) {
        mb_cursor += 1;
        while (mb_cursor < mb_len and isCont(mb_input[mb_cursor])) mb_cursor += 1;
    }
    if (pressed(rl.KEY_HOME)) mb_cursor = 0;
    if (pressed(rl.KEY_END)) mb_cursor = mb_len;
}

// --- action dispatch -------------------------------------------------------
fn afterMove() void {
    if (!sel_extend) anchor = cursor;
    noteActivity();
}
fn applyAction(action: binding.Action) void {
    // Only vertical moves preserve the sticky column; everything else drops it.
    switch (action) {
        .move_up, .move_down, .page_up, .page_down => {},
        else => goal_col = null,
    }
    switch (action) {
        .newline => insertBytes("\n"),
        .indent => insertBytes(cfg.layout.tab),
        .delete_back => deleteBack(),
        .delete_forward => deleteForward(),
        .move_left => {
            moveLeft();
            afterMove();
        },
        .move_right => {
            moveRight();
            afterMove();
        },
        .move_up => {
            moveVertical(-1);
            afterMove();
        },
        .move_down => {
            moveVertical(1);
            afterMove();
        },
        .move_home => {
            moveHome();
            afterMove();
        },
        .move_end => {
            moveEnd();
            afterMove();
        },
        .page_up => {
            var i: usize = 0;
            while (i < page_lines) : (i += 1) moveVertical(-1);
            afterMove();
        },
        .page_down => {
            var i: usize = 0;
            while (i < page_lines) : (i += 1) moveVertical(1);
            afterMove();
        },
        .select_all => {
            anchor = 0;
            cursor = len;
            noteActivity();
        },
        .undo => undo(),
        .redo => redo(),
        .copy => copySelection(),
        .cut => {
            copySelection();
            deleteSelection();
        },
        .paste => pasteClipboard(),
        .save => if (has_file) saveCurrent() else mbStartPrompt(.write_file, "Write file: ", filename),
        .save_as => mbStartPrompt(.write_file, "Write file: ", filename),
        .open => mbStartPrompt(.find_file, "Find file: ", ""),
        .find => mbStartPrompt(.search, "Search: ", ""),
        .toggle_wrap => {
            wrap = !wrap;
            left_col = 0;
            echo(if (wrap) "Wrap on" else "Wrap off");
        },
        .quit => quit_requested = true,
    }
}

/// True on initial press and on key autorepeat.
fn pressed(key: c_int) bool {
    return rl.IsKeyPressed(key) or rl.IsKeyPressedRepeat(key);
}

// --- pixel <-> text mapping ------------------------------------------------
const Metrics = struct {
    char_w: f32,
    line_h: f32,
    text_x0: f32,
    visible: usize,
    visible_cols: usize,
};
fn offsetFromMouse(m: Metrics) usize {
    const mp = rl.GetMousePosition();
    var rf = (mp.y - cfg.layout.margin_y) / m.line_h;
    if (rf < 0) rf = 0;
    var cf = (mp.x - m.text_x0) / m.char_w + 0.5;
    if (cf < 0) cf = 0;
    const click_col = @as(usize, @intFromFloat(cf));
    if (wrap) {
        // Find the logical line + segment under the clicked visual row.
        const target = @as(usize, @intFromFloat(rf));
        var vrow: usize = 0;
        var s = lineStartOfRow(top_line);
        while (true) {
            const e = lineEnd(s);
            const segs = visRows(e - s, m.visible_cols);
            if (target < vrow + segs) {
                const seg = target - vrow;
                const seg_s = s + seg * m.visible_cols;
                const seg_e = @min(e, seg_s + m.visible_cols);
                return @min(seg_s + click_col, seg_e);
            }
            vrow += segs;
            if (e >= len) return e;
            s = e + 1;
        }
    }
    const row = top_line + @as(usize, @intFromFloat(rf));
    const lc = lineCount();
    const rrow = if (row >= lc) lc - 1 else row;
    const start = lineStartOfRow(rrow);
    const llen = lineEnd(start) - start;
    return start + @min(left_col + click_col, llen);
}

pub fn main(init: std.process.Init) void {
    io = init.io;
    gpa = init.gpa;

    rl.SetConfigFlags(rl.FLAG_WINDOW_RESIZABLE);
    rl.InitWindow(cfg.window.width, cfg.window.height, cfg.window.title);
    defer rl.CloseWindow();
    rl.SetTargetFPS(cfg.window.target_fps);
    rl.SetExitKey(0); // ESC is "cancel", not "quit"

    const font_size = cfg.font.size;
    const spacing = cfg.font.spacing;
    var font = rl.LoadFontFromMemory(".ttf", font_ttf, @intCast(font_ttf.len), @intFromFloat(font_size), null, 0);
    if (font.texture.id == 0) font = rl.GetFontDefault();
    defer rl.UnloadFont(font);

    const char_w: f32 = rl.MeasureTextEx(font, "M", font_size, spacing).x;
    const line_h: f32 = font_size + cfg.font.line_gap;
    const margin_x = cfg.layout.margin_x;
    const margin_y = cfg.layout.margin_y;
    blink_base = rl.GetTime();

    // optional filename argument (after window init so echo works)
    if (std.process.Args.Iterator.initAllocator(init.minimal.args, gpa)) |*ait| {
        var args = ait.*;
        defer args.deinit();
        _ = args.next(); // argv[0]
        if (args.next()) |arg| {
            if (arg.len > 0) openPath(arg);
        }
    } else |_| {}

    var line_tmp: [8192]u8 = undefined;
    var status_tmp: [256]u8 = undefined;
    var num_tmp: [16]u8 = undefined;
    var prev_cursor: usize = 1; // force first ensure-visible

    while (running) {
        shift = rl.IsKeyDown(rl.KEY_LEFT_SHIFT) or rl.IsKeyDown(rl.KEY_RIGHT_SHIFT);
        const ctrl = rl.IsKeyDown(rl.KEY_LEFT_CONTROL) or rl.IsKeyDown(rl.KEY_RIGHT_CONTROL);

        // ---- layout metrics (two bottom lines reserved: status + minibuffer) ----
        const win_w: f32 = @floatFromInt(rl.GetScreenWidth());
        const win_h: f32 = @floatFromInt(rl.GetScreenHeight());
        const status_y = win_h - 2 * line_h;
        const mb_y = win_h - line_h;
        const total_lines = lineCount();
        const digits = @max(@as(usize, 2), digitCount(total_lines));
        const gutter_w = @as(f32, @floatFromInt(digits + 1)) * char_w;
        const text_x0 = margin_x + gutter_w;
        const visible = @max(@as(usize, 1), floorToUsize((win_h - margin_y - 2 * line_h) / line_h));
        const visible_cols = @max(@as(usize, 1), floorToUsize((win_w - text_x0) / char_w));
        page_lines = visible;
        view_cols = visible_cols;
        const metrics = Metrics{ .char_w = char_w, .line_h = line_h, .text_x0 = text_x0, .visible = visible, .visible_cols = visible_cols };

        // ---- input ----
        if (help != .none) {
            if (rl.GetKeyPressed() != 0 or rl.IsMouseButtonPressed(rl.MOUSE_BUTTON_LEFT)) help = .none;
        } else if (mb_kind != .none) {
            handleMinibuffer(ctrl);
        } else if (prefix_pending) {
            handlePrefix(ctrl);
        } else {
            handleInput(ctrl, metrics);
            if (rl.WindowShouldClose() or quit_requested) {
                if (dirty) mbStartQuery(.quit, "Save changes? (y) yes  (n) no  (c) cancel") else running = false;
            }
        }

        // ---- scroll: follow caret only when it actually moved ----
        if (cursor != prev_cursor) {
            const cp = cursorLineCol();
            if (wrap) {
                left_col = 0;
                if (cp.line < top_line) top_line = cp.line;
                // Visual rows from top_line down to the caret's own row. Walk the
                // lines once, then scroll down a line at a time (reusing the byte
                // offset) until the caret fits — no repeated scans from the start.
                var s = lineStartOfRow(top_line);
                var used: usize = cp.col / visible_cols;
                {
                    var ss = s;
                    var l = top_line;
                    while (l < cp.line) : (l += 1) {
                        const e = lineEnd(ss);
                        used += visRows(e - ss, visible_cols);
                        ss = e + 1;
                    }
                }
                while (used >= visible and top_line < cp.line) {
                    const e = lineEnd(s);
                    used -= visRows(e - s, visible_cols);
                    s = e + 1;
                    top_line += 1;
                }
            } else {
                if (cp.line < top_line) top_line = cp.line;
                if (cp.line >= top_line + visible) top_line = cp.line - visible + 1;
                if (cp.col < left_col) left_col = cp.col;
                if (cp.col >= left_col + visible_cols) left_col = cp.col - visible_cols + 1;
            }
            prev_cursor = cursor;
        }

        // ---- draw ----
        rl.BeginDrawing();
        rl.ClearBackground(cfg.colors.bg);

        const sel_a = selMin();
        const sel_b = selMax();
        const cp = cursorLineCol();
        const blink_on = !cfg.cursor_blink or
            @mod(rl.GetTime() - blink_base, cfg.cursor_blink_period * 2) < cfg.cursor_blink_period;
        const show_caret = mb_kind == .none and blink_on;

        if (wrap) {
            // Walk logical lines from top_line, laying each out across one or
            // more visual rows of `visible_cols` columns. The caret position is
            // captured during this walk so we don't rescan the buffer for it.
            const caret_seg = cp.col / visible_cols;
            var caret_y: f32 = -1;
            var row: usize = 0;
            var li: usize = top_line;
            var s: usize = lineStartOfRow(top_line);
            outer: while (row < visible) {
                const e = lineEnd(s);
                const segs = visRows(e - s, visible_cols);
                var seg: usize = 0;
                while (seg < segs) : (seg += 1) {
                    if (row >= visible) break :outer;
                    const y = margin_y + @as(f32, @floatFromInt(row)) * line_h;
                    const seg_s = s + seg * visible_cols;
                    const seg_e = @min(e, seg_s + visible_cols);
                    if (li == cp.line and seg == caret_seg) caret_y = y;

                    if (sel_b > sel_a) {
                        const a = std.math.clamp(sel_a, seg_s, seg_e);
                        const b = std.math.clamp(sel_b, seg_s, seg_e);
                        if (b > a) rl.DrawRectangle(
                            @intFromFloat(text_x0 + @as(f32, @floatFromInt(a - seg_s)) * char_w),
                            @intFromFloat(y),
                            @intFromFloat(@as(f32, @floatFromInt(b - a)) * char_w),
                            @intFromFloat(line_h),
                            cfg.colors.selection,
                        );
                    }

                    if (seg == 0) {
                        const num = std.fmt.bufPrintSentinel(&num_tmp, "{d}", .{li + 1}, 0) catch "";
                        const nx = margin_x + @as(f32, @floatFromInt(digits - num.len)) * char_w;
                        rl.DrawTextEx(font, num.ptr, .{ .x = nx, .y = y }, font_size, spacing, cfg.colors.gutter);
                    }

                    const draw_len = @min(seg_e - seg_s, line_tmp.len - 1);
                    @memcpy(line_tmp[0..draw_len], text[seg_s .. seg_s + draw_len]);
                    line_tmp[draw_len] = 0;
                    rl.DrawTextEx(font, &line_tmp, .{ .x = text_x0, .y = y }, font_size, spacing, cfg.colors.fg);
                    row += 1;
                }
                li += 1;
                if (e >= len) break;
                s = e + 1;
            }

            if (show_caret and caret_y >= 0) {
                const cx = text_x0 + @as(f32, @floatFromInt(cp.col % visible_cols)) * char_w;
                rl.DrawRectangleV(.{ .x = cx, .y = caret_y }, .{ .x = 2, .y = line_h }, cfg.colors.cursor);
            }
        } else {
            var li: usize = 0;
            var s: usize = 0;
            while (true) {
                const e = lineEnd(s);
                if (li >= top_line and li < top_line + visible) {
                    const row = li - top_line;
                    const y = margin_y + @as(f32, @floatFromInt(row)) * line_h;

                    if (sel_b > sel_a) {
                        const a = std.math.clamp(sel_a, s, e);
                        const b = std.math.clamp(sel_b, s, e);
                        if (b > a) {
                            const ca = (a - s) -| left_col;
                            const cb = (b - s) -| left_col;
                            if (cb > ca) rl.DrawRectangle(
                                @intFromFloat(text_x0 + @as(f32, @floatFromInt(ca)) * char_w),
                                @intFromFloat(y),
                                @intFromFloat(@as(f32, @floatFromInt(cb - ca)) * char_w),
                                @intFromFloat(line_h),
                                cfg.colors.selection,
                            );
                        }
                    }

                    const num = std.fmt.bufPrintSentinel(&num_tmp, "{d}", .{li + 1}, 0) catch "";
                    const nx = margin_x + @as(f32, @floatFromInt(digits - num.len)) * char_w;
                    rl.DrawTextEx(font, num.ptr, .{ .x = nx, .y = y }, font_size, spacing, cfg.colors.gutter);

                    const vis_start = s + @min(left_col, e - s);
                    const draw_len = @min(e - vis_start, line_tmp.len - 1);
                    @memcpy(line_tmp[0..draw_len], text[vis_start .. vis_start + draw_len]);
                    line_tmp[draw_len] = 0;
                    rl.DrawTextEx(font, &line_tmp, .{ .x = text_x0, .y = y }, font_size, spacing, cfg.colors.fg);
                }
                li += 1;
                if (e >= len) break;
                s = e + 1;
            }

            if (show_caret and cp.line >= top_line and cp.line < top_line + visible and cp.col >= left_col) {
                const row = cp.line - top_line;
                const cx = text_x0 + @as(f32, @floatFromInt(cp.col - left_col)) * char_w;
                const cy = margin_y + @as(f32, @floatFromInt(row)) * line_h;
                rl.DrawRectangleV(.{ .x = cx, .y = cy }, .{ .x = 2, .y = line_h }, cfg.colors.cursor);
            }
        }

        // status (mode) line
        rl.DrawRectangle(0, @intFromFloat(status_y), @intFromFloat(win_w), @intFromFloat(line_h), cfg.colors.status_bg);
        const status = std.fmt.bufPrintSentinel(&status_tmp, "{s}{s}  |  Ln {d}, Col {d}  |  {d} bytes", .{
            filename,
            if (dirty) " *" else "",
            cp.line + 1,
            cp.col + 1,
            len,
        }, 0) catch "te";
        rl.DrawTextEx(font, status.ptr, .{ .x = margin_x, .y = status_y + 2 }, font_size, spacing, cfg.colors.status_fg);

        drawMinibuffer(font, char_w, mb_y, &line_tmp);

        if (help != .none) drawHelp(font, line_h, win_w, win_h, &line_tmp);

        rl.EndDrawing();
    }

    freeHistory();
}

fn handleInput(ctrl: bool, m: Metrics) void {
    // Esc clears the selection (the minibuffer, when open, handles Esc itself).
    if (rl.IsKeyPressed(rl.KEY_ESCAPE)) {
        anchor = cursor;
        noteActivity();
    }
    if (!ctrl) {
        var cp = rl.GetCharPressed();
        while (cp > 0) : (cp = rl.GetCharPressed()) {
            var enc: [4]u8 = undefined;
            const n = std.unicode.utf8Encode(@intCast(cp), &enc) catch 0;
            if (n > 0) {
                insertBytes(enc[0..n]);
                goal_col = null;
            }
        }
    }
    // C-h opens the keybindings help overlay.
    if (ctrl and !shift and rl.IsKeyPressed(rl.KEY_H)) {
        help = .nav;
        return;
    }
    // Emacs prefix: Ctrl-T arms the leader; handlePrefix takes over next frame.
    if (ctrl and !shift and rl.IsKeyPressed(binding.prefix_key)) {
        prefix_pending = true;
        noteActivity();
        return;
    }
    for (binding.bindings) |b| {
        const mod_ok = switch (b.mod) {
            .any => true,
            .ctrl => ctrl and !shift,
            .ctrl_shift => ctrl and shift,
        };
        if (!mod_ok) continue;
        const hit = if (b.repeat) pressed(b.key) else rl.IsKeyPressed(b.key);
        if (hit) {
            // Shift extends the selection only on plain navigation keys, not
            // when it's part of a chord (e.g. Ctrl+Shift+L just moves).
            sel_extend = shift and b.mod == .any;
            applyAction(b.action);
        }
    }
    const wheel = rl.GetMouseWheelMove();
    if (wheel != 0) {
        // With wrap, lines vary in height, so just stop at the last line.
        const max_top = if (wrap) lineCount() - 1 else if (lineCount() > m.visible) lineCount() - m.visible else 0;
        var nt: i64 = @as(i64, @intCast(top_line)) - @as(i64, @intFromFloat(wheel)) * cfg.scroll_speed;
        nt = std.math.clamp(nt, 0, @as(i64, @intCast(max_top)));
        top_line = @intCast(nt);
    }
    if (rl.IsMouseButtonPressed(rl.MOUSE_BUTTON_LEFT)) {
        cursor = offsetFromMouse(m);
        if (!shift) anchor = cursor;
        goal_col = null;
        noteActivity();
    } else if (rl.IsMouseButtonDown(rl.MOUSE_BUTTON_LEFT)) {
        cursor = offsetFromMouse(m);
        goal_col = null;
        noteActivity();
    }
}

// Prefix is armed: resolve the next key. Esc/C-g cancels; C-t t opens the
// named-command prompt; otherwise a chord fires a shortcut directly. Chords
// match with Ctrl optional (C-t s == C-t C-s); Shift selects shifted variants.
fn handlePrefix(ctrl: bool) void {
    if (rl.IsKeyPressed(rl.KEY_ESCAPE) or (ctrl and rl.IsKeyPressed(rl.KEY_G))) {
        prefix_pending = false;
        echo("Quit");
        return;
    }
    // C-t h -> commands help overlay
    if (rl.IsKeyPressed(rl.KEY_H)) {
        prefix_pending = false;
        help = .commands;
        return;
    }
    // typed-command entry: C-t t (or C-t C-t) -> "Command:" prompt
    if (!shift and rl.IsKeyPressed(binding.prefix_key)) {
        prefix_pending = false;
        mbStartPrompt(.command, "Command: ", "");
        return;
    }
    // chord path: C-t <key>, with or without Ctrl
    for (binding.prefix_bindings) |b| {
        const mod_ok = switch (b.mod) {
            .any => true,
            .ctrl => !shift, // Ctrl optional
            .ctrl_shift => shift,
        };
        if (!mod_ok) continue;
        if (rl.IsKeyPressed(b.key)) {
            prefix_pending = false;
            applyAction(b.action);
            return;
        }
    }
    // any other printable key is an undefined chord: cancel
    if (rl.GetCharPressed() > 0) {
        prefix_pending = false;
        echo("Quit");
    }
}

fn drawMinibuffer(font: rl.Font, char_w: f32, y: f32, tmp: []u8) void {
    const fs = cfg.font.size;
    const sp = cfg.font.spacing;
    if (mb_kind == .none) {
        if (prefix_pending) {
            rl.DrawTextEx(font, "C-t-", .{ .x = cfg.layout.margin_x, .y = y + 2 }, fs, sp, cfg.colors.fg);
            return;
        }
        // echo area
        if (echo_len > 0 and rl.GetTime() - echo_time < 4.0) {
            const n = @min(echo_len, tmp.len - 1);
            @memcpy(tmp[0..n], echo_buf[0..n]);
            tmp[n] = 0;
            rl.DrawTextEx(font, tmp.ptr, .{ .x = cfg.layout.margin_x, .y = y + 2 }, fs, sp, cfg.colors.status_fg);
        }
        return;
    }
    // prompt
    rl.DrawTextEx(font, mb_prompt.ptr, .{ .x = cfg.layout.margin_x, .y = y + 2 }, fs, sp, cfg.colors.fg);
    const prompt_w = rl.MeasureTextEx(font, mb_prompt.ptr, fs, sp).x;
    if (mb_kind == .text_prompt) {
        const n = @min(mb_len, tmp.len - 1);
        @memcpy(tmp[0..n], mb_input[0..n]);
        tmp[n] = 0;
        const x0 = cfg.layout.margin_x + prompt_w;
        rl.DrawTextEx(font, tmp.ptr, .{ .x = x0, .y = y + 2 }, fs, sp, cfg.colors.fg);
        // minibuffer caret (solid)
        const cx = x0 + @as(f32, @floatFromInt(mb_cursor)) * char_w;
        rl.DrawRectangleV(.{ .x = cx, .y = y + 2 }, .{ .x = 2, .y = cfg.font.size }, cfg.colors.cursor);
    }
}

// --- help overlay ----------------------------------------------------------
fn modPrefix(mod: binding.Mod) []const u8 {
    return switch (mod) {
        .any => "",
        .ctrl => "C-",
        .ctrl_shift => "C-S-",
    };
}
fn keyLabel(key: c_int) []const u8 {
    return switch (key) {
        rl.KEY_LEFT => "Left",
        rl.KEY_RIGHT => "Right",
        rl.KEY_UP => "Up",
        rl.KEY_DOWN => "Down",
        rl.KEY_HOME => "Home",
        rl.KEY_END => "End",
        rl.KEY_PAGE_UP => "PgUp",
        rl.KEY_PAGE_DOWN => "PgDn",
        rl.KEY_ENTER => "Enter",
        rl.KEY_KP_ENTER => "KpEnter",
        rl.KEY_TAB => "Tab",
        rl.KEY_BACKSPACE => "Backspace",
        rl.KEY_DELETE => "Delete",
        rl.KEY_SPACE => "Space",
        else => "",
    };
}
/// Human-readable chord like "C-S-Left" or "B" into `buf`.
fn comboName(buf: []u8, b: binding.Binding) []const u8 {
    var n: usize = 0;
    const pfx = modPrefix(b.mod);
    @memcpy(buf[n .. n + pfx.len], pfx);
    n += pfx.len;
    const named = keyLabel(b.key);
    if (named.len > 0) {
        @memcpy(buf[n .. n + named.len], named);
        n += named.len;
    } else if (b.key >= 'A' and b.key <= 'Z') {
        buf[n] = @intCast(b.key);
        n += 1;
    } else {
        buf[n] = '?';
        n += 1;
    }
    return buf[0..n];
}
/// Enum tag with underscores turned to spaces, e.g. "move left".
fn actionLabel(buf: []u8, action: binding.Action) []const u8 {
    const name = @tagName(action);
    for (name, 0..) |c, i| buf[i] = if (c == '_') ' ' else c;
    return buf[0..name.len];
}
// Upper bound on Action variants; used to size the dedup bitset for help rows.
const action_slots = 64;

/// Distinct actions across the direct keybindings (one help row each).
fn navRowCount() usize {
    var shown = std.mem.zeroes([action_slots]bool);
    var n: usize = 0;
    for (binding.bindings) |b| {
        const ai = @intFromEnum(b.action);
        if (shown[ai]) continue;
        shown[ai] = true;
        n += 1;
    }
    return n;
}
// Emacs-style: the help grows upward from the echo/status area as a panel of
// lines, rather than a full-screen overlay.
fn drawHelp(font: rl.Font, line_h: f32, win_w: f32, win_h: f32, tmp: []u8) void {
    const status_y = win_h - 2 * line_h; // top of the status line
    const drawLine = struct {
        fn f(ft: rl.Font, s: [:0]const u8, px: f32, py: f32, color: rl.Color) void {
            rl.DrawTextEx(ft, s.ptr, .{ .x = px, .y = py }, cfg.font.size, cfg.font.spacing, color);
        }
    }.f;

    // title + content rows + footer
    const content: usize = if (help == .nav) navRowCount() else binding.commands.len + 1;
    const pad = line_h * 0.5;
    const block_h = @as(f32, @floatFromInt(content + 2)) * line_h + pad * 2;
    const top = @max(0, status_y - block_h);
    rl.DrawRectangle(0, @intFromFloat(top), @intFromFloat(win_w), @intFromFloat(status_y - top), cfg.colors.status_bg);
    rl.DrawRectangle(0, @intFromFloat(top), @intFromFloat(win_w), 1, cfg.colors.gutter);

    const x = cfg.layout.margin_x + 8;
    var y: f32 = status_y - block_h + pad;

    if (help == .nav) {
        drawLine(font, "Navigation & editing keys  (C-h)", x, y, cfg.colors.cursor);
        y += line_h;
        var shown = std.mem.zeroes([action_slots]bool);
        for (binding.bindings) |b| {
            const ai = @intFromEnum(b.action);
            if (shown[ai]) continue;
            shown[ai] = true;
            var cbuf: [96]u8 = undefined;
            var clen: usize = 0;
            var first = true;
            for (binding.bindings) |b2| {
                if (b2.action != b.action) continue;
                if (!first) {
                    cbuf[clen] = ',';
                    cbuf[clen + 1] = ' ';
                    clen += 2;
                }
                first = false;
                var one: [24]u8 = undefined;
                const cs = comboName(&one, b2);
                @memcpy(cbuf[clen .. clen + cs.len], cs);
                clen += cs.len;
            }
            var lbuf: [32]u8 = undefined;
            const lab = actionLabel(&lbuf, b.action);
            const line = std.fmt.bufPrintSentinel(tmp, "{s:<22}{s}", .{ cbuf[0..clen], lab }, 0) catch continue;
            drawLine(font, line, x, y, cfg.colors.fg);
            y += line_h;
        }
    } else {
        drawLine(font, "Commands  (C-t h)  —  C-t then name, or chord", x, y, cfg.colors.cursor);
        y += line_h;
        for (binding.commands) |c| {
            var chord: []const u8 = "";
            var chbuf: [24]u8 = undefined;
            for (binding.prefix_bindings) |pb| {
                if (pb.action != c.action) continue;
                var one: [16]u8 = undefined;
                const cs = comboName(&one, pb);
                @memcpy(chbuf[0..4], "C-t ");
                @memcpy(chbuf[4 .. 4 + cs.len], cs);
                chord = chbuf[0 .. 4 + cs.len];
                break;
            }
            const line = std.fmt.bufPrintSentinel(tmp, "{s:<16}{s}", .{ c.name, chord }, 0) catch continue;
            drawLine(font, line, x, y, cfg.colors.fg);
            y += line_h;
        }
        drawLine(font, "C-t t : type a command", x, y, cfg.colors.gutter);
        y += line_h;
    }
    drawLine(font, "Press any key to close", x, y, cfg.colors.gutter);
}

/// Floor a float to usize, treating <= 0 as 0 (avoids @intFromFloat panics
/// when the window is smaller than the chrome).
fn floorToUsize(v: f32) usize {
    if (v <= 0) return 0;
    return @intFromFloat(v);
}
fn digitCount(n: usize) usize {
    var d: usize = 1;
    var v = n;
    while (v >= 10) : (v /= 10) d += 1;
    return d;
}
