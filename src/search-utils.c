#include "search-utils.h"

#include <string.h>

char *sanitize_user_word(const char *value) {
    if (!value) {
        return NULL;
    }

    char *valid = g_utf8_make_valid(value, -1);
    char *text = g_strdup(valid);
    g_free(valid);
    g_strstrip(text);
    for (char *p = text; *p; p++) {
        if (*p == '\r' || *p == '\n' || *p == '\t') {
            *p = ' ';
        }
    }
    g_strstrip(text);

    if (!*text || strlen(text) > 256) {
        g_free(text);
        return NULL;
    }

    GString *clean = g_string_new("");
    for (const char *p = text; *p; p++) {
        if (g_ascii_isprint(*p) || g_ascii_isspace(*p) || ((unsigned char)*p >= 0x80)) {
            g_string_append_c(clean, *p);
        }
    }
    g_free(text);
    g_strstrip(clean->str);
    if (!*clean->str) {
        g_string_free(clean, TRUE);
        return NULL;
    }
    return g_string_free(clean, FALSE);
}

gboolean text_has_replacement_char(const char *text) {
    return text && strstr(text, "\xEF\xBF\xBD") != NULL;
}

static gboolean dsl_headword_is_escapable_char(char c) {
    return c != '\0' && strchr(" {}~\\@#()[]<>;", c) != NULL;
}

static size_t dsl_headword_brace_tag_len(const char *text) {
    static const char *patterns[] = {
        "{*}",
        "{·}",
        "{ˈ}",
        "{ˌ}",
        "{[']}",
        "{[/']}"
    };

    if (!text || text[0] != '{') {
        return 0;
    }

    for (guint i = 0; i < G_N_ELEMENTS(patterns); i++) {
        if (g_str_has_prefix(text, patterns[i])) {
            return strlen(patterns[i]);
        }
    }

    return 0;
}

gboolean search_query_needs_literal_prefilter_bypass(const char *query) {
    if (!query) {
        return FALSE;
    }

    for (const char *p = query; *p; p = g_utf8_next_char(p)) {
        gunichar ch = g_utf8_get_char(p);
        if (g_unichar_isspace(ch) || g_unichar_isalnum(ch)) {
            continue;
        }
        return TRUE;
    }

    return FALSE;
}

char *normalize_headword_for_search(const char *value, gboolean unescape_dsl) {
    if (!value) {
        return NULL;
    }

    char *valid = g_utf8_make_valid(value, -1);
    GString *out = g_string_new("");
    const char *p = valid;

    while (*p) {
        if (*p == '{') {
            size_t brace_tag_len = dsl_headword_brace_tag_len(p);
            if (brace_tag_len > 0) {
                p += brace_tag_len;
                continue;
            }
            if (unescape_dsl) {
                p++;
                continue;
            }
        }

        /* Raw DSL markers that should be ignored even outside of braces */
        if (*p == '*') { p++; continue; }

        /* UTF-8 middle dot (C2 B7) */
        if ((unsigned char)p[0] == 0xC2 && (unsigned char)p[1] == 0xB7) {
            p += 2;
            continue;
        }

        /* UTF-8 Stress marks (IPA CB 88, CB 8C) */
        if ((unsigned char)p[0] == 0xCB && ((unsigned char)p[1] == 0x88 || (unsigned char)p[1] == 0x8C)) {
            p += 2;
            continue;
        }

        /* DSL-specific square bracket tags in headwords (rare handles formatting) */
        if (g_str_has_prefix(p, "[']")) { p += 3; continue; }
        if (g_str_has_prefix(p, "[/']")) { p += 4; continue; }

        /* Strip actual Unicode combining acute accent (U+0301) for search */
        if (g_str_has_prefix(p, "\xCC\x81")) { p += 2; continue; }

        if (*p == '}' && unescape_dsl) {
            p++;
            continue;
        }



        if (*p == '\\' && p[1] != '\0') {
            if (unescape_dsl) {
                /* Only unescape DSL control escapes; preserve literal leet/backslash patterns. */
                if (dsl_headword_is_escapable_char(p[1])) {
                    const char *next = p + 1;
                    const char *next_end = g_utf8_next_char(next);
                    g_string_append_len(out, next, next_end - next);
                    p = next_end;
                } else {
                    /* Not special, keep the backslash */
                    g_string_append_c(out, '\\');
                    p++;
                }
            } else {
                /* Literal mode: keep everything as-is (e.g. from user search box) */
                g_string_append_c(out, '\\');
                p++;
            }
            continue;
        }

        if (g_ascii_isspace(*p)) {
            g_string_append_c(out, ' ');
            while (g_ascii_isspace(*p)) p++;
            continue;
        }

        const char *next = g_utf8_next_char(p);
        g_string_append_len(out, p, next - p);
        p = next;
    }

    char *normalized = g_string_free(out, FALSE);
    g_free(valid);

    char *trimmed = g_strstrip(normalized);
    if (!trimmed || *trimmed == '\0') {
        g_free(normalized);
        return NULL;
    }
    char *final = g_strdup(trimmed);
    g_free(normalized);
    return final;
}

