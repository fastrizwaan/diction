#include "dict-render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <glib.h>

typedef struct {
    char *str;
    size_t len;
    size_t cap;
} StrBuf;

static void buf_append(StrBuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        b->cap = b->cap == 0 ? 1024 : b->cap * 2;
        while (b->len + n + 1 > b->cap) b->cap *= 2;
        b->str = realloc(b->str, b->cap);
    }
    memcpy(b->str + b->len, s, n);
    b->len += n;
    b->str[b->len] = '\0';
}

static void buf_append_str(StrBuf *b, const char *s) {
    buf_append(b, s, strlen(s));
}

static char *normalize_color_name_key(const char *color_name) {
    GString *key = g_string_new("");
    for (const char *p = color_name; p && *p; p++) {
        if (g_ascii_isalpha(*p)) {
            g_string_append_c(key, (char)g_ascii_tolower(*p));
        }
    }
    return g_string_free(key, FALSE);
}

// Color name mapping for dark mode (too dark -> lighter)
static const char* get_dark_mode_color(const char *color_name) {
    struct { const char *dark; const char *light; } map[] = {
        {"black", "#d8d8d8"},
        {"blue", "#82b7ff"},
        {"darkblue", "#82b7ff"},
        {"mediumblue", "#8ab8ff"},
        {"navy", "#8ab8ff"},
        {"midnightblue", "#a7b8ff"},
        {"royalblue", "#8eb8ff"},
        {"slateblue", "#a993ff"},
        {"darkslateblue", "#ab9cff"},
        {"cyan", "#7ee7e7"},
        {"darkcyan", "#7ee7e7"},
        {"teal", "#7ad8d8"},
        {"turquoise", "#82f0ea"},
        {"green", "#84dc84"},
        {"darkgreen", "#84dc84"},
        {"forestgreen", "#84dc84"},
        {"seagreen", "#89e0a8"},
        {"darkolivegreen", "#b7d97c"},
        {"olive", "#c6cc79"},
        {"red", "#ff9c9c"},
        {"darkred", "#ff9c9c"},
        {"firebrick", "#ff9c9c"},
        {"maroon", "#f0a4b3"},
        {"crimson", "#ff9bb7"},
        {"brown", "#e0b07f"},
        {"saddlebrown", "#e0b07f"},
        {"purple", "#d2a4ff"},
        {"indigo", "#c3adff"},
        {"darkviolet", "#d69cff"},
        {"darkmagenta", "#e29cff"},
        {"violet", "#efb3ff"},
        {"magenta", "#f0adff"},
        {"darkorange", "#ffbf73"},
        {"orange", "#ffc56b"},
        {"goldenrod", "#e6cb74"},
        {"darkslategray", "#8fc9cf"},
        {"darkslategrey", "#8fc9cf"},
        {"dimgray", "#d8d8d8"},
        {"dimgrey", "#d8d8d8"},
        {NULL, NULL}
    };
    char *normalized = normalize_color_name_key(color_name);
    for (int i = 0; map[i].dark; i++) {
        if (strcmp(normalized, map[i].dark) == 0) {
            g_free(normalized);
            return map[i].light;
        }
    }
    g_free(normalized);
    return color_name;
}

// Color name mapping for light mode (too light -> darker)
static const char* get_light_mode_color(const char *color_name) {
    struct { const char *light; const char *dark; } map[] = {
        {"ivory", "darkkhaki"},
        {"lightgray", "gray"},
        {"lightgrey", "gray"},
        {"lightyellow", "goldenrod"},
        {"white", "beige"},
        {"yellow", "darkgoldenrod"},
        {NULL, NULL}
    };
    char *normalized = normalize_color_name_key(color_name);
    for (int i = 0; map[i].light; i++) {
        if (strcmp(normalized, map[i].light) == 0) {
            g_free(normalized);
            return map[i].dark;
        }
    }
    g_free(normalized);
    return color_name;
}

// Simple hex color lightening helper
static void lighten_hex_color(char *output, const char *hex, size_t output_size) {
    char *endptr;
    unsigned long r = 0, g = 0, b = 0;

    if (hex[0] != '#') {
        g_strlcpy(output, hex, output_size);
        return;
    }

    const char *c = hex + 1;
    size_t len = strlen(c);

    if (len == 3) {
        char tmp[4] = {c[0], c[0], '\0'};
        r = strtoul(tmp, &endptr, 16);
        tmp[0] = c[1]; tmp[1] = c[1];
        g = strtoul(tmp, &endptr, 16);
        tmp[0] = c[2]; tmp[1] = c[2];
        b = strtoul(tmp, &endptr, 16);
    } else if (len >= 6) {
        char tmp[3] = {c[0], c[1], '\0'};
        r = strtoul(tmp, &endptr, 16);
        tmp[0] = c[2]; tmp[1] = c[3];
        g = strtoul(tmp, &endptr, 16);
        tmp[0] = c[4]; tmp[1] = c[5];
        b = strtoul(tmp, &endptr, 16);
    } else {
        g_strlcpy(output, hex, output_size);
        return;
    }

    // Lighten by 30%
    r = (size_t)(r + (255 - r) * 0.3);
    g = (size_t)(g + (255 - g) * 0.3);
    b = (size_t)(b + (255 - b) * 0.3);

    g_snprintf(output, output_size, "#%02lx%02lx%02lx", r, g, b);
}

static void darken_hex_color(char *output, const char *hex, size_t output_size, double factor) {
    char *endptr;
    unsigned long r = 0, g = 0, b = 0;

    if (hex[0] != '#') {
        g_strlcpy(output, hex, output_size);
        return;
    }

    const char *c = hex + 1;
    size_t len = strlen(c);

    if (len == 3) {
        char tmp[4] = {c[0], c[0], '\0'};
        r = strtoul(tmp, &endptr, 16);
        tmp[0] = c[1]; tmp[1] = c[1];
        g = strtoul(tmp, &endptr, 16);
        tmp[0] = c[2]; tmp[1] = c[2];
        b = strtoul(tmp, &endptr, 16);
    } else if (len >= 6) {
        char tmp[3] = {c[0], c[1], '\0'};
        r = strtoul(tmp, &endptr, 16);
        tmp[0] = c[2]; tmp[1] = c[3];
        g = strtoul(tmp, &endptr, 16);
        tmp[0] = c[4]; tmp[1] = c[5];
        b = strtoul(tmp, &endptr, 16);
    } else {
        g_strlcpy(output, hex, output_size);
        return;
    }

    r = (unsigned long)(r * factor);
    g = (unsigned long)(g * factor);
    b = (unsigned long)(b * factor);
    g_snprintf(output, output_size, "#%02lx%02lx%02lx", r, g, b);
}

static char *adjust_color_value_for_theme(const char *value, gboolean dark_mode, gboolean is_background) {
    char *trimmed = g_strdup(value);
    g_strstrip(trimmed);

    if (!*trimmed) {
        return trimmed;
    }

    if (g_ascii_strcasecmp(trimmed, "transparent") == 0 ||
        g_str_has_prefix(trimmed, "var(") ||
        g_str_has_prefix(trimmed, "hsl(") ||
        g_str_has_prefix(trimmed, "hsla(") ||
        strstr(trimmed, "gradient(") ||
        strstr(trimmed, "url(")) {
        return trimmed;
    }

    if (g_str_has_prefix(trimmed, "rgb(") || g_str_has_prefix(trimmed, "rgba(")) {
        const char *open = strchr(trimmed, '(');
        const char *close = strrchr(trimmed, ')');
        if (open && close && close > open + 1) {
            char *inner = g_strndup(open + 1, close - open - 1);
            char **parts = g_strsplit(inner, ",", -1);
            if (parts[0] && parts[1] && parts[2]) {
                char *end = NULL;
                long r = strtol(g_strstrip(parts[0]), &end, 10);
                long g = strtol(g_strstrip(parts[1]), &end, 10);
                long b = strtol(g_strstrip(parts[2]), &end, 10);
                if (r >= 0 && r <= 255 && g >= 0 && g <= 255 && b >= 0 && b <= 255) {
                    if (dark_mode) {
                        if (is_background) {
                            if (r > 96 || g > 96 || b > 96) {
                                r = (long)(r * 0.24);
                                g = (long)(g * 0.24);
                                b = (long)(b * 0.24);
                            }
                        } else if (r < 160 || g < 160 || b < 160) {
                            r = r + (long)((230 - r) * 0.45);
                            g = g + (long)((230 - g) * 0.45);
                            b = b + (long)((230 - b) * 0.45);
                        }
                    } else if (!is_background && (r > 240 && g > 240 && b > 240)) {
                        r = 176;
                        g = 176;
                        b = 176;
                    }

                    char *rebuilt = NULL;
                    if (parts[3]) {
                        rebuilt = g_strdup_printf("rgba(%ld, %ld, %ld, %s)",
                                                  CLAMP(r, 0, 255),
                                                  CLAMP(g, 0, 255),
                                                  CLAMP(b, 0, 255),
                                                  g_strstrip(parts[3]));
                    } else {
                        rebuilt = g_strdup_printf("rgb(%ld, %ld, %ld)",
                                                  CLAMP(r, 0, 255),
                                                  CLAMP(g, 0, 255),
                                                  CLAMP(b, 0, 255));
                    }

                    g_strfreev(parts);
                    g_free(inner);
                    g_free(trimmed);
                    return rebuilt;
                }
            }
            g_strfreev(parts);
            g_free(inner);
        }
        return trimmed;
    }

    if (dark_mode) {
        if (trimmed[0] == '#') {
            char adjusted[64];
            if (is_background) {
                darken_hex_color(adjusted, trimmed, sizeof(adjusted), 0.28);
            } else {
                lighten_hex_color(adjusted, trimmed, sizeof(adjusted));
            }
            g_free(trimmed);
            return g_strdup(adjusted);
        }

        if (is_background) {
            if (g_ascii_strcasecmp(trimmed, "white") == 0 ||
                g_ascii_strcasecmp(trimmed, "ivory") == 0 ||
                g_ascii_strcasecmp(trimmed, "lightyellow") == 0) {
                g_free(trimmed);
                return g_strdup("#2b2b2b");
            }
        } else {
            const char *adjusted = get_dark_mode_color(trimmed);
            if (adjusted != trimmed) {
                g_free(trimmed);
                return g_strdup(adjusted);
            }
            if (g_ascii_strcasecmp(trimmed, "black") == 0 ||
                g_ascii_strcasecmp(trimmed, "dimgray") == 0 ||
                g_ascii_strcasecmp(trimmed, "dimgrey") == 0) {
                g_free(trimmed);
                return g_strdup("#d8d8d8");
            }
        }
    } else if (!is_background) {
        const char *adjusted = get_light_mode_color(trimmed);
        if (adjusted != trimmed) {
            g_free(trimmed);
            return g_strdup(adjusted);
        }
    }

    return trimmed;
}

