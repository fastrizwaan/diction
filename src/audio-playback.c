#include "audio-playback.h"

#include <stdio.h>
#include <string.h>

static gboolean spawn_audio_argv(const char *const *argv, const char *label) {
    GError *error = NULL;
    gboolean ok = g_spawn_async(NULL,
                                (char **)argv,
                                NULL,
                                G_SPAWN_SEARCH_PATH |
                                G_SPAWN_STDOUT_TO_DEV_NULL |
                                G_SPAWN_STDERR_TO_DEV_NULL,
                                NULL,
                                NULL,
                                NULL,
                                &error);
    if (ok) {
        fprintf(stderr, "[AUDIO PLAY] Playing with '%s'...\n", label);
        return TRUE;
    }

    g_clear_error(&error);
    return FALSE;
}

static gboolean spawn_audio_shell_command(const char *command, const char *label) {
    const char *argv[] = { "/bin/sh", "-c", command, NULL };
    return spawn_audio_argv(argv, label);
}

static gboolean path_has_extension(const char *path, const char *ext) {
    const char *dot = strrchr(path, '.');
    return dot && g_ascii_strcasecmp(dot, ext) == 0;
}

static gboolean looks_like_url(const char *path) {
    return path && (g_str_has_prefix(path, "http://") || g_str_has_prefix(path, "https://"));
}

static gboolean play_audio_via_pcm_pipeline(const char *audio_path) {
    if (!g_find_program_in_path("ffmpeg")) {
        return FALSE;
    }

    char *quoted = g_shell_quote(audio_path);
    gboolean ok = FALSE;

    if (g_find_program_in_path("pw-play")) {
        char *cmd = g_strdup_printf(
            "ffmpeg -nostdin -loglevel error -i %s -f s16le -acodec pcm_s16le -ac 2 -ar 48000 - | "
            "pw-play --raw --format s16 --channels 2 --rate 48000 -",
            quoted);
        ok = spawn_audio_shell_command(cmd, "ffmpeg | pw-play");
        g_free(cmd);
    } else if (g_find_program_in_path("aplay")) {
        char *cmd = g_strdup_printf(
            "ffmpeg -nostdin -loglevel error -i %s -f s16le -acodec pcm_s16le -ac 2 -ar 48000 - | "
            "aplay -q -f S16_LE -c 2 -r 48000 -",
            quoted);
        ok = spawn_audio_shell_command(cmd, "ffmpeg | aplay");
        g_free(cmd);
    }

    g_free(quoted);
    return ok;
}

static gboolean play_audio_via_gstreamer(const char *audio_path, gboolean is_spx) {
    if (!g_find_program_in_path("gst-launch-1.0")) {
        return FALSE;
    }

    gboolean ok = FALSE;
    char *uri = NULL;
    GError *error = NULL;

    if (looks_like_url(audio_path)) {
        uri = g_strdup(audio_path);
    } else {
        uri = g_filename_to_uri(audio_path, NULL, &error);
        if (!uri) {
            g_clear_error(&error);
            return FALSE;
        }
    }

    const guint buffer_size = is_spx ? 262144 : 65536;
    char *quoted_uri = g_shell_quote(uri);
    char *cmd = g_strdup_printf(
        "gst-launch-1.0 -q playbin uri=%s audio-sink='queue max-size-bytes=%u ! autoaudiosink'",
        quoted_uri, buffer_size);

    ok = spawn_audio_shell_command(cmd, "gst-playbin");

    g_free(cmd);
    g_free(quoted_uri);
    g_free(uri);
    return ok;
}

