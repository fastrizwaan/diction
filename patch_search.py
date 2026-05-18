import re
import sys

with open('src/main.c', 'r') as f:
    content = f.read()

# We need to replace the entire continue_sidebar_search and seed_search_sidebar_fast_rows functions.
# We will inject SearchBatchMsg, sidebar_search_idle_cb, and sidebar_search_thread_func.

new_impl = """
typedef struct {
    SidebarSearchState *state;
    GPtrArray *labels[BUCKET_COUNT];
    GPtrArray *payloads[BUCKET_COUNT];
    char *status_title;
    char *status_subtitle;
    gboolean is_append;
    gboolean is_finished;
} SearchBatchMsg;

static gboolean sidebar_search_idle_cb(gpointer user_data) {
    SearchBatchMsg *msg = user_data;
    if (msg->state && !g_atomic_int_get(&msg->state->cancelled)) {
        for (int i = 0; i < BUCKET_COUNT; i++) {
            if (msg->labels[i] && msg->labels[i]->len > 0) {
                if (!msg->state->list_started) {
                    set_related_rows(msg->labels[i], msg->payloads[i]);
                    msg->state->list_started = TRUE;
                } else {
                    append_related_rows(msg->labels[i], msg->payloads[i]);
                }
            }
        }
        if (msg->status_title) {
            populate_search_sidebar_status(msg->status_title, msg->status_subtitle);
        }
    }
    for (int i = 0; i < BUCKET_COUNT; i++) {
        if (msg->labels[i]) g_ptr_array_free(msg->labels[i], TRUE);
        if (msg->payloads[i]) g_ptr_array_free(msg->payloads[i], TRUE);
    }
    g_free(msg->status_title);
    g_free(msg->status_subtitle);
    if (msg->state) sidebar_search_state_unref(msg->state);
    g_free(msg);
    return G_SOURCE_REMOVE;
}

static gpointer sidebar_search_thread_func(gpointer user_data) {
    SidebarSearchState *state = user_data;
    if (!state) return NULL;

    guint processed = 0;
    const guint max_batch_size = 1000;
    gint64 next_dispatch_time = g_get_monotonic_time() + 50000; // Dispatch every 50ms at most

    // First, do the fast prefix search (formerly seed_search_sidebar_fast_rows)
    if (state->query && state->query_key && !state->is_fts) {
        SearchBatchMsg *seed_msg = g_new0(SearchBatchMsg, 1);
        seed_msg->state = sidebar_search_state_ref(state);
        seed_msg->labels[SEARCH_BUCKET_EXACT] = g_ptr_array_new_with_free_func(g_free);
        seed_msg->payloads[SEARCH_BUCKET_EXACT] = g_ptr_array_new_with_free_func((GDestroyNotify)related_row_payload_free);
        
        guint added = 0;
        const guint max_seed_rows = 512;
        
        for (guint idx = 0; state->search_entries && idx < state->search_entries->len && added < max_seed_rows; idx++) {
            if (g_atomic_int_get(&state->cancelled)) break;
            DictEntry *entry = g_ptr_array_index(state->search_entries, idx);
            size_t pos = flat_index_search_prefix(entry->dict->index, state->query);
            
            while (pos != (size_t)-1 && added < max_seed_rows) {
                if (g_atomic_int_get(&state->cancelled)) break;
                const FlatTreeEntry *node = flat_index_get(entry->dict->index, pos);
                if (!node) break;

                char *raw_word = g_strndup(entry->dict->data + node->h_off, node->h_len);
                char *clean_word = normalize_headword_for_search(raw_word, TRUE);

                if (!clean_word || text_has_replacement_char(clean_word)) {
                    g_free(raw_word);
                    if (clean_word) g_free(clean_word);
                    pos++;
                    if (pos >= flat_index_count(entry->dict->index)) break;
                    continue;
                }

                char *word_key = g_utf8_casefold(clean_word, -1);
                SearchBucket bucket;
                double score;

                if (classify_search_candidate_flexible(state->query_key, state->query_len,
                                                       state->query_compact_key, state->query_compact_len,
                                                       word_key, &bucket, &score)) {
                    if (bucket == SEARCH_BUCKET_EXACT || bucket == SEARCH_BUCKET_PREFIX) {
                        char *render_word = normalize_headword_for_render(raw_word, node->h_len, FALSE);
                        GPtrArray *raw_variants = split_headword_variants(raw_word);
                        GPtrArray *render_variants = split_headword_variants(render_word ? render_word : raw_word);

                        for (guint variant_idx = 0; variant_idx < raw_variants->len && added < max_seed_rows; variant_idx++) {
                            const char *raw_variant = g_ptr_array_index(raw_variants, variant_idx);
                            const char *display_variant = variant_idx < render_variants->len
                                ? g_ptr_array_index(render_variants, variant_idx)
                                : raw_variant;
                            char *clean_variant = normalize_headword_for_search(raw_variant, TRUE);
                            char *variant_key = g_utf8_casefold(clean_variant ? clean_variant : raw_variant, -1);
                            g_free(clean_variant);

                            if (!g_hash_table_contains(state->seen_words, variant_key)) {
                                RelatedRowPayload *payload = g_new0(RelatedRowPayload, 1);
                                payload->type = RELATED_ROW_CANDIDATE;
                                payload->word = g_strdup(raw_variant);
                                payload->sort_key = g_utf8_casefold(display_variant ? display_variant : raw_variant, -1);
                                payload->fuzzy_score = score;

                                g_hash_table_add(state->seen_words, g_strdup(variant_key));
                                g_ptr_array_add(seed_msg->labels[SEARCH_BUCKET_EXACT], g_strdup(display_variant));
                                g_ptr_array_add(seed_msg->payloads[SEARCH_BUCKET_EXACT], payload);
                                added++;
                            }
                            g_free(variant_key);
                        }

                        g_ptr_array_free(raw_variants, TRUE);
                        g_ptr_array_free(render_variants, TRUE);
                        g_free(render_word);
                    } else {
                        g_free(raw_word);
                        g_free(word_key);
                        if (clean_word) g_free(clean_word);
                        break;
                    }
                }

                g_free(raw_word);
                if (clean_word) g_free(clean_word);
                g_free(word_key);
                pos++;
                if (pos >= flat_index_count(entry->dict->index)) break;
            }
        }
        
        if (seed_msg->labels[SEARCH_BUCKET_EXACT]->len > 0) {
            g_idle_add(sidebar_search_idle_cb, seed_msg);
        } else {
            for (int i = 0; i < BUCKET_COUNT; i++) {
                if (seed_msg->labels[i]) g_ptr_array_free(seed_msg->labels[i], TRUE);
                if (seed_msg->payloads[i]) g_ptr_array_free(seed_msg->payloads[i], TRUE);
            }
            sidebar_search_state_unref(seed_msg->state);
            g_free(seed_msg);
        }
    }

    if (g_atomic_int_get(&state->cancelled)) {
        sidebar_search_state_unref(state);
        return NULL;
    }

    // Inform UI if searching
    SearchBatchMsg *status_msg = g_new0(SearchBatchMsg, 1);
    status_msg->state = sidebar_search_state_ref(state);
    if (state->is_fts) {
        if (state->fts_limited) {
            status_msg->status_subtitle = g_strdup_printf("Searching the first %u of %u dictionaries in this scope.",
                                             state->searched_dict_count,
                                             state->scoped_dict_count);
            status_msg->status_title = g_strdup("Full Text Search…");
        } else {
            status_msg->status_title = g_strdup("Full Text Search…");
        }
    } else {
        status_msg->status_title = g_strdup("Searching…");
    }
    g_idle_add(sidebar_search_idle_cb, status_msg);

    while (!g_atomic_int_get(&state->cancelled)) {
        if ((!state->search_entries || state->current_entry_index >= state->search_entries->len) &&
            !state->has_current_pos) {
            // END OF SEARCH - DO GLOBAL SORT & FLUSH
            SearchBatchMsg *final_msg = g_new0(SearchBatchMsg, 1);
            final_msg->state = sidebar_search_state_ref(state);
            final_msg->is_finished = TRUE;
            final_msg->is_append = TRUE;

            for (int i = 0; i < BUCKET_COUNT; i++) {
                guint n = state->global_bucket_labels[i]->len;
                if (n > 1) {
                    BucketItem *items = g_new(BucketItem, n);
                    for (guint j = 0; j < n; j++) {
                        RelatedRowPayload *row_payload = g_ptr_array_index(state->global_bucket_payloads[i], j);
                        items[j].label = g_ptr_array_index(state->global_bucket_labels[i], j);
                        items[j].sort_key = row_payload ? row_payload->sort_key : NULL;
                        items[j].payload = row_payload;
                        items[j].score = items[j].payload ? items[j].payload->fuzzy_score : 0.0;
                    }
                    g_sort_array(items, n, sizeof(BucketItem), compare_bucket_item, GINT_TO_POINTER(i));
                    for (guint j = 0; j < n; j++) {
                        g_ptr_array_index(state->global_bucket_labels[i], j) = items[j].label;
                        g_ptr_array_index(state->global_bucket_payloads[i], j) = items[j].payload;
                    }
                    g_free(items);
                }

                if (n > 0) {
                    final_msg->labels[i] = g_ptr_array_new_with_free_func(g_free);
                    final_msg->payloads[i] = g_ptr_array_new_with_free_func((GDestroyNotify)related_row_payload_free);
                    for (guint j = 0; j < n; j++) {
                        g_ptr_array_add(final_msg->labels[i], g_ptr_array_index(state->global_bucket_labels[i], j));
                        g_ptr_array_add(final_msg->payloads[i], g_ptr_array_index(state->global_bucket_payloads[i], j));
                    }
                    g_ptr_array_set_size(state->global_bucket_labels[i], 0);
                    g_ptr_array_set_size(state->global_bucket_payloads[i], 0);
                }
            }

            if (!state->list_started && g_hash_table_size(state->seen_words) == 0) {
                final_msg->status_title = g_strdup("No results");
                if (state->fts_limited) {
                    final_msg->status_subtitle = g_strdup_printf("Searched %u of %u dictionaries in this scope.",
                                                     state->searched_dict_count, state->scoped_dict_count);
                }
            }
            g_idle_add(sidebar_search_idle_cb, final_msg);
            break;
        }

        if (!state->has_current_pos) {
            gboolean found_dict = FALSE;
            while (state->search_entries && state->current_entry_index < state->search_entries->len) {
                DictEntry *entry = g_ptr_array_index(state->search_entries, state->current_entry_index++);
                state->current_pos = 0;
                state->has_current_pos = TRUE;
                state->current_dict_count = flat_index_count(entry->dict->index);
                if (state->current_dict) dict_entry_unref(state->current_dict);
                state->current_dict = entry;
                dict_entry_ref(state->current_dict);
                found_dict = TRUE;
                break;
            }
            if (!found_dict) continue;
        }

        if (!state->has_current_pos || !state->current_dict) {
            state->has_current_pos = FALSE;
            continue;
        }

        const FlatTreeEntry *node = NULL;
        if (state->is_fts) {
            if (!state->fts_regex) {
                state->has_current_pos = FALSE;
                continue;
            }
            size_t match_pos = dict_search_fts(state->current_dict->dict, state->current_dict->path,
                                               state->query, state->fts_regex, state->current_pos,
                                               app_settings && app_settings->fts_enabled);
            if (match_pos == (size_t)-1) {
                state->has_current_pos = FALSE;
                continue;
            }
            state->current_pos = match_pos;
            node = flat_index_get(state->current_dict->dict->index, state->current_pos);
            state->current_pos++;
            if (state->current_pos >= state->current_dict_count) state->has_current_pos = FALSE;
        } else {
            node = flat_index_get(state->current_dict->dict->index, state->current_pos);
            if (!node) {
                state->has_current_pos = FALSE;
                continue;
            }
            state->current_pos++;
            if (state->current_pos >= state->current_dict_count) state->has_current_pos = FALSE;

            if (!state->skip_fast_prefilter &&
                !fast_strncasestr(state->current_dict->dict->data + node->h_off, node->h_len, state->query)) {
                continue;
            }
        }

        char *word = g_strndup(state->current_dict->dict->data + node->h_off, node->h_len);
        char *clean_word = normalize_headword_for_search(word, TRUE);
        if (!clean_word || text_has_replacement_char(clean_word)) {
            g_free(word);
            g_free(clean_word);
            continue;
        }

        char *word_key = g_utf8_casefold(clean_word, -1);
        SearchBucket bucket;
        double fuzzy_score = 0.0;
        gboolean is_valid_match = FALSE;

        if (state->is_fts) {
            is_valid_match = TRUE;
            bucket = SEARCH_BUCKET_SUBSTRING;
            fuzzy_score = 1.0;
        } else {
            is_valid_match = classify_search_candidate_flexible(state->query_key, state->query_len,
                                                                state->query_compact_key, state->query_compact_len,
                                                                word_key, &bucket, &fuzzy_score);
        }

        if (is_valid_match) {
            char *render_word = normalize_headword_for_render(word, node->h_len, FALSE);
            GPtrArray *raw_variants = split_headword_variants(word);
            GPtrArray *render_variants = split_headword_variants(render_word ? render_word : word);

            for (guint variant_idx = 0; variant_idx < raw_variants->len; variant_idx++) {
                const char *raw_variant = g_ptr_array_index(raw_variants, variant_idx);
                const char *display_variant = variant_idx < render_variants->len
                    ? g_ptr_array_index(render_variants, variant_idx)
                    : raw_variant;
                char *clean_variant = normalize_headword_for_search(raw_variant, TRUE);
                char *variant_key = g_utf8_casefold(clean_variant ? clean_variant : raw_variant, -1);
                g_free(clean_variant);

                if (!g_hash_table_contains(state->seen_words, variant_key)) {
                    RelatedRowPayload *payload = g_new0(RelatedRowPayload, 1);
                    payload->type = RELATED_ROW_CANDIDATE;
                    payload->word = g_strdup(raw_variant);
                    payload->sort_key = g_utf8_casefold(display_variant ? display_variant : raw_variant, -1);
                    payload->fuzzy_score = fuzzy_score;

                    g_hash_table_add(state->seen_words, g_strdup(variant_key));
                    
                    int b = (int)bucket;
                    if (b >= 0 && b < BUCKET_COUNT) {
                        g_ptr_array_add(state->global_bucket_labels[b], g_strdup(display_variant));
                        g_ptr_array_add(state->global_bucket_payloads[b], payload);
                        processed++;
                    } else {
                        related_row_payload_free(payload);
                    }
                }
                g_free(variant_key);
            }
            g_ptr_array_free(raw_variants, TRUE);
            g_ptr_array_free(render_variants, TRUE);
            g_free(render_word);
        }
        
        g_free(word);
        if (clean_word) g_free(clean_word);
        g_free(word_key);

        if (processed >= max_batch_size || g_get_monotonic_time() >= next_dispatch_time) {
            if (processed > 0) {
                SearchBatchMsg *batch_msg = g_new0(SearchBatchMsg, 1);
                batch_msg->state = sidebar_search_state_ref(state);
                batch_msg->is_append = TRUE;
                for (int i = 0; i < BUCKET_COUNT; i++) {
                    guint n = state->global_bucket_labels[i]->len;
                    if (n > 0) {
                        batch_msg->labels[i] = g_ptr_array_new_with_free_func(g_free);
                        batch_msg->payloads[i] = g_ptr_array_new_with_free_func((GDestroyNotify)related_row_payload_free);
                        for (guint j = 0; j < n; j++) {
                            g_ptr_array_add(batch_msg->labels[i], g_ptr_array_index(state->global_bucket_labels[i], j));
                            g_ptr_array_add(batch_msg->payloads[i], g_ptr_array_index(state->global_bucket_payloads[i], j));
                        }
                        g_ptr_array_set_size(state->global_bucket_labels[i], 0);
                        g_ptr_array_set_size(state->global_bucket_payloads[i], 0);
                    }
                }
                g_idle_add(sidebar_search_idle_cb, batch_msg);
                processed = 0;
            }
            next_dispatch_time = g_get_monotonic_time() + 50000;
        }
    }

    sidebar_search_state_unref(state);
    return NULL;
}
"""

start_marker = "static gboolean continue_sidebar_search(gpointer user_data) {"
end_marker = "static void populate_search_sidebar_with_mode(const char *query, gboolean force_fts) {"

s_idx = content.find(start_marker)
e_idx = content.find(end_marker)

if s_idx == -1 or e_idx == -1:
    print("Could not find markers!")
    sys.exit(1)

new_content = content[:s_idx] + new_impl + "\n" + content[e_idx:]

with open('src/main.c', 'w') as f:
    f.write(new_content)

print("Patched continue_sidebar_search successfully")
