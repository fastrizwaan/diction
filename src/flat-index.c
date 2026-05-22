#include "flat-index.h"
#include "dict-cache.h"
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <glib.h>
#include <sqlite3.h>
#include <stdio.h>

static GMutex lazy_load_mutex;

/* ── Path helper ─────────────────────────────────────────── */

char* dict_hw_index_path_for(const char *dict_path)
{
    if (!dict_path) return NULL;
    char *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, dict_path, -1);
    const char *base = dict_cache_base_dir();
    char *path = g_build_filename(base, "diction", "hw", hash, NULL);
    g_free(hash);
    return path;
}


/* ── Headword normalisation helpers ──────────────────────── */

static gboolean is_escapable_char(char c) {
    return c != '\0' && strchr(" {}~\\@#()[]<>;", c) != NULL;
}

static size_t brace_tag_len(const char *s, size_t max_len) {
    static const char *pats[] = {"{*}","{·}","{ˈ}","{ˌ}","{[']}","{[/']}"};
    if (!s || max_len == 0 || s[0] != '{') return 0;
    for (guint i = 0; i < G_N_ELEMENTS(pats); i++) {
        size_t l = strlen(pats[i]);
        if (l <= max_len && memcmp(s, pats[i], l) == 0) return l;
    }
    return 0;
}

static size_t skip_dsl_noise(const char *s, size_t max_len, bool raw) {
    if (max_len == 0) return 0;
    gunichar ch = g_utf8_get_char_validated(s, max_len);
    if (ch == (gunichar)-1 || ch == (gunichar)-2) return 1;
    size_t cl = g_utf8_skip[*(unsigned char*)s];

    if (ch == '{') { size_t t = brace_tag_len(s, max_len); if (t) return t; if (raw) return 1; }
    if (raw && ch == '}') return 1;

    if (g_unichar_isspace(ch) || ch == '*' ||
        ch == 0x00B7 || ch == 0x02C8 || ch == 0x02CC ||
        ch == 0x2018 || ch == 0x2019 || ch == 0x201C || ch == 0x201D ||
        ch == '(' || ch == ')' || ch == '[' || ch == ']' ||
        ch == '-' || ch == '\'' || ch == '`' || ch == '"' ||
        ch == ';' || ch == ':' || ch == '.' || ch == ',' ||
        ch == '!' || ch == '?' || ch == '_' || ch == '/' ||
        ch == '|' || ch == '~' ||
        g_unichar_type(ch) == G_UNICODE_NON_SPACING_MARK)
        return cl;
    return 0;
}

static gunichar base_unichar(gunichar ch) {
    gunichar d[8];
    if (g_unichar_fully_decompose(ch, FALSE, d, 8) > 0)
        return g_unichar_tolower(d[0]);
    return g_unichar_tolower(ch);
}

int compare_dsl_internal(const char *a, size_t la, bool a_raw,
                         const char *b, size_t lb, bool b_raw)
{
    size_t i = 0, j = 0, sk;
    while (i < la || j < lb) {
        while (i < la && (sk = skip_dsl_noise(a+i, la-i, a_raw)) > 0) i += sk;
        while (j < lb && (sk = skip_dsl_noise(b+j, lb-j, b_raw)) > 0) j += sk;
        if (i == la || j == lb) break;

        gunichar ca, cb; size_t laa, lbb;
        if (a[i]=='\\' && i+1<la && is_escapable_char(a[i+1])) { i++; }
        ca = g_utf8_get_char_validated(a+i, la-i);
        laa = (ca!=(gunichar)-1 && ca!=(gunichar)-2) ? g_utf8_skip[*(unsigned char*)(a+i)] : 1; i += laa;

        if (b[j]=='\\' && j+1<lb && is_escapable_char(b[j+1])) { j++; }
        cb = g_utf8_get_char_validated(b+j, lb-j);
        lbb = (cb!=(gunichar)-1 && cb!=(gunichar)-2) ? g_utf8_skip[*(unsigned char*)(b+j)] : 1; j += lbb;

        int d = (int)base_unichar(ca) - (int)base_unichar(cb);
        if (d != 0) return d;
    }
    while (i < la && (sk = skip_dsl_noise(a+i, la-i, a_raw)) > 0) i += sk;
    while (j < lb && (sk = skip_dsl_noise(b+j, lb-j, b_raw)) > 0) j += sk;
    if (i == la && j == lb) return 0;
    return (i == la) ? -1 : 1;
}

