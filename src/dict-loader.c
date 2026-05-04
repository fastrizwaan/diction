#include "dict-loader.h"
#include "dict-cache.h"
#include "settings.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <archive.h>
#include <archive_entry.h>
#include "dict-chunked.h"
#include "dict-fts-index.h"

const char* dict_get_definition(DictMmap *dict, const FlatTreeEntry *entry, size_t *out_len, char **out_to_free) {
    if (!dict || !entry) return NULL;
    if (out_len) *out_len = entry->d_len;
    if (out_to_free) *out_to_free = NULL;

    if (dict->is_compressed && dict->chunk_reader) {
        char *decomp = dict_chunk_reader_get_definition(dict->chunk_reader, (uint64_t)entry->d_off, (uint64_t)entry->d_len);
        if (out_to_free) *out_to_free = decomp;
        return decomp;
    }

    return dict->data + entry->d_off;
}

size_t dict_search_fts(DictMmap *dict, const char *dict_path,
                       const char *query, GRegex *regex,
                       size_t start_pos, gboolean fts_enabled)
{
    if (!fts_enabled || !dict || !dict->index || !query || !regex)
        return (size_t)-1;

    /* Get FTS5 candidate entry IDs — NULL if index missing or no hit. */
    GArray *candidates = dict_fts_query_candidates(dict_path, query,
                                                   start_pos, 2000);
    if (!candidates)
        return (size_t)-1; /* no index or no FTS hit — no fallback scan */

    size_t result = (size_t)-1;
    for (guint i = 0; i < candidates->len; i++) {
        guint32 eid = g_array_index(candidates, guint32, i);
        const FlatTreeEntry *entry = flat_index_get(dict->index, (size_t)eid);
        if (!entry || entry->d_len == 0) continue;

        char *to_free = NULL;
        size_t def_len = 0;
        const char *def = dict_get_definition(dict, entry, &def_len, &to_free);
        gboolean matches = def &&
            g_regex_match_full(regex, def, (gssize)def_len, 0, 0, NULL, NULL);
        g_free(to_free);
        if (matches) {
            result = (size_t)eid;
            break;
        }
    }

    g_array_free(candidates, TRUE);
    return result;
}




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

static int is_xdxf_archive(const char *path, char *out_xdxf, size_t out_sz) {
    struct archive *a = archive_read_new();
    struct archive_entry *entry;
    int found = 0;

    archive_read_support_format_tar(a);
    archive_read_support_format_zip(a);
    archive_read_support_filter_bzip2(a);
    archive_read_support_filter_gzip(a);
    archive_read_support_filter_xz(a);

    if (archive_read_open_filename(a, path, 10240) != ARCHIVE_OK) {
        archive_read_free(a);
        return 0;
    }

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(entry);
        if (name && (ends_with_ci(name, ".xdxf") || ends_with_ci(name, ".xdxf.dz"))) {
            found = 1;
            if (out_xdxf && out_sz > 0) {
                strncpy(out_xdxf, name, out_sz - 1);
                out_xdxf[out_sz - 1] = '\0';
            }
            break;
        }
        archive_read_data_skip(a);
    }

    archive_read_close(a);
    archive_read_free(a);
    return found;
}

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
    if (ends_with_ci(path, ".xdxf") || ends_with_ci(path, ".xdxf.dz"))
        return DICT_FORMAT_XDXF;
    if (ends_with_ci(path, ".tar.bz2") || ends_with_ci(path, ".tar.gz") || ends_with_ci(path, ".tar.xz") || ends_with_ci(path, ".tgz") || ends_with_ci(path, ".zip")) {
        return DICT_FORMAT_XDXF;
    }
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
        case DICT_FORMAT_XDXF:
            dict = parse_xdxf_file(path, cancel_flag, expected_generation);
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

typedef struct {
    char *path;
    DictFormat format;
    char *name;
} DictCandidate;

static DictCandidate *dict_candidate_new(const char *path, DictFormat fmt) {
    DictCandidate *c = g_new0(DictCandidate, 1);
    c->path = g_strdup(path);
    c->format = fmt;
    c->name = basename_noext(path);
    return c;
}

