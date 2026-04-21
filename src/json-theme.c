#include "json-theme.h"
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <string.h>

typedef struct {
    char *name;
    char *bg;
    char *fg;
    char *accent;
    char *link;
    char *border;
    char *heading;
    char *trn;
    char *translit;
    char *ex;
    char *com;
    char *pos;
    char *string;
} JsonTheme;

static GPtrArray *json_themes = NULL;

static void json_theme_free(JsonTheme *theme) {
    if (!theme) return;
    g_free(theme->name);
    g_free(theme->bg);
    g_free(theme->fg);
    g_free(theme->accent);
    g_free(theme->link);
    g_free(theme->border);
    g_free(theme->heading);
    g_free(theme->trn);
    g_free(theme->translit);
    g_free(theme->ex);
    g_free(theme->com);
    g_free(theme->pos);
    g_free(theme->string);
    g_free(theme);
}

/* Removes JSON comments (single/multi) and trailing commas *
 * copied and adapted from ViTE theme-manager.c             */
static char *clean_json_string(const char *input) {
    size_t len = strlen(input);
    char *out = g_malloc(len + 1);
    size_t j = 0;
    gboolean in_string = FALSE;
    gboolean in_single_line_comment = FALSE;
    gboolean in_multi_line_comment = FALSE;
    
    for (size_t i = 0; i < len; i++) {
        if (in_single_line_comment) {
            if (input[i] == '\n') {
                in_single_line_comment = FALSE;
                out[j++] = input[i]; /* preserve line numbers */
            }
            continue;
        }
        if (in_multi_line_comment) {
            if (input[i] == '*' && i + 1 < len && input[i+1] == '/') {
                in_multi_line_comment = FALSE;
                i++;
            } else if (input[i] == '\n') {
                out[j++] = input[i]; /* preserve line numbers */
            }
            continue;
        }
        if (!in_string) {
            if (input[i] == '/' && i + 1 < len) {
                if (input[i+1] == '/') {
                    in_single_line_comment = TRUE;
                    i++;
                    continue;
                } else if (input[i+1] == '*') {
                    in_multi_line_comment = TRUE;
                    i++;
                    continue;
                }
            }
        }
        if (input[i] == '"' && (i == 0 || input[i-1] != '\\' || (i > 1 && input[i-2] == '\\'))) {
            in_string = !in_string;
        }
        out[j++] = input[i];
    }
    out[j] = '\0';
    
    /* Second pass: Remove trailing commas before } or ] */
    for (size_t i = 0; i < j; i++) {
        if (out[i] == ',') {
            /* Look ahead for } or ] */
            size_t k = i + 1;
            while (k < j && g_ascii_isspace(out[k])) k++;
            if (k < j && (out[k] == '}' || out[k] == ']')) {
                out[i] = ' '; /* Erase trailing comma */
            }
        }
    }
    return out;
}

static const char *json_object_get_string_safe(JsonObject *obj, const char *member) {
    if (!obj || !json_object_has_member(obj, member)) return NULL;
    JsonNode *node = json_object_get_member(obj, member);
    if (!JSON_NODE_HOLDS_VALUE(node)) return NULL;
    return json_node_get_string(node);
}

typedef enum {
    SLOT_ACCENT,
    SLOT_HEADING,
    SLOT_LINK,
    SLOT_TRN,
    SLOT_TRANSLIT,
    SLOT_EX,
    SLOT_COM,
    SLOT_POS,
    SLOT_STRING
} TokenSlot;

typedef struct {
    const char *scope;
    TokenSlot slot;
} TokenMap;

static const TokenMap token_map[] = {
    { "keyword",            SLOT_ACCENT },
    { "keyword.control",    SLOT_ACCENT },
    { "entity.name.function", SLOT_HEADING },
    { "entity.name.type",   SLOT_HEADING },
    { "entity.name.class",  SLOT_HEADING },
    { "markup.link",        SLOT_LINK },
    { "variable",           SLOT_TRN },
    { "constant",           SLOT_TRANSLIT },
    { "constant.numeric",   SLOT_POS },
    { "string",             SLOT_STRING },
    { "comment",            SLOT_COM },
    { "markup.italic",      SLOT_EX },
    { NULL, 0 }
};

static gboolean scope_starts_with(const char *scope, const char *prefix) {
    if (!g_str_has_prefix(scope, prefix)) return FALSE;
    size_t plen = strlen(prefix);
    char next = scope[plen];
    return (next == '\0' || next == '.');
}

