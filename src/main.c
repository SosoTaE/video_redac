/*
 * main.c — the CLI entry point and the pipeline's conductor.
 *
 * The whole flow fits on one screen here:
 *
 *   parse_video_project()      video.json  → EditorContext (host)
 *   media_prepare_textures()   text        → RGBA pixels (Cairo, CPU, once)
 *   render_video()             textures    → VRAM → frames → ffmpeg/NVENC
 *   editor_context_free()      everything back
 *
 * No global variables: `ctx` is born and dies here; every other module only
 * ever receives it by pointer.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "media_loader.h"
#include "parser.h"
#include "renderer.h"
#include "types.h"

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "video_redac — a text-to-video renderer (CUDA + NVENC)\n"
            "\n"
            "Usage:\n"
            "  %s [project.json] [options]\n"
            "\n"
            "Options:\n"
            "  -o, --output FILE    output file (default: output.mp4)\n"
            "  -d, --dump           print the parsed project and continue\n"
            "  -n, --dry-run        parse and rasterize only; never touch the GPU\n"
            "  -c, --check          validate the project and exit (0 = clean)\n"
            "  -r, --range A:B      render only seconds A..B (fast preview)\n"
            "  -s, --set KEY=VALUE  set a ${KEY} variable; repeatable\n"
            "  -f, --frame T        render one frame at T seconds (use a .png output)\n"
            "  -j, --json           machine-readable --check / --dump output\n"
            "      --list WHAT      effects | transitions | easings | actions |\n"
            "                       properties | widgets | fonts, as JSON\n"
            "  -h, --help           this help\n"
            "\n"
            "Audio: a top-level 'audio' array of files (no speech synthesis).\n"
            "       The video renders silently first; ffmpeg then muxes the sound.\n"
            "\n"
            "Environment:\n"
            "  VIDEO_REDAC_ENCODER  ffmpeg encoder (default: h264_nvenc)\n",
            prog);
}

/*
 * "12:16" → [12.0, 16.0] seconds. Returns false for anything else.
 */
static bool parse_range(const char *spec, double *out_start, double *out_end)
{
    char  *end = NULL;
    double a   = strtod(spec, &end);
    if (end == spec || *end != ':') {
        return false;
    }

    const char *rest = end + 1;
    char       *end2 = NULL;
    double      b    = strtod(rest, &end2);
    if (end2 == rest || *end2 != '\0' || b <= a || a < 0.0) {
        return false;
    }

    *out_start = a;
    *out_end   = b;
    return true;
}

