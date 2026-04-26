#include "flat-index.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <glib.h>


/* ── helpers ──────────────────────────────────────────── */


static gboolean dsl_headword_is_escapable_char(char c) {
    return c != '\0' && strchr(" {}~\\@#()[]<>;", c) != NULL;
}

static size_t get_dsl_brace_tag_len(const char *s, size_t max_len) {
    static const char *patterns[] = {
        "{*}",
        "{·}",
        "{ˈ}",
        "{ˌ}",
        "{[']}",
        "{[/']}"
    };

    if (!s || max_len == 0 || s[0] != '{') {
        return 0;
    }

    for (guint i = 0; i < G_N_ELEMENTS(patterns); i++) {
        size_t len = strlen(patterns[i]);
        if (len <= max_len && strncmp(s, patterns[i], len) == 0) {
            return len;
        }
    }

    return 0;
}

static size_t get_dsl_ignored_len_ext(const char *s, size_t max_len, bool raw_side) {
    if (max_len == 0) return 0;

    if (s[0] == '{') {
        size_t brace_tag_len = get_dsl_brace_tag_len(s, max_len);
        if (brace_tag_len > 0) {
            return brace_tag_len;
        }
        if (raw_side) {
            return 1;
        }
    }

    if (raw_side && s[0] == '}') {
        return 1;
    }

    char c = s[0];
    if (g_ascii_isspace(c) || c == '*') {
        return 1;
    }
    
    unsigned char u0 = (unsigned char)s[0];
    if (max_len >= 2) {
        unsigned char u1 = (unsigned char)s[1];
        if (u0 == 0xC2 && u1 == 0xB7) return 2; /* U+00B7 Middle Dot */
        if (u0 == 0xCB && (u1 == 0x88 || u1 == 0x8C)) return 2; /* U+02C8, U+02CC Stress marks */
        if (u0 == 0xCC && u1 == 0x81) return 2; /* U+0301 Combining Acute Accent */
    }
    return 0;
}

static int compare_dsl_agnostic(const char *raw, size_t raw_len, const char *clean, size_t clean_len) {
    size_t r = 0, c = 0;
    size_t skip;

    while (r < raw_len || c < clean_len) {
        while (r < raw_len && (skip = get_dsl_ignored_len_ext(raw + r, raw_len - r, true)) > 0) r += skip;
        while (c < clean_len && (skip = get_dsl_ignored_len_ext(clean + c, clean_len - c, false)) > 0) c += skip;

        if (r == raw_len || c == clean_len) break;

        char char_r, char_c;
        if (raw[r] == '\\' && r + 1 < raw_len && dsl_headword_is_escapable_char(raw[r + 1])) {
            r++; char_r = raw[r++];
        } else {
            char_r = raw[r++];
        }
        char_c = clean[c++];

        int diff = g_ascii_tolower(char_r) - g_ascii_tolower(char_c);
        if (diff != 0) return diff;
    }

    while (r < raw_len && (skip = get_dsl_ignored_len_ext(raw + r, raw_len - r, true)) > 0) r += skip;
    while (c < clean_len && (skip = get_dsl_ignored_len_ext(clean + c, clean_len - c, false)) > 0) c += skip;

    if (r == raw_len && c == clean_len) return 0;
    return (r == raw_len) ? -1 : 1;
}

int compare_headword(const char *data, const FlatTreeEntry *entry,
                     const char *query, size_t qlen) {
    return compare_dsl_agnostic(data + entry->h_off, entry->h_len, query, qlen);
}