guint utf8_length_or_bytes(const char *text) {
    if (!text || !*text) {
        return 0;
    }
    return (guint)g_utf8_strlen(text, -1);
}

static guint gestalt_longest_match(const gunichar *a, guint a_start, guint a_end,
                                   const gunichar *b, guint b_start, guint b_end,
                                   guint *out_a, guint *out_b) {
    guint max_len = 0;
    guint best_a = a_start;
    guint best_b = b_start;

    for (guint i = a_start; i < a_end; i++) {
        for (guint j = b_start; j < b_end; j++) {
            if (a[i] == b[j]) {
                guint len = 1;
                while (i + len < a_end && j + len < b_end && a[i + len] == b[j + len]) {
                    len++;
                }
                if (len > max_len) {
                    max_len = len;
                    best_a = i;
                    best_b = j;
                }
            }
        }
    }
    *out_a = best_a;
    *out_b = best_b;
    return max_len;
}

static guint gestalt_matches(const gunichar *a, guint a_start, guint a_end,
                             const gunichar *b, guint b_start, guint b_end) {
    if (a_start >= a_end || b_start >= b_end) {
        return 0;
    }
    guint out_a, out_b;
    guint match_len = gestalt_longest_match(a, a_start, a_end, b, b_start, b_end, &out_a, &out_b);
    if (match_len == 0) {
        return 0;
    }
    guint matches = match_len;
    matches += gestalt_matches(a, a_start, out_a, b, b_start, out_b);
    matches += gestalt_matches(a, out_a + match_len, a_end, b, out_b + match_len, b_end);
    return matches;
}

static double sequence_matcher_ratio(const char *str_a, const char *str_b) {
    if (!str_a || !str_b) return 0.0;
    
    glong len_a_chars = 0, len_b_chars = 0;
    gunichar *a = g_utf8_to_ucs4_fast(str_a, -1, &len_a_chars);
    gunichar *b = g_utf8_to_ucs4_fast(str_b, -1, &len_b_chars);
    
    if (!a || !b) {
        g_free(a);
        g_free(b);
        return 0.0;
    }
    
    if (len_a_chars == 0 && len_b_chars == 0) {
        g_free(a); g_free(b);
        return 1.0;
    }
    if (len_a_chars == 0 || len_b_chars == 0) {
        g_free(a); g_free(b);
        return 0.0;
    }
    
    guint matches = gestalt_matches(a, 0, len_a_chars, b, 0, len_b_chars);
    g_free(a);
    g_free(b);
    return (2.0 * matches) / (double)(len_a_chars + len_b_chars);
}

static const char *candidate_key_without_definite_article(const char *candidate_key) {
    if (!candidate_key) {
        return NULL;
    }

    if (g_str_has_prefix(candidate_key, "the ")) {
        return candidate_key + 4;
    }

    return NULL;
}

static int search_bucket_rank(SearchBucket bucket) {
    return (int)bucket;
}

char *collapse_search_separators(const char *text) {
    if (!text) {
        return NULL;
    }

    GString *out = g_string_sized_new(strlen(text));
    gboolean changed = FALSE;

    for (const char *p = text; *p; p = g_utf8_next_char(p)) {
        gunichar ch = g_utf8_get_char(p);
        if (g_unichar_isspace(ch) || ch == '-' || ch == '_' || ch == '/' || ch == '.' || ch == 0x00B7) {
            changed = TRUE;
            continue;
        }

        const char *next = g_utf8_next_char(p);
        g_string_append_len(out, p, next - p);
    }

    if (!changed) {
        g_string_free(out, TRUE);
        return NULL;
    }

    return g_string_free(out, FALSE);
}

