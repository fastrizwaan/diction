#include "dict-loader.h"
#include "dict-cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

/* ── helpers ─────────────────────────────────────────────── */

static int ends_with_ci(const char *s, const char *suffix) {
    size_t sl = strlen(s), xl = strlen(suffix);
    if (sl < xl) return 0;
    return strcasecmp(s + sl - xl, suffix) == 0;
}

static char *basename_noext(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t len = dot ? (size_t)(dot - base) : strlen(base);
    return g_strndup(base, len);
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

        /* Strip double extension for compressed DSL (.dsl.dz) */
        if (ends_with_ci(base_no_ext, ".dsl.dz")) {
            base_no_ext[strlen(base_no_ext) - 7] = '\0'; /* strip ".dsl.dz" */
        } else if (ends_with_ci(base_no_ext, ".dsl")) {
            base_no_ext[strlen(base_no_ext) - 4] = '\0';
        } else {
            /* Generic: strip the last extension */
            char *dot = strrchr(base_no_ext, '.');
            if (dot) *dot = '\0';
        }
        
        /* 1. Try basename match (e.g. dictname.png / dictname.bmp) */
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

        /* 3. Convert BMP/ICO to PNG so WebKit <img> tags can display them */
        if (dict->icon_path) {
            gboolean is_bmp = g_str_has_suffix(dict->icon_path, ".bmp") ||
                              g_str_has_suffix(dict->icon_path, ".BMP");
            gboolean is_ico = g_str_has_suffix(dict->icon_path, ".ico") ||
                              g_str_has_suffix(dict->icon_path, ".ICO");
            if (is_bmp || is_ico) {
                char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, dict->icon_path, -1);
                const char *base = dict_cache_base_dir();
                char *icons_dir = g_build_filename(base, "diction", "icons", NULL);
                g_mkdir_with_parents(icons_dir, 0755);

                /* e.g. ~/.cache/diction/icons/ab12cd...89.png */
                char *png_path = g_strdup_printf("%s/%s.png", icons_dir, hash);
                g_free(icons_dir);
                g_free(hash);

                if (!g_file_test(png_path, G_FILE_TEST_EXISTS)) {
                    GError *err = NULL;
                    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(dict->icon_path, &err);
                    if (pixbuf) {
                        if (!gdk_pixbuf_save(pixbuf, png_path, "png", &err, NULL)) {
                            g_clear_error(&err);
                            g_free(png_path);
                            png_path = NULL;
                        }
                        g_object_unref(pixbuf);
                    } else {
                        g_clear_error(&err);
                        g_free(png_path);
                        png_path = NULL;
                    }
                }

                if (png_path) {
                    g_free(dict->icon_path);
                    dict->icon_path = png_path;
                }
            }
        }
        
        g_free(base_no_ext);
    }


    return dict;
}


/* ── directory scanner ───────────────────────────────────── */

static void scan_recursive(const char *dirpath,
                           DictLoaderCallback callback, void *user_data,
                           GHashTable *seen_dsl_families,
                           volatile gint *cancel_flag,
                           gint expected_generation,
                           int depth) {
    if (depth > 5) return;
    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected_generation) return;

    GDir *dir = g_dir_open(dirpath, 0, NULL);
    if (!dir) return;

    const char *name;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected_generation) break;
        if (name[0] == '.') continue;

        char *full = g_build_filename(dirpath, name, NULL);
        
        gboolean is_dir = g_file_test(full, G_FILE_TEST_IS_DIR);

        if (is_dir) {
            /* Skip extracted resource folders */
            if (!ends_with_ci(full, ".files") &&
                !ends_with_ci(full, ".dsl.files") &&
                !ends_with_ci(full, ".dsl.dz.files")) {
                scan_recursive(full, callback, user_data, seen_dsl_families, cancel_flag, expected_generation, depth + 1);
            }
            g_free(full);
            continue;
        }

        DictFormat fmt = dict_detect_format(full);
        if (fmt == DICT_FORMAT_UNKNOWN) {
            g_free(full);
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
                g_free(full);
                continue;
            }
            preferred_path = dsl_preferred_variant(full);
            fallback_path = dsl_fallback_variant(preferred_path);
            load_path = preferred_path;
        }

        /* Signal discovery */
        if (callback) {
            DictEntry discovery_entry = {0}; discovery_entry.magic = 0xDEADC0DE; discovery_entry.ref_count = 9999;
            char *base = basename_noext(load_path);
            discovery_entry.name = base;
            discovery_entry.path = (char*)load_path;
            discovery_entry.format = fmt;
            callback(&discovery_entry, DICT_LOADER_EVENT_DISCOVERED, user_data);
            g_free(base);
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
            g_free(full);
            continue;
        }

        if (family_key && seen_dsl_families) {
            g_hash_table_add(seen_dsl_families, family_key);
            family_key = NULL;
        }

        char *owned_load_path = g_strdup(load_path);
        DictEntry *entry = g_new0(DictEntry, 1);
        if (loaded->name && strlen(loaded->name) > 0) {
            char *valid = g_utf8_make_valid(loaded->name, -1);
            entry->name = g_strdup(valid);
            g_free(valid);
        } else {
            char *base = basename_noext(owned_load_path);
            char *valid = g_utf8_make_valid(base, -1);
            entry->name = g_strdup(valid);
            g_free(valid);
            g_free(base);
        }

        char *valid_path = g_utf8_make_valid(owned_load_path, -1);
        entry->path = g_strdup(valid_path);
        g_free(valid_path);
        g_free(owned_load_path);
        g_free(full);
        entry->format = fmt;
        entry->dict = loaded;
        entry->ref_count = 1; entry->magic = 0xDEADC0DE;
        if (loaded->icon_path) {
            entry->icon_path = g_strdup(loaded->icon_path);
        }

        if (callback) {
            callback(entry, DICT_LOADER_EVENT_FINISHED, user_data);
        } else {
            dict_entry_unref(entry);
        }

        g_free(family_key);
        g_free(preferred_path);
        g_free(fallback_path);
    }

    g_dir_close(dir);
}