int main(int argc, char **argv)
{
    const char *input_path  = "video.json";
    const char *output_path = "output.mp4";
    bool        want_dump   = false;
    bool        dry_run     = false;
    bool        want_check  = false;
    bool        want_json   = false;   /* machine-readable diagnostics */
    double      range_start = 0.0, range_end = 0.0;
    double      frame_at    = -1.0;    /* --frame T: a single still */
    char      **defines     = NULL;   /* array of --set values; points into argv */
    int         define_count = 0;

    /* --- 1. Argument handling -------------------------------------------- */
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            free(defines);
            return EXIT_SUCCESS;
        }
        if (strcmp(arg, "--list") == 0) {
            /* Handled before anything else: it needs no project file. */
            if (i + 1 >= argc || !vr_list_table(argv[i + 1])) {
                fprintf(stderr, "error: --list expects one of: effects, transitions, "
                                "easings, actions, properties, widgets, shapes, "
                                "fonts.\n");
                free(defines);
                return EXIT_FAILURE;
            }
            free(defines);
            return EXIT_SUCCESS;
        }
        if (strcmp(arg, "-j") == 0 || strcmp(arg, "--json") == 0) {
            want_json = true;
        } else if (strcmp(arg, "-f") == 0 || strcmp(arg, "--frame") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: %s requires a timestamp in seconds.\n", arg);
                free(defines);
                return EXIT_FAILURE;
            }
            frame_at = atof(argv[++i]);
            if (frame_at < 0.0) {
                fprintf(stderr, "error: --frame needs a timestamp >= 0.\n");
                free(defines);
                return EXIT_FAILURE;
            }
        } else if (strcmp(arg, "-d") == 0 || strcmp(arg, "--dump") == 0) {
            want_dump = true;
        } else if (strcmp(arg, "-n") == 0 || strcmp(arg, "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(arg, "-s") == 0 || strcmp(arg, "--set") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: %s requires KEY=VALUE.\n", arg);
                free(defines);
                return EXIT_FAILURE;
            }
            char **grown = (char **)realloc(defines, (size_t)(define_count + 1) * sizeof(char *));
            if (grown == NULL) {
                free(defines);
                return EXIT_FAILURE;
            }
            defines = grown;
            defines[define_count++] = argv[++i];
        } else if (strcmp(arg, "-c") == 0 || strcmp(arg, "--check") == 0) {
            want_check = true;
        } else if (strcmp(arg, "-r") == 0 || strcmp(arg, "--range") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: %s requires a range, e.g. 12:16\n", arg);
                free(defines);
                return EXIT_FAILURE;
            }
            if (!parse_range(argv[++i], &range_start, &range_end)) {
                fprintf(stderr, "error: bad range '%s' (expected A:B with B > A >= 0).\n",
                        argv[i]);
                free(defines);
                return EXIT_FAILURE;
            }
        } else if (strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "error: %s requires a file name.\n", arg);
                free(defines);
                return EXIT_FAILURE;
            }
            output_path = argv[++i];
        } else if (arg[0] == '-') {
            fprintf(stderr, "error: unknown option '%s'.\n\n", arg);
            print_usage(argv[0]);
            free(defines);
            return EXIT_FAILURE;
        } else {
            input_path = arg; /* a positional argument is the input JSON */
        }
    }

    /* --- 2. Parse the project -------------------------------------------- */
    EditorContext *ctx = parse_video_project_ex(input_path, defines, define_count);
    free(defines);
    if (ctx == NULL) {
        fprintf(stderr, "error: could not load project '%s'.\n", input_path);
        return EXIT_FAILURE;
    }

    /* --- 3. Fill the texture cache (CPU, once) ---------------------------- */
    if (!media_prepare_textures(ctx)) {
        fprintf(stderr, "error: preparing textures failed.\n");
        editor_context_free(ctx);
        return EXIT_FAILURE;
    }

    if (want_dump) {
        if (want_json) {
            editor_context_dump_json(ctx);
        } else {
            editor_context_dump(ctx); /* texture sizes are known by now too */
        }
    }

    /* --- 3b. Validation --------------------------------------------------- */
    if (want_check) {
        int problems = want_json ? editor_context_check_json(ctx)
                                 : editor_context_check(ctx);
        editor_context_free(ctx);
        media_shutdown();
        return (problems == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    /*
     * --frame is a one-frame --range. Expressing it that way rather than as a
     * separate path means the still comes from exactly the same code as the
     * video — a preview that could disagree with the render would be worse than
     * no preview at all.
     */
    if (frame_at >= 0.0) {
        range_start = frame_at;
        range_end   = frame_at + 0.5 / (double)ctx->config.fps;
    }

    ctx->range_start_sec = range_start;
    ctx->range_end_sec   = range_end;

    /* --- 4. Render -------------------------------------------------------- */
    bool ok = true;
    if (dry_run) {
        fprintf(stderr, "dry-run: %zu objects rasterized, GPU skipped.\n",
                ctx->widget_count);
    } else {
        ok = render_video(ctx, output_path);
    }

    /* --- 5. Cleanup ------------------------------------------------------- */
    /* One call releases host memory, VRAM and the arena. */
    editor_context_free(ctx);
    media_shutdown(); /* Cairo/fontconfig global caches */

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
