#ifndef DICT_CACHE_BUILDER_H
#define DICT_CACHE_BUILDER_H

#include <glib.h>
#include "dict-chunked.h"
#include "flat-index.h"

/* ── Legacy compressed cache builder (for definition storage) ── */

typedef struct DictCacheBuilder DictCacheBuilder;

DictCacheBuilder* dict_cache_builder_new(const char *cache_path, uint64_t entry_count);
void dict_cache_builder_add_headword(DictCacheBuilder *b, const char *word, size_t len, uint64_t *out_off);
void dict_cache_builder_add_definition(DictCacheBuilder *b, const char *data, size_t len, uint64_t *out_off);
void dict_cache_builder_flush(DictCacheBuilder *b);
void dict_cache_builder_finalize(DictCacheBuilder *b, FlatTreeEntry *entries, uint64_t actual_count);
void dict_cache_builder_finalize_index_only(DictCacheBuilder *b, FlatTreeEntry *entries, uint64_t actual_count, uint32_t source_encoding, const char *stardict_sts);
void dict_cache_builder_free(DictCacheBuilder *b);

/* ── SQLite headword index builder (replaces binary FlatTreeEntry) ── */

typedef struct DictHwBuilder DictHwBuilder;

/* Create a new headword index database.
 * db_path: path to the .hw.sqlite file to create. */
DictHwBuilder* dict_hw_builder_new(const char *db_path);

/* Add one entry. headword is the raw headword string.
 * d_off/d_len are definition offsets in the source/cache file. */
void dict_hw_builder_add(DictHwBuilder *b,
                         const char *headword, size_t hw_len,
                         uint32_t d_off, uint32_t d_len);

/* Set a metadata key=value pair (stored in the metadata table).
 * Can be called before or during insertion. */
void dict_hw_builder_set_metadata(DictHwBuilder *b,
                                  const char *key, const char *value);

/* Finalize: commit, create index, optimize. Returns TRUE on success. */
gboolean dict_hw_builder_finalize(DictHwBuilder *b);

/* Abort and free. */
void dict_hw_builder_free(DictHwBuilder *b);

#endif
