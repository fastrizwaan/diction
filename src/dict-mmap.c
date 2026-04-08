#include "dict-mmap.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <zlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <utime.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <archive.h>
#include <archive_entry.h>

void insert_balanced(SplayTree *t, TreeEntry *e, int start, int end) {
    if (start > end) return;
    int mid = start + (end - start) / 2;
    if (e[mid].h_len > 0)
        splay_tree_insert(t, (size_t)e[mid].h_off, (size_t)e[mid].h_len, (size_t)e[mid].d_off, (size_t)e[mid].d_len);
    insert_balanced(t, e, start, mid - 1);
    insert_balanced(t, e, mid + 1, end);
}

/* ── Multi-headword aware DSL parser ──────────────────────
 * DSL format allows N consecutive non-indented lines as headwords
 * followed by indented definition lines.  ALL headwords share the
 * same definition block.
 *
 *   a          ← headword 1
 *   ए          ← headword 2
 *   ē          ← headword 3
 *   	[b]...[/b]  ← definition (starts with space/tab)
 */

// Cache directory helpers
static const char* get_cache_base_dir(void) {
    static const char *cache_dir = NULL;
    if (!cache_dir) {
        cache_dir = g_get_user_cache_dir();
    }
    return cache_dir;
}

static char* get_cache_dir_path(void) {
    const char *base = get_cache_base_dir();
    return g_build_filename(base, "diction", "dicts", NULL);
}

static char* get_cached_dict_path(const char *original_path) {
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, original_path, -1);
    const char *base = get_cache_base_dir();
    char *path = g_build_filename(base, "diction", "dicts", hash, NULL);
    g_free(hash);
    return path;
}

static gboolean is_cache_valid(const char *cache_path, const char *original_path) {
    struct stat cache_st, orig_st;

    if (stat(cache_path, &cache_st) != 0) {
        return FALSE;
    }
    if (stat(original_path, &orig_st) != 0) {
        return FALSE;
    }

    // Cache is valid if it's newer than the original
    return cache_st.st_mtime >= orig_st.st_mtime;
}

static gboolean ensure_cache_directory(void) {
    char *cache_dir = get_cache_dir_path();
    int ret = g_mkdir_with_parents(cache_dir, 0755);
    g_free(cache_dir);
    return ret == 0;
}

static char *get_resource_cache_dir_path(const char *original_path) {
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, original_path, -1);
    const char *base = get_cache_base_dir();
    char *path = g_build_filename(base, "diction", "resources", hash, NULL);
    g_free(hash);
    return path;
}

static char *get_resource_stamp_path(const char *resource_dir) {
    return g_build_filename(resource_dir, ".diction-resource-stamp", NULL);
}

static gboolean ensure_resource_cache_directory(const char *resource_dir) {
    return g_mkdir_with_parents(resource_dir, 0755) == 0;
}

static gboolean dir_has_visible_files(const char *dir_path) {
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (!dir) {
        return FALSE;
    }

    const char *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (name[0] == '.') {
            continue;
        }
        g_dir_close(dir);
        return TRUE;
    }

    g_dir_close(dir);
    return FALSE;
}

static char *replace_backslashes(const char *text) {
    char *copy = g_strdup(text ? text : "");
    for (char *p = copy; *p; p++) {
        if (*p == '\\') {
            *p = '/';
        }
    }
    return copy;
}

static gboolean path_has_parent_reference(const char *path) {
    if (!path || !*path) {
        return TRUE;
    }
    char **parts = g_strsplit(path, "/", -1);
    gboolean bad = FALSE;
    for (int i = 0; parts[i]; i++) {
        if (strcmp(parts[i], "..") == 0) {
            bad = TRUE;
            break;
        }
    }
    g_strfreev(parts);
    return bad;
}

static char *sanitize_archive_entry_path(const char *path) {
    char *slashes = replace_backslashes(path);
    char *normalized = g_strdup(slashes);
    g_free(slashes);

    g_strstrip(normalized);
    while (g_str_has_prefix(normalized, "/")) {
        memmove(normalized, normalized + 1, strlen(normalized));
    }
    while (g_str_has_prefix(normalized, "./")) {
        memmove(normalized, normalized + 2, strlen(normalized) - 1);
    }

    if (!*normalized || path_has_parent_reference(normalized)) {
        g_free(normalized);
        return NULL;
    }

    return normalized;
}