static void dict_candidate_free(DictCandidate *c) {
    if (!c) return;
    g_free(c->path);
    g_free(c->name);
    g_free(c);
}

static void scan_recursive(const char *dirpath,
                           GList **candidates_out,
                           GHashTable *seen_dsl_families,
                           volatile gint *cancel_flag,
                           gint expected_generation,
                           int depth) {
    if (depth > 10) return;
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
                scan_recursive(full, candidates_out, seen_dsl_families, cancel_flag, expected_generation, depth + 1);
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

        if (fmt == DICT_FORMAT_DSL && is_dsl_family_path(full)) {
            char *preferred = dsl_preferred_variant(full);
            family_key = dsl_family_key(full);
            if (seen_dsl_families && g_hash_table_contains(seen_dsl_families, family_key)) {
                g_free(family_key);
                g_free(preferred);
                g_free(full);
                continue;
            }
            if (seen_dsl_families) g_hash_table_add(seen_dsl_families, family_key);
            else g_free(family_key);
            
            load_path = preferred;
        }

        *candidates_out = g_list_prepend(*candidates_out, dict_candidate_new(load_path, fmt));
        if (load_path != full) g_free((char*)load_path);
        g_free(full);
    }

    g_dir_close(dir);
}

static void discover_with_find(const char *dirpath, GList **candidates_out, volatile gint *cancel_flag, gint expected_generation, GCancellable *cancellable) {
    char *expanded = NULL;
    if (dirpath[0] == '~') {
        expanded = g_build_filename(g_get_home_dir(), dirpath + 1, NULL);
    } else {
        expanded = g_canonicalize_filename(dirpath, NULL);
    }

    char *quoted_dir = g_shell_quote(expanded);
    char *cmd = g_strdup_printf("find %s -maxdepth 10 -type f", quoted_dir);

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
        fprintf(stderr, "[LOADER] Found file: %s (format=%d)\n", line, fmt);
        if (fmt == DICT_FORMAT_UNKNOWN) {
            g_free(line);
            continue;
        }

        const char *load_path = line;
        char *family_key = NULL;

        if (fmt == DICT_FORMAT_DSL && is_dsl_family_path(line)) {
            char *preferred = dsl_preferred_variant(line);
            family_key = dsl_family_key(line);
            if (g_hash_table_contains(seen_dsl_families, family_key)) {
                g_free(family_key);
                g_free(preferred);
                g_free(line);
                continue;
            }
            g_hash_table_add(seen_dsl_families, family_key);
            load_path = preferred;
        }

        *candidates_out = g_list_prepend(*candidates_out, dict_candidate_new(load_path, fmt));
        if (load_path != line) g_free((char*)load_path);
        g_free(line);
    }
    
    g_subprocess_force_exit(sub);
    g_object_unref(dstream);
    g_object_unref(sub);
    g_hash_table_unref(seen_dsl_families);
}


DictEntry* dict_loader_scan_directory(const char *dirpath) {
    GList *candidates = NULL;
    GHashTable *seen_dsl_families = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    scan_recursive(dirpath, &candidates, seen_dsl_families, NULL, 0, 0);
    g_hash_table_destroy(seen_dsl_families);
    
    candidates = g_list_reverse(candidates);

    DictEntry *head = NULL, *tail = NULL;
    int loaded_count = 0;
    for (GList *l = candidates; l && loaded_count < MAX_DICTS; l = l->next) {

        DictCandidate *c = l->data;
        DictMmap *loaded = dict_load_any(c->path, c->format, NULL, 0);
        if (!loaded) {
            char *fallback = dsl_fallback_variant(c->path);
            if (fallback) {
                loaded = dict_load_any(fallback, c->format, NULL, 0);
                g_free(fallback);
            }
        }
        if (loaded) {
            DictEntry *entry = g_new0(DictEntry, 1);
            entry->name = g_strdup(loaded->name && *loaded->name ? loaded->name : c->name);
            entry->path = g_strdup(c->path);
            entry->format = c->format;
            entry->dict = loaded;
            entry->ref_count = 1; entry->magic = 0xDEADC0DE;
            if (loaded->icon_path) entry->icon_path = g_strdup(loaded->icon_path);
            
            if (!head) head = entry;
            if (tail) tail->next = entry;
            tail = entry;
            loaded_count++;
        }

        dict_candidate_free(c);
    }
    g_list_free(candidates);
    return head;
}