static void apply_token_rule(const char *json_rule, const char *color, JsonTheme *theme, int *slot_scores) {
    char *trimmed_rule = g_strstrip(g_strdup(json_rule));

    for (int i = 0; token_map[i].scope != NULL; i++) {
        if (scope_starts_with(token_map[i].scope, trimmed_rule)) {
            int score = (strlen(trimmed_rule) * 1000) / strlen(token_map[i].scope);
            TokenSlot slot = token_map[i].slot;
            
            if (score > 0 && score >= slot_scores[slot]) {
                char **target = NULL;
                switch (slot) {
                    case SLOT_ACCENT:   target = &theme->accent; break;
                    case SLOT_HEADING:  target = &theme->heading; break;
                    case SLOT_LINK:     target = &theme->link; break;
                    case SLOT_TRN:      target = &theme->trn; break;
                    case SLOT_TRANSLIT: target = &theme->translit; break;
                    case SLOT_EX:       target = &theme->ex; break;
                    case SLOT_COM:      target = &theme->com; break;
                    case SLOT_POS:      target = &theme->pos; break;
                    case SLOT_STRING:   target = &theme->string; break;
                }
                
                if (target) {
                    g_free(*target);
                    *target = g_strdup(color);
                }
                slot_scores[slot] = score;
            }
        }
    }
    g_free(trimmed_rule);
}

static void parse_token_colors(JsonArray *token_colors, JsonTheme *theme) {
    int slot_scores[SLOT_STRING + 1];
    memset(slot_scores, 0, sizeof(slot_scores));

    guint n = json_array_get_length(token_colors);
    for (guint i = 0; i < n; i++) {
        JsonNode *elem = json_array_get_element(token_colors, i);
        if (!JSON_NODE_HOLDS_OBJECT(elem)) continue;
        JsonObject *rule = json_node_get_object(elem);

        if (!json_object_has_member(rule, "settings")) continue;
        JsonObject *settings = json_object_get_object_member(rule, "settings");
        if (!settings) continue;
        
        const char *fg = json_object_get_string_safe(settings, "foreground");
        if (!fg) continue;

        JsonNode *scope_node = json_object_has_member(rule, "scope") ?
            json_object_get_member(rule, "scope") : NULL;
        if (!scope_node) continue;

        GPtrArray *scopes = g_ptr_array_new();
        if (JSON_NODE_HOLDS_ARRAY(scope_node)) {
            JsonArray *arr = json_node_get_array(scope_node);
            guint sn = json_array_get_length(arr);
            for (guint si = 0; si < sn; si++) {
                const char *s = json_array_get_string_element(arr, si);
                if (s) g_ptr_array_add(scopes, (gpointer)s);
            }
        } else if (JSON_NODE_HOLDS_VALUE(scope_node)) {
            const char *s = json_node_get_string(scope_node);
            if (s) {
                char **parts = g_strsplit(s, ",", -1);
                for (int pi = 0; parts[pi]; pi++) {
                    char *trimmed = g_strstrip(g_strdup(parts[pi]));
                    g_ptr_array_add(scopes, trimmed);
                }
                g_strfreev(parts);
            }
        }

        for (guint si = 0; si < scopes->len; si++) {
            const char *scope_str = g_ptr_array_index(scopes, si);
            apply_token_rule(scope_str, fg, theme, slot_scores);
        }
        g_ptr_array_free(scopes, TRUE);
    }
}

static JsonTheme *load_theme_from_json(const char *path) {
    GError *error = NULL;
    char *contents = NULL;
    gsize length = 0;

    if (!g_file_get_contents(path, &contents, &length, &error)) {
        g_warning("Could not read theme file '%s': %s", path, error->message);
        g_error_free(error);
        return NULL;
    }

    char *cleaned_json = clean_json_string(contents);
    g_free(contents);

    JsonParser *parser = json_parser_new();
    if (!json_parser_load_from_data(parser, cleaned_json, -1, &error)) {
        g_warning("Could not parse theme '%s': %s", path, error->message);
        g_error_free(error);
        g_free(cleaned_json);
        g_object_unref(parser);
        return NULL;
    }
    g_free(cleaned_json);

    JsonNode *root = json_parser_get_root(parser);
    if (!JSON_NODE_HOLDS_OBJECT(root)) {
        g_object_unref(parser);
        return NULL;
    }

    JsonObject *obj = json_node_get_object(root);
    const char *name = json_object_get_string_safe(obj, "name");
    if (!name) {
        char *basename = g_path_get_basename(path);
        char *dotpos = strrchr(basename, '.');
        if (dotpos) *dotpos = '\0';
        name = basename;
    }

    JsonTheme *theme = g_new0(JsonTheme, 1);
    theme->name = g_strdup(name);

    if (json_object_has_member(obj, "colors")) {
        JsonObject *colors = json_object_get_object_member(obj, "colors");
        if (colors) {
            const char *val;
            if ((val = json_object_get_string_safe(colors, "editor.background")))
                theme->bg = g_strdup(val);
            if ((val = json_object_get_string_safe(colors, "editor.foreground")))
                theme->fg = g_strdup(val);
            if ((val = json_object_get_string_safe(colors, "textLink.foreground")) ||
                (val = json_object_get_string_safe(colors, "editor.selectionBackground")))
                theme->link = g_strdup(val);
            if ((val = json_object_get_string_safe(colors, "editorLineNumber.foreground")) ||
                (val = json_object_get_string_safe(colors, "tab.border")) ||
                (val = json_object_get_string_safe(colors, "editorGroup.border")))
                theme->border = g_strdup(val);
        }
    }

    if (json_object_has_member(obj, "tokenColors")) {
        JsonArray *token_colors = json_object_get_array_member(obj, "tokenColors");
        if (token_colors) parse_token_colors(token_colors, theme);
    }
    
    /* Fallbacks for essential colors */
    if (!theme->bg) theme->bg = g_strdup("#1e1e1e");
    if (!theme->fg) theme->fg = g_strdup("#d4d4d4");
    if (!theme->border) theme->border = g_strdup("#444444");
    if (!theme->accent) theme->accent = g_strdup(theme->fg);
    if (!theme->heading) theme->heading = g_strdup(theme->fg);
    if (!theme->trn) theme->trn = g_strdup(theme->fg);
    if (!theme->translit) theme->translit = g_strdup(theme->fg);
    if (!theme->ex) theme->ex = g_strdup(theme->fg);
    if (!theme->com) theme->com = g_strdup(theme->fg);
    if (!theme->pos) theme->pos = g_strdup(theme->fg);
    if (!theme->string) theme->string = g_strdup(theme->fg);

    if (!theme->link) {
        /* Intelligent link fallback based on background luminance */
        gboolean is_dark = TRUE;
        int r, g, b;
        if (theme->bg && sscanf(theme->bg + 1, "%02x%02x%02x", &r, &g, &b) == 3) {
            double lum = 0.2126 * r + 0.7152 * g + 0.0722 * b;
            is_dark = lum < 128;
        }
        theme->link = g_strdup(is_dark ? "#7fb0e0" : "#005bbb");
    }

    g_object_unref(parser);
    return theme;
}

