const std = @import("std");

const rl = @import("rl");
const cfg = @import("config.zig");
const binding = @import("binding.zig");
const glyphs = @import("glyphs.zig");

const Dir = std.Io.Dir;

// The editor renders entirely with UnifontEX. The common codepoints below are
// baked into a shared texture atlas for fast batched drawing; everything else
// (CJK, emoji, rarer scripts) is rasterized on demand by `glyphs`. Cover Basic
// Latin, Latin-1/Extended, Greek, and Cyrillic, plus common typographic
// punctuation (dashes, curly quotes, ellipsis…). `inAtlas` must match this set.
const font_codepoints = blk: {
    @setEvalBranchQuota(20000);
    const ranges = [_][2]c_int{
        .{ 0x20, 0x24F }, // Basic Latin, Latin-1 Supplement, Latin Extended-A & B
        .{ 0x370, 0x3FF }, // Greek and Coptic
        .{ 0x400, 0x4FF }, // Cyrillic
    };
    const extra = [_]c_int{
        0x2010, 0x2011, 0x2012, 0x2013, 0x2014, 0x2015, // hyphens & dashes
        0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E, // curly quotes
        0x2020, 0x2021, 0x2022, 0x2026, // dagger, double dagger, bullet, ellipsis
        0x2030, 0x2039, 0x203A, // per mille, angle quotes
        0x20AC, 0x2122, // euro, trademark
    };
    var count: usize = extra.len;
    for (ranges) |r| count += @intCast(r[1] - r[0] + 1);
    var arr: [count]c_int = undefined;
    var i: usize = 0;
    for (ranges) |r| {
        var c: c_int = r[0];
        while (c <= r[1]) : (c += 1) {
            arr[i] = c;
            i += 1;
        }
    }
    for (extra) |e| {
        arr[i] = e;
        i += 1;
    }
    break :blk arr;
};

// Build the shared atlas from UnifontEX at `size` px. We assemble it by hand
// (rather than LoadFontFromMemory) so we can rasterize with FONT_BITMAP — no
// anti-aliasing — and pair it with point filtering, keeping Unifont's pixels
// crisp instead of blurred.
fn buildAtlasFont(size: c_int) rl.Font {
    var f: rl.Font = std.mem.zeroes(rl.Font);
    f.baseSize = size;
    f.glyphPadding = 1;
    var count: c_int = 0;
    f.glyphs = rl.LoadFontData(glyphs.data.ptr, @intCast(glyphs.data.len), size, &font_codepoints, @intCast(font_codepoints.len), rl.FONT_BITMAP, &count);
    if (f.glyphs == null) return f;
    f.glyphCount = count;
    var recs: [*c]rl.Rectangle = null;
    const atlas = rl.GenImageFontAtlas(f.glyphs, &recs, count, size, f.glyphPadding, 0);
    f.recs = recs;
    f.texture = rl.LoadTextureFromImage(atlas);
    rl.UnloadImage(atlas);
    rl.SetTextureFilter(f.texture, rl.TEXTURE_FILTER_POINT);
    return f;
}

// Runtime services handed to us via the Init parameter to main().
var io: std.Io = undefined;
var gpa: std.mem.Allocator = undefined;

// `--screenshot <frames> <path>`: after rendering `frames` frames, save a PNG
// to `path` and quit. Handy for headless verification. 0 = disabled.
var shot_left: i32 = 0;
var shot_path_buf: [4096]u8 = undefined;
var shot_path: [:0]const u8 = "";

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
// Mark (C-Space): while active, movement extends the selection, Emacs-style.
var mark_active: bool = false;
// Repeat count from C-<digit>: the next action runs this many times (null = 1).
var repeat_count: ?usize = null;
var blink_base: f64 = 0;
var quit_requested: bool = false;
var running: bool = true;

// ---------------------------------------------------------------------------
// Minibuffer (Emacs-style): the bottom line. Either a prompt the user types
// into, a single-key question, or a transient echo-area message.
// ---------------------------------------------------------------------------
const MbKind = enum { none, text_prompt, char_query };
const MbIntent = enum { none, find_file, write_file, search, quit, command };

// True after the leader (double-tap Ctrl) until the next key resolves it: a
// chord fires a shortcut directly, a printable key opens the command-name
// prompt.
var prefix_pending: bool = false;

