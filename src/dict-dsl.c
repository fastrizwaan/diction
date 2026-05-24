#include "dict-mmap.h"
#include "flat-index.h"
#include "resource-reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <zlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <sqlite3.h>
#include <utime.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <archive.h>
#include <archive_entry.h>
#include "dict-cache.h"
#include "settings.h"
#include "dict-chunked.h"
#include "dict-cache-builder.h"
#include "dictzip.h"
#include "dict-dsl-index.h"

/* insert_balanced removed — flat index uses sorted array + binary search */

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

char *dsl_prepare_resource_dir(const char *path, ResourceReader **out_reader) {
    char *local_dir = dsl_find_local_resource_dir(path);
    if (local_dir) {
        return local_dir;
    }

    char *zip_path = dsl_find_resource_zip(path);
    if (!zip_path) {
        return NULL;
    }

    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, path, -1);
    const char *base = dict_cache_base_dir();
    char *resource_dir = g_build_filename(base, "diction", "resources", hash, NULL);
    g_free(hash);
    if (g_mkdir_with_parents(resource_dir, 0755) != 0) {
        g_free(resource_dir);
        g_free(zip_path);
        return NULL;
    }

    /* Phase 2: Lazy extraction — scan ZIP but don't extract.
     * Individual files will be extracted on demand by ResourceReader. */
    if (out_reader) {
        *out_reader = resource_reader_open_archive(zip_path, resource_dir);
    }

    g_free(zip_path);
    return resource_dir;
}

void dsl_parse_header_internal(const char *path, char **out_name, char **out_source_lang, char **out_target_lang) {
    if (out_name) *out_name = NULL;
    if (out_source_lang) *out_source_lang = NULL;
    if (out_target_lang) *out_target_lang = NULL;

    size_t len = strlen(path);
    gboolean is_dz = (len > 3 && g_ascii_strcasecmp(path + len - 3, ".dz") == 0);

    gzFile gz = NULL;
    FILE *f = NULL;
    if (is_dz) {
        gz = gzopen(path, "rb");
        if (!gz) return;
    } else {
        f = fopen(path, "rb");
        if (!f) return;
    }

    unsigned char bom[4];
    size_t bom_len = is_dz ? (size_t)gzread(gz, bom, 4) : fread(bom, 1, 4, f);
    int skip = 0, bom_is_utf16 = 0, utf16_be = 0;
    if (bom_len >= 3 && bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF) {
        skip = 3;
    } else if (bom_len >= 2 && bom[0] == 0xFF && bom[1] == 0xFE) {
        skip = 2; bom_is_utf16 = 1;
    } else if (bom_len >= 2 && bom[0] == 0xFE && bom[1] == 0xFF) {
        skip = 2; bom_is_utf16 = 1; utf16_be = 1;
    } else if (bom_len >= 4 && bom[0] != 0 && bom[1] == 0 && bom[2] != 0 && bom[3] == 0) {
        bom_is_utf16 = 1;
    } else if (bom_len >= 4 && bom[0] == 0 && bom[1] != 0 && bom[2] == 0 && bom[3] != 0) {
        bom_is_utf16 = 1; utf16_be = 1;
    }
    if (skip) {
        if (is_dz) gzseek(gz, skip, SEEK_SET);
        else fseek(f, skip, SEEK_SET);
    } else if (bom_len > 0) {
        if (is_dz) gzseek(gz, 0, SEEK_SET);
        else fseek(f, 0, SEEK_SET);
    }

    uint8_t buf[1024];
    for (int line = 0; line < 30; line++) {
        int buf_pos = 0;
        if (bom_is_utf16) {
            while (buf_pos < (int)sizeof(buf) - 2) {
                int r0 = is_dz ? gzgetc(gz) : fgetc(f);
                if (r0 == EOF) break;
                int r1 = is_dz ? gzgetc(gz) : fgetc(f);
                if (r1 == EOF) break;
                uint16_t wc = utf16_be ? ((uint8_t)r0 << 8 | (uint8_t)r1) : ((uint8_t)r0 | (uint8_t)r1 << 8);
                if (wc == '\n') break;
                if (wc <= 0x7F) buf[buf_pos++] = (uint8_t)wc;
            }
        } else {
            if (is_dz) {
                if (!gzgets(gz, (char*)buf, (int)sizeof(buf))) break;
            } else {
                if (!fgets((char*)buf, (int)sizeof(buf), f)) break;
            }
            buf_pos = strlen((char*)buf);
            while (buf_pos > 0 && (buf[buf_pos-1] == '\n' || buf[buf_pos-1] == '\r'))
                buf[--buf_pos] = '\0';
        }
        if (buf_pos == 0) continue;
        if (buf[0] != '#') break;

        char *line_s = (char*)buf;

        if (g_ascii_strncasecmp(line_s, "#NAME", 5) == 0) {
            char *q = strchr(line_s + 5, '"');
            if (q && out_name) {
                q++;
                char *end = strchr(q, '"');
                if (end) { *end = '\0'; *out_name = g_strdup(q); }
            }
        }

        if (g_ascii_strncasecmp(line_s, "#INDEX_LANGUAGE", 15) == 0) {
            char *q = strchr(line_s + 15, '"');
            if (q && out_source_lang) {
                q++;
                char *end = strchr(q, '"');
                if (end) { *end = '\0'; *out_source_lang = g_strdup(q); }
            }
        }

        if (g_ascii_strncasecmp(line_s, "#CONTENTS_LANGUAGE", 18) == 0) {
            char *q = strchr(line_s + 18, '"');
            if (q && out_target_lang) {
                q++;
                char *end = strchr(q, '"');
                if (end) { *end = '\0'; *out_target_lang = g_strdup(q); }
            }
        }

        if ((!out_name || *out_name) && (!out_source_lang || *out_source_lang) && (!out_target_lang || *out_target_lang)) break;
    }

    if (is_dz) gzclose(gz);
    else fclose(f);
}


