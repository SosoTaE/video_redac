/*
 * audio.c — building the soundtrack and muxing it (ffmpeg's second pass).
 *
 * Every track becomes one filter_complex chain:
 *
 *   [N:a] aresample → aformat → atrim → volume → afade(in) → afade(out) → adelay
 *
 * The order matters: trim first, then gain, then the fades, and only then
 * position on the timeline. Any other order puts the fades in the wrong place.
 *
 * Finally every chain is summed with `amix` and passed through `alimiter`, so
 * overlapping clips cannot clip digitally.
 */

#include "audio.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A common format before mixing — differing rates and channel layouts would
 * confuse amix, so every source is normalised first. */
#define AUDIO_RATE   "48000"
#define AUDIO_LAYOUT "stereo"

/* ------------------------------------------------------------------------- */
/* Shell quoting                                                              */
/* ------------------------------------------------------------------------- */

/*
 * Single quotes make the shell take *everything* literally; the only
 * exception is ' itself, closed with the classic '\'' trick.
 * That rules out command injection through a file name.
 */
bool vr_shell_quote(const char *in, char *out, size_t out_size)
{
    if (in == NULL || out == NULL || out_size < 3) {
        return false;
    }

    size_t w = 0;
    out[w++] = '\'';

    for (const char *p = in; *p != '\0'; p++) {
        if (*p == '\'') {
            if (w + 4 >= out_size) return false;
            out[w++] = '\''; out[w++] = '\\'; out[w++] = '\''; out[w++] = '\'';
        } else {
            if (w + 1 >= out_size) return false;
            out[w++] = *p;
        }
    }

    if (w + 2 > out_size) {
        return false;
    }
    out[w++] = '\'';
    out[w]   = '\0';
    return true;
}

char *audio_make_silent_path(const char *output_file)
{
    if (output_file == NULL) {
        return NULL;
    }

    const char *dot   = strrchr(output_file, '.');
    const char *slash = strrchr(output_file, '/');

    /* A dot counts as an extension only if it comes after the last '/'
     * (otherwise "./out" or "a.b/out" would confuse us). */
    if (dot != NULL && slash != NULL && dot < slash) {
        dot = NULL;
    }

    size_t head = (dot != NULL) ? (size_t)(dot - output_file) : strlen(output_file);
    const char *tail = (dot != NULL) ? dot : "";

    size_t need = head + strlen(".silent") + strlen(tail) + 1;
    char  *path = (char *)malloc(need);
    if (path == NULL) {
        return NULL;
    }

    memcpy(path, output_file, head);
    path[head] = '\0';
    strcat(path, ".silent");
    strcat(path, tail);
    return path;
}

/* ------------------------------------------------------------------------- */
/* A growable string (filter_complex can get very long)                       */
/* ------------------------------------------------------------------------- */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
    bool   failed;
} StrBuf;

static void sb_free(StrBuf *sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = sb->cap = 0;
}

static void sb_addf(StrBuf *sb, const char *fmt, ...)
{
    if (sb->failed) {
        return;
    }

    va_list ap;
    va_start(ap, fmt);
    va_list copy;
    va_copy(copy, ap);

    int need = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);

    if (need < 0) {
        sb->failed = true;
        va_end(ap);
        return;
    }

    if (sb->len + (size_t)need + 1 > sb->cap) {
        size_t new_cap = (sb->cap == 0) ? 1024 : sb->cap;
        while (new_cap < sb->len + (size_t)need + 1) {
            new_cap *= 2;
        }
        char *grown = (char *)realloc(sb->data, new_cap);
        if (grown == NULL) {
            sb->failed = true;
            va_end(ap);
            return;
        }
        sb->data = grown;
        sb->cap  = new_cap;
    }

    vsnprintf(sb->data + sb->len, sb->cap - sb->len, fmt, ap);
    sb->len += (size_t)need;
    va_end(ap);
}

/* ------------------------------------------------------------------------- */
/* Determining the source's duration                                          */
/* ------------------------------------------------------------------------- */

/*
 * Asks ffprobe how long the file is.
 *
 * This is needed only to place fade_out correctly: its start is measured from
 * the clip's *end*, so it cannot be computed without knowing the length.
 * On failure we return 0 and the caller falls back to a sensible guess.
 */
static float probe_duration(const char *path)
{
    char quoted[2048];
    if (!vr_shell_quote(path, quoted, sizeof quoted)) {
        return 0.0f;
    }

    char cmd[2560];
    int  n = snprintf(cmd, sizeof cmd,
                      "ffprobe -v error -show_entries format=duration "
                      "-of default=nw=1:nk=1 %s 2>/dev/null", quoted);
    if (n < 0 || (size_t)n >= sizeof cmd) {
        return 0.0f;
    }

    FILE *fp = popen(cmd, "r");
    if (fp == NULL) {
        return 0.0f;
    }

    char  line[128] = { 0 };
    float dur       = 0.0f;
    if (fgets(line, sizeof line, fp) != NULL) {
        dur = strtof(line, NULL);
    }
    pclose(fp);

    return (dur > 0.0f) ? dur : 0.0f;
}

/* ------------------------------------------------------------------------- */
/* Mixing                                                                     */
/* ------------------------------------------------------------------------- */

