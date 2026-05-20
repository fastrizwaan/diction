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

    gunichar ch = g_utf8_get_char_validated(s, max_len);
    if (ch == (gunichar)-1 || ch == (gunichar)-2) return 1; /* Invalid UTF-8, skip 1 byte */

    size_t char_len = g_utf8_skip[*(unsigned char *)s];

    /* 1. Standard DSL tags like {*} or {·} */
    if (ch == '{') {
        size_t brace_tag_len = get_dsl_brace_tag_len(s, max_len);
        if (brace_tag_len > 0) return brace_tag_len;
        if (raw_side) return 1; /* Skip literal { in raw side if not a known tag */
    }
    if (raw_side && ch == '}') return 1;

    /* 2. Common DSL noise and Unicode diacritics */
    if (g_unichar_isspace(ch) || 
        ch == '*' || 
        ch == 0x00B7 || /* Middle dot */
        ch == 0x02C8 || ch == 0x02CC || /* Stress marks */
        ch == 0x2018 || ch == 0x2019 || /* Smart quotes */
        ch == 0x201C || ch == 0x201D ||
        ch == '(' || ch == ')' || ch == '[' || ch == ']' ||
        ch == '-' || ch == '\'' || ch == '`' || ch == '"' ||
        ch == ';' || ch == ':' || ch == '.' || ch == ',' ||
        ch == '!' || ch == '?' || ch == '_' || ch == '/' ||
        ch == '|' || ch == '~' ||
        g_unichar_type(ch) == G_UNICODE_NON_SPACING_MARK) {
        return char_len;
    }

    return 0;
}

static gunichar get_base_unichar(gunichar ch) {
    gunichar decomposed[8];
    if (g_unichar_fully_decompose(ch, FALSE, decomposed, 8) > 0) {
        return g_unichar_tolower(decomposed[0]);
    }
    return g_unichar_tolower(ch);
}

int compare_dsl_internal(const char *a, size_t la, bool a_raw,
                         const char *b, size_t lb, bool b_raw) {
    size_t i = 0, j = 0;
    size_t skip;

    while (i < la || j < lb) {
        while (i < la && (skip = get_dsl_ignored_len_ext(a + i, la - i, a_raw)) > 0) i += skip;
        while (j < lb && (skip = get_dsl_ignored_len_ext(b + j, lb - j, b_raw)) > 0) j += skip;

        if (i == la || j == lb) break;

        gunichar ch_a, ch_b;
        size_t len_a, len_b;

        if (a[i] == '\\' && i + 1 < la && dsl_headword_is_escapable_char(a[i + 1])) {
            i++;
            ch_a = g_utf8_get_char_validated(a + i, la - i);
            len_a = (ch_a != (gunichar)-1 && ch_a != (gunichar)-2) ? g_utf8_skip[*(unsigned char *)(a + i)] : 1;
            i += len_a;
        } else {
            ch_a = g_utf8_get_char_validated(a + i, la - i);
            len_a = (ch_a != (gunichar)-1 && ch_a != (gunichar)-2) ? g_utf8_skip[*(unsigned char *)(a + i)] : 1;
            i += len_a;
        }

        if (b[j] == '\\' && j + 1 < lb && dsl_headword_is_escapable_char(b[j + 1])) {
            j++;
            ch_b = g_utf8_get_char_validated(b + j, lb - j);
            len_b = (ch_b != (gunichar)-1 && ch_b != (gunichar)-2) ? g_utf8_skip[*(unsigned char *)(b + j)] : 1;
            j += len_b;
        } else {
            ch_b = g_utf8_get_char_validated(b + j, lb - j);
            len_b = (ch_b != (gunichar)-1 && ch_b != (gunichar)-2) ? g_utf8_skip[*(unsigned char *)(b + j)] : 1;
            j += len_b;
        }

        int diff = (int)get_base_unichar(ch_a) - (int)get_base_unichar(ch_b);
        if (diff != 0) return diff;
    }

    while (i < la && (skip = get_dsl_ignored_len_ext(a + i, la - i, a_raw)) > 0) i += skip;
    while (j < lb && (skip = get_dsl_ignored_len_ext(b + j, lb - j, b_raw)) > 0) j += skip;

    if (i == la && j == lb) return 0;
    return (i == la) ? -1 : 1;
}

