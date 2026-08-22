% te's own tunable configuration, as plain cfg(Key, Value) facts -- replaces
% what used to be src/config.h's #define knobs. Loaded last (after the
% user's init.pl and scripts/default_bindings.pl -- see script.c's
% scriptInit), so a cfg/2 fact for the same Key in init.pl overrides the
% default here: the same "first assertion wins" pattern default_bindings.pl
% already uses for key_binding/leader_binding/command (clauses are tried in
% assertion order, first match runs). Read at startup (main.c's loadConfig)
% via script.c's scriptCfgFloat/Long/Bool/Text/Color -- not re-queried per
% frame, so editing this file only takes effect after either a restart or
% running the `reload-config` command (leader, then type the name), which
% re-consults init.pl and this file, then re-runs loadConfig.
%
% Colors are rgb(R, G, B) or rgba(R, G, B, A) compound terms, 0-255 per
% channel.

cfg(target_fps, 60).
cfg(window_title, "te").

% Glyph height in pixels. UnifontEX is a 16 px bitmap design, so multiples
% of 16 (16, 32, ...) stay pixel-crisp; other sizes rasterize with uneven,
% "wobbly" stems.
cfg(font_size, 16.0).
% Extra vertical space between lines (line height = size + line_gap).
cfg(font_line_gap, 4.0).

cfg(margin_x, 8.0).
cfg(margin_y, 6.0).

% Text inserted when Tab is pressed (scripts/editing.pl's indent/0 reads
% this directly -- no native round-trip needed for a plain config value).
cfg(tab, "    ").

% Maximum editable file size: a buffer's text array is malloc'd this big
% (main.c's bufferCreate) -- 1 MiB.
cfg(max_file_bytes, 1048576).

% How many lines the mouse wheel scrolls per notch.
cfg(scroll_speed, 3).

% Cursor blinking.
cfg(cursor_blink, true).
cfg(cursor_blink_period, 0.5).

% How many undo steps to keep (scripts/undo_history.pl's eviction).
cfg(undo_depth, 4096).

% Consecutive clean Ctrl taps (Ctrl pressed and released with no other key
% pressed while held) that arm the leader / open the command prompt --
% command_taps should be greater than leader_taps, since the tap count
% keeps climbing past leader_taps until it reaches command_taps or a
% non-Ctrl key resets it (see main.c's detectCtrlTaps).
cfg(leader_taps, 2).
cfg(command_taps, 3).

cfg(color_bg,        rgb(30, 30, 38)).
cfg(color_fg,        rgb(220, 220, 230)).
cfg(color_cursor,    rgb(120, 200, 255)).
cfg(color_selection, rgb(58, 78, 110)).
cfg(color_gutter,    rgb(95, 95, 120)).
cfg(color_status_bg, rgb(50, 50, 64)).
cfg(color_status_fg, rgb(180, 200, 220)).
% Dim overlay drawn behind the unsaved-changes dialog.
cfg(color_overlay,   rgba(0, 0, 0, 160)).
