// Build script for `te`, using nob (https://github.com/tsoding/nob.h).
// Replaces build.zig: compiles src/*.c with cc and links against the
// system-installed raylib and libpcre2-8, plus the Linux desktop system
// libraries raylib needs.
//
//   cc -o nob nob.c && ./nob        # build ./te
//   ./nob run [args...]             # build, then run ./te
#define NOB_IMPLEMENTATION
#include "nob.h"

#include <string.h>

// Linux system libraries raylib's static archive needs (GLFW's X11 backend,
// OpenGL, threading, etc.) -- the same set build.zig links today.
static void append_raylib_system_libs(Nob_Cmd *cmd) {
    nob_cmd_append(cmd, "-lm", "-lpthread", "-ldl", "-lrt",
                   "-lX11", "-lGL", "-lXrandr", "-lXinerama", "-lXcursor", "-lXi", "-lXext", "-lXrender", "-lXfixes");
}

static bool compile_object(const char *src, const char *obj, const char **extra_deps, size_t extra_deps_count) {
    const char **inputs = malloc((1 + extra_deps_count) * sizeof(const char *));
    inputs[0] = src;
    for (size_t i = 0; i < extra_deps_count; i++) inputs[1 + i] = extra_deps[i];
    int rebuild = nob_needs_rebuild(obj, inputs, 1 + extra_deps_count);
    free(inputs);
    if (rebuild < 0) return false;
    if (!rebuild) return true;

    nob_log(NOB_INFO, "compiling %s", obj);
    Nob_Cmd cmd = { 0 };
    nob_cc(&cmd);
    nob_cmd_append(&cmd, "-std=c11", "-Wall", "-Wextra", "-c", src);
    nob_cc_output(&cmd, obj);
    return nob_cmd_run_sync_and_reset(&cmd);
}

static bool build_te(void) {
    if (!nob_mkdir_if_not_exists("build")) return false;

    const char *glyphs_deps[] = { "src/glyphs.h" };
    if (!compile_object("src/glyphs.c", "build/glyphs.o", glyphs_deps, NOB_ARRAY_LEN(glyphs_deps))) return false;

    const char *main_deps[] = { "src/config.h", "src/binding.h", "src/glyphs.h" };
    if (!compile_object("src/main.c", "build/main.o", main_deps, NOB_ARRAY_LEN(main_deps))) return false;

    const char *objs[] = { "build/main.o", "build/glyphs.o" };
    int rebuild = nob_needs_rebuild("te", objs, NOB_ARRAY_LEN(objs));
    if (rebuild < 0) return false;
    if (!rebuild) return true;

    nob_log(NOB_INFO, "linking te");
    Nob_Cmd cmd = { 0 };
    nob_cc(&cmd);
    nob_cmd_append(&cmd, "build/main.o", "build/glyphs.o", "-lraylib", "-lpcre2-8");
    append_raylib_system_libs(&cmd);
    nob_cc_output(&cmd, "te");
    return nob_cmd_run_sync_and_reset(&cmd);
}

int main(int argc, char **argv) {
    NOB_GO_REBUILD_URSELF(argc, argv);

    const char *program = nob_shift_args(&argc, &argv);
    (void)program;

    bool do_run = false;
    if (argc > 0 && strcmp(argv[0], "run") == 0) {
        do_run = true;
        nob_shift_args(&argc, &argv);
    }

    if (!build_te()) return 1;

    if (do_run) {
        Nob_Cmd cmd = { 0 };
        nob_cmd_append(&cmd, "./te");
        for (int i = 0; i < argc; i++) nob_cmd_append(&cmd, argv[i]);
        if (!nob_cmd_run_sync_and_reset(&cmd)) return 1;
    }
    return 0;
}
