const std = @import("std");

/// Absolute path to a fetched dependency's extracted root, read from the build
/// runner's generated dependency table. We use this instead of the usual
/// `b.dependency().artifact()` because raylib's own build.zig targets stable
/// Zig and does not compile under this nightly — so we pull just its source via
/// the package manager and compile it ourselves below.
fn depRoot(comptime name: []const u8) []const u8 {
    const deps = @import("root").dependencies;
    inline for (deps.root_deps) |entry| {
        if (comptime std.mem.eql(u8, entry[0], name)) {
            return @field(deps.packages, entry[1]).build_root;
        }
    }
    @compileError("dependency not found in build.zig.zon: " ++ name);
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const os = target.result.os.tag;

    const exe = b.addExecutable(.{
        .name = "te",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });
    const mod = exe.root_module;
    mod.link_libc = true;

    // --- editor font (UnifontEX), fetched at build time ------------------------
    // Zig's package manager only accepts tarballs/git repos, not a bare .ttf, so
    // a small tool (tools/fetch_font.zig) downloads the ~14 MB font over HTTPS
    // with std.http (no external tools), verifies its SHA-256, and writes it into
    // the build cache (gitignored). We embed that via an anonymous import. The
    // build system caches the result, re-running only when the tool changes.
    const fetch_font = b.addExecutable(.{
        .name = "fetch_font",
        .root_module = b.createModule(.{
            .root_source_file = b.path("tools/fetch_font.zig"),
            .target = b.graph.host,
            .optimize = .ReleaseSafe,
        }),
    });
    const run_fetch = b.addRunArtifact(fetch_font);
    const font_file = run_fetch.addOutputFileArg("UnifontExMono.ttf");
    mod.addAnonymousImport("UnifontExMono.ttf", .{ .root_source_file = font_file });

    // --- raylib source, fetched by the Zig package manager (build.zig.zon) ---
    const srcdir = comptime depRoot("raylib") ++ "/src";
    const lp = struct {
        fn at(comptime sub: []const u8) std.Build.LazyPath {
            return .{ .cwd_relative = srcdir ++ sub };
        }
    };

    // raylib.h -> Zig bindings via translate-c
    const translate = b.addTranslateC(.{
        .root_source_file = lp.at("/raylib.h"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    translate.addIncludePath(lp.at(""));
    mod.addImport("rl", translate.createModule());

    mod.addIncludePath(lp.at(""));
    mod.addIncludePath(lp.at("/external/glfw/include"));

    const base_flags = [_][]const u8{
        "-DPLATFORM_DESKTOP",
        "-DGRAPHICS_API_OPENGL_33",
        "-fno-sanitize=undefined",
        "-Wno-implicit-function-declaration",
    };

    // Core raylib modules are platform-independent C.
    inline for ([_][]const u8{ "/rcore.c", "/rshapes.c", "/rtextures.c", "/rtext.c" }) |f| {
        mod.addCSourceFile(.{ .file = lp.at(f), .flags = &base_flags });
    }

    // GLFW (rglfw.c) picks its windowing backend per OS; link the matching libs.
    switch (os) {
        .linux => {
            mod.addCSourceFile(.{ .file = lp.at("/rglfw.c"), .flags = &(base_flags ++ [_][]const u8{"-D_GLFW_X11"}) });
            for ([_][]const u8{
                "GL", "X11", "Xrandr", "Xinerama", "Xcursor", "Xi",
                "Xext", "Xrender", "Xfixes", "m", "dl", "pthread", "rt",
            }) |lib| mod.linkSystemLibrary(lib, .{});
        },
        .windows => {
            mod.addCSourceFile(.{ .file = lp.at("/rglfw.c"), .flags = &base_flags });
            for ([_][]const u8{ "gdi32", "winmm", "opengl32", "user32", "shell32", "kernel32" }) |lib|
                mod.linkSystemLibrary(lib, .{});
        },
        .macos => {
            // The Cocoa backend is Objective-C; compile rglfw.c as such.
            mod.addCSourceFile(.{ .file = lp.at("/rglfw.c"), .flags = &(base_flags ++ [_][]const u8{"-ObjC"}) });
            for ([_][]const u8{
                "Cocoa", "IOKit", "CoreFoundation", "CoreVideo", "CoreGraphics", "OpenGL", "AppKit",
            }) |fw| mod.linkFramework(fw, .{});
        },
        else => @panic("unsupported target OS for raylib desktop backend"),
    }

    // Emit the binary straight into the project root as ./te (./te.exe on Windows).
    const emit = b.addUpdateSourceFiles();
    emit.addCopyFileToSource(exe.getEmittedBin(), if (os == .windows) "te.exe" else "te");
    b.default_step = &emit.step;

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(&emit.step);
    const run_step = b.step("run", "Run the text editor");
    run_step.dependOn(&run_cmd.step);
}