int compare_dsl_agnostic(const char *raw, size_t raw_len,
                         const char *clean, size_t clean_len)
{
    return compare_dsl_internal(raw, raw_len, true, clean, clean_len, false);
}

int compare_headword(const char *data, const FlatTreeEntry *e,
                     const char *query, size_t qlen)
{
    return compare_dsl_agnostic(data + e->h_off, e->h_len, query, qlen);
}

void build_norm_key_gstring(const char *raw, size_t raw_len, GString *o)
{
    g_string_truncate(o, 0);
    size_t i = 0, sk;
    while (i < raw_len) {
        sk = skip_dsl_noise(raw+i, raw_len-i, true);
        if (sk > 0) { i += sk; continue; }
        if (raw[i]=='\\' && i+1<raw_len && is_escapable_char(raw[i+1])) i++;
        gunichar ch = g_utf8_get_char_validated(raw+i, raw_len-i);
        size_t cl;
        if (ch == (gunichar)-1 || ch == (gunichar)-2) { cl=1; i+=cl; continue; }
        cl = g_utf8_skip[*(unsigned char*)(raw+i)]; i += cl;
        gunichar base = base_unichar(ch);
        char utf[6]; int l = g_unichar_to_utf8(base, utf);
        g_string_append_len(o, utf, l);
    }
}

char* build_norm_key(const char *raw, size_t raw_len, size_t *out_len)
{
    GString *o = g_string_sized_new(raw_len);
    build_norm_key_gstring(raw, raw_len, o);
    *out_len = o->len;
    return g_string_free(o, FALSE);
}


/* ── Alias matching ──────────────────────────────────────── */

static bool has_alias_sep(const char *raw, size_t len) {
    for (size_t i = 0; i < len; i++)
        if (raw[i]==';' && (i==0 || raw[i-1]!='\\')) return true;
    return false;
}

/* Prefix comparison between a raw headword segment and a clean query.
 * Returns 0 if segment starts with query (ignoring DSL noise, case, diacritics). */
static int compare_prefix_raw_segment(const char *raw, size_t raw_len,
                                       const char *prefix, size_t plen)
{
    size_t r = 0, p = 0, sk;
    while (r < raw_len && p < plen) {
        while (r < raw_len && (sk = skip_dsl_noise(raw+r, raw_len-r, true)) > 0) r += sk;
        while (p < plen && (sk = skip_dsl_noise(prefix+p, plen-p, false)) > 0) p += sk;
        if (r == raw_len || p == plen) break;

        gunichar cr, cp; size_t lr, lp;
        if (raw[r]=='\\' && r+1<raw_len && is_escapable_char(raw[r+1])) { r++; }
        cr = g_utf8_get_char_validated(raw+r, raw_len-r);
        lr = (cr!=(gunichar)-1 && cr!=(gunichar)-2) ? g_utf8_skip[*(unsigned char*)(raw+r)] : 1; r += lr;

        if (prefix[p]=='\\' && p+1<plen && is_escapable_char(prefix[p+1])) { p++; }
        cp = g_utf8_get_char_validated(prefix+p, plen-p);
        lp = (cp!=(gunichar)-1 && cp!=(gunichar)-2) ? g_utf8_skip[*(unsigned char*)(prefix+p)] : 1; p += lp;

        int d = (int)base_unichar(cr) - (int)base_unichar(cp);
        if (d != 0) return d;
    }
    if (p == plen) return 0;   /* prefix fully consumed → match */
    return -1;                  /* raw exhausted before prefix → no match */
}

