#pragma once

#include <glib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "flat-index.h"

/* ── DIDX: Dictionary InDeX (index-only, no embedded definitions) ────
 *
 * Unlike the DCMP compressed-cache format, DIDX stores ONLY headword
 * strings and a compact sorted index that points back to byte offsets
 * in the *original* source file.  Definitions are read on-demand via
 * pread() at lookup time.
 *
 * Layout:
 *   [DictIdxHeader]            80 bytes
 *   [source path string]       variable
 *   [headword string pool]     newline-delimited
 *   [DictIdxEntry × N]         20 bytes each
 */

#define DICT_IDX_MAGIC "DIDX"
#define DICT_IDX_VERSION 2

typedef struct {
    char     magic[4];        /* "DIDX"                                 */
    uint32_t version;         /* DICT_IDX_VERSION                       */
    uint64_t entry_count;     /* number of headwords                    */
    uint64_t headwords_off;   /* offset of headword string pool         */
    uint64_t headwords_len;   /* total bytes in headword pool           */
    uint64_t index_off;       /* offset of DictIdxEntry array           */
    uint64_t src_path_off;    /* offset of NUL-terminated source path   */
    uint64_t src_path_len;    /* length of source path (excl. NUL)      */
    int64_t  src_mtime;       /* mtime of source at build time          */
    uint8_t  reserved[8];
} DictIdxHeader;

/* Compact per-entry record — 20 bytes on disk (packed). */
typedef struct __attribute__((packed)) {
    uint32_t h_off;   /* headword offset relative to headwords_off */
    uint16_t h_len;   /* headword length in bytes (max 65535)       */
    uint16_t _pad;    /* alignment / future use                     */
    int64_t  src_off; /* byte offset into the source file           */
    uint32_t src_len; /* definition length in source file bytes      */
} DictIdxEntry;

/* ── Helpers ─────────────────────────────────────────────── */

/* Return TRUE if the mmap'd data starts with DIDX magic. */
gboolean dict_idx_is_index_only(const char *data, size_t size);

/* ── Builder ─────────────────────────────────────────────── */

typedef struct DictIdxBuilder DictIdxBuilder;

/* Create a builder that writes to `out_path`.
 * `source_path` is the original DSL file path stored in the header.
 * `source_mtime` is its modification time.  `entry_count` is a hint
 * (the actual count is finalised later). */
DictIdxBuilder* dict_idx_builder_new(const char *out_path,
                                     const char *source_path,
                                     int64_t     source_mtime,
                                     uint64_t    entry_count_hint);

/* Append one headword + its source-file definition location. */
void dict_idx_builder_add(DictIdxBuilder *b,
                          const char     *headword,
                          size_t          hw_len,
                          int64_t         src_def_off,
                          uint32_t        src_def_len);

/* Sort entries by headword and write the final file.
 * `cancel_flag`/`expected` enable cooperative cancellation. */
gboolean dict_idx_builder_finalize(DictIdxBuilder *b,
                                   volatile gint  *cancel_flag,
                                   gint            expected);

void dict_idx_builder_free(DictIdxBuilder *b);

/* ── Reader ──────────────────────────────────────────────── */

/* Open a DIDX cache file that has already been mmap'd.
 * Populates `out_index` with a FlatIndex (caller frees with flat_index_close).
 * Returns the stored source path (caller must g_free). */
char* dict_idx_open(const char *mmap_data, size_t mmap_size,
                    FlatIndex **out_index);

/* Read a definition from the source file at the given offset/length.
 * Uses pread() for thread-safe, seek-free random access.
 * Caller must g_free() the returned string. */
char* dict_idx_read_definition(int source_fd, int64_t offset, uint32_t length);