/* New signature accepts cancel flag and expected generation for cooperative cancellation. */
DictMmap* dict_mmap_open(const char *path, volatile gint *cancel_flag, gint expected) {
    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) return NULL;
    if (!path) return NULL;
    size_t path_len = strlen(path);
    if (path_len > 4 && strcasecmp(path + path_len - 4, ".mdx") == 0) {
        fprintf(stderr, "MDX decompression mapping is currently in Phase 2 Development.\n");
        return NULL;
    }

    dict_cache_ensure_dir();

    char *hw_path = dict_hw_index_path_for(path);
    gboolean hw_exists = (access(hw_path, F_OK) == 0);
    gboolean hw_valid = hw_exists && dict_cache_is_valid(hw_path, path);

    if (hw_valid) {
        FlatIndex *idx = flat_index_open(hw_path);
        if (idx && idx->count > 0 && flat_index_validate(idx)) {
            if (!flat_index_get_metadata(idx, "source_path")) {
                flat_index_close(idx);
                hw_valid = FALSE;
            } else {
                DictMmap *dict = g_new0(DictMmap, 1);
                dict->fd = -1;
                dict->index = idx;
            dict->source_dir = g_path_get_dirname(path);

            /* Get metadata from index if available */
            const char *m_name = flat_index_get_metadata(idx, "dict_name");
            const char *m_src = flat_index_get_metadata(idx, "source_lang");
            const char *m_tgt = flat_index_get_metadata(idx, "target_lang");
            const char *m_enc = flat_index_get_metadata(idx, "source_encoding");

            if (m_name) dict->name = g_strdup(m_name);
            if (m_src) dict->source_lang = g_strdup(m_src);
            if (m_tgt) dict->target_lang = g_strdup(m_tgt);
            if (m_enc) dict->source_encoding = atoi(m_enc);

            /* Lazy source initialization will happen on first get_definition */
            g_free(hw_path);
            return dict;
            }
        } else if (idx) {
            flat_index_close(idx);
        }
    }

    if (!hw_valid && dict_cache_failure_is_current(hw_path, path)) {
        fprintf(stderr, "[DSL] Skipping cached index failure for %s\n", path);
        g_free(hw_path);
        return NULL;
    }

    DictMmap *dict = g_new0(DictMmap, 1);
    dict->fd = -1;
    dict->tmp_file = NULL;
    dict->source_dir = g_path_get_dirname(path);

    if (!hw_valid) {
        printf("[DSL] Building SQLite headword index for %s\n", path);
        if (!build_dsl_index_only_cache(path, cancel_flag, expected)) {
            fprintf(stderr, "[DSL] Failed to build headword index for %s\n", path);
            const char *sources[] = { path };
            dict_cache_mark_failure(hw_path, sources, 1);
            g_free(hw_path);
            g_free(dict->source_dir);
            resource_reader_close(dict->resource_reader);
            g_free(dict);
            return NULL;
        }
        struct stat src_st;
        if (stat(path, &src_st) == 0) {
            struct utimbuf times = { .actime = src_st.st_mtime, .modtime = src_st.st_mtime };
            utime(hw_path, &times);
        }
        const char *sources[] = { path };
        dict_cache_sync_mtime(hw_path, sources, 1);
        dict_cache_clear_failure(hw_path);
    } else {
        printf("[DSL] Loading headword index from: %s\n", hw_path);
    }

    dict->index = flat_index_open(hw_path);
    if (!dict->index) {
        fprintf(stderr, "[DSL] Failed to open headword index: %s\n", hw_path);
        g_free(hw_path);
        g_free(dict->source_dir);
        resource_reader_close(dict->resource_reader);
        g_free(dict);
        return NULL;
    }

    /* Get metadata from index if available */
    const char *m_name = flat_index_get_metadata(dict->index, "dict_name");
    const char *m_src = flat_index_get_metadata(dict->index, "source_lang");
    const char *m_tgt = flat_index_get_metadata(dict->index, "target_lang");
    const char *m_enc = flat_index_get_metadata(dict->index, "source_encoding");

    if (m_name) dict->name = g_strdup(m_name);
    if (m_src) dict->source_lang = g_strdup(m_src);
    if (m_tgt) dict->target_lang = g_strdup(m_tgt);
    if (m_enc) dict->source_encoding = atoi(m_enc);

    /* Fallback to parsing header if metadata is missing (legacy caches) */
    if (!dict->name) {
        char *name = NULL, *src_lang = NULL, *tgt_lang = NULL;
        dsl_parse_header_internal(path, &name, &src_lang, &tgt_lang);
        if (name) dict->name = name;
        if (src_lang) dict->source_lang = src_lang;
        if (tgt_lang) dict->target_lang = tgt_lang;
    }

    /* If we still don't have an encoding, try to get it from DB (redundant if m_enc exists) */
    if (!m_enc) {
        sqlite3 *tmp = NULL;
        if (sqlite3_open_v2(hw_path, &tmp, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK) {
            sqlite3_stmt *meta_st = NULL;
            sqlite3_prepare_v2(tmp,
                "SELECT value FROM metadata WHERE key = 'source_encoding';",
                -1, &meta_st, NULL);
            if (sqlite3_step(meta_st) == SQLITE_ROW) {
                dict->source_encoding = atoi((const char*)sqlite3_column_text(meta_st, 0));
            }
            sqlite3_finalize(meta_st);
            sqlite3_close(tmp);
        }
    }

    /* Set up source access for definitions */
    if (g_str_has_suffix(path, ".dz")) {
        dict->source_dz = dictzip_open(path);
    } else {
        dict->source_fd = open(path, O_RDONLY);
        if (dict->source_fd >= 0) {
            struct stat s_st;
            fstat(dict->source_fd, &s_st);
            dict->source_size = s_st.st_size;
            dict->source_mmap = mmap(NULL, dict->source_size, PROT_READ, MAP_SHARED, dict->source_fd, 0);
            close(dict->source_fd);
            dict->source_fd = -1;
        }
    }

    if (!dict->name) {
        char *base = g_path_get_basename(path);
        dict->name = g_strdup(base);
        g_free(base);
    }

    g_free(hw_path);
    return dict;
}

