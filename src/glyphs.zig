//! Lazy per-codepoint glyph cache backed by UnifontEX.
//!
//! The main atlas font (also UnifontEX) only bakes a common set of codepoints.
//! Anything outside that set — CJK, emoji, rarer scripts and symbols — is
//! rasterized here one glyph at a time and cached as a GPU texture, so the
//! editor can display essentially all of Unicode without a giant atlas.
//!
//! UnifontEX is a single TrueType file covering every Unicode plane. Glyphs are
//! monochrome and either half-width (one cell) or full-width (two cells).

const std = @import("std");
const rl = @import("rl");

// Embedded once here; main.zig reuses `data` for the shared atlas font so the
// font isn't duplicated in the binary.
pub const data = @embedFile("UnifontExMono.ttf");

pub const Glyph = struct {
    tex: rl.Texture2D,
    has: bool, // a real glyph was rasterized (vs. a blank/missing one)
    cells: u8, // display width: 1 = half-width, 2 = full-width
    ox: f32, // glyph bearing, matching raylib's DrawTextEx offsets
    oy: f32,
};

var cache: std.AutoHashMapUnmanaged(u32, Glyph) = .{};
var alloc: std.mem.Allocator = undefined;
var raster_px: c_int = 16;

pub fn init(a: std.mem.Allocator, px: c_int) void {
    alloc = a;
    raster_px = px;
}

pub fn deinit() void {
    var it = cache.valueIterator();
    while (it.next()) |g| if (g.has) rl.UnloadTexture(g.tex);
    cache.deinit(alloc);
}

fn rasterize(cp: u32) Glyph {
    var cps = [_]c_int{@intCast(cp)};
    var n: c_int = 0;
    // FONT_BITMAP: no anti-aliasing, so Unifont's pixels stay crisp.
    const gi = rl.LoadFontData(data.ptr, @intCast(data.len), raster_px, &cps, 1, rl.FONT_BITMAP, &n);
    var g = Glyph{ .tex = undefined, .has = false, .cells = 1, .ox = 0, .oy = 0 };
    if (gi != null and n > 0) {
        const info = gi[0];
        // Unifont's advance is ~half the em for half-width glyphs, a full em
        // for full-width ones. Split at three-quarters to classify robustly.
        g.cells = if (@as(c_int, info.advanceX) * 4 >= raster_px * 3) 2 else 1;
        if (info.image.width > 0 and info.image.height > 0 and info.image.data != null) {
            // LoadFontData returns GRAYSCALE (coverage in one channel), which
            // uploads as an opaque texture — a black box behind the glyph. Repack
            // as GRAY_ALPHA (white, coverage-in-alpha), matching the atlas, so the
            // background is transparent and the tint colors the glyph.
            const w: usize = @intCast(info.image.width);
            const h: usize = @intCast(info.image.height);
            const src = @as([*]const u8, @ptrCast(info.image.data))[0 .. w * h];
            if (alloc.alloc(u8, w * h * 2)) |ga| {
                defer alloc.free(ga);
                for (0..w * h) |p| {
                    ga[p * 2] = 255; // luminance
                    ga[p * 2 + 1] = src[p]; // alpha = coverage
                }
                const img = rl.Image{
                    .data = @ptrCast(ga.ptr),
                    .width = info.image.width,
                    .height = info.image.height,
                    .mipmaps = 1,
                    .format = rl.PIXELFORMAT_UNCOMPRESSED_GRAY_ALPHA,
                };
                g.tex = rl.LoadTextureFromImage(img);
                rl.SetTextureFilter(g.tex, rl.TEXTURE_FILTER_POINT);
                g.has = true;
                g.ox = @floatFromInt(info.offsetX);
                g.oy = @floatFromInt(info.offsetY);
            } else |_| {}
        }
        rl.UnloadFontData(gi, n);
    }
    return g;
}

/// Cached glyph for `cp`, rasterizing (and caching) on first use.
pub fn get(cp: u32) Glyph {
    if (cache.get(cp)) |g| return g;
    const g = rasterize(cp);
    cache.put(alloc, cp, g) catch {};
    return g;
}

/// Display width of `cp` in cells (1 or 2). Rasterizes lazily to read the
/// advance, then serves from cache.
pub fn cells(cp: u32) u8 {
    return get(cp).cells;
}
