#ifndef VIDEO_REDAC_PARSER_H
#define VIDEO_REDAC_PARSER_H

/*
 * parser.h — video.json → EditorContext.
 */

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reads and parses the project JSON.
 *
 * Returns a freshly allocated EditorContext* (owned by the caller) or NULL on
 * error (the reason is printed to stderr). On success the context already has:
 *   - a populated config,
 *   - the widget arrays plus the built `widgets` index,
 *   - the timeline with target_index resolved,
 *   - an initialised frame_arena.
 * Textures are still *empty* — media_prepare_textures() fills them.
 */
EditorContext *parse_video_project(const char *filepath);

/*
 * The same, plus variables supplied on the command line.
 *
 * `defines` — "key=value" strings (--set). They override the JSON's "vars"
 * block, so one template can serve many projects.
 */
EditorContext *parse_video_project_ex(const char *filepath,
                                      char **defines, int define_count);

/* Frees everything: strings, arrays, textures, the arena and VRAM.
 * Safe on NULL and when called twice. */
void editor_context_free(EditorContext *ctx);

/* Helpers that other modules need as well. */
char *read_file_to_string(const char *filename, size_t *out_len);
bool  parse_hex_color(const char *hex, Color *out);

/* Diagnostic dump to stderr — what the parser actually read. */
void editor_context_dump(const EditorContext *ctx);

/*
 * Validates the project before rendering: text pushed off the canvas,
 * duplicate ids, unresolved timeline targets, empty or zero-sized objects…
 *
 * Returns the number of problems found (0 = clean). Call it after
 * media_prepare_textures(), once sizes are known.
 */
int editor_context_check(const EditorContext *ctx);

/*
 * The same validation, emitting a JSON report on stdout instead of prose on
 * stderr. Written for programs that consume the result — an MCP server, CI —
 * where scraping "✓ no problems found" would be fragile.
 */
int editor_context_check_json(const EditorContext *ctx);

/* The parsed project as JSON on stdout: resolved sizes, positions, timings. */
void editor_context_dump_json(const EditorContext *ctx);

/*
 * Prints one of the engine's name tables as a JSON array:
 * "effects", "transitions", "easings", "actions", "properties", "widgets".
 * Returns false for an unknown table name.
 */
bool vr_list_table(const char *what);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_REDAC_PARSER_H */
