/*
 * dict-fts-index.c — persistent SQLite FTS5 full-text search index.
 *
 * Index lives at:  ~/.cache/diction/fts/<sha1-of-dict-path>.sqlite
 * Schema: one contentless FTS5 virtual table; rowid == flat-index entry_id.
 *
 * SQLite is configured for minimal RAM use:
 *   PRAGMA cache_size = -512   (512 KiB page cache)
 *   PRAGMA temp_store = FILE   (spill temporaries to disk)
 *   PRAGMA mmap_size  = 0      (no memory-mapped I/O)
 *
 * Indexing commits every FTS_BATCH_SIZE rows with a 2 ms pause
 * between batches to keep disk/CPU pressure low.
 */

#include "dict-fts-index.h"
#include "dict-cache.h"
#include <sqlite3.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

/* Rows between COMMIT cycles during indexing */
#define FTS_BATCH_SIZE 500

/* Maximum definition bytes indexed per entry (keeps DB small) */
#define FTS_MAX_DEF_BYTES 8192

/* Maximum candidate rows returned from one FTS5 MATCH query */
#define FTS_MAX_CANDIDATES 2000

/* ------------------------------------------------------------------ */
/*  Path helpers                                                         */
/* ------------------------------------------------------------------ */

char* dict_fts_sqlite_path_for(const char *dict_path)
{
    if (!dict_path) return NULL;
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, dict_path, -1);
    const char *base = dict_cache_base_dir();
    char *fname = g_strdup_printf("%s.sqlite", hash);
    char *path  = g_build_filename(base, "diction", "fts", fname, NULL);
    g_free(fname);
    g_free(hash);
    return path;
}

bool dict_fts_index_exists(const char *dict_path)
{
    char *p = dict_fts_sqlite_path_for(dict_path);
    bool ok  = p && g_file_test(p, G_FILE_TEST_EXISTS);
    g_free(p);
    return ok;
}

void dict_fts_index_delete(const char *dict_path)
{
    char *p = dict_fts_sqlite_path_for(dict_path);
    if (!p) return;
    g_unlink(p);
    char *wal = g_strconcat(p, "-wal", NULL);
    char *shm = g_strconcat(p, "-shm", NULL);
    g_unlink(wal);
    g_unlink(shm);
    g_free(wal);
    g_free(shm);
    g_free(p);
}

/* ------------------------------------------------------------------ */
/*  Shared PRAGMA setup                                                  */
/* ------------------------------------------------------------------ */

static void fts_apply_pragmas(sqlite3 *db)
{
    sqlite3_exec(db,
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous  = NORMAL;"
        "PRAGMA cache_size   = -512;"
        "PRAGMA temp_store   = FILE;"
        "PRAGMA mmap_size    = 0;",
        NULL, NULL, NULL);
}

/* ------------------------------------------------------------------ */
/*  Builder                                                              */
/* ------------------------------------------------------------------ */

struct DictFtsBuilder {
    sqlite3      *db;
    sqlite3_stmt *insert_stmt;
    char         *db_path;   /* final destination */
    char         *tmp_path;  /* written here, renamed on finish */
    guint         batch_count;
    gboolean      in_txn;
};

static gboolean fts_begin(DictFtsBuilder *b)
{
    if (b->in_txn) return TRUE;
    if (sqlite3_exec(b->db, "BEGIN;", NULL, NULL, NULL) != SQLITE_OK)
        return FALSE;
    b->in_txn = TRUE;
    return TRUE;
}