static char *rewrite_style_for_theme(const char *style, gboolean dark_mode) {
    if (!style) {
        return g_strdup("");
    }

    char **parts = g_strsplit(style, ";", -1);
    GString *out = g_string_new("");

    for (int i = 0; parts[i]; i++) {
        char *decl = g_strdup(parts[i]);
        g_strstrip(decl);
        if (!*decl) {
            g_free(decl);
            continue;
        }

        char *colon = strchr(decl, ':');
        if (!colon) {
            if (out->len) g_string_append(out, "; ");
            g_string_append(out, decl);
            g_free(decl);
            continue;
        }

        *colon = '\0';
        char *prop = g_strdup(decl);
        char *val = g_strdup(colon + 1);
        g_strstrip(prop);
        g_strstrip(val);

        char *themed_val = NULL;
        if (g_ascii_strcasecmp(prop, "color") == 0 ||
            g_str_has_suffix(prop, "-color")) {
            gboolean is_background =
                g_ascii_strcasecmp(prop, "background-color") == 0 ||
                g_ascii_strcasecmp(prop, "border-color") == 0 ||
                g_str_has_prefix(prop, "background-");
            themed_val = adjust_color_value_for_theme(val, dark_mode, is_background);
        } else if (g_ascii_strcasecmp(prop, "background") == 0 &&
                   !strstr(val, "url(") && !strstr(val, "gradient(")) {
            themed_val = adjust_color_value_for_theme(val, dark_mode, TRUE);
        } else {
            themed_val = g_strdup(val);
        }

        if (out->len) g_string_append(out, "; ");
        g_string_append_printf(out, "%s: %s", prop, themed_val);

        g_free(themed_val);
        g_free(prop);
        g_free(val);
        g_free(decl);
    }

    g_strfreev(parts);
    return g_string_free(out, FALSE);
}

static char *rewrite_stylesheet_for_theme(const char *css, gboolean dark_mode) {
    if (!css) {
        return g_strdup("");
    }

    GString *out = g_string_new("");
    const char *p = css;

    while (*p) {
        const char *open = strchr(p, '{');
        if (!open) {
            g_string_append(out, p);
            break;
        }

        g_string_append_len(out, p, open - p + 1);

        int depth = 1;
        const char *inner = open + 1;
        const char *cursor = inner;
        while (*cursor && depth > 0) {
            if (*cursor == '{') {
                depth++;
            } else if (*cursor == '}') {
                depth--;
            }
            cursor++;
        }

        if (depth != 0) {
            g_string_append(out, inner);
            break;
        }

        size_t inner_len = (size_t)((cursor - 1) - inner);
        char *block = g_strndup(inner, inner_len);
        char *rewritten = strchr(block, '{')
            ? rewrite_stylesheet_for_theme(block, dark_mode)
            : rewrite_style_for_theme(block, dark_mode);
        g_string_append(out, rewritten);
        g_string_append_c(out, '}');
        g_free(rewritten);
        g_free(block);

        p = cursor;
    }

    return g_string_free(out, FALSE);
}

static void buf_append_escaped_html(StrBuf *b, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
            case '&':
                buf_append_str(b, "&amp;");
                break;
            case '<':
                buf_append_str(b, "&lt;");
                break;
            case '>':
                buf_append_str(b, "&gt;");
                break;
            case '"':
                buf_append_str(b, "&quot;");
                break;
            default:
                buf_append(b, &s[i], 1);
                break;
        }
    }
}

static char *normalize_headword_for_render(const char *text, size_t length, gboolean keep_middle_dot) {
    if (!text) {
        return g_strdup("");
    }

    char *raw = g_strndup(text, length);
    char *valid = g_utf8_make_valid(raw, -1);
    GString *out = g_string_new("");
    const char *p = valid;

    while (*p) {
        if (g_str_has_prefix(p, "{*}")) {
            if (keep_middle_dot) {
                g_string_append(out, "*");
            }
            p += strlen("{*}");
            continue;
        }

        if (g_str_has_prefix(p, "{·}")) {
            if (keep_middle_dot) {
                g_string_append(out, "·");
            }
            p += strlen("{·}");
            continue;
        }

        if (g_str_has_prefix(p, "·")) {
            if (keep_middle_dot) {
                g_string_append(out, "·");
            }
            p += strlen("·");
            continue;
        }

        if (*p == '\\' && p[1] != '\0') {
            const char *next = p + 1;
            const char *next_end = g_utf8_next_char(next);
            g_string_append_len(out, next, next_end - next);
            p = next_end;
            continue;
        }

        const char *next = g_utf8_next_char(p);
        g_string_append_len(out, p, next - p);
        p = next;
    }

    char *normalized = g_string_free(out, FALSE);
    g_free(valid);
    g_free(raw);
    return normalized;
}

static gboolean looks_like_html(const char *text, size_t length) {
    for (size_t i = 0; i + 3 < length; i++) {
        if (text[i] != '<') {
            continue;
        }

        char next = text[i + 1];
        if (g_ascii_isalpha(next) || next == '!' || next == '/' || next == '?') {
            return TRUE;
        }
    }

    return FALSE;
}

static gboolean looks_like_tagged_plain_markup(const char *text, size_t length) {
    if (!text || !length) {
        return FALSE;
    }

    const char *patterns[] = {
        "[c]",
        "[/c]",
        "[com]",
        "[/com]",
        "[lang",
        "[/lang]",
        "[m",
        "[/m]",
        "[trn]",
        "[/trn]",
        "[ref]",
        "[/ref]"
    };

    for (guint i = 0; i < G_N_ELEMENTS(patterns); i++) {
        if (g_strstr_len(text, length, patterns[i])) {
            return TRUE;
        }
    }

    return FALSE;
}