static void discover_with_find(const char *dirpath, DictLoaderCallback callback, void *user_data, volatile gint *cancel_flag, gint expected_generation, GCancellable *cancellable) {
    char *expanded = NULL;
    if (dirpath[0] == '~') {
        expanded = g_build_filename(g_get_home_dir(), dirpath + 1, NULL);
    } else {
        expanded = g_strdup(dirpath);
    }

    char *quoted_dir = g_shell_quote(expanded);
    char *cmd = g_strdup_printf(
        "find %s -maxdepth 5 -type f \\( "
        "-iname \"*.mdx\" -o -iname \"*.dsl\" -o -iname \"*.dsl.dz\" -o "
        "-iname \"*.ifo\" -o -iname \"*.bgl\" -o -iname \"*.slob\" \\) "
        "-not -path \"*/node_modules/*\" -not -path \"*/.git/*\" "
        "-not -path \"*/build/*\" -not -path \"*/dist/*\" "
        "-not -path \"*/__pycache__/*\" 2>/dev/null",
        quoted_dir);

    gchar **argv = NULL;
    g_shell_parse_argv(cmd, NULL, &argv, NULL);
    
    GError *err = NULL;
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    GSubprocess *sub = g_subprocess_launcher_spawnv(launcher, (const gchar * const *)argv, &err);
    g_strfreev(argv);
    g_object_unref(launcher);
    
    g_free(expanded);
    
    if (!sub) {
        if (err) g_error_free(err);
        return;
    }

    GInputStream *stdout_stream = g_subprocess_get_stdout_pipe(sub);
    GDataInputStream *dstream = g_data_input_stream_new(stdout_stream);

    GHashTable *seen_dsl_families = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *line = NULL;
    gsize line_len = 0;

    while ((line = g_data_input_stream_read_line_utf8(dstream, &line_len, cancellable, NULL)) != NULL) {
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected_generation) {
            g_free(line);
            break;
        }

        if (line[0] == '\0') {
            g_free(line);
            continue;
        }

        DictFormat fmt = dict_detect_format(line);
        if (fmt == DICT_FORMAT_UNKNOWN) {
            g_free(line);
            continue;
        }

        const char *load_path = line;
        char *family_key = NULL;
        char *preferred_path = NULL;
        char *fallback_path = NULL;

        if (fmt == DICT_FORMAT_DSL && is_dsl_family_path(line)) {
            family_key = dsl_family_key(line);
            if (g_hash_table_contains(seen_dsl_families, family_key)) {
                g_free(family_key);
                g_free(line);
                continue;
            }
            preferred_path = dsl_preferred_variant(line);
            fallback_path = dsl_fallback_variant(preferred_path);
            load_path = preferred_path;
        }

        if (callback) {
            DictEntry discovery_entry = {0}; discovery_entry.magic = 0xDEADC0DE; discovery_entry.ref_count = 9999;
            char *base = basename_noext(load_path);
            discovery_entry.name = base;
            discovery_entry.path = (char*)load_path;
            discovery_entry.format = fmt;
            callback(&discovery_entry, DICT_LOADER_EVENT_DISCOVERED, user_data);
            g_free(base);
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
            g_free(line);
            continue;
        }

        if (family_key) {
            g_hash_table_add(seen_dsl_families, family_key);
            family_key = NULL;
        }

        DictEntry *entry = g_new0(DictEntry, 1);
        if (loaded->name && strlen(loaded->name) > 0) {
            char *valid = g_utf8_make_valid(loaded->name, -1);
            entry->name = g_strdup(valid);
            g_free(valid);
        } else {
            char *base = basename_noext(load_path);
            char *valid = g_utf8_make_valid(base, -1);
            entry->name = g_strdup(valid);
            g_free(valid);
            g_free(base);
        }

        char *valid_path = g_utf8_make_valid(load_path, -1);
        entry->path = g_strdup(valid_path);
        g_free(valid_path);
        entry->format = fmt;
        entry->dict = loaded;
        entry->ref_count = 1; entry->magic = 0xDEADC0DE;
        if (loaded->icon_path) {
            entry->icon_path = g_strdup(loaded->icon_path);
        }

        if (callback) {
            callback(entry, DICT_LOADER_EVENT_FINISHED, user_data);
        }

        g_free(family_key);
        g_free(preferred_path);
        g_free(fallback_path);
        g_free(line);
    }
    
    g_subprocess_force_exit(sub);
    g_object_unref(dstream);
    g_object_unref(sub);
    g_hash_table_unref(seen_dsl_families);
}