DictFtsBuilder* dict_fts_builder_new(const char *dict_path, GError **err)
{
    /* Ensure cache directory exists */
    const char *base = dict_cache_base_dir();
    char *fts_dir = g_build_filename(base, "diction", "fts", NULL);
    g_mkdir_with_parents(fts_dir, 0755);
    g_free(fts_dir);

    char *db_path  = dict_fts_sqlite_path_for(dict_path);
    char *tmp_path = g_strconcat(db_path, ".tmp", NULL);
    g_unlink(tmp_path); /* remove stale tmp */

    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(tmp_path, &db,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
                             NULL);
    if (rc != SQLITE_OK) {
        if (err)
            g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "FTS: cannot open db: %s",
                        db ? sqlite3_errmsg(db) : "unknown");
        if (db) sqlite3_close(db);
        g_free(db_path);
        g_free(tmp_path);
        return NULL;
    }

    fts_apply_pragmas(db);

    /* Contentless FTS5: stores only the inverted index, not the raw text.
     * rowid == flat-index entry_id.
     * We index a single 'text' column that combines headword + definition. */
    const char *schema =
        "CREATE VIRTUAL TABLE IF NOT EXISTS fts USING fts5("
        "  text,"
        "  content='',"
        "  tokenize='unicode61 remove_diacritics 1'"
        ");";

    if (sqlite3_exec(db, schema, NULL, NULL, NULL) != SQLITE_OK) {
        if (err)
            g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "FTS schema error: %s", sqlite3_errmsg(db));
        sqlite3_close(db);
        g_unlink(tmp_path);
        g_free(db_path);
        g_free(tmp_path);
        return NULL;
    }

    sqlite3_stmt *stmt = NULL;
    sqlite3_prepare_v2(db,
        "INSERT INTO fts(rowid, text) VALUES (?, ?);",
        -1, &stmt, NULL);

    DictFtsBuilder *b = g_new0(DictFtsBuilder, 1);
    b->db          = db;
    b->insert_stmt = stmt;
    b->db_path     = db_path;
    b->tmp_path    = tmp_path;
    b->batch_count = 0;
    b->in_txn      = FALSE;

    fts_begin(b);
    return b;
}

gboolean dict_fts_builder_add(DictFtsBuilder *b,
                              guint        entry_id,
                              const char  *headword, gsize hw_len,
                              const char  *definition, gsize def_len)
{
    if (!b || !b->insert_stmt) return FALSE;

    gsize dlen = MIN(def_len, FTS_MAX_DEF_BYTES);

    /* Build combined text: headword \n definition */
    GString *text = g_string_sized_new(hw_len + 1 + dlen);
    if (headword && hw_len)
        g_string_append_len(text, headword, (gssize)hw_len);
    if (definition && dlen) {
        g_string_append_c(text, '\n');
        g_string_append_len(text, definition, (gssize)dlen);
    }

    sqlite3_reset(b->insert_stmt);
    sqlite3_bind_int64(b->insert_stmt, 1, (sqlite3_int64)entry_id);
    sqlite3_bind_text(b->insert_stmt, 2,
                      text->str, (int)text->len, SQLITE_TRANSIENT);
    g_string_free(text, TRUE);

    int rc = sqlite3_step(b->insert_stmt);
    if (rc != SQLITE_DONE) {
        fprintf(stderr, "[FTS] insert error: %s\n", sqlite3_errmsg(b->db));
        return FALSE;
    }

    b->batch_count++;
    return TRUE;
}

gboolean dict_fts_builder_commit_batch(DictFtsBuilder *b)
{
    if (!b || !b->in_txn) return TRUE;
    if (sqlite3_exec(b->db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK)
        return FALSE;
    b->in_txn      = FALSE;
    b->batch_count = 0;
    g_usleep(2000); /* 2 ms pause — reduce disk/CPU pressure */
    return fts_begin(b);
}

gboolean dict_fts_builder_finish(DictFtsBuilder *b, GError **err)
{
    if (!b) return FALSE;

    if (b->in_txn) {
        if (sqlite3_exec(b->db, "COMMIT;", NULL, NULL, NULL) != SQLITE_OK) {
            if (err)
                g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "FTS final commit: %s", sqlite3_errmsg(b->db));
            dict_fts_builder_abort(b);
            return FALSE;
        }
        b->in_txn = FALSE;
    }

    /* Optimise the FTS5 index (merges segments) */
    sqlite3_exec(b->db,
        "INSERT INTO fts(fts) VALUES('optimize');",
        NULL, NULL, NULL);

    if (b->insert_stmt) { sqlite3_finalize(b->insert_stmt); b->insert_stmt = NULL; }
    sqlite3_close(b->db); b->db = NULL;

    /* Atomic rename tmp → final */
    if (g_rename(b->tmp_path, b->db_path) != 0) {
        if (err)
            g_set_error(err, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "FTS rename failed: %s", g_strerror(errno));
        g_unlink(b->tmp_path);
        g_free(b->db_path); g_free(b->tmp_path); g_free(b);
        return FALSE;
    }

    g_free(b->db_path); g_free(b->tmp_path); g_free(b);
    return TRUE;
}

