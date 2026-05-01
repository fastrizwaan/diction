#include "resource-reader.h"
#include <archive.h>
#include <archive_entry.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <glib/gstdio.h>
#include "dict-cache.h"

struct ResourceReader {
    char *extract_dir;
    void *backend_data;
    char* (*get_func)(ResourceReader *reader, const char *name);
    gboolean (*has_func)(ResourceReader *reader, const char *name);
    void (*close_func)(ResourceReader *reader);
};

typedef struct {
    char *archive_path;  /* path to the ZIP file */
    GHashTable *entries; /* case-insensitive: normalized name → archive name (owned) */
} ZipBackend;

/* Normalize a resource name: lowercase, forward slashes, strip leading sep */
static char* normalize_resource_name(const char *name) {
    if (!name) return NULL;
    /* Skip leading path separators and ./ */
    while (*name == '/' || *name == '\\') name++;
    if (name[0] == '.' && (name[1] == '/' || name[1] == '\\')) name += 2;

    char *result = g_utf8_strdown(name, -1);
    /* Normalize backslashes to forward slashes */
    for (char *p = result; *p; p++) {
        if (*p == '\\') *p = '/';
    }
    return result;
}

/* Extract a single file from the ZIP archive to the extract_dir */
static gboolean zip_extract_single_file(const char *zip_path, const char *archive_name,
                                     const char *dest_path) {
    struct archive *a = archive_read_new();
    archive_read_support_format_zip(a);
    archive_read_support_filter_all(a);

    if (archive_read_open_filename(a, zip_path, 65536) != ARCHIVE_OK) {
        archive_read_free(a);
        return FALSE;
    }

    gboolean found = FALSE;
    struct archive_entry *entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *pathname = archive_entry_pathname(entry);
        if (pathname && strcmp(pathname, archive_name) == 0) {
            la_int64_t entry_size = archive_entry_size(entry);
            guint64 bytes_needed = entry_size > 0 ? (guint64) entry_size : 0;
            if (!dict_cache_prepare_target_path(dest_path, bytes_needed)) {
                break;
            }

            /* Extract to dest_path */
            FILE *f = fopen(dest_path, "wb");
            if (f) {
                const void *buf;
                size_t size;
                int64_t offset;
                gboolean write_ok = TRUE;
                while (archive_read_data_block(a, &buf, &size, &offset) == ARCHIVE_OK) {
                    if (fwrite(buf, 1, size, f) != size) {
                        write_ok = FALSE;
                        break;
                    }
                }
                fclose(f);
                if (write_ok) {
                    found = TRUE;
                } else {
                    g_unlink(dest_path);
                }
            }
            break;
        }
        archive_read_data_skip(a);
    }

    archive_read_free(a);
    return found;
}

static char* zip_get(ResourceReader *reader, const char *name) {
    ZipBackend *zip = reader->backend_data;
    char *normalized = normalize_resource_name(name);
    if (!normalized) return NULL;

    const char *archive_name = g_hash_table_lookup(zip->entries, normalized);
    if (!archive_name) {
        g_free(normalized);
        return NULL;
    }

    char *dest_path = g_build_filename(reader->extract_dir, normalized, NULL);

    if (g_file_test(dest_path, G_FILE_TEST_EXISTS)) {
        g_free(normalized);
        return dest_path;
    }

    if (zip_extract_single_file(zip->archive_path, archive_name, dest_path)) {
        g_free(normalized);
        return dest_path;
    }

    g_free(normalized);
    g_free(dest_path);
    return NULL;
}

static gboolean zip_has(ResourceReader *reader, const char *name) {
    ZipBackend *zip = reader->backend_data;
    char *normalized = normalize_resource_name(name);
    if (!normalized) return FALSE;
    gboolean found = g_hash_table_contains(zip->entries, normalized);
    g_free(normalized);
    return found;
}

static void zip_close(ResourceReader *reader) {
    ZipBackend *zip = reader->backend_data;
    g_free(zip->archive_path);
    g_hash_table_destroy(zip->entries);
    g_free(zip);
}

ResourceReader* resource_reader_open_archive(const char *archive_path, const char *extract_dir) {
    if (!archive_path || !extract_dir) return NULL;
    g_mkdir_with_parents(extract_dir, 0755);

    struct archive *a = archive_read_new();
    archive_read_support_format_all(a);
    archive_read_support_filter_all(a);

    if (archive_read_open_filename(a, archive_path, 65536) != ARCHIVE_OK) {
        fprintf(stderr, "[ResourceReader] Failed to open archive: %s: %s\n",
                archive_path, archive_error_string(a));
        archive_read_free(a);
        return NULL;
    }

    GHashTable *entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    struct archive_entry *entry;
    int count = 0;

    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *pathname = archive_entry_pathname(entry);
        if (!pathname || archive_entry_filetype(entry) == AE_IFDIR) {
            archive_read_data_skip(a);
            continue;
        }

        char *normalized = normalize_resource_name(pathname);
        if (normalized) {
            g_hash_table_insert(entries, normalized, g_strdup(pathname));
            count++;
        }
        archive_read_data_skip(a);
    }
    archive_read_free(a);

    ZipBackend *zip = g_new0(ZipBackend, 1);
    zip->archive_path = g_strdup(archive_path);
    zip->entries = entries;

    ResourceReader *reader = resource_reader_new(extract_dir, zip, zip_get, zip_has, zip_close);
    printf("[ResourceReader] Indexed %d entries from %s (lazy extraction)\n", count, archive_path);
    return reader;
}

ResourceReader* resource_reader_new(const char *extract_dir, void *backend_data,
                                    char* (*get_func)(ResourceReader*, const char*),
                                    gboolean (*has_func)(ResourceReader*, const char*),
                                    void (*close_func)(ResourceReader*)) {
    ResourceReader *r = g_new0(ResourceReader, 1);
    r->extract_dir = g_strdup(extract_dir);
    r->backend_data = backend_data;
    r->get_func = get_func;
    r->has_func = has_func;
    r->close_func = close_func;
    return r;
}

void* resource_reader_get_backend(ResourceReader *reader) {
    if (!reader) return NULL;
    return reader->backend_data;
}

char* resource_reader_get(ResourceReader *reader, const char *name) {
    if (!reader || !name) return NULL;
    return reader->get_func ? reader->get_func(reader, name) : NULL;
}

gboolean resource_reader_has(ResourceReader *reader, const char *name) {
    if (!reader || !name) return FALSE;
    return reader->has_func ? reader->has_func(reader, name) : FALSE;
}

const char* resource_reader_get_dir(ResourceReader *reader) {
    if (!reader) return NULL;
    return reader->extract_dir;
}

void resource_reader_close(ResourceReader *reader) {
    if (!reader) return;
    if (reader->close_func) reader->close_func(reader);
    g_free(reader->extract_dir);
    g_free(reader);
}
