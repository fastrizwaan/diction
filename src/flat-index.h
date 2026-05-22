#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <glib.h>

/* FlatTreeEntry: h_off/h_len reference into FlatIndex.headword_buf (in-memory).
 * d_off/d_len reference into the definition source (cache or source file). */
typedef struct {
    uint32_t h_off;
    uint32_t h_len;
    uint32_t d_off;
    uint32_t d_len;
} FlatTreeEntry;

/* NormKey for binary search */
typedef struct {
    uint32_t off;
    uint16_t len;
} NormKey;

/* FlatIndex: SQLite-backed sorted read-only headword index.
 * Entries are loaded into memory at open time for O(1) positional access. */
typedef struct {
    size_t           count;
    FlatTreeEntry   *entries;       /* in-memory array indexed by position */
    char            *headword_buf;  /* all headwords concatenated */
    size_t           buf_size;      /* size of headword_buf */
    guint32         *sorted_ids;    /* entry IDs sorted by normalized (== index order) */
    char            *norm_buf;      /* normalized keys contiguous buffer */
    NormKey         *norm_keys;     /* parallel array: one per entry */
    char            *db_path;       /* path to .hw.sqlite file (informational) */
    GHashTable      *metadata;      /* key -> value mapping from metadata table */
} FlatIndex;

/* Open a headword index from a SQLite database file. */
FlatIndex* flat_index_open(const char *db_path);

/* Get a metadata value from the index. Returns NULL if not found. */
const char* flat_index_get_metadata(const FlatIndex *idx, const char *key);

/* Free the FlatIndex (entries/headword_buf/norm cache). */
void flat_index_close(FlatIndex *idx);

/* Exact match search (case-insensitive). Returns position or (size_t)-1. */
size_t flat_index_search(const FlatIndex *idx, const char *query);
size_t flat_index_search_fast(const FlatIndex *idx, const char *query);

/* Prefix search. Returns first matching position or (size_t)-1. */
size_t flat_index_search_prefix(const FlatIndex *idx, const char *prefix);
size_t flat_index_search_prefix_fast(const FlatIndex *idx, const char *prefix);

/* Positional access. */
const FlatTreeEntry* flat_index_get(const FlatIndex *idx, size_t pos);
const FlatTreeEntry* flat_index_successor(const FlatIndex *idx, size_t pos);
const FlatTreeEntry* flat_index_random(const FlatIndex *idx);
size_t flat_index_count(const FlatIndex *idx);

/* Validate index structural sanity. */
bool flat_index_validate(const FlatIndex *idx);

/* DSL comparison helpers — used by consumers and build_norm_key(). */
int compare_dsl_internal(const char *a, size_t la, bool a_raw,
                         const char *b, size_t lb, bool b_raw);
int compare_dsl_agnostic(const char *raw, size_t raw_len,
                         const char *clean, size_t clean_len);
int compare_headword(const char *data, const FlatTreeEntry *entry,
                     const char *query, size_t qlen);

/* Alias-aware matching helpers used by main.c. */
bool flat_index_entry_matches_query(const char *data, const FlatTreeEntry *entry,
                                    const char *query, size_t qlen);
bool flat_index_entry_matches_prefix(const char *data, const FlatTreeEntry *entry,
                                     const char *prefix, size_t plen);

/* Normalize a raw headword: lowercase, decompose Unicode, strip DSL noise.
 * Returns newly allocated UTF-8 string; caller must g_free(). */
char* build_norm_key(const char *raw, size_t raw_len, size_t *out_len);

/* Compute path: ~/.cache/diction/hw/<sha1-of-dict-path> (caller frees). */
char* dict_hw_index_path_for(const char *dict_path);
