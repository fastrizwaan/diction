#include "dict-cache-builder.h"
#include "dict-cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sqlite3.h>

struct DictCacheBuilder {
    char *cache_path;
    FILE *file;
    FILE *defs_file;
    DictCacheHeader header;
    DictCacheHeader defs_header;
    DictChunkWriter *writer;
    uint64_t headwords_len;
};

DictCacheBuilder* dict_cache_builder_new(const char *cache_path, uint64_t entry_count) {
    FILE *f = fopen(cache_path, "wb");
    if (!f) return NULL;

    DictCacheBuilder *b = g_new0(DictCacheBuilder, 1);
    b->cache_path = g_strdup(cache_path);
    b->file = f;
    b->defs_file = tmpfile();
    if (!b->defs_file) {
        fclose(f);
        g_free(b);
        return NULL;
    }

    b->header.magic[0] = 'D'; b->header.magic[1] = 'C'; b->header.magic[2] = 'M'; b->header.magic[3] = 'P';
    b->header.version = DICT_CACHE_VERSION;
    b->header.entry_count = entry_count;
    b->header.headwords_off = sizeof(DictCacheHeader);
    b->defs_header = b->header;
    
    /* Placeholder for header */
    fwrite(&b->header, sizeof(DictCacheHeader), 1, f);
    fwrite(&b->defs_header, sizeof(DictCacheHeader), 1, b->defs_file);
    
    b->writer = dict_chunk_writer_new(b->defs_file, &b->defs_header);
    
    return b;
}

void dict_cache_builder_add_headword(DictCacheBuilder *b, const char *word, size_t len, uint64_t *out_off) {
    *out_off = b->header.headwords_off + b->headwords_len;
    fwrite(word, 1, len, b->file);
    fwrite("\n", 1, 1, b->file);
    b->headwords_len += (len + 1);
}

void dict_cache_builder_add_definition(DictCacheBuilder *b, const char *data, size_t len, uint64_t *out_off) {
    dict_chunk_writer_append_definition(b->writer, data, len, out_off);
}

void dict_cache_builder_flush(DictCacheBuilder *b) {
    if (b && b->file) fflush(b->file);
}

void dict_cache_builder_finalize(DictCacheBuilder *b, FlatTreeEntry *entries, uint64_t actual_count) {
    if (!b || !b->file) return;

    b->header.entry_count = actual_count;
    b->header.headwords_len = b->headwords_len;

    dict_chunk_writer_finalize(b->writer);
    b->header.total_uncompressed_size = b->defs_header.total_uncompressed_size;
    b->header.chunk_count = b->defs_header.chunk_count;

    uint64_t chunk_base = (uint64_t)ftell(b->file);

    if (b->defs_header.chunk_table_off > sizeof(DictCacheHeader)) {
        char buf[65536];
        uint64_t remaining = b->defs_header.chunk_table_off - sizeof(DictCacheHeader);
        fseek(b->defs_file, sizeof(DictCacheHeader), SEEK_SET);
        while (remaining > 0) {
            size_t want = (remaining < sizeof(buf)) ? (size_t)remaining : sizeof(buf);
            size_t got = fread(buf, 1, want, b->defs_file);
            if (got == 0) break;
            fwrite(buf, 1, got, b->file);
            remaining -= got;
        }
    }
    
    b->header.chunk_table_off = (uint64_t)ftell(b->file);

    if (b->defs_header.chunk_count > 0) {
        uint64_t *chunk_offsets = g_new0(uint64_t, (size_t)b->defs_header.chunk_count);
        fseek(b->defs_file, (long)b->defs_header.chunk_table_off, SEEK_SET);
        if (fread(chunk_offsets, sizeof(uint64_t), (size_t)b->defs_header.chunk_count, b->defs_file) == (size_t)b->defs_header.chunk_count) {
            for (uint64_t i = 0; i < b->defs_header.chunk_count; i++) {
                if (chunk_offsets[i] >= sizeof(DictCacheHeader)) {
                    chunk_offsets[i] = chunk_base + (chunk_offsets[i] - sizeof(DictCacheHeader));
                }
            }
            fwrite(chunk_offsets, sizeof(uint64_t), (size_t)b->defs_header.chunk_count, b->file);
        }
        g_free(chunk_offsets);
    }

    uint64_t index_off = (uint64_t)ftell(b->file);
    fwrite(entries, sizeof(FlatTreeEntry), b->header.entry_count, b->file);
    
    b->header.index_off = index_off;
    
    fseek(b->file, 0, SEEK_SET);
    fwrite(&b->header, sizeof(DictCacheHeader), 1, b->file);
}

