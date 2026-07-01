//! Editor configuration — all the knobs in one place. Tweak and rebuild.

const rl = @import("rl");

pub const window = struct {
    pub const width: c_int = 960;
    pub const height: c_int = 640;
    pub const title = "te";
    pub const target_fps: c_int = 60;
};

pub const font = struct {
    /// Glyph height in pixels. UnifontEX is a 16 px bitmap design, so multiples
    /// of 16 (16, 32, …) stay pixel-crisp; other sizes rasterize with uneven,
    /// "wobbly" stems.
    pub const size: f32 = 16;
    /// Extra horizontal space between glyphs.
    pub const spacing: f32 = 0;
    /// Extra vertical space between lines (line height = size + line_gap).
    pub const line_gap: f32 = 4;
};

pub const layout = struct {
    pub const margin_x: f32 = 8;
    pub const margin_y: f32 = 6;
    /// Text inserted when Tab is pressed.
    pub const tab = "    ";
};

/// Maximum editable file size (the text buffer is a fixed array this big).
pub const max_file_bytes = 1 << 20;

/// How many lines the mouse wheel scrolls per notch.
pub const scroll_speed: i32 = 3;

/// Cursor blinking.
pub const cursor_blink = true;
pub const cursor_blink_period: f64 = 0.5;

/// How many undo steps to keep.
pub const undo_depth = 512;

pub const colors = struct {
    pub const bg = rgb(30, 30, 38);
    pub const fg = rgb(220, 220, 230);
    pub const cursor = rgb(120, 200, 255);
    pub const selection = rgb(58, 78, 110);
    pub const gutter = rgb(95, 95, 120);
    pub const status_bg = rgb(50, 50, 64);
    pub const status_fg = rgb(180, 200, 220);
    /// Dim overlay drawn behind the unsaved-changes dialog.
    pub const overlay = rgba(0, 0, 0, 160);
};

fn rgb(r: u8, g: u8, b: u8) rl.Color {
    return .{ .r = r, .g = g, .b = b, .a = 255 };
}

fn rgba(r: u8, g: u8, b: u8, a: u8) rl.Color {
    return .{ .r = r, .g = g, .b = b, .a = a };
}