static int compare_prefix(const char *data, const FlatTreeEntry *entry,
                           const char *prefix, size_t plen) {
    size_t r = 0, c = 0;
    const char *raw = data + entry->h_off;
    size_t raw_len = entry->h_len;
    size_t skip;

    while (r < raw_len && c < plen) {
        while (r < raw_len && (skip = get_dsl_ignored_len_ext(raw + r, raw_len - r, true)) > 0) r += skip;
        while (c < plen && (skip = get_dsl_ignored_len_ext(prefix + c, plen - c, false)) > 0) c += skip;

        if (r == raw_len || c == plen) break;

        char char_r, char_c;
        if (raw[r] == '\\' && r + 1 < raw_len && dsl_headword_is_escapable_char(raw[r + 1])) {
            r++; char_r = raw[r++];
        } else {
            char_r = raw[r++];
        }
        char_c = prefix[c++];

        int diff = g_ascii_tolower(char_r) - g_ascii_tolower(char_c);
        if (diff != 0) return diff;
    }
    if (c == plen) return 0;
    return -1;
}

/* Comparator for qsort during cache building */
typedef struct {
    const char *data;
} SortCtx;

static __thread const char *sort_data_ptr = NULL;

static int sort_compare(const void *a, const void *b) {
    const FlatTreeEntry *ea = (const FlatTreeEntry *)a;
    const FlatTreeEntry *eb = (const FlatTreeEntry *)b;
    const char *ra = sort_data_ptr + ea->h_off;
    const char *rb = sort_data_ptr + eb->h_off;
    size_t la = (size_t)ea->h_len;
    size_t lb = (size_t)eb->h_len;

    size_t i = 0, j = 0;
    size_t skip;

    while (i < la || j < lb) {
        while (i < la && (skip = get_dsl_ignored_len_ext(ra + i, la - i, true)) > 0) i += skip;
        while (j < lb && (skip = get_dsl_ignored_len_ext(rb + j, lb - j, true)) > 0) j += skip;

        if (i == la || j == lb) break;

        char char_a, char_b;
        if (ra[i] == '\\' && i + 1 < la && dsl_headword_is_escapable_char(ra[i + 1])) {
            i++; char_a = ra[i++];
        } else {
            char_a = ra[i++];
        }

        if (rb[j] == '\\' && j + 1 < lb && dsl_headword_is_escapable_char(rb[j + 1])) {
            j++; char_b = rb[j++];
        } else {
            char_b = rb[j++];
        }

        int diff = g_ascii_tolower(char_a) - g_ascii_tolower(char_b);
        if (diff != 0) return diff;
    }

    while (i < la && (skip = get_dsl_ignored_len_ext(ra + i, la - i, true)) > 0) i += skip;
    while (j < lb && (skip = get_dsl_ignored_len_ext(rb + j, lb - j, true)) > 0) j += skip;

    if (i == la && j == lb) {
        /* Tie-breaker: Case-sensitive exact comparison of the RAW bytes */
        size_t min_len = (la < lb) ? la : lb;
        int diff = strncmp(ra, rb, min_len);
        if (diff != 0) return diff;
        if (la < lb) return -1;
        if (la > lb) return 1;
        return 0;
    }
    if (i == la) return -1;
    return 1;
}

/* ── public API ──────────────────────────────────────── */

FlatIndex* flat_index_open(const char *data, size_t size) {
    if (!data || size < 8) return NULL;

    uint64_t count = *(const uint64_t *)data;
    size_t index_size = (size_t)count * sizeof(FlatTreeEntry);

    if (count == 0 || index_size + 8 > size) {
        /* No index or invalid — return empty index */
        FlatIndex *idx = g_new0(FlatIndex, 1);
        if (!idx) return NULL;
        idx->entries = NULL;
        idx->count = 0;
        idx->mmap_data = data;
        idx->mmap_size = size;
        return idx;
    }

    size_t index_off = size - index_size;
    const FlatTreeEntry *entries = (const FlatTreeEntry *)(data + index_off);

    FlatIndex *idx = g_new0(FlatIndex, 1);
    if (!idx) return NULL;
    idx->entries = entries;
    idx->count = (size_t)count;
    idx->mmap_data = data;
    idx->mmap_size = size;
    return idx;
}

void flat_index_close(FlatIndex *idx) {
    g_free(idx); /* entries are mmap'd, not heap-allocated */
}