int compare_dsl_agnostic(const char *raw, size_t raw_len, const char *clean, size_t clean_len) {
    return compare_dsl_internal(raw, raw_len, true, clean, clean_len, false);
}

static int compare_prefix_raw_segment(const char *raw, size_t raw_len,
                                      const char *prefix, size_t plen);
static gboolean raw_headword_matches_alias_segment(const char *raw, size_t raw_len,
                                                   const char *query, size_t qlen,
                                                   gboolean prefix_mode);

int compare_headword(const char *data, const FlatTreeEntry *entry,
                     const char *query, size_t qlen) {
    return compare_dsl_agnostic(data + entry->h_off, entry->h_len, query, qlen);
}

bool flat_index_entry_matches_query(const char *data, const FlatTreeEntry *entry,
                                    const char *query, size_t qlen) {
    if (!data || !entry || !query) return false;
    if (compare_dsl_agnostic(data + entry->h_off, entry->h_len, query, qlen) == 0) {
        return true;
    }
    return raw_headword_matches_alias_segment(data + entry->h_off, entry->h_len, query, qlen, FALSE);
}

bool flat_index_entry_matches_prefix(const char *data, const FlatTreeEntry *entry,
                                     const char *prefix, size_t plen) {
    if (!data || !entry || !prefix) return false;
    if (compare_prefix_raw_segment(data + entry->h_off, entry->h_len, prefix, plen) == 0) {
        return true;
    }
    return raw_headword_matches_alias_segment(data + entry->h_off, entry->h_len, prefix, plen, TRUE);
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

        gunichar ch_r, ch_c;
        size_t len_r, len_c;

        if (raw[r] == '\\' && r + 1 < raw_len && dsl_headword_is_escapable_char(raw[r + 1])) {
            r++;
            ch_r = g_utf8_get_char_validated(raw + r, raw_len - r);
            len_r = (ch_r != (gunichar)-1 && ch_r != (gunichar)-2) ? g_utf8_skip[*(unsigned char *)(raw + r)] : 1;
            r += len_r;
        } else {
            ch_r = g_utf8_get_char_validated(raw + r, raw_len - r);
            len_r = (ch_r != (gunichar)-1 && ch_r != (gunichar)-2) ? g_utf8_skip[*(unsigned char *)(raw + r)] : 1;
            r += len_r;
        }

        if (prefix[c] == '\\' && c + 1 < plen && dsl_headword_is_escapable_char(prefix[c + 1])) {
            c++;
            ch_c = g_utf8_get_char_validated(prefix + c, plen - c);
            len_c = (ch_c != (gunichar)-1 && ch_c != (gunichar)-2) ? g_utf8_skip[*(unsigned char *)(prefix + c)] : 1;
            c += len_c;
        } else {
            ch_c = g_utf8_get_char_validated(prefix + c, plen - c);
            len_c = (ch_c != (gunichar)-1 && ch_c != (gunichar)-2) ? g_utf8_skip[*(unsigned char *)(prefix + c)] : 1;
            c += len_c;
        }

        int diff = (int)get_base_unichar(ch_r) - (int)get_base_unichar(ch_c);
        if (diff != 0) return diff;
    }
    if (c == plen) return 0;
    return -1;
}