static void clear_directory_contents(const char *dir_path) {
    GDir *dir = g_dir_open(dir_path, 0, NULL);
    if (!dir) {
        return;
    }

    const char *name = NULL;
    while ((name = g_dir_read_name(dir)) != NULL) {
        if (name[0] == '.') {
            continue;
        }

        char *child = g_build_filename(dir_path, name, NULL);
        if (g_file_test(child, G_FILE_TEST_IS_DIR)) {
            clear_directory_contents(child);
            g_rmdir(child);
        } else {
            g_remove(child);
        }
        g_free(child);
    }

    g_dir_close(dir);
}

static gboolean resource_stamp_matches(const char *resource_dir, const char *zip_path) {
    struct stat zip_st;
    if (g_stat(zip_path, &zip_st) != 0) {
        return FALSE;
    }

    char *stamp_path = get_resource_stamp_path(resource_dir);
    gchar *contents = NULL;
    gboolean ok = FALSE;

    if (g_file_get_contents(stamp_path, &contents, NULL, NULL) && contents) {
        gchar *expected = g_strdup_printf("%lld:%lld",
            (long long)zip_st.st_mtime, (long long)zip_st.st_size);
        ok = g_strcmp0(contents, expected) == 0;
        g_free(expected);
    }

    g_free(contents);
    g_free(stamp_path);
    return ok;
}

static void write_resource_stamp(const char *resource_dir, const char *zip_path) {
    struct stat zip_st;
    if (g_stat(zip_path, &zip_st) != 0) {
        return;
    }

    char *stamp_path = get_resource_stamp_path(resource_dir);
    gchar *contents = g_strdup_printf("%lld:%lld",
        (long long)zip_st.st_mtime, (long long)zip_st.st_size);
    g_file_set_contents(stamp_path, contents, -1, NULL);
    g_free(contents);
    g_free(stamp_path);
}

