#pragma once

#include <glib.h>
#include <stddef.h>
#include <stdbool.h>

/* ---------- Path helpers ---------- */

/* Returns the SQLite DB path for the given dictionary source path.
 * Stored at ~/.cache/diction/fts/<sha1>.sqlite  (caller frees). */
char* dict_fts_sqlite_path_for(const char *dict_path);

/* Returns TRUE if a valid FTS index exists on disk. */
bool  dict_fts_index_exists(const char *dict_path);

/* Delete the FTS index for dict_path (WAL/SHM files too). */
void  dict_fts_index_delete(const char *dict_path);

/* ---------- Builder (run on a worker thread) ---------- */

typedef struct DictFtsBuilder DictFtsBuilder;

/* Open a new indexing session writing to a temp file.
 * Returns NULL on failure (err set if non-NULL). */
DictFtsBuilder* dict_fts_builder_new(const char *dict_path, GError **err);

/* Add one entry.  entry_id is the FlatIndex position (== future rowid). */
gboolean dict_fts_builder_add(DictFtsBuilder *b,
                              guint        entry_id,
                              const char  *headword, gsize hw_len,
                              const char  *definition, gsize def_len);

/* Commit the current batch (call every ~500 rows).
 * Includes a 2 ms sleep to reduce disk/CPU pressure. */
gboolean dict_fts_builder_commit_batch(DictFtsBuilder *b);

/* Finalise: optimize FTS, close DB, atomic-rename tmp→final. */
gboolean dict_fts_builder_finish(DictFtsBuilder *b, GError **err);

/* Abort: rollback, close, delete tmp file. */
void dict_fts_builder_abort(DictFtsBuilder *b);

/* ---------- Query (used by dict-loader.c) ---------- */

/* Return a sorted GArray<guint32> of candidate entry_ids >= start_pos
 * that the FTS5 index thinks match the query, up to `limit` candidates.
 * Returns NULL if the index is missing, the query produces no FTS5
 * tokens, or any SQLite error occurs.  No scan fallback.
 * Caller frees the GArray. */
GArray* dict_fts_query_candidates(const char *dict_path,
                                  const char *query,
                                  size_t      start_pos,
                                  guint       limit);