/* Check if any alias segment matches the query (exact or prefix). */
static bool raw_headword_matches_alias_segment(const char *raw, size_t raw_len,
                                                const char *query, size_t qlen,
                                                bool prefix_mode)
{
    if (!raw || !query || qlen == 0 || !has_alias_sep(raw, raw_len))
        return false;

    size_t seg_start = 0;
    for (size_t i = 0; i <= raw_len; i++) {
        bool at_end = (i == raw_len);
        bool at_sep = (!at_end && raw[i] == ';' && (i == 0 || raw[i-1] != '\\'));
        if (!at_end && !at_sep) continue;
        size_t seg_len = i - seg_start;
        if (seg_len > 0) {
            bool matched = prefix_mode
                ? (compare_prefix_raw_segment(raw + seg_start, seg_len, query, qlen) == 0)
                : (compare_dsl_agnostic(raw + seg_start, seg_len, query, qlen) == 0);
            if (matched) return true;
        }
        seg_start = i + 1;
    }
    return false;
}

bool flat_index_entry_matches_query(const char *data, const FlatTreeEntry *e,
                                    const char *query, size_t qlen)
{
    if (!data || !e || !query) return false;
    if (compare_dsl_agnostic(data+e->h_off, e->h_len, query, qlen) == 0) return true;
    return raw_headword_matches_alias_segment(data+e->h_off, e->h_len, query, qlen, false);
}

bool flat_index_entry_matches_prefix(const char *data, const FlatTreeEntry *e,
                                     const char *prefix, size_t plen)
{
    if (!data || !e || !prefix) return false;
    return compare_prefix_raw_segment(data+e->h_off, e->h_len, prefix, plen) == 0 ||
           raw_headword_matches_alias_segment(data+e->h_off, e->h_len, prefix, plen, true);
}


/* ── SQLite PRAGMA setup ─────────────────────────────────── */

static void apply_pragmas(sqlite3 *db)
{
    sqlite3_exec(db,
        "PRAGMA journal_mode = WAL;"
        "PRAGMA synchronous  = OFF;"
        "PRAGMA cache_size   = -16384;" /* 16MB cache */
        "PRAGMA temp_store   = MEMORY;"
        "PRAGMA mmap_size    = 536870912;" /* 512MB mmap */
        "PRAGMA query_only   = ON;",
        NULL, NULL, NULL);
}


/* ── Public API ──────────────────────────────────────────── */

static void flat_index_load_data(FlatIndex *idx) {
    g_mutex_lock(&lazy_load_mutex);
    if (idx->is_loaded) {
        g_mutex_unlock(&lazy_load_mutex);
        return;
    }

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(idx->db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        g_mutex_unlock(&lazy_load_mutex);
        return;
    }
    apply_pragmas(db);

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
        "SELECT headword, d_off, d_len, normalized FROM entries "
        "ORDER BY normalized;",
        -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        g_mutex_unlock(&lazy_load_mutex);
        return;
    }

    size_t n = idx->count;
    idx->entries    = g_new(FlatTreeEntry, n);
    idx->sorted_ids = g_new(guint32, n);
    idx->norm_keys  = g_new(NormKey, n);

    /* Accumulate headwords and norm keys in dynamic buffers */
    size_t hw_cap = n * 64, hw_len = 0;
    char  *hw_buf = g_malloc(hw_cap);
    size_t nm_cap = n * 64, nm_len = 0;
    char  *nm_buf = g_malloc(nm_cap);

    size_t pos = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char  *hw_text  = (const char*)sqlite3_column_text(st, 0);
        int          hw_sz    = sqlite3_column_bytes(st, 0);
        int64_t      d_off    = sqlite3_column_int64(st, 1);
        int64_t      d_len    = sqlite3_column_int64(st, 2);
        const char  *nm_text  = (const char*)sqlite3_column_text(st, 3);
        int          nm_sz    = sqlite3_column_bytes(st, 3);

        idx->entries[pos].h_off = (uint32_t)hw_len;
        idx->entries[pos].h_len = (uint32_t)hw_sz;

        if (hw_len + hw_sz + 1 > hw_cap) {
            hw_cap = hw_len + hw_sz + 1 + n * 64;
            hw_buf = g_realloc(hw_buf, hw_cap);
        }
        memcpy(hw_buf + hw_len, hw_text, hw_sz);
        hw_len += hw_sz;
        hw_buf[hw_len++] = '\n';

        idx->entries[pos].d_off = (uint32_t)d_off;
        idx->entries[pos].d_len = (uint32_t)d_len;

        idx->norm_keys[pos].off = (uint32_t)nm_len;
        idx->norm_keys[pos].len = (uint16_t)(nm_sz > 65535 ? 65535 : nm_sz);

        if (nm_len + nm_sz > nm_cap) {
            nm_cap = nm_len + nm_sz + n * 64;
            nm_buf = g_realloc(nm_buf, nm_cap);
        }
        memcpy(nm_buf + nm_len, nm_text, nm_sz);
        nm_len += nm_sz;

        idx->sorted_ids[pos] = (guint32)pos;
        pos++;
    }
    sqlite3_finalize(st);
    sqlite3_close(db);

    idx->buf_size = hw_len;
    idx->norm_buf = nm_buf;
    idx->headword_buf = hw_buf;
    idx->is_loaded = TRUE;

    g_mutex_unlock(&lazy_load_mutex);
}

