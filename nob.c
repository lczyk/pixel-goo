#if 0
if [ ! -f nob ]; then cc -o nob "$0" || exit 1; fi
./nob "$@"
exit
#endif
// Bootstrap: `cc -o nob nob.c && ./nob` (or just `sh nob.c`). After that nob
// rebuilds itself when nob.c changes (NOB_GO_REBUILD_URSELF below).
//
// Targets: build (default), run, clean.

#define NOB_IMPLEMENTATION
#define NOB_STRIP_PREFIX
#include "lib/nob.h"

#include <string.h>

#define SHADER_DIR "shaders" // holds .vert/.frag sources + generated <name>.h (gitignored)
#define BIN_DIR "bin"

static const char *bin_path = BIN_DIR "/goo";

// Each shader has a .vert + .frag embedded into <name>.h by embed_shader.py
static const char *shaders[] = {"copy", "density", "position", "screen", "trail", "upscale", "velocity"};

static const char *sources[] = {
    "main.c", "shader.c", "buffer.c", "rgfw_impl.c",
    // dropt cli option parser (lib/dropt/) -- not header-only, compile its TUs in
    "lib/dropt/dropt.c", "lib/dropt/dropt_string.c", "lib/dropt/dropt_handlers.c",
};

static void add_platform_libs(Cmd *cmd) {
#if defined(__APPLE__)
    cmd_append(cmd, "-framework", "Cocoa", "-framework", "CoreVideo", "-framework", "IOKit", "-framework", "OpenGL");
#elif defined(_WIN32)
    cmd_append(cmd, "-lopengl32", "-lgdi32");
#else
    cmd_append(cmd, "-lX11", "-lXrandr", "-lGL", "-lm");
#endif
}

static bool generate_shaders(void) {
    // Generated headers land next to their sources in shaders/ (gitignored there)
    Cmd cmd = {0};
    for (size_t i = 0; i < NOB_ARRAY_LEN(shaders); i++) {
        const char *name = shaders[i];
        cmd_append(&cmd, "python3", "embed_shader.py",
                   temp_sprintf("%s/%s.vert", SHADER_DIR, name),
                   temp_sprintf("%s/%s.frag", SHADER_DIR, name),
                   temp_sprintf("%s/%s.h", SHADER_DIR, name));
        if (!cmd_run(&cmd)) return false;
    }
    return true;
}

static bool build_goo(void) {
    if (!mkdir_if_not_exists(BIN_DIR)) return false;
    Cmd cmd = {0};
    nob_cc(&cmd);
    cmd_append(&cmd, "-O3");
    cmd_append(&cmd, "-Ilib", "-Ilib/dropt", "-I" SHADER_DIR);
    cmd_append(&cmd, "-o", bin_path);
    for (size_t i = 0; i < NOB_ARRAY_LEN(sources); i++) cmd_append(&cmd, sources[i]);
    add_platform_libs(&cmd);
    return cmd_run(&cmd);
}

static void usage(const char *program) {
    nob_log(NOB_INFO, "usage: %s [build|run|clean|help]", program);
    nob_log(NOB_INFO, "  build  compile to %s", bin_path);
    nob_log(NOB_INFO, "  run    build and run");
    nob_log(NOB_INFO, "  clean  remove %s/ and generated %s/*.h", BIN_DIR, SHADER_DIR);
    nob_log(NOB_INFO, "  help   (default) show this message");
}

int main(int argc, char **argv) {
    NOB_GO_REBUILD_URSELF(argc, argv);

    const char *program = argv[0];
    const char *target = argc > 1 ? argv[1] : "help";

    if (strcmp(target, "help") == 0) {
        usage(program);
        return 0;
    }

    if (strcmp(target, "clean") == 0) {
        // remove the binary dir + the generated shader headers (cmd_run has no
        // shell, so no glob: list each shaders/<name>.h explicitly)
        Cmd cmd = {0};
        cmd_append(&cmd, "rm", "-rf", BIN_DIR);
        for (size_t i = 0; i < NOB_ARRAY_LEN(shaders); i++) {
            cmd_append(&cmd, temp_sprintf("%s/%s.h", SHADER_DIR, shaders[i]));
        }
        return cmd_run(&cmd) ? 0 : 1;
    }

    if (strcmp(target, "build") != 0 && strcmp(target, "run") != 0) {
        nob_log(NOB_ERROR, "unknown target: %s", target);
        usage(program);
        return 1;
    }

    if (!generate_shaders()) return 1;
    if (!build_goo()) return 1;

    if (strcmp(target, "run") == 0) {
        // forward everything after "run" to goo, e.g. `nob run --help -p 1000`
        Cmd cmd = {0};
        cmd_append(&cmd, bin_path);
        for (int i = 2; i < argc; i++) cmd_append(&cmd, argv[i]);
        return cmd_run(&cmd) ? 0 : 1;
    }

    return 0;
}