void audio_play_file(const char *audio_path) {
    fprintf(stderr, "[AUDIO PLAY] Attempting to play: %s\n", audio_path);

    /* Prefer GStreamer-based playback which handles many formats
     * and allows configuring a larger internal queue for `.spx` files
     * to reduce stuttering. Fall back to ffmpeg->pw-play/aplay if needed. */
    if (play_audio_via_gstreamer(audio_path, path_has_extension(audio_path, ".spx"))) {
        return;
    }

    if (!looks_like_url(audio_path) &&
        (path_has_extension(audio_path, ".spx") ||
         path_has_extension(audio_path, ".ogg") ||
         path_has_extension(audio_path, ".oga"))) {
        if (play_audio_via_pcm_pipeline(audio_path)) {
            return;
        }
    }

    if (g_find_program_in_path("ffplay")) {
        const char *argv[] = { "ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet", audio_path, NULL };
        if (spawn_audio_argv(argv, "ffplay")) {
            return;
        }
    }

    if (g_find_program_in_path("mpg123")) {
        const char *argv[] = { "mpg123", "-q", audio_path, NULL };
        if (spawn_audio_argv(argv, "mpg123")) {
            return;
        }
    }

    if (g_find_program_in_path("play")) {
        const char *argv[] = { "play", "-q", audio_path, NULL };
        if (spawn_audio_argv(argv, "play")) {
            return;
        }
    }

    if (g_find_program_in_path("paplay")) {
        const char *argv[] = { "paplay", audio_path, NULL };
        if (spawn_audio_argv(argv, "paplay")) {
            return;
        }
    }

    fprintf(stderr, "[AUDIO ERROR] No usable audio player found\n");
}

static char *query_param_dup(const char *query, const char *key) {
    if (!query || !key) {
        return NULL;
    }

    char **pairs = g_strsplit(query, "&", -1);
    char *value = NULL;

    for (int i = 0; pairs[i]; i++) {
        char *eq = strchr(pairs[i], '=');
        if (!eq) {
            continue;
        }

        *eq = '\0';
        if (strcmp(pairs[i], key) == 0) {
            value = g_uri_unescape_string(eq + 1, NULL);
            *eq = '=';
            break;
        }
        *eq = '=';
    }

    g_strfreev(pairs);
    return value;
}

gboolean audio_try_play_encoded_sound_uri(const char *uri, AudioResourceResolver resolver, gpointer user_data) {
    const char *query = strchr(uri, '?');
    if (!query) {
        return FALSE;
    }

    char *audio_url = query_param_dup(query + 1, "url");
    char *audio_path_param = query_param_dup(query + 1, "path");
    char *resource_dir = query_param_dup(query + 1, "dir");
    char *sound_file = query_param_dup(query + 1, "file");

    if (audio_url && *audio_url) {
        fprintf(stderr, "[AUDIO CLICKED] URL: %s\n", audio_url);
        audio_play_file(audio_url);
        g_free(audio_url);
        g_free(audio_path_param);
        g_free(resource_dir);
        g_free(sound_file);
        return TRUE;
    }

    g_free(audio_url);

    if (audio_path_param && *audio_path_param) {
        fprintf(stderr, "[AUDIO CLICKED] Path: %s\n", audio_path_param);
        if (g_file_test(audio_path_param, G_FILE_TEST_EXISTS)) {
            audio_play_file(audio_path_param);
        } else {
            fprintf(stderr, "[AUDIO ERROR] File not found: %s\n", audio_path_param);
        }
        g_free(audio_path_param);
        g_free(resource_dir);
        g_free(sound_file);
        return TRUE;
    }

    g_free(audio_path_param);

    if (!resource_dir || !sound_file) {
        g_free(resource_dir);
        g_free(sound_file);
        return FALSE;
    }

    fprintf(stderr, "[AUDIO CLICKED] Resource dir: %s\n", resource_dir);
    fprintf(stderr, "[AUDIO CLICKED] File: %s\n", sound_file);

    char *audio_path = NULL;
    if (resolver) {
        audio_path = resolver(resource_dir, sound_file, user_data);
    }

    if (!audio_path) {
        audio_path = g_build_filename(resource_dir, sound_file, NULL);
    }

    if (audio_path && g_file_test(audio_path, G_FILE_TEST_EXISTS)) {
        audio_play_file(audio_path);
    } else {
        fprintf(stderr, "[AUDIO ERROR] File not found: %s\n", audio_path ? audio_path : sound_file);
    }

    g_free(audio_path);
    g_free(resource_dir);
    g_free(sound_file);
    return TRUE;
}