void dict_cache_builder_finalize_index_only(DictCacheBuilder *b, FlatTreeEntry *entries, uint64_t actual_count, uint32_t source_encoding, const char *stardict_sts) {
    if (!b || !b->file) return;

    b->header.entry_count = actual_count;
    b->header.headwords_len = b->headwords_len;
    b->header.source_encoding = source_encoding;

    if (stardict_sts) {
        strncpy(b->header.stardict_sts, stardict_sts, sizeof(b->header.stardict_sts) - 1);
        b->header.stardict_sts[sizeof(b->header.stardict_sts) - 1] = '\0';
    } else {
        memset(b->header.stardict_sts, 0, sizeof(b->header.stardict_sts));
    }

    /* No compressed definition chunks. */
    b->header.total_uncompressed_size = 0;
    b->header.chunk_count = 0;
    b->header.chunk_table_off = 0;

    /* Write the sorted flat tree entries directly after the headwords */
    uint64_t index_off = (uint64_t)ftell(b->file);
    fwrite(entries, sizeof(FlatTreeEntry), b->header.entry_count, b->file);
    
    b->header.index_off = index_off;
    
    /* Rewrite header */
    fseek(b->file, 0, SEEK_SET);
    fwrite(&b->header, sizeof(DictCacheHeader), 1, b->file);
}

void dict_cache_builder_free(DictCacheBuilder *b) {
    if (b) {
        dict_chunk_writer_free(b->writer);
        if (b->file) fclose(b->file);
        if (b->defs_file) fclose(b->defs_file);
        g_free(b->cache_path);
        g_free(b);
    }
}

/* ── SQLite headword index builder ───────────────────────── */

#define HW_BATCH_SIZE 50000

struct DictHwBuilder {
    sqlite3      *db;
    sqlite3_stmt *insert_stmt;
    sqlite3_stmt *meta_stmt;
    char         *db_path;
    int           batch_count;
    gboolean      in_txn;
    GString      *norm_str;
};

static gboolean hw_begin(DictHwBuilder *b) {
    if (b->in_txn) return TRUE;
    if (sqlite3_exec(b->db, "BEGIN;", NULL, NULL, NULL) != SQLITE_OK) return FALSE;
    b->in_txn = TRUE;
    return TRUE;
}