static gboolean extract_zip_to_directory(const char *zip_path, const char *output_dir, const char *dict_path) {
    extern void settings_scan_progress_notify(const char *path, int percent);
    /* Count total entries first so we can compute progress */
    int total_entries = 0;
    struct archive *counter = archive_read_new();
    struct archive_entry *centry = NULL;
    archive_read_support_filter_all(counter);
    archive_read_support_format_zip(counter);
    if (archive_read_open_filename(counter, zip_path, 10240) == ARCHIVE_OK) {
        int cstatus = ARCHIVE_OK;
        while ((cstatus = archive_read_next_header(counter, &centry)) == ARCHIVE_OK) {
            total_entries++;
            archive_read_data_skip(counter);
        }
    }
    archive_read_close(counter);
    archive_read_free(counter);
    if (total_entries == 0) total_entries = 1;

    struct archive *reader = archive_read_new();
    struct archive_entry *entry = NULL;
    gboolean ok = TRUE;
    int status = ARCHIVE_OK;

    archive_read_support_filter_all(reader);
    archive_read_support_format_zip(reader);

    clear_directory_contents(output_dir);

    if (archive_read_open_filename(reader, zip_path, 10240) != ARCHIVE_OK) {
        fprintf(stderr, "[DSL RESOURCES] Failed to open ZIP %s: %s\n",
                zip_path, archive_error_string(reader));
        ok = FALSE;
        goto done;
    }

    int processed = 0;
    int last_percent = -1;
    while ((status = archive_read_next_header(reader, &entry)) != ARCHIVE_EOF) {
        if (status < ARCHIVE_WARN) {
            fprintf(stderr, "[DSL RESOURCES] Failed reading ZIP entry from %s: %s\n",
                    zip_path, archive_error_string(reader));
            ok = FALSE;
            break;
        }

        const char *raw_path = archive_entry_pathname(entry);
        char *safe_rel = sanitize_archive_entry_path(raw_path);
        if (!safe_rel) {
            archive_read_data_skip(reader);
            continue;
        }

        char *dest_path = g_build_filename(output_dir, safe_rel, NULL);
        mode_t mode = archive_entry_perm(entry);
        mode_t fallback_mode = mode ? mode : 0644;
        mode_t filetype = archive_entry_filetype(entry);

        if (filetype == AE_IFDIR) {
            if (g_mkdir_with_parents(dest_path, mode ? mode : 0755) != 0) {
                fprintf(stderr, "[DSL RESOURCES] Failed creating directory %s\n", dest_path);
                g_free(dest_path);
                g_free(safe_rel);
                ok = FALSE;
                break;
            }
        } else if (filetype == AE_IFREG || filetype == 0) {
            char *parent_dir = g_path_get_dirname(dest_path);
            if (g_mkdir_with_parents(parent_dir, 0755) != 0) {
                fprintf(stderr, "[DSL RESOURCES] Failed creating parent directory for %s\n", dest_path);
                g_free(parent_dir);
                g_free(dest_path);
                g_free(safe_rel);
                ok = FALSE;
                break;
            }
            g_free(parent_dir);

            int fd = g_open(dest_path, O_CREAT | O_TRUNC | O_WRONLY, fallback_mode);
            if (fd < 0) {
                fprintf(stderr, "[DSL RESOURCES] Failed opening %s for writing\n", dest_path);
                g_free(dest_path);
                g_free(safe_rel);
                ok = FALSE;
                break;
            }

            status = archive_read_data_into_fd(reader, fd);
            close(fd);
            if (status < ARCHIVE_WARN) {
                fprintf(stderr, "[DSL RESOURCES] Failed extracting %s: %s\n",
                        dest_path, archive_error_string(reader));
                g_free(dest_path);
                g_free(safe_rel);
                ok = FALSE;
                break;
            }
            g_chmod(dest_path, fallback_mode);
            /* Update progress for this dict */
            processed++;
            int pct = (processed * 100) / total_entries;
            if (pct != last_percent) {
                last_percent = pct;
                settings_scan_progress_notify(dict_path ? dict_path : zip_path, pct);
            }
        } else {
            archive_read_data_skip(reader);
        }

        g_free(dest_path);
        g_free(safe_rel);
    }

done:
    archive_read_close(reader);
    archive_read_free(reader);
    return ok;
}

static char *dsl_find_local_resource_dir(const char *path) {
    char *candidate = g_strconcat(path, ".files", NULL);
    if (g_file_test(candidate, G_FILE_TEST_IS_DIR)) {
        return candidate;
    }
    g_free(candidate);

    if (g_str_has_suffix(path, ".dz")) {
        char *without_dz = g_strndup(path, strlen(path) - 3);
        candidate = g_strconcat(without_dz, ".files", NULL);
        g_free(without_dz);
        if (g_file_test(candidate, G_FILE_TEST_IS_DIR)) {
            return candidate;
        }
        g_free(candidate);
    } else if (g_str_has_suffix(path, ".dsl")) {
        candidate = g_strconcat(path, ".dz.files", NULL);
        if (g_file_test(candidate, G_FILE_TEST_IS_DIR)) {
            return candidate;
        }
        g_free(candidate);
    }

    return NULL;
}

static char *dsl_find_resource_zip(const char *path) {
    char *candidate = g_strconcat(path, ".files.zip", NULL);
    if (g_file_test(candidate, G_FILE_TEST_EXISTS)) {
        return candidate;
    }
    g_free(candidate);

    if (g_str_has_suffix(path, ".dz")) {
        char *without_dz = g_strndup(path, strlen(path) - 3);
        candidate = g_strconcat(without_dz, ".files.zip", NULL);
        g_free(without_dz);
        if (g_file_test(candidate, G_FILE_TEST_EXISTS)) {
            return candidate;
        }
        g_free(candidate);
    } else if (g_str_has_suffix(path, ".dsl")) {
        candidate = g_strconcat(path, ".dz.files.zip", NULL);
        if (g_file_test(candidate, G_FILE_TEST_EXISTS)) {
            return candidate;
        }
        g_free(candidate);
    }

    return NULL;
}