static char *normalize_tagged_plain_markup(const char *text, size_t length, size_t *out_length) {
    if (!text) {
        if (out_length) {
            *out_length = 0;
        }
        return g_strdup("");
    }

    GString *out = g_string_sized_new(length + 16);

    gboolean recent_visible_bullet = FALSE;

    for (size_t i = 0; i < length; ) {
        if (text[i] == '\\' && i + 1 < length && text[i + 1] == '\\') {
            g_string_append_c(out, '\\');
            recent_visible_bullet = FALSE;
            i += 2;
            continue;
        }

        if (text[i] == '[') {
            size_t end = i + 1;
            while (end < length && text[end] != ']') {
                end++;
            }

            if (end < length) {
                size_t tag_len = end - i - 1;
                const char *tag = text + i + 1;

                if ((tag_len == 1 && tag[0] == 'c') ||
                    (tag_len == 2 && tag[0] == '/' && tag[1] == 'c') ||
                    (tag_len == 4 && strncmp(tag, "lang", 4) == 0) ||
                    (tag_len == 5 && strncmp(tag, "/lang", 5) == 0) ||
                    (tag_len > 4 && strncmp(tag, "lang", 4) == 0 && g_ascii_isspace(tag[4]))) {
                    i = end + 1;
                    continue;
                }

                if (tag_len == 1 && tag[0] == '*') {
                    if (recent_visible_bullet) {
                        i = end + 1;
                        continue;
                    }
                } else if (tag_len == 2 && tag[0] == '/' && tag[1] == '*') {
                    i = end + 1;
                    continue;
                }
            }
        }

        g_string_append_c(out, text[i]);
        if (g_str_has_prefix(text + i, "▪") || g_str_has_prefix(text + i, "•")) {
            recent_visible_bullet = TRUE;
        } else if (!g_ascii_isspace(text[i])) {
            recent_visible_bullet = FALSE;
        }
        i++;
    }

    char *collapsed = g_string_free(out, FALSE);
    GRegex *dup_bullet = g_regex_new("([▪•])\\s*•\\s*", 0, 0, NULL);
    if (!dup_bullet) {
        if (out_length) {
            *out_length = strlen(collapsed);
        }
        return collapsed;
    }

    char *normalized = g_regex_replace(dup_bullet, collapsed, -1, 0, "\\1 ", 0, NULL);
    g_regex_unref(dup_bullet);
    g_free(collapsed);

    if (!normalized) {
        if (out_length) {
            *out_length = 0;
        }
        return g_strdup("");
    }

    if (out_length) {
        *out_length = strlen(normalized);
    }
    return normalized;
}

static char *normalize_resource_reference(const char *value, char **suffix_out) {
    const char *suffix = value + strlen(value);
    for (const char *p = value; *p; p++) {
        if (*p == '?' || *p == '#') {
            suffix = p;
            break;
        }
    }

    char *suffix_copy = g_strdup(suffix);
    char *path_part = g_strndup(value, suffix - value);
    char *decoded = g_uri_unescape_string(path_part, NULL);
    char *working = decoded ? decoded : g_strdup(path_part);
    char *normalized = g_strdup(working);
    char *dst = normalized;

    const char *src = working;
    while (*src == '/' || *src == '\\' || *src == '.') {
        src++;
    }

    while (*src) {
        *dst++ = (*src == '\\') ? '/' : *src;
        src++;
    }
    *dst = '\0';

    g_free(path_part);
    g_free(working);

    if (suffix_out) {
        *suffix_out = suffix_copy;
    } else {
        g_free(suffix_copy);
    }

    return normalized;
}

static char *resolve_local_resource_path(const char *resource_dir, const char *source_dir, const char *value);
static char *build_local_resource_uri(const char *resource_dir, const char *source_dir, const char *value);

static char *build_sound_uri(const char *resource_dir, const char *source_dir, const char *sound_file) {
    if (!sound_file) {
        return g_strdup(sound_file ? sound_file : "");
    }

    char *normalized = normalize_resource_reference(sound_file, NULL);
    char *resolved_path = resolve_local_resource_path(resource_dir, source_dir, normalized);
    char *uri = NULL;

    if (resolved_path) {
        char *escaped_path = g_uri_escape_string(resolved_path, NULL, FALSE);
        uri = g_strdup_printf("sound:///play?path=%s", escaped_path);
        g_free(escaped_path);
        g_free(resolved_path);
    } else if (resource_dir) {
        char *escaped_dir = g_uri_escape_string(resource_dir, NULL, FALSE);
        char *escaped_file = g_uri_escape_string(normalized, NULL, FALSE);
        uri = g_strdup_printf("sound:///play?dir=%s&file=%s", escaped_dir, escaped_file);
        g_free(escaped_dir);
        g_free(escaped_file);
    } else if (source_dir) {
        char *path = g_build_filename(source_dir, normalized, NULL);
        char *escaped_path = g_uri_escape_string(path, NULL, FALSE);
        uri = g_strdup_printf("sound:///play?path=%s", escaped_path);
        g_free(escaped_path);
        g_free(path);
    } else {
        uri = g_strdup(sound_file);
    }

    g_free(normalized);
    return uri;
}

static char *build_remote_sound_uri(const char *url) {
    if (!url) {
        return g_strdup("");
    }

    char *escaped_url = g_uri_escape_string(url, NULL, FALSE);
    char *uri = g_strdup_printf("sound:///play?url=%s", escaped_url);
    g_free(escaped_url);
    return uri;
}

static gboolean file_has_extension_ci(const char *path, const char *ext) {
    if (!path || !ext) {
        return FALSE;
    }
    const char *dot = strrchr(path, '.');
    return dot && g_ascii_strcasecmp(dot, ext) == 0;
}