// Ctrl double/triple-tap detection. A "tap" is Ctrl pressed then released with
// no other key in between. Two taps arm the leader; three open the command
// prompt. Taps must come within `ctrl_tap_window` of each other.
const ctrl_tap_window: f64 = 0.4;
var ctrl_taps: u8 = 0;
var ctrl_tap_at: f64 = 0;
var ctrl_clean: bool = false; // current Ctrl hold has seen no other key yet

// Help overlay: C-h lists the direct keybindings, the leader + h lists the named
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
    if (undo_n == 0) {
        echo("no more undo");
        return;
    }
    undo_n -= 1;
    const e = undo_stack[undo_n];
    replaceRange(e.pos, e.pos + e.inserted.len, e.removed);
    cursor = e.cur_before;
    anchor = cursor;
    pushRaw(&redo_stack, &redo_n, e);
    noteActivity();
    // Undone back to the start: the buffer matches where history began, so
    // treat the file as untouched again.
    if (undo_n == 0) {
        dirty = false;
        echo("no more undo");
    } else dirty = true;
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
/// Word constituent: ASCII alnum/underscore, or any non-ASCII byte (so
/// multi-byte letters count as part of a word).
fn isWordChar(b: u8) bool {
    return (b >= 'a' and b <= 'z') or (b >= 'A' and b <= 'Z') or
        (b >= '0' and b <= '9') or b == '_' or b >= 0x80;
}
/// Start of the next word: finish the current word, then skip separators so
/// the cursor lands on the first constituent of the following word.
fn moveWordStartRight() void {
    while (cursor < len and isWordChar(text[cursor])) cursor += 1;
    while (cursor < len and !isWordChar(text[cursor])) cursor += 1;
}
/// End of the next word: skip separators, then skip the word so the cursor
/// lands just past its last constituent.
fn moveWordEndRight() void {
    while (cursor < len and !isWordChar(text[cursor])) cursor += 1;
    while (cursor < len and isWordChar(text[cursor])) cursor += 1;
}
/// Start of the previous word.
fn moveWordStartLeft() void {
    while (cursor > 0 and !isWordChar(text[cursor - 1])) cursor -= 1;
    while (cursor > 0 and isWordChar(text[cursor - 1])) cursor -= 1;
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
    const col = goal_col orelse colsIn(ls, cursor);
    goal_col = col;
    if (delta < 0) {
        if (ls == 0) return;
        const prev_start = lineStart(ls - 1);
        cursor = byteAtCol(prev_start, ls - 1, col);
    } else {
        const le = lineEnd(cursor);
        if (le >= len) return;
        const next_start = le + 1;
        cursor = byteAtCol(next_start, lineEnd(next_start), col);
    }
}
// Move one visual (wrapped) row, keeping the same on-screen column. Within a
// long logical line this steps between its segments; at a segment edge it
// crosses to the adjacent logical line's nearest row.
fn moveVisual(delta: i32) void {
    const cols = @max(@as(usize, 1), view_cols);
    const ls = lineStart(cursor);
    const le = lineEnd(cursor);
    const col = colsIn(ls, cursor); // display column within this logical line
    const sub = col / cols; // which visual row within this logical line
    goal_col = goal_col orelse (col % cols);
    const vcol = @min(goal_col.?, cols - 1); // on-screen column to preserve
    if (delta < 0) {
        if (sub > 0) {
            cursor = byteAtCol(ls, le, (sub - 1) * cols + vcol);
        } else {
            if (ls == 0) return;
            const prev_start = lineStart(ls - 1);
            const prev_cols = colsIn(prev_start, ls - 1);
            const last_sub = visRows(prev_cols, cols) - 1;
            cursor = byteAtCol(prev_start, ls - 1, last_sub * cols + vcol);
        }
    } else {
        const llen = colsIn(ls, le);
        const last_sub = visRows(llen, cols) - 1;
        if (sub < last_sub) {
            cursor = byteAtCol(ls, le, (sub + 1) * cols + vcol);
        } else {
            if (le >= len) return;
            const next_start = le + 1;
            cursor = byteAtCol(next_start, lineEnd(next_start), vcol);
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
    while (i < cursor) {
        if (text[i] == '\n') {
            line += 1;
            col = 0;
            i += 1;
        } else {
            const d = decodeCp(i, cursor);
            col += cpCells(d.cp);
            i += d.len;
        }
    }
    return .{ .line = line, .col = col };
}
// --- soft wrap -------------------------------------------------------------
// Number of visual rows a logical line of `line_cols` display columns occupies
// at `cols` columns (>= 1, so an empty line still takes one row).
fn visRows(line_cols: usize, cols: usize) usize {
    return @max(1, (line_cols + cols - 1) / cols);
}
// Decode one UTF-8 codepoint at text[i]; on malformed bytes fall back to a
// single byte so the editor never gets stuck mid-buffer.
const Cp = struct { cp: u32, len: usize };
fn decodeCp(i: usize, stop: usize) Cp {
    const b = text[i];
    if (b < 0x80) return .{ .cp = b, .len = 1 };
    const n = std.unicode.utf8ByteSequenceLength(b) catch return .{ .cp = b, .len = 1 };
    if (i + n > stop) return .{ .cp = b, .len = 1 };
    const cp = std.unicode.utf8Decode(text[i .. i + n]) catch return .{ .cp = b, .len = 1 };
    return .{ .cp = cp, .len = n };
}
// Whether `cp` is baked into the shared atlas (fast batched path). Must match
// the ranges in `font_codepoints`.
fn inAtlas(cp: u32) bool {
    return (cp >= 0x20 and cp <= 0x24F) or
        (cp >= 0x370 and cp <= 0x3FF) or
        (cp >= 0x400 and cp <= 0x4FF) or
        (cp >= 0x2010 and cp <= 0x2026) or
        cp == 0x2030 or cp == 0x2039 or cp == 0x203A or cp == 0x20AC or cp == 0x2122;
}
// Display width of `cp` in cells. Atlas glyphs are half-width; anything else
// asks Unifont (full-width CJK/emoji occupy two cells).
fn cpCells(cp: u32) usize {
    if (cp < 0x20) return 1;
    if (inAtlas(cp)) return 1;
    return glyphs.cells(cp);
}
// Sum of display columns in text[a..b].
fn colsIn(a: usize, b: usize) usize {
    var n: usize = 0;
    var i = a;
    while (i < b) {
        const d = decodeCp(i, b);
        n += cpCells(d.cp);
        i += d.len;
    }
    return n;
}
// Byte offset of the `col`-th display column within [start, stop). A full-width
// glyph that would straddle `col` is not split: the offset lands just before it.
fn byteAtCol(start: usize, stop: usize, col: usize) usize {
    var i = start;
    var c: usize = 0;
    while (i < stop and c < col) {
        const d = decodeCp(i, stop);
        const w = cpCells(d.cp);
        if (c + w > col) break;
        c += w;
        i += d.len;
    }
    return i;
}

// Draw text[s..e] from pixel (x0, y): consecutive atlas codepoints are batched
// into one DrawTextEx run, while each fall-back glyph (CJK, emoji, rarer
// scripts) is blitted from the lazy Unifont cache into its cell(s).
var draw_tmp: [8192]u8 = undefined;
fn flushRun(font: rl.Font, fsize: f32, sp: f32, a: usize, b: usize, x: f32, y: f32, color: rl.Color) void {
    if (b <= a) return;
    const n = @min(b - a, draw_tmp.len - 1);
    @memcpy(draw_tmp[0..n], text[a .. a + n]);
    draw_tmp[n] = 0;
    rl.DrawTextEx(font, &draw_tmp, .{ .x = x, .y = y }, fsize, sp, color);
}
fn drawCells(font: rl.Font, cw: f32, fsize: f32, sp: f32, x0: f32, y: f32, s: usize, e: usize, color: rl.Color) void {
    var x = x0;
    var i = s;
    var run_start = s;
    var run_x = x0;
    while (i < e) {
        const d = decodeCp(i, e);
        if (d.cp < 0x20 or inAtlas(d.cp)) {
            x += cw;
            i += d.len;
            continue;
        }
        flushRun(font, fsize, sp, run_start, i, run_x, y, color);
        const g = glyphs.get(d.cp);
        if (g.has) rl.DrawTextureV(g.tex, .{ .x = x + g.ox, .y = y + g.oy }, color);
        x += @as(f32, @floatFromInt(g.cells)) * cw;
        i += d.len;
        run_start = i;
        run_x = x;
    }
    flushRun(font, fsize, sp, run_start, i, run_x, y, color);
}

// --- clipboard -------------------------------------------------------------
fn copyRange(a: usize, b: usize) void {
    if (b <= a) return;
    const buf = gpa.allocSentinel(u8, b - a, 0) catch return;
    defer gpa.free(buf);
    @memcpy(buf[0 .. b - a], text[a..b]);
    rl.SetClipboardText(buf.ptr);
}
fn copySelection() void {
    if (!hasSel()) return;
    copyRange(selMin(), selMax());
}

// --- whole-line operations -------------------------------------------------
// Byte range of the current line including its trailing newline (if any).
fn currentLineSpan() struct { start: usize, end: usize } {
    const ls = lineStart(cursor);
    const le = lineEnd(cursor);
    return .{ .start = ls, .end = if (le < len) le + 1 else le };
}
// Swap the current line with an adjacent one. `down` picks the line below,
// else above. The cursor rides along, keeping its column.
fn swapLine(down: bool) void {
    const ls = lineStart(cursor);
    const le = lineEnd(cursor);
    const col = cursor - ls;
    if (down) {
        if (le >= len) return; // last line: nothing below
        const ns = le + 1;
        const ne = lineEnd(ns);
        const a = le - ls; // current line length
        const b = ne - ns; // next line length
        const buf = gpa.alloc(u8, ne - ls) catch return;
        defer gpa.free(buf);
        @memcpy(buf[0..b], text[ns..ne]);
        buf[b] = '\n';
        @memcpy(buf[b + 1 ..][0..a], text[ls..le]);
        edit(ls, ne, buf);
        cursor = ls + b + 1 + @min(col, a);
    } else {
        if (ls == 0) return; // first line: nothing above
        const ps = lineStart(ls - 1);
        const pe = ls - 1; // previous line end (the newline before us)
        const a = pe - ps; // previous line length
        const b = le - ls; // current line length
        const buf = gpa.alloc(u8, le - ps) catch return;
        defer gpa.free(buf);
        @memcpy(buf[0..b], text[ls..le]);
        buf[b] = '\n';
        @memcpy(buf[b + 1 ..][0..a], text[ps..pe]);
        edit(ps, le, buf);
        cursor = ps + @min(col, b);
    }
    anchor = cursor;
}
// Paste the clipboard as whole line(s) above the current line.
fn pasteLine() void {
    const c = rl.GetClipboardText();
    if (c == null) return;
    const s = std.mem.span(@as([*:0]const u8, @ptrCast(c)));
    if (s.len == 0) return;
    const ls = lineStart(cursor);
    if (s[s.len - 1] == '\n') {
        edit(ls, ls, s);
    } else {
        const buf = gpa.alloc(u8, s.len + 1) catch return;
        defer gpa.free(buf);
        @memcpy(buf[0..s.len], s);
        buf[s.len] = '\n';
        edit(ls, ls, buf);
    }
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
    freeHistory(); // the saved buffer is the new baseline; drop undo/redo history
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
/// Tab completion for the command prompt: fill in the longest common prefix
/// of the matching command names (the full name when only one matches).
fn mbComplete() void {
    if (mb_intent != .command) return;
    const prefix = mb_input[0..mb_len];
    var matches: usize = 0;
    var lcp: []const u8 = "";
    for (binding.commands) |c| {
        if (!std.mem.startsWith(u8, c.name, prefix)) continue;
        if (matches == 0) {
            lcp = c.name;
        } else {
            var i: usize = 0;
            const m = @min(lcp.len, c.name.len);
            while (i < m and lcp[i] == c.name[i]) i += 1;
            lcp = lcp[0..i];
        }
        matches += 1;
    }
    if (matches == 0) {
        echo("No match");
        return;
    }
    const m = @min(lcp.len, mb_input.len);
    @memcpy(mb_input[0..m], lcp[0..m]);
    mb_len = m;
    mb_cursor = m;
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
    if (rl.IsKeyPressed(rl.KEY_TAB)) {
        mbComplete();
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
// Run an action, honoring a pending C-<digit> repeat count: the action fires
// `repeat_count` times and the echo area reports it (e.g. "move left x3").
fn runAction(action: binding.Action) void {
    const n = @min(repeat_count orelse 1, 9999);
    repeat_count = null;
    var i: usize = 0;
    while (i < n) : (i += 1) applyAction(action);
    if (n > 1) {
        var buf: [40]u8 = undefined;
        echoFmt("{s} x{d}", .{ actionLabel(&buf, action), n });
    }
}
fn applyAction(action: binding.Action) void {
    // Only vertical moves preserve the sticky column; everything else drops it.
    switch (action) {
        .move_up, .move_down, .page_up, .page_down => {},
        else => goal_col = null,
    }
    // Echo the committed action to the minibuffer. Actions that set their own
    // message (e.g. wrap on/off) or open a prompt run below and override this.
    var action_label: [32]u8 = undefined;
    echo(actionLabel(&action_label, action));
    switch (action) {
        .newline => insertBytes("\n"),
        .open_line_below => {
            const pos = lineEnd(cursor);
            edit(pos, pos, "\n"); // cursor lands at the start of the new line
        },
        .open_line_above => {
            const ls = lineStart(cursor);
            edit(ls, ls, "\n");
            cursor = ls; // move onto the fresh blank line above
            anchor = ls;
        },
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
        .move_word_start_left => {
            moveWordStartLeft();
            afterMove();
        },
        .move_word_start_right => {
            moveWordStartRight();
            afterMove();
        },
        .move_word_end_right => {
            moveWordEndRight();
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
        .move_line_left => {
            const ls = lineStart(cursor);
            if (ls < len and (text[ls] == ' ' or text[ls] == '\t')) {
                const c = cursor;
                edit(ls, ls + 1, "");
                cursor = if (c > ls) c - 1 else ls;
                anchor = cursor;
            }
        },
        .move_line_right => {
            const ls = lineStart(cursor);
            const c = cursor;
            edit(ls, ls, " ");
            cursor = c + 1;
            anchor = cursor;
        },
        .move_line_up => swapLine(false),
        .move_line_down => swapLine(true),
        .cut_line => {
            const s = currentLineSpan();
            copyRange(s.start, s.end);
            edit(s.start, s.end, "");
        },
        .copy_line => {
            const s = currentLineSpan();
            copyRange(s.start, s.end);
        },
        .paste_line => pasteLine(),
        .select_line => {
            const s = currentLineSpan();
            anchor = s.start;
            cursor = s.end;
            noteActivity();
        },
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
            const segs = visRows(colsIn(s, e), m.visible_cols);
            if (target < vrow + segs) {
                const seg = target - vrow;
                const seg_s = byteAtCol(s, e, seg * m.visible_cols);
                const seg_e = byteAtCol(s, e, (seg + 1) * m.visible_cols);
                return byteAtCol(seg_s, seg_e, click_col);
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
    return byteAtCol(start, lineEnd(start), left_col + click_col);
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
    var font = buildAtlasFont(@intFromFloat(font_size));
    if (font.texture.id == 0) font = rl.GetFontDefault();
    defer rl.UnloadFont(font);
    glyphs.init(gpa, @intFromFloat(font_size));
    defer glyphs.deinit();

    const char_w: f32 = rl.MeasureTextEx(font, "M", font_size, spacing).x;
    const line_h: f32 = font_size + cfg.font.line_gap;
    const margin_x = cfg.layout.margin_x;
    const margin_y = cfg.layout.margin_y;
    blink_base = rl.GetTime();

    // Arguments (parsed after window init so echo works): an optional file to
    // open, and `--screenshot <frames> <path>`.
    if (std.process.Args.Iterator.initAllocator(init.minimal.args, gpa)) |*ait| {
        var args = ait.*;
        defer args.deinit();
        _ = args.next(); // argv[0]
        while (args.next()) |arg| {
            if (std.mem.eql(u8, arg, "--screenshot")) {
                const frames = args.next() orelse break;
                const path = args.next() orelse break;
                shot_left = std.fmt.parseInt(i32, frames, 10) catch 1;
                if (shot_left < 1) shot_left = 1;
                const m = @min(path.len, shot_path_buf.len - 1);
                @memcpy(shot_path_buf[0..m], path[0..m]);
                shot_path_buf[m] = 0;
                shot_path = shot_path_buf[0..m :0];
            } else if (arg.len > 0) {
                openPath(arg);
            }
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
        } else {
            detectCtrlTaps(); // may arm the leader or open the command prompt
            if (mb_kind != .none) {
                // command prompt just opened by a triple Ctrl tap
            } else if (prefix_pending) {
                handlePrefix(ctrl);
            } else {
                handleInput(ctrl, metrics);
                if (rl.WindowShouldClose() or quit_requested) {
                    if (dirty) mbStartQuery(.quit, "Save changes? (y) yes  (n) no  (c) cancel") else running = false;
                }
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
                        used += visRows(colsIn(ss, e), visible_cols);
                        ss = e + 1;
                    }
                }
                while (used >= visible and top_line < cp.line) {
                    const e = lineEnd(s);
                    used -= visRows(colsIn(s, e), visible_cols);
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
                const segs = visRows(colsIn(s, e), visible_cols);
                var seg: usize = 0;
                while (seg < segs) : (seg += 1) {
                    if (row >= visible) break :outer;
                    const y = margin_y + @as(f32, @floatFromInt(row)) * line_h;
                    const seg_s = byteAtCol(s, e, seg * visible_cols);
                    const seg_e = byteAtCol(s, e, (seg + 1) * visible_cols);
                    if (li == cp.line and seg == caret_seg) caret_y = y;

                    if (sel_b > sel_a) {
                        const a = std.math.clamp(sel_a, seg_s, seg_e);
                        const b = std.math.clamp(sel_b, seg_s, seg_e);
                        if (b > a) rl.DrawRectangle(
                            @intFromFloat(text_x0 + @as(f32, @floatFromInt(colsIn(seg_s, a))) * char_w),
                            @intFromFloat(y),
                            @intFromFloat(@as(f32, @floatFromInt(colsIn(a, b))) * char_w),
                            @intFromFloat(line_h),
                            cfg.colors.selection,
                        );
                    }

                    if (seg == 0) {
                        const num = std.fmt.bufPrintSentinel(&num_tmp, "{d}", .{li + 1}, 0) catch "";
                        const nx = margin_x + @as(f32, @floatFromInt(digits - num.len)) * char_w;
                        rl.DrawTextEx(font, num.ptr, .{ .x = nx, .y = y }, font_size, spacing, cfg.colors.gutter);
                    }

                    drawCells(font, char_w, font_size, spacing, text_x0, y, seg_s, seg_e, cfg.colors.fg);
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
                            const ca = colsIn(s, a) -| left_col;
                            const cb = colsIn(s, b) -| left_col;
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

                    const vis_start = byteAtCol(s, e, left_col);
                    drawCells(font, char_w, font_size, spacing, text_x0, y, vis_start, e, cfg.colors.fg);
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
        if (shot_left > 0) {
            shot_left -= 1;
            if (shot_left == 0) {
                _ = rl.TakeScreenshot(shot_path.ptr);
                running = false;
            }
        }
    }

    freeHistory();
}

fn handleInput(ctrl: bool, m: Metrics) void {
    // Esc clears the selection, mark, and any pending repeat count (the
    // minibuffer, when open, handles Esc itself).
    if (rl.IsKeyPressed(rl.KEY_ESCAPE)) {
        anchor = cursor;
        mark_active = false;
        repeat_count = null;
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
                mark_active = false; // self-insert ends the mark (Emacs-style)
                repeat_count = null;
            }
        }
    }
    // C-h opens the keybindings help overlay.
    if (ctrl and !shift and rl.IsKeyPressed(rl.KEY_H)) {
        help = .nav;
        return;
    }
    // C-Enter opens a blank line below; C-Shift-Enter opens one above. Handled
    // here (not via the table) so the plain-Enter binding doesn't also fire.
    if (ctrl and (rl.IsKeyPressed(rl.KEY_ENTER) or rl.IsKeyPressed(rl.KEY_KP_ENTER))) {
        runAction(if (shift) .open_line_above else .open_line_below);
        return;
    }
    // C-Space toggles the selection mark: while set, movement extends the region.
    if (ctrl and rl.IsKeyPressed(rl.KEY_SPACE)) {
        mark_active = !mark_active;
        anchor = cursor;
        echo(if (mark_active) "Mark set" else "Mark deactivated");
        noteActivity();
        return;
    }
    // C-<digit> accumulates a repeat count for the next action.
    if (ctrl and !shift) {
        var d: c_int = 0;
        while (d <= 9) : (d += 1) {
            if (rl.IsKeyPressed(rl.KEY_ZERO + d) or rl.IsKeyPressed(rl.KEY_KP_0 + d)) {
                const cur = repeat_count orelse 0;
                repeat_count = @min(cur * 10 + @as(usize, @intCast(d)), 9999);
                echoFmt("Repeat: {d}", .{repeat_count.?});
                noteActivity();
                return;
            }
        }
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
            // Extend the selection when the mark is active, or on a plain
            // Shift+navigation key (not when Shift is part of a chord).
            sel_extend = mark_active or (shift and b.mod == .any);
            runAction(b.action);
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

// Detect Ctrl double/triple taps. A tap is Ctrl pressed then released with no
// other key pressed while it was held. Two taps within `ctrl_tap_window` arm
// the leader; a third opens the command prompt. Runs only in normal editing
// mode. Draining the key-pressed queue here is safe: bindings use IsKeyPressed
// and text uses GetCharPressed, both independent of GetKeyPressed.
fn detectCtrlTaps() void {
    const lc = rl.KEY_LEFT_CONTROL;
    const rc = rl.KEY_RIGHT_CONTROL;
    if (rl.IsKeyPressed(lc) or rl.IsKeyPressed(rc)) ctrl_clean = true;
    var k = rl.GetKeyPressed();
    while (k != 0) : (k = rl.GetKeyPressed()) {
        if (k != lc and k != rc) {
            ctrl_clean = false; // a real key was pressed with Ctrl: not a tap
            ctrl_taps = 0; // and it breaks any tap run in progress
        }
    }
    if (rl.IsKeyReleased(lc) or rl.IsKeyReleased(rc)) {
        if (ctrl_clean) {
            const now = rl.GetTime();
            ctrl_taps = if (now - ctrl_tap_at <= ctrl_tap_window) ctrl_taps + 1 else 1;
            ctrl_tap_at = now;
            if (ctrl_taps == 2) {
                prefix_pending = true;
                noteActivity();
            } else if (ctrl_taps >= 3) {
                ctrl_taps = 0;
                prefix_pending = false;
                mbStartPrompt(.command, "Command: ", "");
            }
        }
        ctrl_clean = false;
    }
}

// Prefix is armed: resolve the next key. Esc/C-g cancels; otherwise a chord
// fires a shortcut directly. Chords match with Ctrl optional (leader s ==
// leader C-s); Shift selects shifted variants.
fn handlePrefix(ctrl: bool) void {
    if (rl.IsKeyPressed(rl.KEY_ESCAPE) or (ctrl and rl.IsKeyPressed(rl.KEY_G))) {
        prefix_pending = false;
        echo("Quit");
        return;
    }
    // leader h -> commands help overlay
    if (rl.IsKeyPressed(rl.KEY_H)) {
        prefix_pending = false;
        help = .commands;
        return;
    }
    // chord path: leader <key>, with or without Ctrl
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
            rl.DrawTextEx(font, "Ctrlx2-", .{ .x = cfg.layout.margin_x, .y = y + 2 }, fs, sp, cfg.colors.fg);
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
        // command prompt: dim list of matching completions (Tab to fill in)
        if (mb_intent == .command) {
            var hint: [256]u8 = undefined;
            var hl: usize = 0;
            for (binding.commands) |c| {
                if (!std.mem.startsWith(u8, c.name, mb_input[0..mb_len])) continue;
                if (hl != 0 and hl < hint.len) {
                    hint[hl] = ' ';
                    hl += 1;
                }
                const take = @min(c.name.len, hint.len - 1 - hl);
                @memcpy(hint[hl .. hl + take], c.name[0..take]);
                hl += take;
                if (hl >= hint.len - 1) break;
            }
            hint[hl] = 0;
            const hx = x0 + @as(f32, @floatFromInt(mb_len)) * char_w + 2 * char_w;
            rl.DrawTextEx(font, &hint, .{ .x = hx, .y = y + 2 }, fs, sp, cfg.colors.gutter);
        }
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
        drawLine(font, "Commands  (Ctrlx2 = double-tap Ctrl, then h)  —  then name, or chord", x, y, cfg.colors.cursor);
        y += line_h;
        for (binding.commands) |c| {
            var chord: []const u8 = "";
            var chbuf: [24]u8 = undefined;
            for (binding.prefix_bindings) |pb| {
                if (pb.action != c.action) continue;
                var one: [16]u8 = undefined;
                const cs = comboName(&one, pb);
                const pfx = "Ctrlx2 ";
                @memcpy(chbuf[0..pfx.len], pfx);
                @memcpy(chbuf[pfx.len .. pfx.len + cs.len], cs);
                chord = chbuf[0 .. pfx.len + cs.len];
                break;
            }
            const line = std.fmt.bufPrintSentinel(tmp, "{s:<16}{s}", .{ c.name, chord }, 0) catch continue;
            drawLine(font, line, x, y, cfg.colors.fg);
            y += line_h;
        }
        drawLine(font, "Ctrlx3 (triple-tap Ctrl) : type a command", x, y, cfg.colors.gutter);
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