static gboolean classify_search_candidate(const char *query_key,
                                          guint query_len,
                                          const char *candidate_key,
                                          SearchBucket *bucket_out,
                                          double *fuzzy_score_out) {
    if (!query_key || !candidate_key || !*candidate_key) {
        if (candidate_key && *candidate_key && query_key && !*query_key) {
            if (bucket_out) *bucket_out = SEARCH_BUCKET_PREFIX;
            if (fuzzy_score_out) *fuzzy_score_out = 0.0;
            return TRUE;
        }
        return FALSE;
    }

    /* 1. EXACT */
    if (g_strcmp0(candidate_key, query_key) == 0) {
        if (bucket_out) *bucket_out = SEARCH_BUCKET_EXACT;
        if (fuzzy_score_out) *fuzzy_score_out = 1.0;
        return TRUE;
    }

    /* 2. SUFFIX */
    if (g_str_has_suffix(candidate_key, query_key)) {
        if (bucket_out) *bucket_out = SEARCH_BUCKET_SUFFIX;
        if (fuzzy_score_out) *fuzzy_score_out = 0.0;
        return TRUE;
    }

    /* 3. PREFIX */
    if (g_str_has_prefix(candidate_key, query_key)) {
        if (bucket_out) *bucket_out = SEARCH_BUCKET_PREFIX;
        if (fuzzy_score_out) *fuzzy_score_out = 0.0;
        return TRUE;
    }

    /* 4. PHRASE / 5. SUBSTRING */
    const char *match = strstr(candidate_key, query_key);
    gboolean is_phrase = FALSE;
    gboolean is_substring = FALSE;

    if (match != NULL) {
        is_substring = TRUE;
        gsize qlen = strlen(query_key);
        const char *m = match;
        
        while (m != NULL) {
            char before = (m > candidate_key) ? *(m - 1) : '\0';
            char after = *(m + qlen);
            
            gboolean valid_before = (before == '\0' || before == ' ' || before == '-' || before == '_' || before == '/');
            gboolean valid_after = (after == '\0' || after == ' ' || after == '-' || after == '_' || after == '/');
            
            if (valid_before && valid_after) {
                is_phrase = TRUE;
                break;
            }
            m = strstr(m + 1, query_key);
        }
        
        if (is_phrase) {
            if (bucket_out) *bucket_out = SEARCH_BUCKET_PHRASE;
            if (fuzzy_score_out) *fuzzy_score_out = 0.0;
            return TRUE;
        }
    }

    /* 5. SUBSTRING */
    if (is_substring) {
        if (bucket_out) *bucket_out = SEARCH_BUCKET_SUBSTRING;
        if (fuzzy_score_out) *fuzzy_score_out = 0.0;
        return TRUE;
    }

    /* 6. FUZZY */
    if (query_len < 3) {
        return FALSE;
    }

    if (query_len >= 3 && strlen(candidate_key) > 32) {
        return FALSE; // skip expensive fuzzy early
    }

    guint candidate_len = utf8_length_or_bytes(candidate_key);
    guint length_delta = candidate_len > query_len ? candidate_len - query_len : query_len - candidate_len;
    if (length_delta > MAX(6U, query_len)) {
        return FALSE;
    }

    double ratio = sequence_matcher_ratio(query_key, candidate_key);
    double min_ratio = query_len >= 5 ? 0.74 : 0.78;
    if (ratio < min_ratio) {
        return FALSE;
    }

    if (bucket_out) *bucket_out = SEARCH_BUCKET_FUZZY;
    if (fuzzy_score_out) *fuzzy_score_out = ratio;
    return TRUE;
}

gboolean classify_search_candidate_flexible(const char *query_key,
                                                   guint query_len,
                                                   const char *query_compact_key,
                                                   guint query_compact_len,
                                                   const char *candidate_key,
                                                   SearchBucket *bucket_out,
                                                   double *fuzzy_score_out) {
    SearchBucket best_bucket = SEARCH_BUCKET_FUZZY;
    double best_score = 0.0;
    gboolean found = classify_search_candidate(query_key, query_len, candidate_key, &best_bucket, &best_score);

    const char *articleless = candidate_key_without_definite_article(candidate_key);
    if (articleless && *articleless) {
        SearchBucket alt_bucket = SEARCH_BUCKET_FUZZY;
        double alt_score = 0.0;
        gboolean alt_found = classify_search_candidate(query_key, query_len, articleless, &alt_bucket, &alt_score);
        if (alt_found &&
            (!found ||
             search_bucket_rank(alt_bucket) < search_bucket_rank(best_bucket) ||
             (search_bucket_rank(alt_bucket) == search_bucket_rank(best_bucket) && alt_score > best_score))) {
            best_bucket = alt_bucket;
            best_score = alt_score;
            found = TRUE;
        }
    }

    if (query_compact_key && *query_compact_key) {
        char *compact_candidate = collapse_search_separators(candidate_key);
        if (compact_candidate && *compact_candidate) {
            SearchBucket alt_bucket = SEARCH_BUCKET_FUZZY;
            double alt_score = 0.0;
            gboolean alt_found = classify_search_candidate(query_compact_key, query_compact_len, compact_candidate, &alt_bucket, &alt_score);
            if (alt_found &&
                (!found ||
                 search_bucket_rank(alt_bucket) < search_bucket_rank(best_bucket) ||
                 (search_bucket_rank(alt_bucket) == search_bucket_rank(best_bucket) && alt_score > best_score))) {
                best_bucket = alt_bucket;
                best_score = alt_score;
                found = TRUE;
            }

            const char *compact_articleless = candidate_key_without_definite_article(compact_candidate);
            if (compact_articleless && *compact_articleless) {
                alt_bucket = SEARCH_BUCKET_FUZZY;
                alt_score = 0.0;
                alt_found = classify_search_candidate(query_compact_key, query_compact_len, compact_articleless, &alt_bucket, &alt_score);
                if (alt_found &&
                    (!found ||
                     search_bucket_rank(alt_bucket) < search_bucket_rank(best_bucket) ||
                     (search_bucket_rank(alt_bucket) == search_bucket_rank(best_bucket) && alt_score > best_score))) {
                    best_bucket = alt_bucket;
                    best_score = alt_score;
                    found = TRUE;
                }
            }
        }
        g_free(compact_candidate);
    }

    if (found) {
        if (bucket_out) *bucket_out = best_bucket;
        if (fuzzy_score_out) *fuzzy_score_out = best_score;
    }

    return found;
}