void dict_loader_scan_directory_streaming(const char *dirpath, DictLoaderCallback callback, void *user_data, volatile gint *cancel_flag, gint expected_generation, GCancellable *cancellable) {
    if (!dirpath || !callback) return;

    GList *candidates = NULL;
    gboolean has_find = g_file_test("/usr/bin/find", G_FILE_TEST_EXISTS);

    if (has_find) {
        discover_with_find(dirpath, &candidates, cancel_flag, expected_generation, cancellable);
    } else {
        GHashTable *seen_dsl_families = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        scan_recursive(dirpath, &candidates, seen_dsl_families, cancel_flag, expected_generation, 0);
        g_hash_table_unref(seen_dsl_families);
    }
    
    candidates = g_list_reverse(candidates);

    /* Phase 1: Signal DISCOVERED to UI immediately */
    int discovery_count = 0;
    for (GList *l = candidates; l; l = l->next) {
        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected_generation) break;
        DictCandidate *c = l->data;
        DictEntry discovery_entry = {0}; discovery_entry.magic = 0xDEADC0DE; discovery_entry.ref_count = 9999;
        discovery_entry.name = c->name;
        discovery_entry.path = c->path;
        discovery_entry.format = c->format;
        callback(&discovery_entry, DICT_LOADER_EVENT_DISCOVERED, user_data);
        fprintf(stderr, "[SCANNER] Discovered %s\n", c->path);
        discovery_count++;
    }

    /* Phase 2: Throttled loading and indexing */
    int loaded_count = 0;
    fprintf(stderr, "[SCANNER] Phase 2: Starting to load %d discovered files (Limit: %d)\n", discovery_count, MAX_DICTS);
    for (GList *l = candidates; l && loaded_count < MAX_DICTS; l = l->next) {

        if (cancel_flag && g_atomic_int_get(cancel_flag) != expected_generation) break;
        DictCandidate *c = l->data;

        callback(NULL, DICT_LOADER_EVENT_STARTED, user_data);

        fprintf(stderr, "[SCANNER] Loading %s...\n", c->path);
        DictMmap *loaded = dict_load_any(c->path, c->format, cancel_flag, expected_generation);
        fprintf(stderr, "[SCANNER] Finished %s -> %s\n", c->path, loaded ? "SUCCESS" : "FAILED");
        if (!loaded) {
            char *fallback = dsl_fallback_variant(c->path);
            if (fallback) {
                loaded = dict_load_any(fallback, c->format, cancel_flag, expected_generation);
                g_free(fallback);
            }
        }

        if (loaded) {
            DictEntry *entry = g_new0(DictEntry, 1);
            entry->name = g_strdup(loaded->name && *loaded->name ? loaded->name : c->name);
            entry->path = g_strdup(c->path);
            entry->format = c->format;
            entry->dict = loaded;
            entry->ref_count = 1; entry->magic = 0xDEADC0DE;
            if (loaded->icon_path) entry->icon_path = g_strdup(loaded->icon_path);

            callback(entry, DICT_LOADER_EVENT_FINISHED, user_data);
            loaded_count++;
        } else {

            callback(NULL, DICT_LOADER_EVENT_FAILED, user_data);
        }
        
        dict_candidate_free(c);
        l->data = NULL; /* Mark as freed to avoid double-free below */
    }

    /* Free remaining unprocessed candidates (from early cancellation break) */
    for (GList *l = candidates; l; l = l->next) {
        if (l->data) dict_candidate_free(l->data);
    }
    g_list_free(candidates);

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
        g_free(entry->dict_id);
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