static void scan_collect_callback(DictEntry *entry, DictLoaderEventType event, void *user_data) {
    if (event == DICT_LOADER_EVENT_FINISHED && entry) {
        GList **head = user_data;
        *head = g_list_prepend(*head, entry);
    }
}

DictEntry* dict_loader_scan_directory(const char *dirpath) {
    GList *head = NULL;
    GHashTable *seen_dsl_families = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    scan_recursive(dirpath, scan_collect_callback, &head, seen_dsl_families, NULL, 0, 0);
    g_hash_table_destroy(seen_dsl_families);
    
    DictEntry *result = NULL;
    if (head) {
        result = (DictEntry*)head->data;
        GList *curr = head;
        while (curr->next) {
            ((DictEntry*)curr->data)->next = (DictEntry*)curr->next->data;
            curr = curr->next;
        }
        g_list_free(head);
    }
    return result;
}

void dict_loader_scan_directory_streaming(const char *dirpath, DictLoaderCallback callback, void *user_data, volatile gint *cancel_flag, gint expected_generation, GCancellable *cancellable) {
    if (!dirpath || !callback) return;

    gboolean has_find = g_file_test("/usr/bin/find", G_FILE_TEST_EXISTS);

    if (has_find) {
        discover_with_find(dirpath, callback, user_data, cancel_flag, expected_generation, cancellable);
    } else {
        GHashTable *seen_dsl_families = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

        scan_recursive(dirpath, callback, user_data, seen_dsl_families, cancel_flag, expected_generation, 0);

        g_hash_table_unref(seen_dsl_families);
    }
    
    callback(NULL, DICT_LOADER_EVENT_FINISHED, user_data);
}

void dict_entry_ref(DictEntry *entry) {
    if (!entry) return;
    if (entry->magic != 0xDEADC0DE) {
        fprintf(stderr, "[FATAL] dict_entry_ref: Invalid magic 0x%08X at %p\n", entry->magic, entry);
        return;
    }
    g_atomic_int_inc(&entry->ref_count);
}

void dict_entry_unref(DictEntry *entry) {
    if (!entry) return;
    if (entry->magic != 0xDEADC0DE) {
        fprintf(stderr, "[FATAL] dict_entry_unref: Invalid magic 0x%08X at %p\n", entry->magic, entry);
        return;
    }
    if (g_atomic_int_dec_and_test(&entry->ref_count)) {
        entry->magic = 0xD1EDD1ED;
        if (entry->dict) dict_mmap_close(entry->dict);
        g_free(entry->name);
        g_free(entry->path);
        g_free(entry->guessed_lang_group);
        g_free(entry->icon_path);
        g_free(entry);
    }
}

void dict_loader_free_list(DictEntry *head) {
    while (head) {
        DictEntry *next = head->next;
        dict_entry_unref(head);
        head = next;
    }
}