size_t flat_index_search(const FlatIndex *idx, const char *query) {
    if (!idx || !idx->entries || idx->count == 0 || !query)
        return (size_t)-1;

    size_t qlen = strlen(query);
    size_t lo = 0, hi = idx->count;
    size_t result = (size_t)-1;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = compare_headword(idx->mmap_data, &idx->entries[mid], query, qlen);
        if (cmp < 0) {
            lo = mid + 1;
        } else if (cmp > 0) {
            hi = mid;
        } else {
            result = mid;
            hi = mid; /* find first match */
        }
    }

    return result;
}

size_t flat_index_search_prefix(const FlatIndex *idx, const char *prefix) {
    if (!idx || !idx->entries || idx->count == 0 || !prefix)
        return (size_t)-1;

    size_t plen = strlen(prefix);
    if (plen == 0) return 0; /* empty prefix matches everything */

    size_t lo = 0, hi = idx->count;
    size_t result = (size_t)-1;

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int cmp = compare_prefix(idx->mmap_data, &idx->entries[mid], prefix, plen);
        if (cmp < 0) {
            lo = mid + 1;
        } else if (cmp > 0) {
            hi = mid;
        } else {
            result = mid;
            hi = mid; /* find first match */
        }
    }

    return result;
}

size_t flat_index_search_fts(const FlatIndex *idx, GRegex *regex, size_t start_pos) {
    if (!idx || !idx->entries || start_pos >= idx->count || !regex)
        return (size_t)-1;

    for (size_t i = start_pos; i < idx->count; i++) {
        const FlatTreeEntry *entry = &idx->entries[i];
        if (entry->d_len == 0) continue;

        const char *def = idx->mmap_data + entry->d_off;
        /* Match without string copy, bounding to definition length. */
        if (g_regex_match_full(regex, def, (gssize)entry->d_len, 0, 0, NULL, NULL)) {
            return i;
        }
    }

    return (size_t)-1;
}

const FlatTreeEntry* flat_index_get(const FlatIndex *idx, size_t pos) {
    if (!idx || !idx->entries || pos >= idx->count)
        return NULL;
    return &idx->entries[pos];
}

const FlatTreeEntry* flat_index_successor(const FlatIndex *idx, size_t pos) {
    if (!idx || !idx->entries || pos + 1 >= idx->count)
        return NULL;
    return &idx->entries[pos + 1];
}

const FlatTreeEntry* flat_index_random(const FlatIndex *idx) {
    if (!idx || !idx->entries || idx->count == 0)
        return NULL;
    size_t pos = (size_t)rand() % idx->count;
    return &idx->entries[pos];
}

size_t flat_index_count(const FlatIndex *idx) {
    if (!idx) return 0;
    return idx->count;
}

bool flat_index_validate(const FlatIndex *idx) {
    if (!idx || !idx->entries) return false;

    size_t data_region_end = idx->mmap_size - (idx->count * sizeof(FlatTreeEntry));

    for (size_t i = 0; i < idx->count; i++) {
        int64_t h_off = idx->entries[i].h_off;
        uint64_t h_len = idx->entries[i].h_len;
        int64_t d_off = idx->entries[i].d_off;
        uint64_t d_len = idx->entries[i].d_len;

        if (h_off < 8 || (uint64_t)h_off >= data_region_end) return false;
        if (d_off < 8 || (uint64_t)d_off >= data_region_end) return false;
        if ((uint64_t)h_off + h_len > data_region_end) return false;
        if ((uint64_t)d_off + d_len > data_region_end) return false;
    }

    return true;
}

void flat_index_sort_entries(FlatTreeEntry *entries, size_t count,
                             const char *data, size_t data_size) {
    (void)data_size;
    if (!entries || count == 0 || !data) return;
    sort_data_ptr = data;
    qsort(entries, count, sizeof(FlatTreeEntry), sort_compare);
    sort_data_ptr = NULL;
}
