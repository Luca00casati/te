//! Build-time font fetcher: downloads UnifontEX over HTTPS with std.http (no
//! external tools), verifies its SHA-256, and writes it to the path given as
//! argv[1]. A checksum mismatch fails the build. Invoked by build.zig.

const std = @import("std");

const url = "https://raw.githubusercontent.com/stgiga/UnifontEX/main/UnifontExMono.ttf";
const expected_sha256 = "d2840072b230b46dde7a6156f6f45ed0dac37b1def2ce0fdbf88aaf7bb3f3352";

pub fn main(init: std.process.Init) !void {
    const io = init.io;
    const gpa = init.gpa;

    var args = try std.process.Args.Iterator.initAllocator(init.minimal.args, gpa);
    defer args.deinit();
    _ = args.next(); // argv[0]
    const out_path = args.next() orelse {
        std.debug.print("usage: fetch_font <output-path>\n", .{});
        return error.MissingOutputPath;
    };

    var client: std.http.Client = .{ .allocator = gpa, .io = io };
    defer client.deinit();

    var body: std.Io.Writer.Allocating = .init(gpa);
    defer body.deinit();

    const res = try client.fetch(.{
        .location = .{ .url = url },
        .response_writer = &body.writer,
    });
    if (res.status != .ok) {
        std.debug.print("fetch: HTTP {d} for {s}\n", .{ @intFromEnum(res.status), url });
        return error.HttpStatus;
    }

    const data = body.writer.buffered();

    var digest: [32]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(data, &digest, .{});
    var hex: [64]u8 = undefined;
    const chars = "0123456789abcdef";
    for (digest, 0..) |b, i| {
        hex[i * 2] = chars[b >> 4];
        hex[i * 2 + 1] = chars[b & 0x0f];
    }
    if (!std.mem.eql(u8, &hex, expected_sha256)) {
        std.debug.print("font checksum mismatch:\n  expected {s}\n  got      {s}\n", .{ expected_sha256, hex });
        return error.ChecksumMismatch;
    }

    try std.Io.Dir.cwd().writeFile(io, .{ .sub_path = out_path, .data = data });
}
