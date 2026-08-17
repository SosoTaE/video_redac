#ifndef VIDEO_REDAC_AUDIO_H
#define VIDEO_REDAC_AUDIO_H

/*
 * audio.h — mixing the soundtrack and muxing it with the video.
 *
 * The model is deliberately simple: the video is rendered *silently* first
 * (exactly as before), then a second ffmpeg pass builds the soundtrack and
 * muxes it with a *copy* of the video stream (`-c:v copy`). Which means:
 *
 *   - audio never touches the CUDA pipeline — zero effect on render speed;
 *   - the video is not re-encoded, so no quality is lost;
 *   - with no audio configured, the second pass does not run at all.
 *
 * TTS is deliberately not part of this module: only *files* are accepted
 * (music, SFX, recorded voice). Speech synthesis lives outside the project.
 */

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Single-quotes a string for the shell.
 *
 * It lives here rather than in renderer.cu because both ffmpeg invocations
 * need it — the video pipe and the audio mixer. One implementation means
 * injection safety is reviewed in exactly one place.
 */
bool vr_shell_quote(const char *in, char *out, size_t out_size);

/*
 * "out.mp4" → "out.silent.mp4" — the temporary silent file's name.
 * The extension is preserved so ffmpeg still infers the right container.
 * Returns a malloc'd string (the caller frees it) or NULL.
 */
char *audio_make_silent_path(const char *output_file);

/*
 * Builds the soundtrack from `ctx->audio` and muxes it with `silent_video`'s
 * video stream, writing the result to `output_file`.
 *
 * Returns false on failure (the reason goes to stderr). Requires ffmpeg on
 * PATH, plus ffprobe to place fade-outs correctly.
 */
bool audio_mux(const EditorContext *ctx, const char *silent_video, const char *output_file);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_REDAC_AUDIO_H */
