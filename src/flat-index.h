#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <glib.h>

/* TreeEntry: same layout used in cache files. Each entry stores
 * byte offsets and lengths into the mmap'd cache data for headword
 * and definition. */
typedef struct {
    int64_t  h_off;
    uint64_t h_len;
    int64_t  d_off;
    uint64_t d_len;
} FlatTreeEntry;

/* FlatIndex: a sorted, read-only index backed by mmap'd data.
 * Zero heap allocation for the entries themselves — they point
 * directly into the mmap'd cache file. */
typedef struct {
    const FlatTreeEntry *entries; /* points into mmap'd data */
    size_t count;
    const char *mmap_data;       /* base pointer of mmap'd file */
    size_t mmap_size;
} FlatIndex;

/* Create a FlatIndex from mmap'd cache data. Reads the count from
 * the first 8 bytes and locates the TreeEntry array at the end.
 * Returns NULL on failure. The returned struct is heap-allocated
 * (single small alloc). */
FlatIndex* flat_index_open(const char *data, size_t size);

/* Free the FlatIndex struct (does NOT unmap data). */
void flat_index_close(FlatIndex *idx);

/* Search for an exact (case-insensitive) match. Returns the index
 * position of the first match, or (size_t)-1 if not found. */
size_t flat_index_search(const FlatIndex *idx, const char *query);

/* Search for the first entry whose headword starts with `prefix`
 * (case-insensitive). Returns position or (size_t)-1. */
size_t flat_index_search_prefix(const FlatIndex *idx, const char *prefix);

/* Full-text search inside definition payloads using a precompiled GRegex.
 * Iterates through entries starting at `start_pos` until a match is found.
 * Returns the matching position or (size_t)-1 if none. */
size_t flat_index_search_fts(const FlatIndex *idx, GRegex *regex, size_t start_pos);

/* Get entry at position `pos`. Returns NULL if out of range. */
const FlatTreeEntry* flat_index_get(const FlatIndex *idx, size_t pos);

/* Get the next entry (successor). Returns NULL if at end. */
const FlatTreeEntry* flat_index_successor(const FlatIndex *idx, size_t pos);

/* Get a random entry. Returns NULL if index is empty. */
const FlatTreeEntry* flat_index_random(const FlatIndex *idx);

/* Get entry count. */
size_t flat_index_count(const FlatIndex *idx);

/* Validate that all entries in the index have sane offsets.
 * Returns true if valid, false if corrupt. */
bool flat_index_validate(const FlatIndex *idx);

/* Sort a mutable TreeEntry array by headword (case-insensitive).
 * Used during cache building. */
void flat_index_sort_entries(FlatTreeEntry *entries, size_t count,
                             const char *data, size_t data_size);

/* Compare an entry's headword against a query using agnostic rules.
 * Exposed for iterating group matches in main.c. */
int compare_headword(const char *data, const FlatTreeEntry *entry,
                     const char *query, size_t qlen);