static inline void flat_index_ensure_loaded(FlatIndex *idx) {
    if (idx && !idx->is_loaded && idx->count > 0) {
        flat_index_load_data(idx);
    }
}

FlatIndex* flat_index_open(const char *db_path)
{
    if (!db_path) return NULL;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    apply_pragmas(db);

    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM entries;", -1, &st, NULL);
    if (sqlite3_step(st) != SQLITE_ROW) {
        sqlite3_finalize(st); sqlite3_close(db); return NULL;
    }
    size_t n = (size_t)sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);

    FlatIndex *idx = g_new0(FlatIndex, 1);
    idx->count   = n;
    idx->db_path = g_strdup(db_path);
    idx->metadata = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    /* Read metadata */
    {
        sqlite3_stmt *mst = NULL;
        if (sqlite3_prepare_v2(db, "SELECT key, value FROM metadata;", -1, &mst, NULL) == SQLITE_OK) {
            while (sqlite3_step(mst) == SQLITE_ROW) {
                const char *key = (const char*)sqlite3_column_text(mst, 0);
                const char *val = (const char*)sqlite3_column_text(mst, 1);
                if (key && val) {
                    g_hash_table_insert(idx->metadata, g_strdup(key), g_strdup(val));
                }
            }
            sqlite3_finalize(mst);
        }
    }

    sqlite3_close(db);
    return idx;
}

const char* flat_index_get_metadata(const FlatIndex *idx, const char *key) {
    if (!idx || !idx->metadata || !key) return NULL;
    return g_hash_table_lookup(idx->metadata, key);
}

void flat_index_close(FlatIndex *idx)
{
    if (!idx) return;
    if (idx->metadata) g_hash_table_unref(idx->metadata);
    g_free(idx->entries);
    g_free(idx->headword_buf);
    g_free(idx->sorted_ids);
    g_free(idx->norm_buf);
    g_free(idx->norm_keys);
    g_free(idx->db_path);
    g_free(idx);
}

/* Binary search helpers */

static inline const char* entry_norm(const FlatIndex *idx, size_t i, size_t *len) {
    flat_index_ensure_loaded((FlatIndex*)idx);
    *len = idx->norm_keys[i].len;
    return idx->norm_buf + idx->norm_keys[i].off;
}

static int cmp_norm(const FlatIndex *idx, size_t i,
                    const char *q, size_t ql) {
    flat_index_ensure_loaded((FlatIndex*)idx);
    size_t el; const char *e = entry_norm(idx, i, &el);
    size_t ml = el < ql ? el : ql;
    int d = memcmp(e, q, ml);
    if (d) return d;
    if (el < ql) return -1;
    if (el > ql) return  1;
    return 0;
}

static int cmp_norm_prefix(const FlatIndex *idx, size_t i,
                           const char *p, size_t pl) {
    flat_index_ensure_loaded((FlatIndex*)idx);
    size_t el; const char *e = entry_norm(idx, i, &el);
    size_t ml = el < pl ? el : pl;
    int d = memcmp(e, p, ml);
    if (d) return d;
    if (el < pl) return -1;
    return 0;
}

static size_t search_exact(const FlatIndex *idx, const char *query)
{
    flat_index_ensure_loaded((FlatIndex*)idx);
    size_t ql = strlen(query);
    size_t nml = 0;
    char *nm = build_norm_key(query, ql, &nml);

    size_t lo = 0, hi = idx->count, res = (size_t)-1;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cmp_norm(idx, mid, nm, nml);
        if (c < 0)      lo = mid + 1;
        else if (c > 0) hi = mid;
        else            { res = mid; hi = mid; }
    }
    g_free(nm);
    return res;
}