static gboolean media_is_audio_file(const char *path) {
    const char *exts[] = { ".wav", ".mp3", ".ogg", ".oga", ".spx", ".flac", ".m4a", NULL };
    for (int i = 0; exts[i]; i++) {
        if (file_has_extension_ci(path, exts[i])) {
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean media_is_image_file(const char *path) {
    const char *exts[] = { ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp", ".svg", NULL };
    for (int i = 0; exts[i]; i++) {
        if (file_has_extension_ci(path, exts[i])) {
            return TRUE;
        }
    }
    return FALSE;
}

static void append_dsl_media_reference(StrBuf *b,
                                       const char *media_ref,
                                       const char *resource_dir,
                                       const char *source_dir) {
    if (!b || !media_ref) {
        return;
    }

    char *clean = g_strdup(media_ref);
    g_strstrip(clean);
    if (!*clean) {
        g_free(clean);
        return;
    }

    if (media_is_audio_file(clean)) {
        char *sound_uri = build_sound_uri(resource_dir, source_dir, clean);
        buf_append_str(b, "<a class='dict-audio' href='");
        buf_append_str(b, sound_uri);
        buf_append_str(b, "' title='");
        buf_append_escaped_html(b, clean, strlen(clean));
        buf_append_str(b, "'>🔊</a>");
        g_free(sound_uri);
    } else if (media_is_image_file(clean)) {
        char *img_uri = build_local_resource_uri(resource_dir, source_dir, clean);
        buf_append_str(b, "<img class='dsl-media-image' src='");
        buf_append_str(b, img_uri);
        buf_append_str(b, "' alt='");
        buf_append_escaped_html(b, clean, strlen(clean));
        buf_append_str(b, "' loading='lazy'>");
        g_free(img_uri);
    } else {
        char *file_uri = build_local_resource_uri(resource_dir, source_dir, clean);
        buf_append_str(b, "<a class='dict-link' href='");
        buf_append_str(b, file_uri);
        buf_append_str(b, "'>");
        buf_append_escaped_html(b, clean, strlen(clean));
        buf_append_str(b, "</a>");
        g_free(file_uri);
    }

    g_free(clean);
}

static char *resolve_local_resource_path(const char *resource_dir, const char *source_dir, const char *value) {
    if (!value) {
        return NULL;
    }

    char *normalized = normalize_resource_reference(value, NULL);
    char *path = NULL;

    if (source_dir) {
        path = g_build_filename(source_dir, normalized, NULL);
        if (g_file_test(path, G_FILE_TEST_EXISTS)) {
            g_free(normalized);
            return path;
        }
        g_free(path);
        path = NULL;
    }

    if (resource_dir) {
        path = g_build_filename(resource_dir, normalized, NULL);
        if (g_file_test(path, G_FILE_TEST_EXISTS) || !source_dir) {
            g_free(normalized);
            return path;
        }
        g_free(path);
    }

    g_free(normalized);
    return NULL;
}

static char *build_local_resource_uri(const char *resource_dir, const char *source_dir, const char *value) {
    if ((!resource_dir && !source_dir) || !value) {
        return g_strdup(value ? value : "");
    }

    char *suffix = NULL;
    char *normalized = normalize_resource_reference(value, &suffix);
    char *path = resolve_local_resource_path(resource_dir, source_dir, normalized);
    if (!path) {
        path = source_dir ? g_build_filename(source_dir, normalized, NULL)
                          : g_build_filename(resource_dir, normalized, NULL);
    }
    char *uri = g_filename_to_uri(path, NULL, NULL);

    if (!uri) {
        uri = g_strdup_printf("file://%s", path);
    }

    if (suffix && *suffix) {
        char *with_suffix = g_strconcat(uri, suffix, NULL);
        g_free(uri);
        uri = with_suffix;
    }

    g_free(path);
    g_free(normalized);
    g_free(suffix);
    return uri;
}

static char *get_html_attribute_value(const char *tag, const char *attr_name) {
    const char quotes[] = { '"', '\'', '\0' };

    for (int i = 0; quotes[i]; i++) {
        char quote = quotes[i];
        char *attr_pattern = g_strdup_printf("%s=%c", attr_name, quote);
        char *attr_pos = strstr(tag, attr_pattern);
        g_free(attr_pattern);

        if (!attr_pos) {
            continue;
        }

        char *value_start = attr_pos + strlen(attr_name) + 2;
        char *value_end = strchr(value_start, quote);
        if (!value_end) {
            break;
        }

        return g_strndup(value_start, value_end - value_start);
    }

    return NULL;
}

static char *extract_wiktionary_audio_url(const char *tag) {
    char *onclick = get_html_attribute_value(tag, "onclick");
    if (!onclick) {
        return NULL;
    }

    char *call = strstr(onclick, "kyw.a(this,");
    if (!call) {
        g_free(onclick);
        return NULL;
    }

    char *arg = strchr(call, ',');
    if (!arg) {
        g_free(onclick);
        return NULL;
    }

    arg++;
    while (*arg && g_ascii_isspace(*arg)) {
        arg++;
    }

    if (*arg != '\'' && *arg != '"') {
        g_free(onclick);
        return NULL;
    }

    char quote = *arg++;
    char *end = strchr(arg, quote);
    if (!end || end == arg) {
        g_free(onclick);
        return NULL;
    }

    char *token = g_strndup(arg, end - arg);
    char *url = g_strdup_printf("https://upload.wikimedia.org/wikipedia/commons/%s.ogg", token);
    g_free(token);
    g_free(onclick);
    return url;
}

static char *rewrite_css_urls(const char *css, const char *resource_dir, const char *source_dir) {
    if ((!resource_dir && !source_dir) || !css) {
        return g_strdup(css ? css : "");
    }

    GString *out = g_string_new("");
    const char *p = css;

    while (*p) {
        if (g_ascii_strncasecmp(p, "url(", 4) == 0) {
            const char *content_start = p + 4;
            const char *content_end = strchr(content_start, ')');
            if (!content_end) {
                g_string_append(out, p);
                break;
            }

            while (content_start < content_end && g_ascii_isspace(*content_start)) {
                content_start++;
            }

            const char *trim_end = content_end;
            while (trim_end > content_start && g_ascii_isspace(*(trim_end - 1))) {
                trim_end--;
            }

            char quote = '\0';
            if (trim_end > content_start && (*content_start == '"' || *content_start == '\'')) {
                quote = *content_start;
                content_start++;
                if (trim_end > content_start && *(trim_end - 1) == quote) {
                    trim_end--;
                }
            }

            char *inner = g_strndup(content_start, trim_end - content_start);
            char *replacement = NULL;

            if (strstr(inner, "://") || g_str_has_prefix(inner, "data:")) {
                replacement = g_strdup(inner);
            } else {
                replacement = build_local_resource_uri(resource_dir, source_dir, inner);
            }

            g_string_append(out, "url(");
            if (quote) {
                g_string_append_c(out, quote);
            }
            g_string_append(out, replacement);
            if (quote) {
                g_string_append_c(out, quote);
            }
            g_string_append_c(out, ')');

            g_free(inner);
            g_free(replacement);
            p = content_end + 1;
            continue;
        }

        g_string_append_c(out, *p++);
    }

    return g_string_free(out, FALSE);
}

static char *normalize_headword_for_link_target(const char *text) {
    char *normalized = normalize_headword_for_render(text, text ? strlen(text) : 0, FALSE);
    g_strstrip(normalized);
    return normalized;
}

static char *replace_attribute_value(const char *tag,
                                     const char *attr_name,
                                     const char *new_value,
                                     char quote_char) {
    char *attr_pattern = g_strdup_printf("%s=%c", attr_name, quote_char);
    char *attr_pos = strstr(tag, attr_pattern);
    g_free(attr_pattern);
    if (!attr_pos) {
        return g_strdup(tag);
    }

    char *value_start = attr_pos + strlen(attr_name) + 2;
    char *value_end = strchr(value_start, quote_char);
    if (!value_end) {
        return g_strdup(tag);
    }

    GString *result = g_string_sized_new(strlen(tag) + strlen(new_value) + 16);
    g_string_append_len(result, tag, value_start - tag);
    g_string_append(result, new_value);
    g_string_append(result, value_end);
    return g_string_free(result, FALSE);
}

static char *remove_html_attribute(const char *tag, const char *attr_name) {
    const char quotes[] = { '"', '\'', '\0' };

    for (int i = 0; quotes[i]; i++) {
        char quote = quotes[i];
        char *attr_pattern = g_strdup_printf("%s=%c", attr_name, quote);
        char *attr_pos = strstr(tag, attr_pattern);
        g_free(attr_pattern);

        if (!attr_pos) {
            continue;
        }

        char *value_start = attr_pos + strlen(attr_name) + 2;
        char *value_end = strchr(value_start, quote);
        if (!value_end) {
            break;
        }

        char *attr_start = attr_pos;
        while (attr_start > tag && g_ascii_isspace(*(attr_start - 1))) {
            attr_start--;
        }

        GString *result = g_string_sized_new(strlen(tag) + 1);
        g_string_append_len(result, tag, attr_start - tag);
        g_string_append(result, value_end + 1);
        return g_string_free(result, FALSE);
    }

    return g_strdup(tag);
}

static char *inline_local_stylesheet_if_possible(const char *tag, const char *resource_dir, const char *source_dir, gboolean dark_mode) {
    char *rel = get_html_attribute_value(tag, "rel");
    char *rel_lower = rel ? g_ascii_strdown(rel, -1) : NULL;
    if (!rel_lower || !g_strrstr(rel_lower, "stylesheet")) {
        g_free(rel_lower);
        g_free(rel);
        return g_strdup(tag);
    }

    g_free(rel_lower);
    g_free(rel);

    char *href = get_html_attribute_value(tag, "href");
    if (!href) {
        return g_strdup(tag);
    }

    gboolean is_local = !(strstr(href, "://") || g_str_has_prefix(href, "data:"));
    char *result = NULL;

    if ((resource_dir || source_dir) && is_local) {
        char *path = resolve_local_resource_path(resource_dir, source_dir, href);
        char *css = NULL;
        gsize css_len = 0;

        if (g_file_get_contents(path, &css, &css_len, NULL)) {
            char *with_urls = rewrite_css_urls(css, resource_dir, source_dir);
            char *with_theme = rewrite_stylesheet_for_theme(with_urls, dark_mode);
            result = g_strdup_printf("<style data-diction-inline-css='1'>%s</style>", with_theme);
            g_free(with_theme);
            g_free(with_urls);
        }

        g_free(css);
        g_free(path);
    }

    g_free(href);
    return result ? result : g_strdup(tag);
}

static char* process_html_tag_attribute(const char *tag, const char *attr_name, const char *resource_dir, const char *source_dir, gboolean dark_mode) {
    const char quotes[] = { '"', '\'', '\0' };

    for (int i = 0; quotes[i]; i++) {
        char quote = quotes[i];
        char *attr_pattern = g_strdup_printf("%s=%c", attr_name, quote);
        char *attr_pos = strstr(tag, attr_pattern);
        g_free(attr_pattern);

        if (!attr_pos) {
            continue;
        }

        char *value_start = attr_pos + strlen(attr_name) + 2;
        char *value_end = strchr(value_start, quote);
        if (!value_end) {
            break;
        }

        char *value = g_strndup(value_start, value_end - value_start);
        char *new_value = NULL;

        if (strcmp(attr_name, "style") == 0) {
            char *with_urls = rewrite_css_urls(value, resource_dir, source_dir);
            new_value = rewrite_style_for_theme(with_urls, dark_mode);
            g_free(with_urls);
        } else if (strcmp(attr_name, "color") == 0) {
            new_value = adjust_color_value_for_theme(value, dark_mode, FALSE);
        } else if (strcmp(attr_name, "bgcolor") == 0) {
            new_value = adjust_color_value_for_theme(value, dark_mode, TRUE);
        } else if (g_str_has_prefix(value, "entry://") || g_str_has_prefix(value, "bword://")) {
            const char *target = strstr(value, "://") + 3;
            char *escaped = g_uri_escape_string(target, NULL, FALSE);
            new_value = g_strdup_printf("dict://%s", escaped);
            g_free(escaped);
        } else if (g_str_has_prefix(value, "sound://")) {
            if (resource_dir) {
                new_value = build_sound_uri(resource_dir, source_dir, value + 8);
            } else {
                new_value = g_strdup(value);
            }
        } else if (strstr(value, "://") || value[0] == '#' || g_str_has_prefix(value, "data:")) {
            new_value = g_strdup(value);
        } else {
            if (resource_dir || source_dir) {
                new_value = build_local_resource_uri(resource_dir, source_dir, value);
            } else {
                new_value = g_strdup(value);
            }
        }

        char *updated = replace_attribute_value(tag, attr_name, new_value, quote);
        g_free(value);
        g_free(new_value);
        return updated;
    }

    return g_strdup(tag);
}

static char *process_html_common_attributes(const char *tag, const char *resource_dir, const char *source_dir, gboolean dark_mode) {
    char *processed = process_html_tag_attribute(tag, "style", resource_dir, source_dir, dark_mode);
    char *updated = process_html_tag_attribute(processed, "poster", resource_dir, source_dir, dark_mode);
    g_free(processed);
    processed = process_html_tag_attribute(updated, "color", resource_dir, source_dir, dark_mode);
    g_free(updated);
    updated = process_html_tag_attribute(processed, "bgcolor", resource_dir, source_dir, dark_mode);
    g_free(processed);
    return updated;
}

// Helper function to process srcset attributes in img tags
static char* process_html_srcset_attribute(const char *tag, const char *resource_dir, const char *source_dir) {
    char *result = g_strdup(tag);
    const char quotes[] = { '"', '\'', '\0' };

    for (int q = 0; quotes[q]; q++) {
        char quote = quotes[q];
        char *srcset_pattern = g_strdup_printf("srcset=%c", quote);
        char *srcset_pos = strstr(result, srcset_pattern);
        g_free(srcset_pattern);

        if (!srcset_pos) {
            continue;
        }

        char *value_start = srcset_pos + 8;
        char *value_end = strchr(value_start, quote);
        if (!value_end) {
            break;
        }

        size_t value_len = value_end - value_start;
        char *srcset_value = g_strndup(value_start, value_len);
        char **sources = g_strsplit(srcset_value, ",", -1);
        GString *new_srcset = g_string_new("");

        for (int i = 0; sources[i]; i++) {
            char *source = g_strstrip(sources[i]);
            if (*source) {
                char **parts = g_strsplit(source, " ", -1);
                if (parts[0] && *parts[0]) {
                    char *new_url = NULL;
                    if (strstr(parts[0], "://") || g_str_has_prefix(parts[0], "data:")) {
                        new_url = g_strdup(parts[0]);
                    } else {
                        new_url = (resource_dir || source_dir) ? build_local_resource_uri(resource_dir, source_dir, parts[0]) : g_strdup(parts[0]);
                    }

                    g_string_append(new_srcset, new_url);
                    g_free(new_url);

                    for (int j = 1; parts[j]; j++) {
                        if (*parts[j]) {
                            g_string_append_c(new_srcset, ' ');
                            g_string_append(new_srcset, parts[j]);
                        }
                    }
                }
                g_strfreev(parts);

                if (sources[i + 1]) {
                    g_string_append_c(new_srcset, ',');
                }
            }
        }

        char *new_result = replace_attribute_value(result, "srcset", new_srcset->str, quote);
        g_free(result);
        result = new_result;

        g_strfreev(sources);
        g_free(srcset_value);
        g_string_free(new_srcset, TRUE);
        break;
    }

    return result;
}

static char *unescape_xml_entities(const char *text) {
    if (!text) {
        return NULL;
    }

    GString *out = g_string_new("");
    const char *p = text;

    while (*p) {
        if (*p != '&') {
            g_string_append_c(out, *p++);
            continue;
        }

        const char *semi = strchr(p, ';');
        if (!semi) {
            g_string_append_c(out, *p++);
            continue;
        }

        size_t entity_len = semi - p + 1;
        if (g_str_has_prefix(p, "&lt;")) {
            g_string_append_c(out, '<');
        } else if (g_str_has_prefix(p, "&gt;")) {
            g_string_append_c(out, '>');
        } else if (g_str_has_prefix(p, "&amp;")) {
            g_string_append_c(out, '&');
        } else if (g_str_has_prefix(p, "&quot;")) {
            g_string_append_c(out, '"');
        } else if (g_str_has_prefix(p, "&apos;")) {
            g_string_append_c(out, '\'');
        } else if (entity_len >= 4 && p[1] == '#') {
            guint32 codepoint = 0;
            gboolean ok = FALSE;

            if ((p[2] == 'x' || p[2] == 'X') && entity_len > 4) {
                char *digits = g_strndup(p + 3, entity_len - 4);
                if (digits && *digits) {
                    char *endptr = NULL;
                    unsigned long parsed = strtoul(digits, &endptr, 16);
                    ok = endptr && *endptr == '\0' && g_unichar_validate((gunichar)parsed);
                    codepoint = (guint32)parsed;
                }
                g_free(digits);
            } else {
                char *digits = g_strndup(p + 2, entity_len - 3);
                if (digits && *digits) {
                    char *endptr = NULL;
                    unsigned long parsed = strtoul(digits, &endptr, 10);
                    ok = endptr && *endptr == '\0' && g_unichar_validate((gunichar)parsed);
                    codepoint = (guint32)parsed;
                }
                g_free(digits);
            }

            if (ok) {
                char utf8[7] = {0};
                int n = g_unichar_to_utf8((gunichar)codepoint, utf8);
                g_string_append_len(out, utf8, n);
            } else {
                g_string_append_len(out, p, entity_len);
            }
        } else {
            g_string_append_len(out, p, entity_len);
        }

        p = semi + 1;
    }

    return g_string_free(out, FALSE);
}

static char *substitute_mdx_stylesheet(const char *text, size_t length, const char *stylesheet_blob, size_t *out_length) {
    char *copy = g_strndup(text ? text : "", length);
    if (!copy || !stylesheet_blob || !*stylesheet_blob) {
        if (out_length) {
            *out_length = copy ? strlen(copy) : 0;
        }
        return copy;
    }

    char **lines = g_strsplit_set(stylesheet_blob, "\r\n", -1);
    GPtrArray *filtered = g_ptr_array_new();
    for (int i = 0; lines[i]; i++) {
        if (*lines[i]) {
            g_ptr_array_add(filtered, lines[i]);
        }
    }

    GHashTable *styles = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, (GDestroyNotify)g_strfreev);
    for (guint i = 0; i + 2 < filtered->len; i += 3) {
        char **pair = g_new0(char *, 3);
        pair[0] = unescape_xml_entities((const char *)g_ptr_array_index(filtered, i + 1));
        pair[1] = unescape_xml_entities((const char *)g_ptr_array_index(filtered, i + 2));
        g_hash_table_insert(styles, GINT_TO_POINTER(atoi((const char *)g_ptr_array_index(filtered, i))), pair);
    }

    GString *out = g_string_new("");
    const char *p = copy;
    char *pending_suffix = NULL;

    while (*p) {
        const char *tick = strchr(p, '`');
        if (!tick || !tick[1]) {
            g_string_append(out, p);
            break;
        }

        const char *num_start = tick + 1;
        const char *num_end = num_start;
        while (*num_end && g_ascii_isdigit(*num_end)) {
            num_end++;
        }
        if (*num_end != '`' || num_end == num_start) {
            g_string_append_len(out, p, (tick - p) + 1);
            p = tick + 1;
            continue;
        }

        g_string_append_len(out, p, tick - p);

        char *id_text = g_strndup(num_start, num_end - num_start);
        char **pair = g_hash_table_lookup(styles, GINT_TO_POINTER(atoi(id_text)));
        g_free(id_text);

        if (pair) {
            if (pending_suffix) {
                g_string_append(out, pending_suffix);
                g_clear_pointer(&pending_suffix, g_free);
            }
            g_string_append(out, pair[0] ? pair[0] : "");
            pending_suffix = g_strdup(pair[1] ? pair[1] : "");
        } else if (pending_suffix) {
            g_string_append(out, pending_suffix);
            g_clear_pointer(&pending_suffix, g_free);
        }

        p = num_end + 1;
    }

    if (pending_suffix) {
        g_string_append(out, pending_suffix);
        g_free(pending_suffix);
    }

    g_hash_table_destroy(styles);
    g_ptr_array_free(filtered, TRUE);
    g_strfreev(lines);
    g_free(copy);

    if (out_length) {
        *out_length = out->len;
    }
    return g_string_free(out, FALSE);
}

static char *strip_html_tags_to_text(const char *html) {
    if (!html) {
        return g_strdup("");
    }

    GString *out = g_string_new("");
    gboolean in_tag = FALSE;

    for (const char *p = html; *p; p++) {
        if (*p == '<') {
            in_tag = TRUE;
            continue;
        }
        if (*p == '>') {
            in_tag = FALSE;
            continue;
        }
        if (!in_tag) {
            g_string_append_c(out, *p);
        }
    }

    return g_string_free(out, FALSE);
}

static char *finalize_placeholder_dict_links(char *html) {
    if (!html) {
        return NULL;
    }

    const char *placeholder = "<a class='dict-link' href='#'>";
    const size_t placeholder_len = strlen(placeholder);
    GString *out = g_string_new("");
    const char *p = html;

    while (*p) {
        const char *anchor = strstr(p, placeholder);
        if (!anchor) {
            g_string_append(out, p);
            break;
        }

        g_string_append_len(out, p, anchor - p);

        const char *inner_start = anchor + placeholder_len;
        const char *close = strstr(inner_start, "</a>");
        if (!close) {
            g_string_append(out, anchor);
            break;
        }

        char *inner_html = g_strndup(inner_start, close - inner_start);
        char *plain = strip_html_tags_to_text(inner_html);
        char *target_raw = unescape_xml_entities(plain);
        char *target = normalize_headword_for_link_target(target_raw);
        g_strstrip(target);

        if (*target) {
            char *escaped = g_uri_escape_string(target, NULL, FALSE);
            g_string_append_printf(out, "<a class='dict-link' href='dict://%s'>", escaped);
            g_string_append(out, inner_html);
            g_string_append(out, "</a>");
            g_free(escaped);
        } else {
            g_string_append_len(out, anchor, (close + 4) - anchor);
        }

        g_free(target_raw);
        g_free(target);
        g_free(plain);
        g_free(inner_html);
        p = close + 4;
    }

    g_free(html);
    return g_string_free(out, FALSE);
}

char* dsl_render_to_html(const char *dsl_text,
                         size_t length,
                         const char *headword,
                         size_t hw_length,
                         DictFormat format,
                         const char *resource_dir,
                         const char *source_dir,
                         const char *mdx_stylesheet,
                         int dark_mode) {
    StrBuf b = {NULL, 0, 0};
    char *styled_text = NULL;
    char *normalized_plain_text = NULL;
    char *display_headword = normalize_headword_for_render(headword, hw_length, TRUE);

    if (format == DICT_FORMAT_MDX) {
        styled_text = substitute_mdx_stylesheet(dsl_text, length, mdx_stylesheet, &length);
        if (styled_text) {
            dsl_text = styled_text;
        }
    }

    // Theme colors based on Python Diction's _build_theme_css
    const char *body_color = dark_mode ? "#dddddd" : "#222222";
    const char *bg_color = dark_mode ? "#1e1e1e" : "#ffffff";
    const char *link_color = dark_mode ? "#89b4ff" : "#005bbb";
    const char *trn_color = dark_mode ? "#89b4ff" : "#1a73e8";
    const char *ex_color = dark_mode ? "#9ae59a" : "#5f6368";
    const char *com_color = dark_mode ? "#9ae59a" : "#006600";
    const char *pos_color = dark_mode ? "#9ae59a" : "#c90016";
    const char *translit_color = dark_mode ? "#888888" : "#808080";
    const char *heading_color = dark_mode ? "#89b4ff" : "#0b5394";
    const char *border_color = dark_mode ? "#444444" : "#cccccc";

    // Add GoldenDict-like styling with theme-aware colors
    buf_append_str(&b,
        "<style>"
        "body{font-family: system-ui, sans-serif; line-height: 1.4; color: ");
    buf_append_str(&b, body_color);
    buf_append_str(&b, "; background: ");
    buf_append_str(&b, bg_color);
    buf_append_str(&b, ";}"
        "img{max-width:100%; height:auto; vertical-align:middle;}"
        ".dict-audio{display:inline-block; line-height:0;}"
        ".dict-audio img{cursor:pointer;}"
        "table{max-width:100%; border-collapse:collapse;}"
        "td,th{vertical-align:top;}"
        "pre,code{white-space:pre-wrap;}"
        ".dict-link {color: ");
    buf_append_str(&b, link_color);
    buf_append_str(&b, "; text-decoration: none;}"
        ".dict-link:hover {text-decoration: underline;}"
        ".dsl-media-image{display:block; max-width:100%; height:auto; margin:0.35em 0;}"
        ".trn{color: ");
    buf_append_str(&b, trn_color);
    buf_append_str(&b, ";}"
        ".ex{color: ");
    buf_append_str(&b, ex_color);
    buf_append_str(&b, "; font-style: italic;}"
        ".com{color: ");
    buf_append_str(&b, com_color);
    buf_append_str(&b, ";}"
        ".pos{color: ");
    buf_append_str(&b, pos_color);
    buf_append_str(&b, "; font-weight: bold;}"
        ".translit{color: ");
    buf_append_str(&b, translit_color);
    buf_append_str(&b, "; font-style: italic;}"
        ".m-line{margin-top: 2px; margin-bottom: 2px;}"
        "hr{border: none; border-top: 1px solid ");
    buf_append_str(&b, border_color);
    buf_append_str(&b, "; margin: 10px 0;}"
        "</style>"
    );
    if (dark_mode) {
        buf_append_str(&b,
            "<style>"
            "pre,code{background:#242424; color:#ececec;}"
            "table,td,th{border-color:#555555;}"
            "a{color:#8fc7ff;}"
            "</style>");
    } else {
        buf_append_str(&b,
            "<style>"
            "pre,code{background:#f5f5f5; color:#222222;}"
            "table,td,th{border-color:#d0d0d0;}"
            "</style>");
    }

    buf_append_str(&b, "<h2 style='color:");
    buf_append_str(&b, heading_color);
    buf_append_str(&b, "; margin-bottom: 0.5em;'>");
    buf_append_escaped_html(&b, display_headword, strlen(display_headword));
    buf_append_str(&b, "</h2>\n<div>");

    if (format == DICT_FORMAT_MDX || format == DICT_FORMAT_STARDICT || format == DICT_FORMAT_BGL) {
        gboolean treat_as_html = (format == DICT_FORMAT_MDX || looks_like_html(dsl_text, length));
        gboolean treat_as_tagged_plain = (!treat_as_html && looks_like_tagged_plain_markup(dsl_text, length));

        if (!treat_as_html && !treat_as_tagged_plain) {
            buf_append_escaped_html(&b, dsl_text, length);
            buf_append_str(&b, "</div>");
            g_free(normalized_plain_text);
            g_free(styled_text);
            g_free(display_headword);
            return finalize_placeholder_dict_links(b.str);
        }

        if (treat_as_html) {
            /* Comprehensive HTML attribute rewriting for MDX */
            size_t head = 0;
            while (head < length) {
                /* Find HTML tags */
                const char *tag_start = strchr(dsl_text + head, '<');
                if (!tag_start) {
                    buf_append(&b, dsl_text + head, length - head);
                    break;
                }
                
                /* Append content before tag */
                buf_append(&b, dsl_text + head, tag_start - (dsl_text + head));
                head = tag_start - dsl_text;
                
                /* Parse tag */
                const char *tag_end = strchr(dsl_text + head, '>');
                if (!tag_end) {
                    buf_append(&b, dsl_text + head, length - head);
                    break;
                }
                
                size_t tag_len = tag_end - tag_start + 1;
            char *tag = g_strndup(tag_start, tag_len);
                /* Process tag based on type */
                char tag_name[32];
                const char *name_start = tag + 1;
                while (*name_start == '/' || g_ascii_isspace(*name_start)) {
                    name_start++;
                }
                const char *tag_name_end = name_start;
                while (*tag_name_end &&
                       !g_ascii_isspace(*tag_name_end) &&
                       *tag_name_end != '>' &&
                       *tag_name_end != '/') {
                    tag_name_end++;
                }
                size_t name_len = tag_name_end - name_start;
                if (name_len >= sizeof(tag_name)) name_len = sizeof(tag_name) - 1;
                for (size_t j = 0; j < name_len; j++) {
                    tag_name[j] = (char)g_ascii_tolower(name_start[j]);
                }
                tag_name[name_len] = '\0';
                
                if (strcmp(tag_name, "a") == 0 || strcmp(tag_name, "area") == 0) {
                    /* Link tags - check if it's an audio link */
                    if (strstr(tag, "sound://")) {
                        char *processed_tag = process_html_tag_attribute(tag, "href", resource_dir, source_dir, dark_mode);
                        const char *close_anchor = g_strstr_len(dsl_text + head + tag_len, length - head - tag_len, "</a>");
                        buf_append_str(&b, processed_tag);
                        buf_append_str(&b, "🔊</a>");
                        g_free(processed_tag);
                        g_free(tag);

                        if (close_anchor) {
                            head = (close_anchor - dsl_text) + 4;
                        } else {
                            head += tag_len;
                        }
                        continue;
                    }
                    
                    /* Regular link - handle href */
                    char *processed_tag = process_html_tag_attribute(tag, "href", resource_dir, source_dir, dark_mode);
                    char *final_tag = process_html_common_attributes(processed_tag, resource_dir, source_dir, dark_mode);
                    g_free(processed_tag);
                    buf_append_str(&b, final_tag);
                    g_free(final_tag);
                } else if (strcmp(tag_name, "link") == 0) {
                    /* Inline local stylesheets so color rewriting applies to their rules too */
                    char *inlined_or_tag = inline_local_stylesheet_if_possible(tag, resource_dir, source_dir, dark_mode);
                    if (strcmp(inlined_or_tag, tag) == 0) {
                        char *processed_tag = process_html_tag_attribute(tag, "href", resource_dir, source_dir, dark_mode);
                        char *final_tag = process_html_common_attributes(processed_tag, resource_dir, source_dir, dark_mode);
                        g_free(processed_tag);
                        buf_append_str(&b, final_tag);
                        g_free(final_tag);
                    } else {
                        buf_append_str(&b, inlined_or_tag);
                    }
                    g_free(inlined_or_tag);
                } else if (strcmp(tag_name, "img") == 0) {
                    /* Images - handle src and srcset */
                    char *wiktionary_audio_url = extract_wiktionary_audio_url(tag);
                    char *img_tag = wiktionary_audio_url ? remove_html_attribute(tag, "onclick") : g_strdup(tag);
                    char *processed_tag = process_html_tag_attribute(img_tag, "src", resource_dir, source_dir, dark_mode);
                    g_free(img_tag);
                    char *with_srcset = process_html_srcset_attribute(processed_tag, resource_dir, source_dir);
                    g_free(processed_tag);
                    processed_tag = process_html_common_attributes(with_srcset, resource_dir, source_dir, dark_mode);
                    g_free(with_srcset);
                    if (wiktionary_audio_url) {
                        char *sound_uri = build_remote_sound_uri(wiktionary_audio_url);
                        buf_append_str(&b, "<a class='dict-audio' href='");
                        buf_append_str(&b, sound_uri);
                        buf_append_str(&b, "'>");
                        buf_append_str(&b, processed_tag);
                        buf_append_str(&b, "</a>");
                        g_free(sound_uri);
                        g_free(wiktionary_audio_url);
                    } else {
                        buf_append_str(&b, processed_tag);
                    }
                    g_free(processed_tag);
                } else if (strcmp(tag_name, "style") == 0) {
                    const char *close_style = g_strstr_len(dsl_text + head + tag_len, length - head - tag_len, "</style>");
                    buf_append_str(&b, tag);
                    if (close_style) {
                        size_t css_len = close_style - (dsl_text + head + tag_len);
                        char *css = g_strndup(dsl_text + head + tag_len, css_len);
                        char *rewritten_css = rewrite_css_urls(css, resource_dir, source_dir);
                        char *themed_css = rewrite_stylesheet_for_theme(rewritten_css, dark_mode);
                        buf_append_str(&b, themed_css);
                        buf_append_str(&b, "</style>");
                        g_free(css);
                        g_free(rewritten_css);
                        g_free(themed_css);
                        g_free(tag);
                        head = (close_style - dsl_text) + 8;
                        continue;
                    }
                } else if (strcmp(tag_name, "script") == 0) {
                    /* Skip script tags with src */
                    if (strstr(tag, "src=\"")) {
                        /* Find matching closing script tag */
                        const char *close_script = strstr(dsl_text + head + tag_len, "</script>");
                        if (close_script) {
                            head = (close_script - dsl_text) + 9; // Skip </script>
                            g_free(tag);
                            continue;
                        }
                    }
                    buf_append_str(&b, tag);
                } else if (strcmp(tag_name, "source") == 0 || strcmp(tag_name, "audio") == 0 || strcmp(tag_name, "video") == 0) {
                    /* Media tags - handle src */
                    char *processed_tag = process_html_tag_attribute(tag, "src", resource_dir, source_dir, dark_mode);
                    char *with_common = process_html_common_attributes(processed_tag, resource_dir, source_dir, dark_mode);
                    g_free(processed_tag);
                    processed_tag = with_common;
                    buf_append_str(&b, processed_tag);
                    g_free(processed_tag);
                } else if (strcmp(tag_name, "object") == 0) {
                    /* Object tags - handle data */
                    char *processed_tag = process_html_tag_attribute(tag, "data", resource_dir, source_dir, dark_mode);
                    char *with_common = process_html_common_attributes(processed_tag, resource_dir, source_dir, dark_mode);
                    g_free(processed_tag);
                    processed_tag = with_common;
                    buf_append_str(&b, processed_tag);
                    g_free(processed_tag);
                } else {
                    /* Other tags - pass through */
                    char *processed_tag = process_html_common_attributes(tag, resource_dir, source_dir, dark_mode);
                    buf_append_str(&b, processed_tag);
                    g_free(processed_tag);
                }
                
                g_free(tag);
                head += tag_len;
            }
        }

        if (treat_as_html) {
            buf_append_str(&b, "</div>");
            g_free(normalized_plain_text);
            g_free(styled_text);
            g_free(display_headword);
            return finalize_placeholder_dict_links(b.str);
        }

        normalized_plain_text = normalize_tagged_plain_markup(dsl_text, length, &length);
        if (normalized_plain_text) {
            dsl_text = normalized_plain_text;
        }
    }

    size_t i = 0;
    
    // State tracking
    int in_media = 0;
    int m_open = 0;
    GString *media_buf = g_string_new("");
    
    while (i < length) {
        // Strip out {{ macros }} entirely
        if (dsl_text[i] == '{' && i + 1 < length && dsl_text[i+1] == '{') {
            size_t end = i + 2;
            while (end + 1 < length && !(dsl_text[end] == '}' && dsl_text[end+1] == '}')) {
                end++;
            }
            if (end + 1 < length) {
                i = end + 2;
                continue;
            }
        }
        
        if (in_media) {
            if (dsl_text[i] == '[' && i + 3 < length &&
                dsl_text[i + 1] == '/' && dsl_text[i + 2] == 's' && dsl_text[i + 3] == ']') {
                append_dsl_media_reference(&b, media_buf->str, resource_dir, source_dir);
                g_string_truncate(media_buf, 0);
                in_media = 0;
                i += 4;
                continue;
            }
            if (i + 6 < length &&
                dsl_text[i] == '[' && dsl_text[i + 1] == '/' &&
                dsl_text[i + 2] == 'i' && dsl_text[i + 3] == 'm' &&
                dsl_text[i + 4] == 'g' && dsl_text[i + 5] == ']') {
                append_dsl_media_reference(&b, media_buf->str, resource_dir, source_dir);
                g_string_truncate(media_buf, 0);
                in_media = 0;
                i += 6;
                continue;
            }

            g_string_append_c(media_buf, dsl_text[i]);
            i++;
            continue;
        }

        // Escape brackets
        if (dsl_text[i] == '\\' && i + 1 < length) {
            char next = dsl_text[i+1];
            if (next == '\\') {
                buf_append_str(&b, "\\");
                i += 2;
                continue;
            }
            if (next == '[' || next == ']' || next == '(' || next == ')' || next == '{' || next == '}' || next == '~') {
                buf_append(&b, &dsl_text[i+1], 1);
                i += 2;
                continue;
            }
        }

        if (dsl_text[i] == '[') {
             size_t end = i + 1;
             while (end < length && dsl_text[end] != ']') end++;
             if (end < length) {
                 size_t tag_len = end - i - 1;
                 const char *tag = dsl_text + i + 1;
                 
                 // Handle color tags like [c darkblue]
                 if (tag_len > 2 && tag[0] == 'c' && tag[1] == ' ') {
                     if (!in_media) {
                         buf_append_str(&b, "<span style='color:");

                         // Extract color name/value
                         char color_name[64];
                         size_t color_len = tag_len - 2;
                         if (color_len >= sizeof(color_name)) color_len = sizeof(color_name) - 1;
                         memcpy(color_name, tag + 2, color_len);
                         color_name[color_len] = '\0';

                         char *final_color = adjust_color_value_for_theme(color_name, dark_mode, FALSE);
                         buf_append_str(&b, final_color);
                         buf_append_str(&b, "'>");
                         g_free(final_color);
                     }
                 }
                 else if (tag_len == 1 && tag[0] == 'c') {
                     /* Bare [c] appears in some BGL content as a no-op marker. */
                 }
                 else if (tag_len == 2 && strncmp(tag, "/c", 2) == 0) {
                     if (!in_media) buf_append_str(&b, "</span>");
                 }
                 else if ((tag_len == 4 && strncmp(tag, "lang", 4) == 0) ||
                          (tag_len > 4 && strncmp(tag, "lang", 4) == 0 && isspace((unsigned char)tag[4]))) {
                     /* Language metadata is not visible article content. */
                 }
                 else if (tag_len == 5 && strncmp(tag, "/lang", 5) == 0) {
                     /* no-op */
                 }
                 
                 // M-Line handling [m1], [m2]...
                 else if (tag_len >= 1 && tag[0] == 'm' && (tag_len == 1 || isdigit(tag[1]))) {
                     if (!in_media) {
                         if (m_open) buf_append_str(&b, "</div>");
                         
                         int level = (tag_len > 1 && isdigit(tag[1])) ? (tag[1] - '0') : 1;
                         char mbuf[128];
                         snprintf(mbuf, sizeof(mbuf), "<div class='m-line' style='margin-left: %.1fem'>", (double)level * 1.2);
                         buf_append_str(&b, mbuf);
                         m_open = 1;
                     }
                 }
                 else if (tag_len == 2 && strncmp(tag, "/m", 2) == 0) {
                     if (!in_media && m_open) {
                         buf_append_str(&b, "</div>");
                         m_open = 0;
                     }
                 }
                 
                 // Links handling [ref]...[/ref]
                 else if (tag_len == 3 && strncmp(tag, "ref", 3) == 0) {
                     if (!in_media) buf_append_str(&b, "<a class='dict-link' href='#'>");
                 }
                 else if (tag_len == 4 && strncmp(tag, "/ref", 4) == 0) {
                     if (!in_media) buf_append_str(&b, "</a>");
                 }
                 
                 // Media references [s]...[/s]
                 else if (tag_len == 1 && tag[0] == 's') {
                     g_string_truncate(media_buf, 0);
                     in_media = 1;
                 }
                 else if (tag_len == 2 && tag[0] == '/' && tag[1] == 's') {
                     append_dsl_media_reference(&b, media_buf->str, resource_dir, source_dir);
                     g_string_truncate(media_buf, 0);
                     in_media = 0;
                 }
                 else if (tag_len == 3 && strncmp(tag, "img", 3) == 0) {
                     g_string_truncate(media_buf, 0);
                     in_media = 1;
                 }
                 else if (tag_len == 4 && strncmp(tag, "/img", 4) == 0) {
                     append_dsl_media_reference(&b, media_buf->str, resource_dir, source_dir);
                     g_string_truncate(media_buf, 0);
                     in_media = 0;
                 }
                 
                 // Transcription [t]...[/t]
                 else if (tag_len == 1 && tag[0] == 't') {
                     const char *t_color = dark_mode ? "#9ae59a" : "#1e8e3e";
                     if (!in_media) {
                         buf_append_str(&b, "<span style='color: ");
                         buf_append_str(&b, t_color);
                         buf_append_str(&b, "; font-family: sans-serif;'>");
                     }
                 }
                 else if (tag_len == 2 && tag[0] == '/' && tag[1] == 't') {
                     if (!in_media) buf_append_str(&b, "</span>");
                 }
                 
                 // Basics
                 else if (tag_len == 1 && tag[0] == 'b') { if(!in_media) buf_append_str(&b, "<b>"); }
                 else if (tag_len == 2 && strncmp(tag, "/b", 2) == 0) { if(!in_media) buf_append_str(&b, "</b>"); }
                 else if (tag_len == 1 && tag[0] == 'i') { if(!in_media) buf_append_str(&b, "<i>"); }
                 else if (tag_len == 2 && strncmp(tag, "/i", 2) == 0) { if(!in_media) buf_append_str(&b, "</i>"); }
                 else if (tag_len == 1 && tag[0] == 'u') { if(!in_media) buf_append_str(&b, "<u>"); }
                 else if (tag_len == 2 && strncmp(tag, "/u", 2) == 0) { if(!in_media) buf_append_str(&b, "</u>"); }
                 
                 // Superscript/Subscript
                 else if (tag_len == 3 && strncmp(tag, "sup", 3) == 0) { if(!in_media) buf_append_str(&b, "<sup>"); }
                 else if (tag_len == 4 && strncmp(tag, "/sup", 4) == 0) { if(!in_media) buf_append_str(&b, "</sup>"); }
                 else if (tag_len == 3 && strncmp(tag, "sub", 3) == 0) { if(!in_media) buf_append_str(&b, "<sub>"); }
                 else if (tag_len == 4 && strncmp(tag, "/sub", 4) == 0) { if(!in_media) buf_append_str(&b, "</sub>"); }
                 
                 // Bullet/list marker [*] is treated as structural, not visible text.
                 else if (tag_len == 1 && tag[0] == '*') { /* no-op */ }
                 else if (tag_len == 2 && strncmp(tag, "/*", 2) == 0) { /* ignore */ }
                 
                 else if (tag_len == 1 && tag[0] == 'p') { if(!in_media) buf_append_str(&b, "<span class='pos'>"); }
                 else if (tag_len == 2 && strncmp(tag, "/p", 2) == 0) { if(!in_media) buf_append_str(&b, "</span>"); }
                 else if (tag_len == 3 && strncmp(tag, "trn", 3) == 0) { if(!in_media) buf_append_str(&b, "<span class='trn'>"); }
                 else if (tag_len == 4 && strncmp(tag, "/trn", 4) == 0) { if(!in_media) buf_append_str(&b, "</span>"); }
                 else if (tag_len == 4 && strncmp(tag, "!trs", 4) == 0) { /* no-op */ }
                 else if (tag_len == 5 && strncmp(tag, "/!trs", 5) == 0) { /* no-op */ }
                 else if (tag_len == 2 && strncmp(tag, "ex", 2) == 0) { if(!in_media) buf_append_str(&b, "<span class='ex'>"); }
                 else if (tag_len == 3 && strncmp(tag, "/ex", 3) == 0) { if(!in_media) buf_append_str(&b, "</span>"); }
                 else if (tag_len == 3 && strncmp(tag, "com", 3) == 0) { if(!in_media) buf_append_str(&b, "<span class='com'>"); }
                 else if (tag_len == 4 && strncmp(tag, "/com", 4) == 0) { if(!in_media) buf_append_str(&b, "</span>"); }
                 
                 else {
                     // Potential transcription if unknown tag with no space (e.g. [frait])
                     int has_space = 0;
                     for(size_t j=0; j<tag_len; j++) if(isspace(tag[j])) has_space = 1;

                     if (!in_media) {
                         if (!has_space && tag_len > 0 && tag[0] != '/') {
                             const char *trans_color = dark_mode ? "#9ae59a" : "#1e8e3e";
                             buf_append_str(&b, "<span style='color: ");
                             buf_append_str(&b, trans_color);
                             buf_append_str(&b, ";'>[");
                             buf_append(&b, tag, tag_len);
                             buf_append_str(&b, "]</span>");
                         } else {
                             // Pass through as text but escaped
                             buf_append_str(&b, "[");
                             buf_append(&b, tag, tag_len);
                             buf_append_str(&b, "]");
                         }
                     }
                 }
                 
                 i = end + 1;
                 continue;
             }
        } 
        else if (dsl_text[i] == '<' && i + 1 < length && dsl_text[i+1] == '<') {
            size_t end = i + 2;
            while (end + 1 < length && !(dsl_text[end] == '>' && dsl_text[end+1] == '>')) end++;
            if (end + 1 < length) {
                    if (!in_media) {
                        size_t word_len = end - i - 2;
                        char *display_word = normalize_headword_for_render(dsl_text + i + 2, word_len, TRUE);
                    buf_append_str(&b, "<a class='dict-link' href='#'>");
                    buf_append_escaped_html(&b, display_word, strlen(display_word));
                    buf_append_str(&b, "</a>");
                    g_free(display_word);
                }
                i = end + 2;
                continue;
            }
        }
        else if (dsl_text[i] == '<') {
            if (!in_media) {
                // Potential sense identifier like <A>
                size_t end = i + 1;
                while (end < length && dsl_text[end] != '>' && (end - i < 10)) end++;
                if (end < length && dsl_text[end] == '>') {
                    const char *sense_color = dark_mode ? "#9ae59a" : "#c90016";
                    buf_append_str(&b, "<b style='color: ");
                    buf_append_str(&b, sense_color);
                    buf_append_str(&b, ";'>&lt;");
                    buf_append(&b, dsl_text + i + 1, end - i - 1);
                    buf_append_str(&b, "&gt;</b>");
                    i = end + 1;
                    continue;
                } else {
                    buf_append_str(&b, "&lt;");
                    i++;
                    continue;
                }
            } else { i++; continue; }
        }
        else if (dsl_text[i] == '\n') {
            if (!in_media) buf_append_str(&b, "\n");
            i++;
            continue;
        } 
        else if (dsl_text[i] == '~') {
            if (!in_media) buf_append_escaped_html(&b, display_headword, strlen(display_headword));
            i++;
            continue;
        } 
        else if (dsl_text[i] == '>') {
            if (!in_media) buf_append_str(&b, "&gt;");
            i++;
            continue;
        } 
        else if (dsl_text[i] == '&') {
            if (!in_media) buf_append_str(&b, "&amp;");
            i++;
            continue;
        }
        
        if (!in_media) buf_append(&b, &dsl_text[i], 1);
        i++;
    }
    
    if (m_open) buf_append_str(&b, "</div>");
    g_string_free(media_buf, TRUE);
    
    if (b.str == NULL) {
        buf_append_str(&b, " ");
    } else {
        buf_append_str(&b, "</div>");
    }
    
    g_free(normalized_plain_text);
    g_free(styled_text);
    g_free(display_headword);
    return finalize_placeholder_dict_links(b.str);
}
