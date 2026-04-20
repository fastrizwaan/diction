#include "dict-loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── helpers ─────────────────────────────────────────────── */

static int ends_with_ci(const char *s, const char *suffix) {
    size_t sl = strlen(s), xl = strlen(suffix);
    if (sl < xl) return 0;
    return strcasecmp(s + sl - xl, suffix) == 0;
}

static char *path_join(const char *dir, const char *name) {
    size_t dl = strlen(dir), nl = strlen(name);
    int need_sep = (dl > 0 && dir[dl-1] != '/');
    char *out = malloc(dl + nl + 2);
    memcpy(out, dir, dl);
    if (need_sep) out[dl++] = '/';
    memcpy(out + dl, name, nl + 1);
    return out;
}

static char *basename_noext(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t len = dot ? (size_t)(dot - base) : strlen(base);
    char *out = malloc(len + 1);
    memcpy(out, base, len);
    out[len] = '\0';
    return out;
}

static gboolean file_exists_at(const char *path) {
    return path && access(path, F_OK) == 0;
}

static gboolean is_dsl_family_path(const char *path) {
    return ends_with_ci(path, ".dsl") || ends_with_ci(path, ".dsl.dz");
}

static char *dsl_family_key(const char *path) {
    if (ends_with_ci(path, ".dsl.dz")) {
        return g_strndup(path, strlen(path) - 3);
    }
    return g_strdup(path);
}

static char *dsl_preferred_variant(const char *path) {
    if (ends_with_ci(path, ".dsl.dz")) {
        return g_strdup(path);
    }
    if (ends_with_ci(path, ".dsl")) {
        char *compressed = g_strconcat(path, ".dz", NULL);
        if (file_exists_at(compressed)) {
            return compressed;
        }
        g_free(compressed);
    }
    return g_strdup(path);
}

static char *dsl_fallback_variant(const char *preferred_path) {
    if (ends_with_ci(preferred_path, ".dsl.dz")) {
        char *plain = g_strndup(preferred_path, strlen(preferred_path) - 3);
        if (file_exists_at(plain)) {
            return plain;
        }
        g_free(plain);
    } else if (ends_with_ci(preferred_path, ".dsl")) {
        char *compressed = g_strconcat(preferred_path, ".dz", NULL);
        if (file_exists_at(compressed)) {
            return compressed;
        }
        g_free(compressed);
    }
    return NULL;
}

/* ── format detection ────────────────────────────────────── */

DictFormat dict_detect_format(const char *path) {
    if (ends_with_ci(path, ".dsl.dz") || ends_with_ci(path, ".dsl"))
        return DICT_FORMAT_DSL;
    if (ends_with_ci(path, ".ifo"))
        return DICT_FORMAT_STARDICT;
    if (ends_with_ci(path, ".mdx"))
        return DICT_FORMAT_MDX;
    if (ends_with_ci(path, ".bgl"))
        return DICT_FORMAT_BGL;
    if (ends_with_ci(path, ".slob"))
        return DICT_FORMAT_SLOB;
    return DICT_FORMAT_UNKNOWN;
}

/* ── single dictionary loader ────────────────────────────── */

/* Forward declarations for format-specific parsers */
extern DictMmap* parse_bgl_file(const char *path, volatile gint *cancel_flag, gint expected);
extern DictMmap* parse_mdx_file(const char *path, volatile gint *cancel_flag, gint expected);
extern DictMmap* parse_stardict(const char *ifo_path, volatile gint *cancel_flag, gint expected);
extern DictMmap* parse_slob_file(const char *path, volatile gint *cancel_flag, gint expected);

DictMmap* dict_load_any(const char *path, DictFormat fmt, volatile gint *cancel_flag, gint expected_generation) {
    DictMmap *dict = NULL;
    switch (fmt) {
        case DICT_FORMAT_DSL:
            dict = dict_mmap_open(path, cancel_flag, expected_generation);
            break;

        case DICT_FORMAT_STARDICT:
            dict = parse_stardict(path, cancel_flag, expected_generation);
            break;

        case DICT_FORMAT_MDX:
            dict = parse_mdx_file(path, cancel_flag, expected_generation);
            break;

        case DICT_FORMAT_BGL:
            dict = parse_bgl_file(path, cancel_flag, expected_generation);
            break;
        case DICT_FORMAT_SLOB:
            dict = parse_slob_file(path, cancel_flag, expected_generation);
            break;

        default:
            dict = NULL;
            break;
    }

    if (dict && !dict->icon_path) {
        const char *img_exts[] = {".png", ".ico", ".jpg", ".jpeg", ".bmp", NULL};
        char *base_no_ext = g_strdup(path);
        char *dot = strrchr(base_no_ext, '.');
        if (dot) *dot = '\0';
        
        /* 1. Try basename match (e.g. dictname.png) */
        for (int i = 0; img_exts[i]; i++) {
            char *icon_candidate = g_strconcat(base_no_ext, img_exts[i], NULL);
            if (g_file_test(icon_candidate, G_FILE_TEST_EXISTS)) {
                dict->icon_path = icon_candidate;
                break;
            }
            g_free(icon_candidate);
        }
        
        /* 2. Try generic names (e.g. icon.png) in the same directory */
        if (!dict->icon_path) {
            char *dir = g_path_get_dirname(path);
            const char *generic_names[] = {"icon.png", "icon.ico", "icon.jpg", "logo.png", NULL};
            for (int i = 0; generic_names[i]; i++) {
                char *icon_candidate = g_build_filename(dir, generic_names[i], NULL);
                if (g_file_test(icon_candidate, G_FILE_TEST_EXISTS)) {
                    dict->icon_path = icon_candidate;
                    break;
                }
                g_free(icon_candidate);
            }
            g_free(dir);
        }
        
        g_free(base_no_ext);
    }


    return dict;
}