void dict_fts_builder_abort(DictFtsBuilder *b)
{
    if (!b) return;
    if (b->in_txn)      sqlite3_exec(b->db, "ROLLBACK;", NULL, NULL, NULL);
    if (b->insert_stmt) sqlite3_finalize(b->insert_stmt);
    if (b->db)          sqlite3_close(b->db);
    if (b->tmp_path)    g_unlink(b->tmp_path);
    g_free(b->db_path); g_free(b->tmp_path); g_free(b);
}

/* ------------------------------------------------------------------ */
/*  Query                                                                */
/* ------------------------------------------------------------------ */

/* Build a safe FTS5 MATCH expression from a plain text query.
 * Each whitespace-delimited token is wrapped in double-quotes (FTS5 phrase).
 * Internal double-quotes are doubled per FTS5 spec.
 * Returns NULL if no non-empty tokens, caller frees otherwise. */
static char* build_fts5_query(const char *query)
{
    if (!query || !*query) return NULL;

    char **tokens = g_strsplit_set(query, " \t\n\r", -1);
    GString *out  = g_string_new("");

    for (int i = 0; tokens[i]; i++) {
        const char *tok = tokens[i];
        if (!*tok) continue;
        if (out->len > 0) g_string_append_c(out, ' ');
        g_string_append_c(out, '"');
        for (const char *p = tok; *p; p++) {
            if (*p == '"') g_string_append_c(out, '"'); /* escape */
            g_string_append_c(out, *p);
        }
        g_string_append_c(out, '"');
    }

    g_strfreev(tokens);
    if (out->len == 0) { g_string_free(out, TRUE); return NULL; }
    return g_string_free(out, FALSE);
}

GArray* dict_fts_query_candidates(const char *dict_path,
                                  const char *query,
                                  size_t      start_pos,
                                  guint       limit)
{
    if (!dict_path || !query) return NULL;

    if (!dict_fts_index_exists(dict_path))
        return NULL; /* no index — no fallback */

    char *db_path = dict_fts_sqlite_path_for(dict_path);
    sqlite3 *db   = NULL;
    int rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL);
    g_free(db_path);
    if (rc != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    fts_apply_pragmas(db);

    char *fts_query = build_fts5_query(query);
    if (!fts_query) { sqlite3_close(db); return NULL; }

    /* Retrieve up to `limit` rowids >= start_pos, ordered ascending */
    const char *sql =
        "SELECT rowid FROM fts WHERE fts MATCH ? AND rowid >= ?"
        " ORDER BY rowid LIMIT ?;";

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        g_free(fts_query);
        sqlite3_close(db);
        return NULL;
    }

    sqlite3_bind_text(stmt,  1, fts_query, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, (sqlite3_int64)start_pos);
    sqlite3_bind_int(stmt,   3, (int)MIN(limit, (guint)FTS_MAX_CANDIDATES));
    g_free(fts_query);

    GArray *arr = g_array_new(FALSE, FALSE, sizeof(guint32));

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        sqlite3_int64 rowid = sqlite3_column_int64(stmt, 0);
        if (rowid >= 0) {
            guint32 eid = (guint32)rowid;
            g_array_append_val(arr, eid);
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (arr->len == 0) { g_array_free(arr, TRUE); return NULL; }
    return arr;
}