static char *dsl_prepare_resource_dir(const char *path) {
    char *local_dir = dsl_find_local_resource_dir(path);
    if (local_dir) {
        return local_dir;
    }

    char *zip_path = dsl_find_resource_zip(path);
    if (!zip_path) {
        return NULL;
    }

    char *resource_dir = get_resource_cache_dir_path(path);
    if (!ensure_resource_cache_directory(resource_dir)) {
        g_free(resource_dir);
        g_free(zip_path);
        return NULL;
    }

    if ((!dir_has_visible_files(resource_dir) || !resource_stamp_matches(resource_dir, zip_path)) &&
        !extract_zip_to_directory(zip_path, resource_dir, path)) {
        g_free(resource_dir);
        g_free(zip_path);
        return NULL;
    }

    write_resource_stamp(resource_dir, zip_path);
    g_free(zip_path);
    return resource_dir;
}

typedef struct {
    size_t offset;
    size_t length;
} HwSpan;

static size_t parse_dsl_into_tree(DictMmap *dict, TreeEntry **out_entries) {
    const char *p   = dict->data;
    const char *end = p + dict->size;

    /* Dynamic headword accumulator */
    HwSpan *hws     = NULL;
    size_t  hw_count = 0;
    size_t  hw_cap   = 0;

    size_t def_offset = 0;
    size_t def_len    = 0;
    int    in_def     = 0;
    size_t word_count = 0;

    TreeEntry *entries = NULL;
    size_t entry_cap = 0;

    /* Flush: insert every collected headword with the current def block */
    #define FLUSH_HEADWORDS() do {                                       \
        if (in_def && hw_count > 0 && def_len > 0) {                    \
            for (size_t _i = 0; _i < hw_count; _i++) {                  \
                splay_tree_insert(dict->index,                           \
                    hws[_i].offset, hws[_i].length,                      \
                    def_offset, def_len);                                \
                if (out_entries) {                                       \
                    if (word_count >= entry_cap) {                       \
                        entry_cap = (entry_cap == 0) ? 1024 : entry_cap * 2; \
                        entries = realloc(entries, entry_cap * sizeof(TreeEntry)); \
                    }                                                    \
                    entries[word_count].h_off = (int64_t)hws[_i].offset; \
                    entries[word_count].h_len = (uint64_t)hws[_i].length; \
                    entries[word_count].d_off = (int64_t)def_offset;    \
                    entries[word_count].d_len = (uint64_t)def_len;      \
                }                                                        \
                word_count++;                                            \
            }                                                            \
        }                                                                \
        hw_count = 0;                                                    \
        in_def   = 0;                                                    \
        def_len  = 0;                                                    \
    } while (0)

    while (p < end) {
        const char *line_start = p;

        /* Advance to end-of-line */
        while (p < end && *p != '\n') p++;
        size_t len = (size_t)(p - line_start);
        if (p < end && *p == '\n') p++;

        /* Skip empty / comment / bare-CR lines */
        if (len == 0) continue;
        
        if (line_start[0] == '#') {
            /* Support #NAME "Dictionary Name" header */
            if (len > 6 && strncasecmp(line_start, "#NAME", 5) == 0) {
                const char *val_start = line_start + 5;
                while (val_start < line_start + len && (*val_start == ' ' || *val_start == '\t' || *val_start == '\"')) 
                    val_start++;
                const char *val_end = line_start + len;
                while (val_end > val_start && (*(val_end - 1) == '\r' || *(val_end - 1) == '\"' || *(val_end - 1) == ' ' || *(val_end - 1) == '\t'))
                    val_end--;
                
                if (val_end > val_start && !dict->name) {
                    dict->name = strndup(val_start, (size_t)(val_end - val_start));
                    printf("[DSL] Found dictionary name: %s\n", dict->name);
                }
            }
            continue;
        }

        size_t actual_len = len;
        if (actual_len > 0 && line_start[actual_len - 1] == '\r')
            actual_len--;
        if (actual_len == 0)
            continue;

        /* Skip UTF-8 BOM on the very first line */
        if (line_start == dict->data && actual_len >= 3 &&
            (unsigned char)line_start[0] == 0xef &&
            (unsigned char)line_start[1] == 0xbb &&
            (unsigned char)line_start[2] == 0xbf) {
            line_start += 3;
            actual_len -= 3;
            if (actual_len == 0) continue;
        }

        int is_indented = (line_start[0] == ' ' || line_start[0] == '\t');

        if (!is_indented) {
            /* ── headword line ── */
            if (in_def) {
                /* We were reading a definition block → flush previous group */
                FLUSH_HEADWORDS();
            }

            /* Skip DSL header macros like {{...}} */
            if (line_start[0] == '{' && actual_len > 1 && line_start[1] == '{')
                continue;

            /* Grow the headword array if needed */
            if (hw_count >= hw_cap) {
                hw_cap = hw_cap == 0 ? 16 : hw_cap * 2;
                hws = realloc(hws, hw_cap * sizeof(HwSpan));
            }
            hws[hw_count].offset = (size_t)(line_start - dict->data);
            hws[hw_count].length = actual_len;
            hw_count++;

            /* Tentatively set def_offset to right after this line */
            def_offset = (size_t)(p - dict->data);
            def_len    = 0;
        } else {
            /* ── definition line (indented) ── */
            in_def  = 1;
            def_len = (size_t)(p - dict->data) - def_offset;
        }
    }

    /* Final flush */
    FLUSH_HEADWORDS();
    #undef FLUSH_HEADWORDS

    free(hws);
    if (out_entries) *out_entries = entries;
    printf("[DEBUG] parse_dsl_into_tree: indexed %zu headwords.\n", word_count);
    return word_count;
}