/* ── directory scanner ───────────────────────────────────── */

static void scan_recursive(const char *dirpath, DictEntry **head,
                           DictLoaderCallback callback, void *user_data,
                           GHashTable *seen_dsl_families,
                           volatile gint *cancel_flag,
                           gint expected_generation) {
    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected_generation) return;

    DIR *d = opendir(dirpath);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected_generation) break;

        if (ent->d_name[0] == '.') continue;

        char *full = path_join(dirpath, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) { free(full); continue; }

        if (S_ISDIR(st.st_mode)) {
            /* Skip extracted resource folders */
            if (!ends_with_ci(full, ".files") &&
                !ends_with_ci(full, ".dsl.files") &&
                !ends_with_ci(full, ".dsl.dz.files")) {
                scan_recursive(full, head, callback, user_data, seen_dsl_families, cancel_flag, expected_generation);
            }
            free(full);
            continue;
        }

        DictFormat fmt = dict_detect_format(full);
        if (fmt == DICT_FORMAT_UNKNOWN) {
            free(full);
            continue;
        }

        const char *load_path = full;
        char *family_key = NULL;
        char *preferred_path = NULL;
        char *fallback_path = NULL;

        if (fmt == DICT_FORMAT_DSL && is_dsl_family_path(full)) {
            family_key = dsl_family_key(full);
            if (seen_dsl_families && g_hash_table_contains(seen_dsl_families, family_key)) {
                g_free(family_key);
                free(full);
                continue;
            }
            preferred_path = dsl_preferred_variant(full);
            fallback_path = dsl_fallback_variant(preferred_path);
            load_path = preferred_path;
        }

        /* Signal discovery */
        if (callback) {
            DictEntry discovery_entry = {0};
            char *base = basename_noext(load_path);
            discovery_entry.name = base;
            discovery_entry.path = (char*)load_path;
            discovery_entry.format = fmt;
            callback(&discovery_entry, DICT_LOADER_EVENT_DISCOVERED, user_data);
            free(base);
        }

        if (callback) {
             callback(NULL, DICT_LOADER_EVENT_STARTED, user_data);
        }

        DictMmap *loaded = dict_load_any(load_path, fmt, cancel_flag, expected_generation);
        if (!loaded && fallback_path && g_strcmp0(fallback_path, load_path) != 0) {
            loaded = dict_load_any(fallback_path, fmt, cancel_flag, expected_generation);
            if (loaded) {
                load_path = fallback_path;
            }
        }
        if (!loaded) {
            if (callback) callback(NULL, DICT_LOADER_EVENT_FAILED, user_data);
            g_free(family_key);
            g_free(preferred_path);
            g_free(fallback_path);
            free(full);
            continue;
        }

        if (family_key && seen_dsl_families) {
            g_hash_table_add(seen_dsl_families, family_key);
            family_key = NULL;
        }

        char *owned_load_path = g_strdup(load_path);
        DictEntry *entry = calloc(1, sizeof(DictEntry));
        if (loaded->name && strlen(loaded->name) > 0) {
            char *valid = g_utf8_make_valid(loaded->name, -1);
            entry->name = strdup(valid);
            g_free(valid);
        } else {
            char *base = basename_noext(owned_load_path);
            char *valid = g_utf8_make_valid(base, -1);
            entry->name = strdup(valid);
            g_free(valid);
            free(base);
        }

        char *valid_path = g_utf8_make_valid(owned_load_path, -1);
        entry->path = strdup(valid_path);
        g_free(valid_path);
        g_free(owned_load_path);
        free(full);
        entry->format = fmt;
        entry->dict = loaded;
        if (loaded->icon_path) {
            entry->icon_path = strdup(loaded->icon_path);
        }

        if (callback) {
            callback(entry, DICT_LOADER_EVENT_FINISHED, user_data);
        }

        if (head) {
            entry->next = *head;
            *head = entry;
        }

        g_free(family_key);
        g_free(preferred_path);
        g_free(fallback_path);
    }

    closedir(d);
}

DictEntry* dict_loader_scan_directory(const char *dirpath) {
    DictEntry *head = NULL;
    GHashTable *seen_dsl_families = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    scan_recursive(dirpath, &head, NULL, NULL, seen_dsl_families, NULL, 0);
    g_hash_table_destroy(seen_dsl_families);
    return head;
}

void dict_loader_scan_directory_streaming(const char *dirpath, DictLoaderCallback callback, void *user_data, volatile gint *cancel_flag, gint expected_generation) {
    GHashTable *seen_dsl_families = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    scan_recursive(dirpath, NULL, callback, user_data, seen_dsl_families, cancel_flag, expected_generation);
    g_hash_table_destroy(seen_dsl_families);
}

void dict_loader_free(DictEntry *head) {
    while (head) {
        DictEntry *next = head->next;
        if (head->dict) dict_mmap_close(head->dict);
        free(head->name);
        free(head->path);
        free(head->guessed_lang_group);
        free(head->icon_path);
        free(head);
        head = next;
    }
}
