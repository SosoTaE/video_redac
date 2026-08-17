/*
 * main.c — CLI-ის შესასვლელი და pipeline-ის დირიჟორი.
 *
 * მთელი ნაკადი აქ ერთ ეკრანზე ჩანს:
 *
 *   parse_video_project()      video.json  → EditorContext (host)
 *   media_prepare_textures()   ტექსტი      → RGBA პიქსელები (Cairo, CPU, ერთხელ)
 *   render_video()             ტექსტურები  → VRAM → კადრები → ffmpeg/NVENC
 *   editor_context_free()      ყველაფერი უკან
 *
 * არავითარი გლობალური ცვლადი: `ctx` აქ იბადება და აქვე კვდება; ყველა სხვა
 * მოდული მას მხოლოდ მაჩვენებლით იღებს.
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
            "video_redac — ტექსტიდან ვიდეოს რენდერერი (CUDA + NVENC)\n"
            "\n"
            "გამოყენება:\n"
            "  %s [video.json] [პარამეტრები]\n"
            "\n"
            "პარამეტრები:\n"
            "  -o, --output FILE    გამომავალი ფაილი (default: output.mp4)\n"
            "  -d, --dump           დაბეჭდე დაპარსული პროექტი და გააგრძელე\n"
            "  -n, --dry-run        მხოლოდ დაპარსე და დაარასტერიზე; GPU-ს ნუ შეეხები\n"
            "  -c, --check          შეამოწმე პროექტი და გამოდი (0 = სუფთა)\n"
            "  -r, --range A:B      დაარენდერე მხოლოდ A-დან B წამამდე (გადასახედად)\n"
            "  -s, --set KEY=VALUE  ცვლადის მნიშვნელობა (${KEY} JSON-ში); მრავალჯერ\n"
            "  -h, --help           ეს დახმარება\n"
            "\n"
            "ხმა: JSON-ის ზედა დონეზე 'audio' მასივი (მზა ფაილები; TTS არ არის).\n"
            "     ვიდეო ჯერ მუნჯად ირენდერება, მერე ffmpeg ურთავს ფონოგრამას.\n"
            "\n"
            "გარემოს ცვლადები:\n"
            "  VIDEO_REDAC_ENCODER  ffmpeg-ის ენკოდერი (default: h264_nvenc)\n",
            prog);
}

/*
 * "12:16" → [12.0, 16.0] წამი. აბრუნებს false-ს ნებისმიერ სხვა ფორმაზე.
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
    double      range_start = 0.0, range_end = 0.0;
    char      **defines     = NULL;   /* --set-ების მასივი; argv-ს მიუთითებს */
    int         define_count = 0;

    /* --- 1. არგუმენტების დამუშავება ------------------------------------- */
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            print_usage(argv[0]);
            free(defines);
            return EXIT_SUCCESS;
        }
        if (strcmp(arg, "-d") == 0 || strcmp(arg, "--dump") == 0) {
            want_dump = true;
        } else if (strcmp(arg, "-n") == 0 || strcmp(arg, "--dry-run") == 0) {
            dry_run = true;
        } else if (strcmp(arg, "-s") == 0 || strcmp(arg, "--set") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "შეცდომა: %s მოითხოვს KEY=VALUE-ს.\n", arg);
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
                fprintf(stderr, "შეცდომა: %s მოითხოვს დიაპაზონს, მაგ: 12:16\n", arg);
                free(defines);
                return EXIT_FAILURE;
            }
            if (!parse_range(argv[++i], &range_start, &range_end)) {
                fprintf(stderr, "შეცდომა: არასწორი დიაპაზონი '%s' (უნდა იყოს A:B, B > A ≥ 0).\n",
                        argv[i]);
                free(defines);
                return EXIT_FAILURE;
            }
        } else if (strcmp(arg, "-o") == 0 || strcmp(arg, "--output") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "შეცდომა: %s მოითხოვს ფაილის სახელს.\n", arg);
                free(defines);
                return EXIT_FAILURE;
            }
            output_path = argv[++i];
        } else if (arg[0] == '-') {
            fprintf(stderr, "შეცდომა: უცნობი პარამეტრი '%s'.\n\n", arg);
            print_usage(argv[0]);
            free(defines);
            return EXIT_FAILURE;
        } else {
            input_path = arg; /* პოზიციური არგუმენტი = შემავალი JSON */
        }
    }

    /* --- 2. პროექტის დაპარსვა -------------------------------------------- */
    EditorContext *ctx = parse_video_project_ex(input_path, defines, define_count);
    free(defines);
    if (ctx == NULL) {
        fprintf(stderr, "შეცდომა: პროექტი '%s' ვერ ჩაიტვირთა.\n", input_path);
        return EXIT_FAILURE;
    }

    /* --- 3. ტექსტურების ქეშის შევსება (CPU, ერთხელ) ----------------------- */
    if (!media_prepare_textures(ctx)) {
        fprintf(stderr, "შეცდომა: ტექსტურების მომზადება ჩავარდა.\n");
        editor_context_free(ctx);
        return EXIT_FAILURE;
    }

    if (want_dump) {
        editor_context_dump(ctx); /* ტექსტურების ზომებიც უკვე ცნობილია */
    }

    /* --- 3b. ვალიდაცია ---------------------------------------------------- */
    if (want_check) {
        int problems = editor_context_check(ctx);
        editor_context_free(ctx);
        media_shutdown();
        return (problems == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    ctx->range_start_sec = range_start;
    ctx->range_end_sec   = range_end;

    /* --- 4. რენდერი ------------------------------------------------------- */
    bool ok = true;
    if (dry_run) {
        fprintf(stderr, "dry-run: %zu ობიექტი დარასტერიზდა, GPU გამოტოვებულია.\n",
                ctx->widget_count);
    } else {
        ok = render_video(ctx, output_path);
    }

    /* --- 5. გასუფთავება --------------------------------------------------- */
    /* ერთი გამოძახება ათავისუფლებს host მეხსიერებას, VRAM-ს და არენას. */
    editor_context_free(ctx);
    media_shutdown(); /* Cairo/fontconfig-ის გლობალური ქეშები */

    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
