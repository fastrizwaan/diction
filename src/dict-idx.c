#include "dict-idx.h"
#include "flat-index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ── Magic detection ────────────────────────────────────── */

gboolean dict_idx_is_index_only(const char *data, size_t size) {
    if (!data || size < 4) return FALSE;
    return memcmp(data, DICT_IDX_MAGIC, 4) == 0;
}

/* ── Builder ────────────────────────────────────────────── */

typedef struct {
    uint32_t h_off;      /* offset into headword pool */
    uint16_t h_len;
    int64_t  src_off;
    uint32_t src_len;
} BuilderEntry;

struct DictIdxBuilder {
    char          *out_path;
    char          *source_path;
    int64_t        source_mtime;
    FILE          *hw_file;       /* temp file for headword pool */
    uint64_t       hw_len;        /* bytes written to headword pool */
    GArray        *entries;       /* GArray<BuilderEntry> */
};

DictIdxBuilder* dict_idx_builder_new(const char *out_path,
                                     const char *source_path,
                                     int64_t     source_mtime,
                                     uint64_t    entry_count_hint) {
    DictIdxBuilder *b = g_new0(DictIdxBuilder, 1);
    b->out_path      = g_strdup(out_path);
    b->source_path   = g_strdup(source_path);
    b->source_mtime  = source_mtime;
    b->hw_file       = tmpfile();
    if (!b->hw_file) {
        g_free(b->out_path);
        g_free(b->source_path);
        g_free(b);
        return NULL;
    }
    b->entries = g_array_sized_new(FALSE, FALSE, sizeof(BuilderEntry),
                                   (guint)entry_count_hint);
    return b;
}

void dict_idx_builder_add(DictIdxBuilder *b,
                          const char     *headword,
                          size_t          hw_len,
                          int64_t         src_def_off,
                          uint32_t        src_def_len) {
    if (!b) return;

    BuilderEntry e;
    e.h_off   = (uint32_t)b->hw_len;
    e.h_len   = (hw_len > 65535) ? 65535 : (uint16_t)hw_len;
    e.src_off = src_def_off;
    e.src_len = src_def_len;
    g_array_append_val(b->entries, e);

    fwrite(headword, 1, hw_len, b->hw_file);
    fwrite("\n", 1, 1, b->hw_file);
    b->hw_len += hw_len + 1;
}

/* Sort comparator — reuses the DSL-agnostic headword comparison logic
 * from flat-index.c via the public flat_index_sort_entries helper.
 * We build a temporary FlatTreeEntry array for sorting, then map back. */

gboolean dict_idx_builder_finalize(DictIdxBuilder *b,
                                   volatile gint  *cancel_flag,
                                   gint            expected) {
    if (!b) return FALSE;

    uint64_t count = b->entries->len;
    if (count == 0) return FALSE;

    /* 1. Compute layout offsets */
    size_t src_path_len = strlen(b->source_path);
    DictIdxHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.magic, DICT_IDX_MAGIC, 4);
    hdr.version      = DICT_IDX_VERSION;
    hdr.entry_count  = count;
    hdr.src_path_off = sizeof(DictIdxHeader);
    hdr.src_path_len = (uint64_t)src_path_len;
    hdr.headwords_off = hdr.src_path_off + src_path_len + 1; /* +1 for NUL */
    hdr.headwords_len = b->hw_len;
    hdr.src_mtime     = b->source_mtime;
    /* index_off is set after writing headwords */

    /* 2. Read headword pool from temp file */
    char *hw_pool = g_malloc((gsize)b->hw_len);
    fseek(b->hw_file, 0, SEEK_SET);
    if (fread(hw_pool, 1, (size_t)b->hw_len, b->hw_file) != (size_t)b->hw_len) {
        g_free(hw_pool);
        return FALSE;
    }

    /* 3. Build FlatTreeEntry array for sorting.
     * h_off is relative to start of hw_pool.  We set it to
     * absolute-file-offset (headwords_off + relative) so the sort
     * comparator (which uses data + h_off) works correctly when we pass
     * a "data" pointer offset by -headwords_off.  HOWEVER, the simplest
     * approach is to temporarily create a buffer where the headword pool
     * starts at index 0 and sort using that. */
    FlatTreeEntry *sort_entries = g_new0(FlatTreeEntry, (gsize)count);
    for (uint64_t i = 0; i < count; i++) {
        BuilderEntry *be = &g_array_index(b->entries, BuilderEntry, i);
        sort_entries[i].h_off = (int64_t)be->h_off;
        sort_entries[i].h_len = (uint64_t)be->h_len;
        sort_entries[i].d_off = (int64_t)be->src_off;
        sort_entries[i].d_len = (uint64_t)be->src_len;
    }

    /* Sort by headword using the DSL-agnostic comparator */
    flat_index_sort_entries(sort_entries, (size_t)count, hw_pool, (size_t)b->hw_len);

    if (cancel_flag && g_atomic_int_get(cancel_flag) != expected) {
        g_free(hw_pool);
        g_free(sort_entries);
        return FALSE;
    }

    /* 4. Write the final file */
    FILE *f = fopen(b->out_path, "wb");
    if (!f) {
        g_free(hw_pool);
        g_free(sort_entries);
        return FALSE;
    }

    /* Placeholder header */
    fwrite(&hdr, sizeof(hdr), 1, f);

    /* Source path (NUL-terminated) */
    fwrite(b->source_path, 1, src_path_len + 1, f);

    /* Headword pool */
    fwrite(hw_pool, 1, (size_t)b->hw_len, f);

    /* Index offset = current position */
    hdr.index_off = (uint64_t)ftell(f);

    /* Write compact DictIdxEntry records (20 bytes each, packed) */
    for (uint64_t i = 0; i < count; i++) {
        DictIdxEntry de;
        de.h_off   = (uint32_t)sort_entries[i].h_off;
        de.h_len   = (uint16_t)sort_entries[i].h_len;
        de._pad    = 0;
        de.src_off = sort_entries[i].d_off;
        de.src_len = (uint32_t)sort_entries[i].d_len;
        fwrite(&de, sizeof(DictIdxEntry), 1, f);
    }

    /* Rewrite header with correct index_off */
    fseek(f, 0, SEEK_SET);
    fwrite(&hdr, sizeof(hdr), 1, f);

    fclose(f);
    g_free(hw_pool);
    g_free(sort_entries);
    return TRUE;
}