static size_t convert_utf16le_to_utf8(const unsigned char *in_buf, size_t in_len, unsigned char *out_buf) {
    size_t in = 0, out = 0;
    while (in + 1 < in_len) {
        uint32_t wc = in_buf[in] | (in_buf[in+1] << 8);
        in += 2;
        if (wc >= 0xD800 && wc <= 0xDBFF && in + 1 < in_len) {
            uint32_t wc2 = in_buf[in] | (in_buf[in+1] << 8);
            if (wc2 >= 0xDC00 && wc2 <= 0xDFFF) {
                in += 2;
                wc = 0x10000 + ((wc & 0x3FF) << 10) + (wc2 & 0x3FF);
            }
        }
        if (wc < 0x80) { out_buf[out++] = wc; }
        else if (wc < 0x800) {
            out_buf[out++] = 0xC0 | (wc >> 6);
            out_buf[out++] = 0x80 | (wc & 0x3F);
        } else if (wc < 0x10000) {
            out_buf[out++] = 0xE0 | (wc >> 12);
            out_buf[out++] = 0x80 | ((wc >> 6) & 0x3F);
            out_buf[out++] = 0x80 | (wc & 0x3F);
        } else {
            out_buf[out++] = 0xF0 | (wc >> 18);
            out_buf[out++] = 0x80 | ((wc >> 12) & 0x3F);
            out_buf[out++] = 0x80 | ((wc >> 6) & 0x3F);
            out_buf[out++] = 0x80 | (wc & 0x3F);
        }
    }
    return out;
}

static size_t convert_utf16be_to_utf8(const unsigned char *in_buf, size_t in_len, unsigned char *out_buf) {
    size_t in = 0, out = 0;
    while (in + 1 < in_len) {
        uint32_t wc = (in_buf[in] << 8) | in_buf[in+1];
        in += 2;
        if (wc >= 0xD800 && wc <= 0xDBFF && in + 1 < in_len) {
            uint32_t wc2 = (in_buf[in] << 8) | in_buf[in+1];
            if (wc2 >= 0xDC00 && wc2 <= 0xDFFF) {
                in += 2;
                wc = 0x10000 + ((wc & 0x3FF) << 10) + (wc2 & 0x3FF);
            }
        }
        if (wc < 0x80) { out_buf[out++] = wc; }
        else if (wc < 0x800) {
            out_buf[out++] = 0xC0 | (wc >> 6);
            out_buf[out++] = 0x80 | (wc & 0x3F);
        } else if (wc < 0x10000) {
            out_buf[out++] = 0xE0 | (wc >> 12);
            out_buf[out++] = 0x80 | ((wc >> 6) & 0x3F);
            out_buf[out++] = 0x80 | (wc & 0x3F);
        } else {
            out_buf[out++] = 0xF0 | (wc >> 18);
            out_buf[out++] = 0x80 | ((wc >> 12) & 0x3F);
            out_buf[out++] = 0x80 | ((wc >> 6) & 0x3F);
            out_buf[out++] = 0x80 | (wc & 0x3F);
        }
    }
    return out;
}

