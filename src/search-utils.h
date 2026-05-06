#pragma once

#include <glib.h>

typedef enum {
    SEARCH_BUCKET_EXACT = 0,
    SEARCH_BUCKET_SUFFIX,
    SEARCH_BUCKET_PREFIX,
    SEARCH_BUCKET_PHRASE,
    SEARCH_BUCKET_SUBSTRING,
    SEARCH_BUCKET_FUZZY
} SearchBucket;

char *sanitize_user_word(const char *value);
gboolean text_has_replacement_char(const char *text);
gboolean search_query_needs_literal_prefilter_bypass(const char *query);
char *normalize_headword_for_search(const char *value, gboolean unescape_dsl);
guint utf8_length_or_bytes(const char *text);
char *collapse_search_separators(const char *text);
gboolean classify_search_candidate_flexible(const char *query_key,
                                           guint query_len,
                                           const char *query_compact_key,
                                           guint query_compact_len,
                                           const char *candidate_key,
                                           SearchBucket *bucket_out,
                                           double *fuzzy_score_out);