static size_t search_prefix(const FlatIndex *idx, const char *prefix)
{
    flat_index_ensure_loaded((FlatIndex*)idx);
    size_t pl = strlen(prefix);
    if (pl == 0) return 0;
    size_t nml = 0;
    char *nm = build_norm_key(prefix, pl, &nml);

    size_t lo = 0, hi = idx->count, res = (size_t)-1;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = cmp_norm_prefix(idx, mid, nm, nml);
        if (c < 0)      lo = mid + 1;
        else if (c > 0) hi = mid;
        else            { res = mid; hi = mid; }
    }
    g_free(nm);
    return res;
}

size_t flat_index_search(const FlatIndex *idx, const char *query)
{
    if (!idx || idx->count == 0 || !query)
        return (size_t)-1;
    flat_index_ensure_loaded((FlatIndex*)idx);
    size_t r = search_exact(idx, query);
    if (r != (size_t)-1) return r;
    size_t ql = strlen(query);
    for (size_t i = 0; i < idx->count; i++)
        if (raw_headword_matches_alias_segment(
                idx->headword_buf + idx->entries[i].h_off,
                idx->entries[i].h_len, query, ql, false))
            return i;
    return (size_t)-1;
}

size_t flat_index_search_fast(const FlatIndex *idx, const char *query)
{
    if (!idx || idx->count == 0 || !query)
        return (size_t)-1;
    flat_index_ensure_loaded((FlatIndex*)idx);
    return search_exact(idx, query);
}

size_t flat_index_search_prefix(const FlatIndex *idx, const char *prefix)
{
    if (!idx || idx->count == 0 || !prefix)
        return (size_t)-1;
    flat_index_ensure_loaded((FlatIndex*)idx);
    size_t r = search_prefix(idx, prefix);
    if (r != (size_t)-1) return r;
    size_t pl = strlen(prefix);
    for (size_t i = 0; i < idx->count; i++)
        if (raw_headword_matches_alias_segment(
                idx->headword_buf + idx->entries[i].h_off,
                idx->entries[i].h_len, prefix, pl, true))
            return i;
    return (size_t)-1;
}

size_t flat_index_search_prefix_fast(const FlatIndex *idx, const char *prefix)
{
    if (!idx || idx->count == 0 || !prefix)
        return (size_t)-1;
    flat_index_ensure_loaded((FlatIndex*)idx);
    return search_prefix(idx, prefix);
}

const FlatTreeEntry* flat_index_get(const FlatIndex *idx, size_t pos)
{
    if (!idx || pos >= idx->count) return NULL;
    flat_index_ensure_loaded((FlatIndex*)idx);
    return &idx->entries[pos];
}

const FlatTreeEntry* flat_index_successor(const FlatIndex *idx, size_t pos)
{
    if (!idx || pos + 1 >= idx->count) return NULL;
    flat_index_ensure_loaded((FlatIndex*)idx);
    return &idx->entries[pos + 1];
}

const FlatTreeEntry* flat_index_random(const FlatIndex *idx)
{
    if (!idx || idx->count == 0) return NULL;
    flat_index_ensure_loaded((FlatIndex*)idx);
    return &idx->entries[(size_t)rand() % idx->count];
}

size_t flat_index_count(const FlatIndex *idx)
{
    return idx ? idx->count : 0;
}

bool flat_index_validate(const FlatIndex *idx)
{
    if (!idx) return false;
    flat_index_ensure_loaded((FlatIndex*)idx);
    if (!idx->entries) return false;
    if (idx->count == 0) return true;
    size_t s[] = {0, idx->count > 1 ? idx->count - 1 : 0};
    size_t ns = idx->count > 1 ? 2 : 1;
    for (size_t k = 0; k < ns; k++) {
        size_t i = s[k];
        if ((size_t)idx->entries[i].h_off >= idx->buf_size) return false;
        if ((size_t)idx->entries[i].h_off + idx->entries[i].h_len > idx->buf_size) return false;
    }
    return true;
}