/* New signature accepts cancel flag and expected generation for cooperative cancellation. */
DictMmap* dict_mmap_open(const char *path, volatile gint *cancel_flag, gint expected) {
    if (!path) return NULL;
    size_t path_len = strlen(path);
    if (path_len > 4 && strcasecmp(path + path_len - 4, ".mdx") == 0) {
        fprintf(stderr, "MDX decompression mapping is currently in Phase 2 Development.\n");
        return NULL;
    }

    // Ensure cache directory exists
    ensure_cache_directory();

    // Get cache path for this dictionary
    char *cache_path = get_cached_dict_path(path);
    gboolean cache_exists = (access(cache_path, F_OK) == 0);
    gboolean cache_valid = cache_exists && is_cache_valid(cache_path, path);

    DictMmap *dict = (DictMmap*)calloc(1, sizeof(DictMmap));
    dict->fd = -1;
    dict->tmp_file = NULL;
    dict->source_dir = g_path_get_dirname(path);
    dict->resource_dir = dsl_prepare_resource_dir(path);

    if (cache_valid) {
        // Use cached version directly
        printf("Loading Dictionary from cache: %s\n", cache_path);
        dict->fd = open(cache_path, O_RDONLY);
        if (dict->fd < 0) {
            g_free(cache_path);
            free(dict);
            return NULL;
        }

        struct stat st;
        if (fstat(dict->fd, &st) < 0 || st.st_size < 8) {
            close(dict->fd);
            g_free(cache_path);
            free(dict);
            return NULL;
        }
        dict->size = st.st_size;

        void *map = mmap(NULL, dict->size, PROT_READ, MAP_PRIVATE, dict->fd, 0);
        if (map == MAP_FAILED) {
            close(dict->fd);
            g_free(cache_path);
            free(dict);
            return NULL;
        }

        dict->data = (const char*)map;
        dict->index = splay_tree_new(dict->data, dict->size);

        // Fast load: read index from end
        uint64_t count = *(uint64_t*)dict->data;
        int need_index = (count == 0);

        TreeEntry *entries = NULL;
        size_t index_size = 0;
        if (count > 0) {
            index_size = count * sizeof(TreeEntry);
            if (dict->size > index_size + 8) {
                entries = (TreeEntry*)(dict->data + (dict->size - index_size));

                /* Validate index entries to avoid using stale/corrupt caches
                 * (which can happen when on-disk cache formats change). If any
                 * entry points outside the data region, fall back to
                 * re-indexing by parsing the original file. */
                size_t data_region_end = dict->size - index_size; /* first byte of index */
                gboolean valid_index = TRUE;
                for (uint64_t i = 0; i < count; i++) {
                    int64_t h_off = entries[i].h_off;
                    uint64_t h_len = entries[i].h_len;
                    int64_t d_off = entries[i].d_off;
                    uint64_t d_len = entries[i].d_len;
                    /* Basic sanity checks */
                    if (h_off < 8 || (uint64_t)h_off >= data_region_end) { valid_index = FALSE; break; }
                    if (d_off < 8 || (uint64_t)d_off >= data_region_end) { valid_index = FALSE; break; }
                    if (h_len == 0 || d_len == 0) { valid_index = FALSE; break; }
                    if ((uint64_t)h_off + h_len > data_region_end) { valid_index = FALSE; break; }
                    if ((uint64_t)d_off + d_len > data_region_end) { valid_index = FALSE; break; }
                }
                if (valid_index) {
                    insert_balanced(dict->index, entries, 0, (int)count - 1);
                    printf("[DSL] Fast-loaded %lu entries from cache.\n", (unsigned long)count);
                    /* Ensure a name exists for UI (some caches omit it) */
                    if (!dict->name) {
                        char *base = g_path_get_basename(path);
                        dict->name = g_strdup(base);
                        g_free(base);
                    }
                } else {
                    fprintf(stderr, "[DSL] Cache index validation failed for %s — rebuilding index.\n", path);
                    need_index = 1;
                }
            } else {
                need_index = 1;
            }
        }

        if (need_index) {
            printf("[DSL] Cache exists but lacks index. Performing auto-upgrade...\n");
            TreeEntry *entries = NULL;
            size_t word_count = parse_dsl_into_tree(dict, &entries);

            /* Check for cancellation before attempting to upgrade cache */
            if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) {
                // abort and cleanup
                if (dict->fd >= 0) close(dict->fd);
                if (dict->tmp_file) fclose(dict->tmp_file);
                free(dict);
                g_free(cache_path);
                return NULL;
            }

            if (word_count > 0 && entries) {
                // Re-open O_RDWR to upgrade cache
                int fd_rw = open(cache_path, O_RDWR);
                if (fd_rw >= 0) {
                    lseek(fd_rw, 0, SEEK_END);
                    write(fd_rw, entries, word_count * sizeof(TreeEntry));
                    lseek(fd_rw, 0, SEEK_SET);
                    uint64_t final_cnt = (uint64_t)word_count;
                    write(fd_rw, &final_cnt, 8);
                    close(fd_rw);

                    // Re-mmap the newly appended file part
                    munmap((void*)dict->data, dict->size);
                    struct stat st_new;
                    fstat(dict->fd, &st_new);
                    dict->size = st_new.st_size;
                    dict->data = mmap(NULL, dict->size, PROT_READ, MAP_PRIVATE, dict->fd, 0);
                        /* Ensure the splay-tree uses the new mmap base pointer
                         * (parse_dsl_into_tree inserted nodes into dict->index
                         * while using the previous mapping). Update the tree so
                         * subsequent searches read from the correct memory. */
                        if (dict->index) {
                            dict->index->mmap_data = dict->data;
                            dict->index->mmap_size = dict->size;
                        }
                    printf("[DSL] Auto-upgraded cache for %s (%lu entries).\n", path, (unsigned long)word_count);
                }
                free(entries);
            }
        }
    } else {
        // Need to extract and convert
        printf("Loading Dictionary: %s\n", path);
        gzFile gz = gzopen(path, "rb");
        if (!gz) {
            g_free(cache_path);
            free(dict);
            return NULL;
        }

        // Open cache file for writing
        FILE *cache_file = fopen(cache_path, "wb");
        if (!cache_file) {
            gzclose(gz);
            g_free(cache_path);
            free(dict);
            return NULL;
        }

        // Write placeholder for count
        uint64_t zero_count = 0;
        fwrite(&zero_count, 8, 1, cache_file);

        // Determine encoding by reading first 4 bytes
        unsigned char bom[4];
        int bom_len = gzread(gz, bom, 4);
        if (bom_len < 2) {
            gzclose(gz);
            fclose(cache_file);
            g_free(cache_path);
            free(dict);
            return NULL;
        }

        int is_utf16le = 0;
        int is_utf16be = 0;
        int copy_offset = 0;

        if (bom[0] == 0xFF && bom[1] == 0xFE) {
            is_utf16le = 1;
            copy_offset = 2;
        } else if (bom[0] == 0xFE && bom[1] == 0xFF) {
            is_utf16be = 1;
            copy_offset = 2;
        } else if (bom_len >= 4 && bom[0] != 0 && bom[1] == 0 && bom[2] != 0 && bom[3] == 0) {
            is_utf16le = 1;
            copy_offset = 0;
        } else if (bom_len >= 4 && bom[0] == 0 && bom[1] != 0 && bom[2] == 0 && bom[3] != 0) {
            is_utf16be = 1;
            copy_offset = 0;
        } else if (bom_len >= 3 && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) {
            copy_offset = 3;
        }

        // Write whatever we read past the BOM
        if (bom_len > copy_offset) {
            if (is_utf16le) {
                unsigned char out[10];
                size_t olen = convert_utf16le_to_utf8(bom + copy_offset, bom_len - copy_offset, out);
                fwrite(out, 1, olen, cache_file);
            } else if (is_utf16be) {
                unsigned char out[10];
                size_t olen = convert_utf16be_to_utf8(bom + copy_offset, bom_len - copy_offset, out);
                fwrite(out, 1, olen, cache_file);
            } else {
                fwrite(bom + copy_offset, 1, bom_len - copy_offset, cache_file);
            }
        }

        unsigned char in_buf[65536];
        unsigned char out_buf[65536 * 2];

        unsigned char pending_byte = 0;
        int has_pending = 0;
        int bytes_read;

        while ((bytes_read = gzread(gz, in_buf + has_pending, 65536 - has_pending)) > 0) {
            /* Honor cancellation request as soon as possible */
            if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) {
                gzclose(gz);
                fclose(cache_file);
                unlink(cache_path);
                g_free(cache_path);
                free(dict);
                return NULL;
            }
            int total = bytes_read + has_pending;
            size_t process_len = total;

            if ((is_utf16le || is_utf16be) && (total % 2 != 0)) {
                pending_byte = in_buf[total - 1];
                has_pending = 1;
                process_len = total - 1;
            } else {
                has_pending = 0;
            }

            if (process_len > 0) {
                if (is_utf16le) {
                    size_t olen = convert_utf16le_to_utf8(in_buf, process_len, out_buf);
                    fwrite(out_buf, 1, olen, cache_file);
                } else if (is_utf16be) {
                    size_t olen = convert_utf16be_to_utf8(in_buf, process_len, out_buf);
                    fwrite(out_buf, 1, olen, cache_file);
                } else {
                    fwrite(in_buf, 1, process_len, cache_file);
                }
            }

            if (has_pending) {
                in_buf[0] = pending_byte;
            }
        }
        gzclose(gz);
        fflush(cache_file);
        fclose(cache_file);

        // Update cache file mtime to match source
        struct stat src_st;
        if (stat(path, &src_st) == 0) {
            struct utimbuf times;
            times.actime = src_st.st_mtime;
            times.modtime = src_st.st_mtime;
            utime(cache_path, &times);
        }

        // Now open the cached file
        dict->fd = open(cache_path, O_RDWR);
        if (dict->fd < 0) {
            g_free(cache_path);
            free(dict);
            return NULL;
        }

        struct stat st;
        if (fstat(dict->fd, &st) < 0 || st.st_size == 0) {
            close(dict->fd);
            g_free(cache_path);
            free(dict);
            return NULL;
        }
        dict->size = st.st_size;

        void *map = mmap(NULL, dict->size, PROT_READ | PROT_WRITE, MAP_SHARED, dict->fd, 0);
        if (map == MAP_FAILED) {
            close(dict->fd);
            g_free(cache_path);
            free(dict);
            return NULL;
        }

        dict->data = (const char*)map;
        dict->index = splay_tree_new(dict->data, dict->size);

        TreeEntry *entries = NULL;
        size_t count = parse_dsl_into_tree(dict, &entries);

        if (count > 0 && entries) {
            // Append entries to cache file
            lseek(dict->fd, 0, SEEK_END);
            write(dict->fd, entries, count * sizeof(TreeEntry));

            // Update count at beginning
            lseek(dict->fd, 0, SEEK_SET);
            uint64_t final_count = (uint64_t)count;
            write(dict->fd, &final_count, 8);

            free(entries);
        }

        // Remap as read-only for final use
        munmap(map, dict->size);
        struct stat st_final;
        fstat(dict->fd, &st_final);
        dict->size = st_final.st_size;
        dict->data = mmap(NULL, dict->size, PROT_READ, MAP_PRIVATE, dict->fd, 0);
        /* If a splay-tree was populated against the previous mapping,
         * update its mmap base so node comparisons read from the new
         * mapping address. This avoids use-after-unmap crashes. */
        if (dict->index) {
            dict->index->mmap_data = dict->data;
            dict->index->mmap_size = dict->size;
        }
    }

    return dict;
}

void dict_mmap_close(DictMmap *dict) {
    if (dict) {
        splay_tree_free(dict->index);
        if (dict->data) munmap((void*)dict->data, dict->size);
        if (dict->fd >= 0) close(dict->fd);
        if (dict->name) free(dict->name);
        if (dict->source_dir) free(dict->source_dir);
        if (dict->resource_dir) free(dict->resource_dir);
        if (dict->mdx_stylesheet) free(dict->mdx_stylesheet);
        free(dict);
    }
}