DictHwBuilder* dict_hw_builder_new(const char *db_path) {
    char *dir = g_path_get_dirname(db_path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                        NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }

    sqlite3_exec(db,
        "PRAGMA journal_mode = OFF;"
        "PRAGMA synchronous  = OFF;"
        "PRAGMA cache_size   = -65536;" /* 64MB cache for building */
        "PRAGMA temp_store   = MEMORY;"
        "PRAGMA locking_mode = EXCLUSIVE;",
        NULL, NULL, NULL);

    const char *schema =
        "CREATE TABLE IF NOT EXISTS entries ("
        "  id INTEGER PRIMARY KEY,"
        "  headword TEXT NOT NULL,"
        "  normalized TEXT NOT NULL,"
        "  d_off INTEGER NOT NULL,"
        "  d_len INTEGER NOT NULL"
        ");"
        "CREATE TABLE IF NOT EXISTS metadata ("
        "  key TEXT PRIMARY KEY,"
        "  value TEXT NOT NULL"
        ");";
    if (sqlite3_exec(db, schema, NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT INTO entries(headword, normalized, d_off, d_len) "
        "VALUES (?, ?, ?, ?);",
        -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }

    sqlite3_stmt *mstmt = NULL;
    if (sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO metadata(key, value) VALUES (?, ?);",
        -1, &mstmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        return NULL;
    }

    DictHwBuilder *b = g_new0(DictHwBuilder, 1);
    b->db          = db;
    b->insert_stmt = stmt;
    b->meta_stmt   = mstmt;
    b->db_path     = g_strdup(db_path);
    b->batch_count = 0;
    b->in_txn      = FALSE;
    b->norm_str    = g_string_sized_new(128);

    hw_begin(b);
    return b;
}

void dict_hw_builder_add(DictHwBuilder *b,
                         const char *headword, size_t hw_len,
                         uint32_t d_off, uint32_t d_len)
{
    if (!b || !b->insert_stmt) return;

    build_norm_key_gstring(headword, hw_len, b->norm_str);

    sqlite3_reset(b->insert_stmt);
    sqlite3_bind_text(b->insert_stmt, 1, headword, (int)hw_len, SQLITE_TRANSIENT);
    sqlite3_bind_text(b->insert_stmt, 2, b->norm_str->str, (int)b->norm_str->len, SQLITE_TRANSIENT);
    sqlite3_bind_int64(b->insert_stmt, 3, (sqlite3_int64)d_off);
    sqlite3_bind_int64(b->insert_stmt, 4, (sqlite3_int64)d_len);

    if (sqlite3_step(b->insert_stmt) != SQLITE_DONE) {
        fprintf(stderr, "[HW] insert error: %s\n", sqlite3_errmsg(b->db));
    }

    b->batch_count++;
    if (b->batch_count >= HW_BATCH_SIZE) {
        if (sqlite3_exec(b->db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
            fprintf(stderr, "[HW] commit error: %s\n", sqlite3_errmsg(b->db));
        }
        b->in_txn = FALSE;
        b->batch_count = 0;
        hw_begin(b);
    }
}

void dict_hw_builder_set_metadata(DictHwBuilder *b,
                                  const char *key, const char *value)
{
    if (!b || !b->meta_stmt || !key || !value) return;
    sqlite3_reset(b->meta_stmt);
    sqlite3_bind_text(b->meta_stmt, 1, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(b->meta_stmt, 2, value, -1, SQLITE_STATIC);
    if (sqlite3_step(b->meta_stmt) != SQLITE_DONE) {
        fprintf(stderr, "[HW] metadata error: %s\n", sqlite3_errmsg(b->db));
    }
}

gboolean dict_hw_builder_finalize(DictHwBuilder *b) {
    if (!b) return FALSE;

    if (b->in_txn) {
        if (sqlite3_exec(b->db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
            fprintf(stderr, "[HW] finalize commit error: %s\n", sqlite3_errmsg(b->db));
        }
        b->in_txn = FALSE;
    }

    /* Create B-tree index for fast prefix search */
    if (sqlite3_exec(b->db,
        "CREATE INDEX IF NOT EXISTS idx_norm ON entries(normalized);",
        NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "[HW] index error: %s\n", sqlite3_errmsg(b->db));
    }

    /* Clean up */
    if (b->insert_stmt) { sqlite3_finalize(b->insert_stmt); b->insert_stmt = NULL; }
    if (b->meta_stmt)   { sqlite3_finalize(b->meta_stmt);   b->meta_stmt   = NULL; }
    if (b->norm_str)    { g_string_free(b->norm_str, TRUE); b->norm_str = NULL; }
    sqlite3_close(b->db);
    g_free(b->db_path);
    g_free(b);
    return TRUE;
}

void dict_hw_builder_free(DictHwBuilder *b) {
    if (!b) return;
    if (b->in_txn && b->db) sqlite3_exec(b->db, "ROLLBACK;", NULL, NULL, NULL);
    if (b->insert_stmt) { sqlite3_finalize(b->insert_stmt); b->insert_stmt = NULL; }
    if (b->meta_stmt)   { sqlite3_finalize(b->meta_stmt);   b->meta_stmt   = NULL; }
    if (b->norm_str)    { g_string_free(b->norm_str, TRUE); b->norm_str = NULL; }
    if (b->db) sqlite3_close(b->db);
    if (b->db_path) {
        unlink(b->db_path);
        char *wal = g_strconcat(b->db_path, "-wal", NULL);
        char *shm = g_strconcat(b->db_path, "-shm", NULL);
        unlink(wal); g_free(wal);
        unlink(shm); g_free(shm);
    }
    g_free(b->db_path);
    g_free(b);
}
