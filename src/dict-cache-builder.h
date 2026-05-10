#ifndef DICT_CACHE_BUILDER_H
#define DICT_CACHE_BUILDER_H

#include <glib.h>
#include "dict-chunked.h"
#include "flat-index.h"

typedef struct DictCacheBuilder DictCacheBuilder;

DictCacheBuilder* dict_cache_builder_new(const char *cache_path, uint64_t entry_count);
void dict_cache_builder_add_headword(DictCacheBuilder *b, const char *word, size_t len, uint64_t *out_off);
void dict_cache_builder_add_definition(DictCacheBuilder *b, const char *data, size_t len, uint64_t *out_off);
void dict_cache_builder_flush(DictCacheBuilder *b);
void dict_cache_builder_finalize(DictCacheBuilder *b, FlatTreeEntry *entries, uint64_t actual_count);
void dict_cache_builder_finalize_index_only(DictCacheBuilder *b, FlatTreeEntry *entries, uint64_t actual_count, uint32_t source_encoding, const char *stardict_sts);
void dict_cache_builder_free(DictCacheBuilder *b);

#endif