bool audio_mux(const EditorContext *ctx, const char *silent_video, const char *output_file)
{
    if (ctx == NULL || ctx->audio_count == 0) {
        return false;
    }

    float video_dur = (float)ctx->config.duration_ms / 1000.0f;

    StrBuf inputs;  memset(&inputs, 0, sizeof inputs);
    StrBuf filter;  memset(&filter, 0, sizeof filter);
    StrBuf mixmap;  memset(&mixmap, 0, sizeof mixmap);

    int used = 0; /* how many tracks actually made it in */

    for (size_t i = 0; i < ctx->audio_count; i++) {
        const AudioTrack *t = &ctx->audio[i];
        if (t->path == NULL) {
            continue;
        }

        float avail = video_dur - t->start;
        if (avail <= 0.01f) {
            fprintf(stderr, "warning: audio '%s' starts past the end of the video — skipped.\n",
                    t->path);
            continue;
        }

        /* How long it should sound. */
        float dur;
        if (t->duration > 0.0f) {
            dur = t->duration;
        } else if (t->loop) {
            dur = avail;             /* loop to the end of the video */
        } else {
            float src = probe_duration(t->path) - t->in;
            dur = (src > 0.0f) ? src : avail;
        }
        if (dur > avail) {
            dur = avail;             /* never run past the video */
        }

        char quoted[2048];
        if (!vr_shell_quote(t->path, quoted, sizeof quoted)) {
            fprintf(stderr, "warning: audio path too long — skipped.\n");
            continue;
        }

        /* --- input stream options (strictly before -i) --- */
        if (t->loop) {
            sb_addf(&inputs, "-stream_loop -1 ");
        }
        if (t->in > 0.0f) {
            sb_addf(&inputs, "-ss %.3f ", (double)t->in);
        }
        sb_addf(&inputs, "-i %s ", quoted);

        /* --- the filter chain --- */
        int ff_index = used + 1; /* 0 = the video */

        sb_addf(&filter,
                "[%d:a]aresample=" AUDIO_RATE
                ",aformat=sample_fmts=fltp:channel_layouts=" AUDIO_LAYOUT
                ",atrim=0:%.3f,asetpts=N/SR/TB,volume=%.4f",
                ff_index, (double)dur, (double)t->volume);

        if (t->fade_in > 0.0f) {
            float fi = (t->fade_in > dur) ? dur : t->fade_in;
            sb_addf(&filter, ",afade=t=in:st=0:d=%.3f", (double)fi);
        }
        if (t->fade_out > 0.0f) {
            float fo = (t->fade_out > dur) ? dur : t->fade_out;
            sb_addf(&filter, ",afade=t=out:st=%.3f:d=%.3f", (double)(dur - fo), (double)fo);
        }
        if (t->start > 0.0f) {
            /* all=1 → delay every channel by the same amount. */
            sb_addf(&filter, ",adelay=delays=%.0f:all=1", (double)(t->start * 1000.0f));
        }

        sb_addf(&filter, "[a%d];", used);
        sb_addf(&mixmap, "[a%d]", used);
        used++;
    }

    if (used == 0) {
        fprintf(stderr, "error: not a single audio track was usable.\n");
        sb_free(&inputs); sb_free(&filter); sb_free(&mixmap);
        return false;
    }

    /* Sum + limiter. normalize=0 → tracks are not automatically attenuated. */
    if (used > 1) {
        sb_addf(&filter, "%samix=inputs=%d:normalize=0:duration=longest,"
                         "alimiter=limit=0.95[aout]", mixmap.data, used);
    } else {
        sb_addf(&filter, "%salimiter=limit=0.95[aout]", mixmap.data);
    }

    char quoted_in[2048], quoted_out[2048];
    if (!vr_shell_quote(silent_video, quoted_in, sizeof quoted_in) ||
        !vr_shell_quote(output_file, quoted_out, sizeof quoted_out)) {
        sb_free(&inputs); sb_free(&filter); sb_free(&mixmap);
        return false;
    }

    /* Quote the filter_complex — it contains [ ] ; , | characters. */
    StrBuf quoted_filter; memset(&quoted_filter, 0, sizeof quoted_filter);
    size_t qf_size = filter.len * 4 + 8;
    quoted_filter.data = (char *)malloc(qf_size);
    if (quoted_filter.data == NULL || filter.failed || inputs.failed || mixmap.failed ||
        !vr_shell_quote(filter.data, quoted_filter.data, qf_size)) {
        fprintf(stderr, "error: building the audio filter failed.\n");
        sb_free(&inputs); sb_free(&filter); sb_free(&mixmap); sb_free(&quoted_filter);
        return false;
    }

    StrBuf cmd; memset(&cmd, 0, sizeof cmd);
    sb_addf(&cmd, "ffmpeg -hide_banner -loglevel error -y -i %s %s"
                  "-filter_complex %s "
                  /* The video is *copied* — never re-encoded. */
                  "-map 0:v -c:v copy -map '[aout]' -c:a aac -b:a 192k -ar " AUDIO_RATE " "
                  "-t %.3f -movflags +faststart %s",
                  quoted_in, inputs.data, quoted_filter.data,
                  (double)video_dur, quoted_out);

    bool ok = !cmd.failed;
    if (ok) {
        fprintf(stderr, "audio: mixing and muxing %d track(s)…\n", used);

        int status = system(cmd.data);
        if (status != 0) {
            fprintf(stderr, "error: audio mux failed (ffmpeg exit %d).\n", status);
            fprintf(stderr, "the command was:\n%s\n", cmd.data);
            ok = false;
        }
    }

    sb_free(&inputs);
    sb_free(&filter);
    sb_free(&mixmap);
    sb_free(&quoted_filter);
    sb_free(&cmd);
    return ok;
}