static void scan_theme_dir(const char *dir_path) {
    if (!g_file_test(dir_path, G_FILE_TEST_IS_DIR)) return;
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (!dir) return;

    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        char *full_path = g_build_filename(dir_path, name, NULL);
        if (g_file_test(full_path, G_FILE_TEST_IS_DIR)) {
            scan_theme_dir(full_path);
        } else if (g_str_has_suffix(name, ".json")) {
            JsonTheme *theme = load_theme_from_json(full_path);
            if (theme) {
                g_ptr_array_add(json_themes, theme);
            }
        }
        g_free(full_path);
    }
    g_dir_close(dir);
}

void json_theme_manager_init(void) {
    if (json_themes) return;
    json_themes = g_ptr_array_new_with_free_func((GDestroyNotify)json_theme_free);

    /* 1. Global themes directories (handles /app/share/ in Flatpak and /usr/share/ on host) */
    const gchar *const *data_dirs = g_get_system_data_dirs();
    for (int i = 0; data_dirs[i] != NULL; i++) {
        char *datadir = g_build_filename(data_dirs[i], "diction", "themes", NULL);
        scan_theme_dir(datadir);
        g_free(datadir);
    }

    /* Local runtime paths if running flatpak locally or dev */
    char *cwd_data_themes = g_build_filename("data", "themes", NULL);
    scan_theme_dir(cwd_data_themes);
    g_free(cwd_data_themes);

    char *cwd_vscode_themes = g_build_filename("vscode-themes", NULL);
    scan_theme_dir(cwd_vscode_themes);
    g_free(cwd_vscode_themes);

    /* 2. User directory */
    char *userdir = g_build_filename(g_get_user_data_dir(), "diction", "themes", NULL);
    scan_theme_dir(userdir);
    g_free(userdir);
}

void json_theme_manager_cleanup(void) {
    if (json_themes) {
        g_ptr_array_free(json_themes, TRUE);
        json_themes = NULL;
    }
}

int json_theme_get_count(void) {
    return json_themes ? json_themes->len : 0;
}

const char* json_theme_get_name(int index) {
    if (!json_themes || index < 0 || (guint)index >= json_themes->len) return NULL;
    JsonTheme *theme = g_ptr_array_index(json_themes, index);
    return theme->name;
}

gboolean json_theme_get_palette_by_name(const char *name, dsl_theme_palette *out_palette) {
    if (!json_themes || !name || !out_palette) return FALSE;

    for (guint i = 0; i < json_themes->len; i++) {
        JsonTheme *theme = g_ptr_array_index(json_themes, i);
        if (g_strcmp0(theme->name, name) == 0) {
            out_palette->bg = theme->bg;
            out_palette->fg = theme->fg;
            out_palette->accent = theme->accent;
            out_palette->link = theme->link;
            out_palette->border = theme->border;
            out_palette->heading = theme->heading;
            out_palette->trn = theme->trn;
            out_palette->translit = theme->translit;
            out_palette->ex = theme->ex;
            out_palette->com = theme->com;
            out_palette->pos = theme->pos;
            out_palette->string = theme->string;
            return TRUE;
        }
    }
    return FALSE;
}