static int compare_prefix_raw_segment(const char *raw, size_t raw_len,
                                      const char *prefix, size_t plen) {
    size_t r = 0, c = 0;
    size_t skip;

    while (r < raw_len && c < plen) {
        while (r < raw_len && (skip = get_dsl_ignored_len_ext(raw + r, raw_len - r, true)) > 0) r += skip;
        while (c < plen && (skip = get_dsl_ignored_len_ext(prefix + c, plen - c, false)) > 0) c += skip;

        if (r == raw_len || c == plen) break;

        gunichar ch_r, ch_c;
        size_t len_r, len_c;

        if (raw[r] == '\\' && r + 1 < raw_len && dsl_headword_is_escapable_char(raw[r + 1])) {
            r++;
            ch_r = g_utf8_get_char_validated(raw + r, raw_len - r);
            len_r = (ch_r != (gunichar)-1 && ch_r != (gunichar)-2) ? g_utf8_skip[*(unsigned char *)(raw + r)] : 1;
            r += len_r;
        } else {
            ch_r = g_utf8_get_char_validated(raw + r, raw_len - r);
            len_r = (ch_r != (gunichar)-1 && ch_r != (gunichar)-2) ? g_utf8_skip[*(unsigned char *)(raw + r)] : 1;
            r += len_r;
        }

        if (prefix[c] == '\\' && c + 1 < plen && dsl_headword_is_escapable_char(prefix[c + 1])) {
            c++;
            ch_c = g_utf8_get_char_validated(prefix + c, plen - c);
            len_c = (ch_c != (gunichar)-1 && ch_c != (gunichar)-2) ? g_utf8_skip[*(unsigned char *)(prefix + c)] : 1;
            c += len_c;
        } else {
            ch_c = g_utf8_get_char_validated(prefix + c, plen - c);
            len_c = (ch_c != (gunichar)-1 && ch_c != (gunichar)-2) ? g_utf8_skip[*(unsigned char *)(prefix + c)] : 1;
            c += len_c;
        }

        int diff = (int)get_base_unichar(ch_r) - (int)get_base_unichar(ch_c);
        if (diff != 0) return diff;
    }

    if (c == plen) return 0;
    return -1;
}