void dict_idx_builder_free(DictIdxBuilder *b) {
    if (!b) return;
    if (b->hw_file) fclose(b->hw_file);
    if (b->entries) g_array_free(b->entries, TRUE);
    g_free(b->out_path);
    g_free(b->source_path);
    g_free(b);
}

/* ── Reader ─────────────────────────────────────────────── */

char* dict_idx_open(const char *mmap_data, size_t mmap_size,
                    FlatIndex **out_index) {
    if (!mmap_data || mmap_size < sizeof(DictIdxHeader)) return NULL;
    if (memcmp(mmap_data, DICT_IDX_MAGIC, 4) != 0) return NULL;

    const DictIdxHeader *hdr = (const DictIdxHeader *)mmap_data;
    if (hdr->version != DICT_IDX_VERSION) return NULL;
    if (hdr->entry_count == 0) return NULL;

    uint64_t count = hdr->entry_count;

    /* Validate that the index region fits in the mmap */
    uint64_t index_end = hdr->index_off + count * sizeof(DictIdxEntry);
    if (index_end > mmap_size) return NULL;

    /* Validate headword pool fits */
    if (hdr->headwords_off + hdr->headwords_len > mmap_size) return NULL;

    /* Read stored source path */
    char *source_path = NULL;
    if (hdr->src_path_off + hdr->src_path_len < mmap_size) {
        source_path = g_strndup(mmap_data + hdr->src_path_off,
                                (gsize)hdr->src_path_len);
    }

    /* Build FlatIndex by expanding DictIdxEntry → FlatTreeEntry.
     * FlatTreeEntry.h_off is set to absolute offset in the mmap
     * (headwords_off + relative), and d_off/d_len store the source-file
     * offsets (not mmap offsets). */
    if (out_index) {
        FlatTreeEntry *flat = g_new(FlatTreeEntry, (gsize)count);
        const DictIdxEntry *didx = (const DictIdxEntry *)(mmap_data + hdr->index_off);

        for (uint64_t i = 0; i < count; i++) {
            flat[i].h_off = (int64_t)(hdr->headwords_off + didx[i].h_off);
            flat[i].h_len = (uint64_t)didx[i].h_len;
            flat[i].d_off = didx[i].src_off;
            flat[i].d_len = (uint64_t)didx[i].src_len;
        }

        FlatIndex *idx = g_new0(FlatIndex, 1);
        idx->entries   = flat;
        idx->count     = (size_t)count;
        idx->mmap_data = mmap_data;
        idx->mmap_size = mmap_size;

        *out_index = idx;
    }

    return source_path;
}

/* Read a definition directly from the source file using pread(). */
char* dict_idx_read_definition(int source_fd, int64_t offset, uint32_t length) {
    if (source_fd < 0 || length == 0) return g_strdup("");

    char *buf = g_malloc((gsize)length + 1);
    ssize_t nread = pread(source_fd, buf, (size_t)length, (off_t)offset);
    if (nread <= 0) {
        g_free(buf);
        return g_strdup("");
    }
    buf[nread] = '\0';
    return buf;
}