static gboolean raw_headword_has_alias_separator(const char *raw, size_t raw_len) {
    if (!raw) return FALSE;

    for (size_t i = 0; i < raw_len; i++) {
        if (raw[i] == ';' && (i == 0 || raw[i - 1] != '\\')) {
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean raw_headword_matches_alias_segment(const char *raw, size_t raw_len,
                                                   const char *query, size_t qlen,
                                                   gboolean prefix_mode) {
    if (!raw || !query || qlen == 0 || !raw_headword_has_alias_separator(raw, raw_len)) {
        return FALSE;
    }

    size_t seg_start = 0;
    for (size_t i = 0; i <= raw_len; i++) {
        gboolean at_end = (i == raw_len);
        gboolean at_sep = (!at_end && raw[i] == ';' && (i == 0 || raw[i - 1] != '\\'));
        if (!at_end && !at_sep) {
            continue;
        }

        size_t seg_len = i - seg_start;
        if (seg_len > 0) {
            gboolean matched = prefix_mode
                ? (compare_prefix_raw_segment(raw + seg_start, seg_len, query, qlen) == 0)
                : (compare_dsl_agnostic(raw + seg_start, seg_len, query, qlen) == 0);
            if (matched) {
                return TRUE;
            }
        }

        seg_start = i + 1;
    }

    return FALSE;
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

    int res = compare_dsl_internal(ra, la, true, rb, lb, true);
    if (res != 0) return res;

    /* Tie-breaker: Case-sensitive exact comparison of the RAW bytes */
    size_t min_len = (la < lb) ? la : lb;
    int diff = strncmp(ra, rb, min_len);
    if (diff != 0) return diff;
    if (la < lb) return -1;
    if (la > lb) return 1;
    return 0;
}

/* ── public API ──────────────────────────────────────── */

#include "dict-chunked.h"

FlatIndex* flat_index_open(const char *data, size_t size) {
    if (!data || size < 8) return NULL;

    uint64_t count = 0;
    size_t index_off = 0;

    if (dict_cache_is_compressed(data, size)) {
        const DictCacheHeader *h = (const DictCacheHeader *)data;
        if (h->version != DICT_CACHE_VERSION) {
            /* Stale cache from a different format version — treat as empty
             * so the loader rebuilds it. */
            FlatIndex *idx = g_new0(FlatIndex, 1);
            if (!idx) return NULL;
            idx->entries = NULL;
            idx->count = 0;
            idx->mmap_data = data;
            idx->mmap_size = size;
            return idx;
        }
        count = h->entry_count;
        index_off = (size_t)h->index_off;
    } else {
        count = *(const uint64_t *)data;
        size_t index_size = (size_t)count * sizeof(FlatTreeEntry);
        if (count == 0 || index_size + 8 > size) {
            index_off = 0;
        } else {
            index_off = size - index_size;
        }
    }

    if (count == 0 || index_off == 0 || index_off >= size) {
        /* No index or invalid — return empty index */
        FlatIndex *idx = g_new0(FlatIndex, 1);
        if (!idx) return NULL;
        idx->entries = NULL;
        idx->count = 0;
        idx->mmap_data = data;
        idx->mmap_size = size;
        return idx;
    }

    const FlatTreeEntry *entries = (const FlatTreeEntry *)(data + index_off);

    FlatIndex *idx = g_new0(FlatIndex, 1);
    if (!idx) return NULL;
    idx->entries = entries;
    idx->count = (size_t)count;
    idx->mmap_data = data;
    idx->mmap_size = size;
    /* Pre-compute normalized search keys for fast memcmp-based binary search */
    flat_index_build_norm_cache(idx);
    return idx;
}

void flat_index_close(FlatIndex *idx) {
    if (idx) {
        g_free(idx->norm_keys_buf);
        g_free(idx->norm_keys);
    }
    g_free(idx); /* entries are mmap'd, not heap-allocated */
}

/* ── Normalized key helpers ────────────────────────────── */

/* Build a single normalized key from a raw headword.
 * Returns a newly allocated UTF-8 string (lowercase, decomposed, DSL noise stripped).
 * Caller must g_free() the result. *out_len is set to the byte length. */
static char *build_norm_key_for_raw(const char *raw, size_t raw_len, size_t *out_len) {
    GString *out = g_string_sized_new(raw_len);
    size_t i = 0;
    size_t skip;

    while (i < raw_len) {
        /* Skip DSL noise characters */
        skip = get_dsl_ignored_len_ext(raw + i, raw_len - i, true);
        if (skip > 0) { i += skip; continue; }

        /* Handle DSL escapes */
        if (raw[i] == '\\' && i + 1 < raw_len && dsl_headword_is_escapable_char(raw[i + 1])) {
            i++;
        }

        gunichar ch = g_utf8_get_char_validated(raw + i, raw_len - i);
        size_t char_len;
        if (ch == (gunichar)-1 || ch == (gunichar)-2) {
            char_len = 1;
            i += char_len;
            continue;
        }
        char_len = g_utf8_skip[*(unsigned char *)(raw + i)];
        i += char_len;

        /* Decompose to base and lowercase */
        gunichar base = get_base_unichar(ch);
        char utf8[6];
        int len = g_unichar_to_utf8(base, utf8);
        g_string_append_len(out, utf8, len);
    }

    *out_len = out->len;
    return g_string_free(out, FALSE);
}

void flat_index_build_norm_cache(FlatIndex *idx) {
    if (!idx || !idx->entries || idx->count == 0) return;
    if (idx->norm_keys) return; /* Already built */

    /* Pass 1: compute total buffer size needed */
    size_t total_key_bytes = 0;
    for (size_t i = 0; i < idx->count; i++) {
        /* Upper bound: each raw byte could produce at most 4 UTF-8 bytes after decompose.
         * But in practice, normalized keys are shorter than raw. Use raw length as estimate. */
        total_key_bytes += idx->entries[i].h_len + 4; /* +4 for safety margin */
    }

    idx->norm_keys = g_new(NormKey, idx->count);
    idx->norm_keys_buf = g_malloc(total_key_bytes);

    size_t buf_pos = 0;
    for (size_t i = 0; i < idx->count; i++) {
        const char *raw = idx->mmap_data + idx->entries[i].h_off;
        size_t raw_len = idx->entries[i].h_len;
        size_t key_len = 0;
        char *key = build_norm_key_for_raw(raw, raw_len, &key_len);

        /* Ensure we don't overflow */
        if (buf_pos + key_len > total_key_bytes) {
            total_key_bytes = buf_pos + key_len + idx->count * 4; /* grow */
            idx->norm_keys_buf = g_realloc(idx->norm_keys_buf, total_key_bytes);
        }

        memcpy(idx->norm_keys_buf + buf_pos, key, key_len);
        idx->norm_keys[i].off = (uint32_t)buf_pos;
        idx->norm_keys[i].len = (uint16_t)(key_len > 65535 ? 65535 : key_len);
        buf_pos += key_len;
        g_free(key);
    }

    /* Shrink buffer to actual size */
    if (buf_pos < total_key_bytes) {
        idx->norm_keys_buf = g_realloc(idx->norm_keys_buf, buf_pos > 0 ? buf_pos : 1);
    }
}

/* Fast comparison using pre-computed normalized keys.
 * Returns <0, 0, >0 like strcmp. */
static int compare_norm_key(const FlatIndex *idx, size_t entry_idx,
                            const char *query_norm, size_t query_norm_len) {
    const NormKey *nk = &idx->norm_keys[entry_idx];
    const char *entry_key = idx->norm_keys_buf + nk->off;
    size_t entry_len = nk->len;

    size_t min_len = entry_len < query_norm_len ? entry_len : query_norm_len;
    int cmp = memcmp(entry_key, query_norm, min_len);
    if (cmp != 0) return cmp;
    if (entry_len < query_norm_len) return -1;
    if (entry_len > query_norm_len) return 1;
    return 0;
}

/* Fast prefix comparison using pre-computed normalized keys. */
static int compare_norm_prefix(const FlatIndex *idx, size_t entry_idx,
                               const char *prefix_norm, size_t prefix_norm_len) {
    const NormKey *nk = &idx->norm_keys[entry_idx];
    const char *entry_key = idx->norm_keys_buf + nk->off;
    size_t entry_len = nk->len;

    size_t cmp_len = entry_len < prefix_norm_len ? entry_len : prefix_norm_len;
    int cmp = memcmp(entry_key, prefix_norm, cmp_len);
    if (cmp != 0) return cmp;
    /* If entry is shorter than prefix, it can't match */
    if (entry_len < prefix_norm_len) return -1;
    return 0; /* entry starts with prefix */
}

static size_t flat_index_search_impl(const FlatIndex *idx,
                                     const char *query,
                                     gboolean alias_fallback) {
    if (!idx || !idx->entries || idx->count == 0 || !query)
        return (size_t)-1;

    size_t qlen = strlen(query);
    size_t lo = 0, hi = idx->count;
    size_t result = (size_t)-1;

    /* Use fast norm-cache path if available */
    if (idx->norm_keys) {
        size_t norm_len = 0;
        char *query_norm = build_norm_key_for_raw(query, qlen, &norm_len);

        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            int cmp = compare_norm_key(idx, mid, query_norm, norm_len);
            if (cmp < 0) {
                lo = mid + 1;
            } else if (cmp > 0) {
                hi = mid;
            } else {
                result = mid;
                hi = mid; /* find first match */
            }
        }
        g_free(query_norm);
    } else {
        /* Fallback: original per-character comparison */
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
    }

    if (result == (size_t)-1 && alias_fallback) {
        for (size_t i = 0; i < idx->count; i++) {
            if (raw_headword_matches_alias_segment(idx->mmap_data + idx->entries[i].h_off,
                                                   idx->entries[i].h_len,
                                                   query, qlen, FALSE)) {
                return i;
            }
        }
    }

    return result;
}

size_t flat_index_search(const FlatIndex *idx, const char *query) {
    return flat_index_search_impl(idx, query, TRUE);
}

size_t flat_index_search_fast(const FlatIndex *idx, const char *query) {
    return flat_index_search_impl(idx, query, FALSE);
}

static size_t flat_index_search_prefix_impl(const FlatIndex *idx,
                                            const char *prefix,
                                            gboolean alias_fallback) {
    if (!idx || !idx->entries || idx->count == 0 || !prefix)
        return (size_t)-1;

    size_t plen = strlen(prefix);
    if (plen == 0) return 0; /* empty prefix matches everything */

    size_t lo = 0, hi = idx->count;
    size_t result = (size_t)-1;

    /* Use fast norm-cache path if available */
    if (idx->norm_keys) {
        size_t norm_len = 0;
        char *prefix_norm = build_norm_key_for_raw(prefix, plen, &norm_len);

        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            int cmp = compare_norm_prefix(idx, mid, prefix_norm, norm_len);
            if (cmp < 0) {
                lo = mid + 1;
            } else if (cmp > 0) {
                hi = mid;
            } else {
                result = mid;
                hi = mid; /* find first match */
            }
        }
        g_free(prefix_norm);
    } else {
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
    }

    if (result == (size_t)-1 && alias_fallback) {
        for (size_t i = 0; i < idx->count; i++) {
            if (raw_headword_matches_alias_segment(idx->mmap_data + idx->entries[i].h_off,
                                                   idx->entries[i].h_len,
                                                   prefix, plen, TRUE)) {
                return i;
            }
        }
    }

    return result;
}

size_t flat_index_search_prefix(const FlatIndex *idx, const char *prefix) {
    return flat_index_search_prefix_impl(idx, prefix, TRUE);
}

size_t flat_index_search_prefix_fast(const FlatIndex *idx, const char *prefix) {
    return flat_index_search_prefix_impl(idx, prefix, FALSE);
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

    gboolean is_comp = dict_cache_is_compressed(idx->mmap_data, idx->mmap_size);
    size_t headword_region_end = idx->mmap_size - (idx->count * sizeof(FlatTreeEntry));
    size_t data_region_end = headword_region_end;
    
    if (is_comp) {
        const DictCacheHeader *h = (const DictCacheHeader *)idx->mmap_data;
        headword_region_end = (size_t)(h->headwords_off + h->headwords_len);
    }

    if (idx->count == 0) return true;

    /* Validate structural boundaries first */
    size_t required_space = idx->count * sizeof(FlatTreeEntry);
    if (idx->mmap_size < required_space) return false;

    /* O(1) Validation: Check first, last, and a few sample entries to avoid page-faulting the entire index on startup. */
    size_t sample_indices[16];
    size_t num_samples = 0;
    
    sample_indices[num_samples++] = 0;
    if (idx->count > 1) {
        sample_indices[num_samples++] = idx->count - 1;
    }
    
    /* 10 random/middle samples */
    for (int s = 0; s < 10 && idx->count > 2; s++) {
        sample_indices[num_samples++] = 1 + (size_t)(rand() % (idx->count - 2));
    }

    for (size_t s = 0; s < num_samples; s++) {
        size_t i = sample_indices[s];
        uint32_t h_off = idx->entries[i].h_off;
        uint32_t h_len = idx->entries[i].h_len;
        uint32_t d_off = idx->entries[i].d_off;
        uint32_t d_len = idx->entries[i].d_len;

        if (h_off < 8 || (size_t)h_off >= headword_region_end) return false;
        if ((size_t)h_off + h_len > headword_region_end) return false;

        if (!is_comp) {
            if (d_off < 8 || (size_t)d_off >= data_region_end) return false;
            if ((size_t)d_off + d_len > data_region_end) return false;
        }
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
