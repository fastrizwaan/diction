#include <gtk/gtk.h>
#include <adwaita.h>
#include <webkit/webkit.h>
#include <sys/stat.h>
#include <time.h>
#include "dict-mmap.h"
#include "dict-loader.h"
#include "dict-render.h"
#include "settings.h"

static DictEntry *all_dicts = NULL;
static DictEntry *active_entry = NULL;
static WebKitWebView *web_view = NULL;
static GtkListBox *dict_listbox = NULL;
static AdwStyleManager *style_manager = NULL;
static GtkSearchEntry *search_entry = NULL;
static char *last_search_query = NULL;
static AppSettings *app_settings = NULL;
static GtkListBox *history_listbox = NULL;
static GtkListBox *related_listbox = NULL;
static GtkListBox *favorites_listbox = NULL;
static GtkListBox *groups_listbox = NULL;
static GtkWidget *favorite_toggle_btn = NULL;
static GPtrArray *history_words = NULL;
static GPtrArray *favorite_words = NULL;
static char *active_scope_id = NULL;
static guint search_execute_source_id = 0;

#define HISTORY_FILE_NAME "history.json"
#define FAVORITES_FILE_NAME "favorites.json"
#define SEARCH_SIDEBAR_MIN_BROAD_QUERY_LEN 3

static void populate_dict_sidebar(void);      // forward declaration
static void start_async_dict_loading(void);   // forward declaration
static void on_search_changed(GtkSearchEntry *entry, gpointer user_data); // forward declaration
static void on_random_clicked(GtkButton *btn, gpointer user_data);
static void refresh_search_results(void);
static void populate_search_sidebar(const char *query);
static void execute_search_now(void);

typedef struct {
    char *query;
    char *query_key;
    guint query_len;
    GHashTable *best;
    DictEntry *current_entry;
    DictEntry *current_dict;
    SplayNode *current_node;
    guint dict_rank;
    guint source_id;
} SidebarSearchState;

static SidebarSearchState *sidebar_search_state = NULL;

static gboolean spawn_audio_argv(const char *const *argv, const char *label) {
    GError *error = NULL;
    gboolean ok = g_spawn_async(NULL,
                                (char **)argv,
                                NULL,
                                G_SPAWN_SEARCH_PATH |
                                G_SPAWN_STDOUT_TO_DEV_NULL |
                                G_SPAWN_STDERR_TO_DEV_NULL,
                                NULL,
                                NULL,
                                NULL,
                                &error);
    if (ok) {
        fprintf(stderr, "[AUDIO PLAY] Playing with '%s'...\n", label);
        return TRUE;
    }

    g_clear_error(&error);
    return FALSE;
}

static gboolean spawn_audio_shell_command(const char *command, const char *label) {
    const char *argv[] = { "/bin/sh", "-c", command, NULL };
    return spawn_audio_argv(argv, label);
}

static gboolean path_has_extension(const char *path, const char *ext) {
    const char *dot = strrchr(path, '.');
    return dot && g_ascii_strcasecmp(dot, ext) == 0;
}

static gboolean looks_like_url(const char *path) {
    return path && (g_str_has_prefix(path, "http://") || g_str_has_prefix(path, "https://"));
}

static gboolean play_audio_via_pcm_pipeline(const char *audio_path) {
    if (!g_find_program_in_path("ffmpeg")) {
        return FALSE;
    }

    char *quoted = g_shell_quote(audio_path);
    gboolean ok = FALSE;

    if (g_find_program_in_path("pw-play")) {
        char *cmd = g_strdup_printf(
            "ffmpeg -nostdin -loglevel error -i %s -f s16le -acodec pcm_s16le -ac 2 -ar 48000 - | "
            "pw-play --raw --format s16 --channels 2 --rate 48000 -",
            quoted);
        ok = spawn_audio_shell_command(cmd, "ffmpeg | pw-play");
        g_free(cmd);
    } else if (g_find_program_in_path("aplay")) {
        char *cmd = g_strdup_printf(
            "ffmpeg -nostdin -loglevel error -i %s -f s16le -acodec pcm_s16le -ac 2 -ar 48000 - | "
            "aplay -q -f S16_LE -c 2 -r 48000 -",
            quoted);
        ok = spawn_audio_shell_command(cmd, "ffmpeg | aplay");
        g_free(cmd);
    }

    g_free(quoted);
    return ok;
}

static void play_audio_file(const char *audio_path) {
    fprintf(stderr, "[AUDIO PLAY] Attempting to play: %s\n", audio_path);

    if (!looks_like_url(audio_path) &&
        (path_has_extension(audio_path, ".spx") ||
        path_has_extension(audio_path, ".ogg") ||
        path_has_extension(audio_path, ".oga"))) {
        if (play_audio_via_pcm_pipeline(audio_path)) {
            return;
        }
    }

    if (g_find_program_in_path("ffplay")) {
        const char *argv[] = { "ffplay", "-nodisp", "-autoexit", "-loglevel", "quiet", audio_path, NULL };
        if (spawn_audio_argv(argv, "ffplay")) {
            return;
        }
    }

    if (g_find_program_in_path("mpg123")) {
        const char *argv[] = { "mpg123", "-q", audio_path, NULL };
        if (spawn_audio_argv(argv, "mpg123")) {
            return;
        }
    }

    if (g_find_program_in_path("play")) {
        const char *argv[] = { "play", "-q", audio_path, NULL };
        if (spawn_audio_argv(argv, "play")) {
            return;
        }
    }

    if (g_find_program_in_path("paplay")) {
        const char *argv[] = { "paplay", audio_path, NULL };
        if (spawn_audio_argv(argv, "paplay")) {
            return;
        }
    }

    fprintf(stderr, "[AUDIO ERROR] No usable audio player found\n");
}

static char *query_param_dup(const char *query, const char *key) {
    if (!query || !key) {
        return NULL;
    }

    char **pairs = g_strsplit(query, "&", -1);
    char *value = NULL;

    for (int i = 0; pairs[i]; i++) {
        char *eq = strchr(pairs[i], '=');
        if (!eq) {
            continue;
        }

        *eq = '\0';
        if (strcmp(pairs[i], key) == 0) {
            value = g_uri_unescape_string(eq + 1, NULL);
            *eq = '=';
            break;
        }
        *eq = '=';
    }

    g_strfreev(pairs);
    return value;
}

static gboolean try_play_encoded_sound_uri(const char *uri) {
    const char *query = strchr(uri, '?');
    if (!query) {
        return FALSE;
    }

    char *audio_url = query_param_dup(query + 1, "url");
    char *resource_dir = query_param_dup(query + 1, "dir");
    char *sound_file = query_param_dup(query + 1, "file");

    if (audio_url && *audio_url) {
        fprintf(stderr, "[AUDIO CLICKED] URL: %s\n", audio_url);
        play_audio_file(audio_url);
        g_free(audio_url);
        g_free(resource_dir);
        g_free(sound_file);
        return TRUE;
    }

    g_free(audio_url);

    if (!resource_dir || !sound_file) {
        g_free(resource_dir);
        g_free(sound_file);
        return FALSE;
    }

    fprintf(stderr, "[AUDIO CLICKED] Resource dir: %s\n", resource_dir);
    fprintf(stderr, "[AUDIO CLICKED] File: %s\n", sound_file);

    char *audio_path = g_build_filename(resource_dir, sound_file, NULL);
    if (g_file_test(audio_path, G_FILE_TEST_EXISTS)) {
        play_audio_file(audio_path);
    } else {
        fprintf(stderr, "[AUDIO ERROR] File not found: %s\n", audio_path);
    }

    g_free(audio_path);
    g_free(resource_dir);
    g_free(sound_file);
    return TRUE;
}

static char *sanitize_user_word(const char *value) {
    if (!value) {
        return NULL;
    }

    char *text = g_strdup(value);
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

typedef enum {
    SEARCH_BUCKET_EXACT = 0,
    SEARCH_BUCKET_SUFFIX,
    SEARCH_BUCKET_PREFIX,
    SEARCH_BUCKET_SUBSTRING,
    SEARCH_BUCKET_FUZZY
} SearchBucket;

typedef struct {
    char *word;
    char *word_key;
    char *dict_name;
    guint dict_rank;
    SearchBucket bucket;
    double fuzzy_score;
    guint length_delta;
} SearchCandidate;

static void search_candidate_free(SearchCandidate *candidate) {
    if (!candidate) {
        return;
    }
    g_free(candidate->word);
    g_free(candidate->word_key);
    g_free(candidate->dict_name);
    g_free(candidate);
}

static const char *search_bucket_label(SearchBucket bucket) {
    switch (bucket) {
    case SEARCH_BUCKET_EXACT:
        return "Exact";
    case SEARCH_BUCKET_SUFFIX:
        return "Suffix";
    case SEARCH_BUCKET_PREFIX:
        return "Prefix";
    case SEARCH_BUCKET_SUBSTRING:
        return "Substring";
    case SEARCH_BUCKET_FUZZY:
        return "Fuzzy";
    default:
        return "Match";
    }
}

static GtkWidget *create_search_section_row(const char *title) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *label = gtk_label_new(title ? title : "");
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_set_margin_start(label, 12);
    gtk_widget_set_margin_end(label, 12);
    gtk_widget_set_margin_top(label, 10);
    gtk_widget_set_margin_bottom(label, 4);
    gtk_widget_add_css_class(label, "heading");
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
    gtk_widget_set_sensitive(row, FALSE);
    return row;
}

static guint utf8_length_or_bytes(const char *text) {
    if (!text || !*text) {
        return 0;
    }
    return (guint)g_utf8_strlen(text, -1);
}

static guint levenshtein_distance_bytes(const char *a, const char *b) {
    size_t len_a = a ? strlen(a) : 0;
    size_t len_b = b ? strlen(b) : 0;

    if (len_a == 0) {
        return (guint)len_b;
    }
    if (len_b == 0) {
        return (guint)len_a;
    }

    guint *prev = g_new(guint, len_b + 1);
    guint *curr = g_new(guint, len_b + 1);

    for (size_t j = 0; j <= len_b; j++) {
        prev[j] = (guint)j;
    }

    for (size_t i = 1; i <= len_a; i++) {
        curr[0] = (guint)i;
        for (size_t j = 1; j <= len_b; j++) {
            guint cost = (a[i - 1] == b[j - 1]) ? 0U : 1U;
            guint deletion = prev[j] + 1U;
            guint insertion = curr[j - 1] + 1U;
            guint substitution = prev[j - 1] + cost;
            guint best = deletion < insertion ? deletion : insertion;
            curr[j] = best < substitution ? best : substitution;
        }

        guint *tmp = prev;
        prev = curr;
        curr = tmp;
    }

    guint distance = prev[len_b];
    g_free(prev);
    g_free(curr);
    return distance;
}

static double fuzzy_similarity_score(const char *query_key, const char *candidate_key) {
    size_t len_q = query_key ? strlen(query_key) : 0;
    size_t len_c = candidate_key ? strlen(candidate_key) : 0;
    size_t longest = len_q > len_c ? len_q : len_c;
    if (longest == 0) {
        return 1.0;
    }

    guint distance = levenshtein_distance_bytes(query_key, candidate_key);
    if (distance >= longest) {
        return 0.0;
    }

    return 1.0 - ((double)distance / (double)longest);
}

static gboolean classify_search_candidate(const char *query_key,
                                          guint query_len,
                                          const char *candidate_key,
                                          SearchBucket *bucket_out,
                                          double *fuzzy_score_out) {
    if (!query_key || !candidate_key || !*candidate_key) {
        return FALSE;
    }

    if (g_strcmp0(candidate_key, query_key) == 0) {
        if (bucket_out) {
            *bucket_out = SEARCH_BUCKET_EXACT;
        }
        if (fuzzy_score_out) {
            *fuzzy_score_out = 1.0;
        }
        return TRUE;
    }

    if (g_str_has_suffix(candidate_key, query_key)) {
        if (bucket_out) {
            *bucket_out = SEARCH_BUCKET_SUFFIX;
        }
        if (fuzzy_score_out) {
            *fuzzy_score_out = 0.0;
        }
        return TRUE;
    }

    if (g_str_has_prefix(candidate_key, query_key)) {
        if (bucket_out) {
            *bucket_out = SEARCH_BUCKET_PREFIX;
        }
        if (fuzzy_score_out) {
            *fuzzy_score_out = 0.0;
        }
        return TRUE;
    }

    if (strstr(candidate_key, query_key) != NULL) {
        if (bucket_out) {
            *bucket_out = SEARCH_BUCKET_SUBSTRING;
        }
        if (fuzzy_score_out) {
            *fuzzy_score_out = 0.0;
        }
        return TRUE;
    }

    if (query_len < 3) {
        return FALSE;
    }

    guint candidate_len = utf8_length_or_bytes(candidate_key);
    guint length_delta = candidate_len > query_len ? candidate_len - query_len : query_len - candidate_len;
    if (length_delta > MAX(6U, query_len)) {
        return FALSE;
    }

    double ratio = fuzzy_similarity_score(query_key, candidate_key);
    double min_ratio = query_len >= 5 ? 0.74 : 0.78;
    if (ratio < min_ratio) {
        return FALSE;
    }

    if (bucket_out) {
        *bucket_out = SEARCH_BUCKET_FUZZY;
    }
    if (fuzzy_score_out) {
        *fuzzy_score_out = ratio;
    }
    return TRUE;
}

static gboolean search_candidate_should_replace(const SearchCandidate *current,
                                                SearchBucket bucket,
                                                double fuzzy_score,
                                                guint dict_rank,
                                                guint length_delta,
                                                const char *display_word) {
    if (!current) {
        return TRUE;
    }

    if ((int)bucket != (int)current->bucket) {
        return ((int)bucket < (int)current->bucket);
    }

    if (bucket == SEARCH_BUCKET_FUZZY) {
        if (fuzzy_score != current->fuzzy_score) {
            return fuzzy_score > current->fuzzy_score;
        }
        if (length_delta != current->length_delta) {
            return length_delta < current->length_delta;
        }
    }

    if (dict_rank != current->dict_rank) {
        return dict_rank < current->dict_rank;
    }

    return g_ascii_strcasecmp(display_word, current->word) < 0;
}

static gint compare_search_candidates(gconstpointer a, gconstpointer b) {
    const SearchCandidate *left = *(SearchCandidate * const *)a;
    const SearchCandidate *right = *(SearchCandidate * const *)b;

    if (left->bucket != right->bucket) {
        return (gint)left->bucket - (gint)right->bucket;
    }

    if (left->bucket == SEARCH_BUCKET_FUZZY && left->fuzzy_score != right->fuzzy_score) {
        return left->fuzzy_score > right->fuzzy_score ? -1 : 1;
    }

    int word_cmp = g_ascii_strcasecmp(left->word, right->word);
    if (word_cmp != 0 && left->bucket != SEARCH_BUCKET_FUZZY) {
        return word_cmp;
    }

    if (left->dict_rank != right->dict_rank) {
        return left->dict_rank < right->dict_rank ? -1 : 1;
    }

    if (left->length_delta != right->length_delta) {
        return left->length_delta < right->length_delta ? -1 : 1;
    }

    if (word_cmp != 0) {
        return word_cmp;
    }

    return g_ascii_strcasecmp(left->dict_name, right->dict_name);
}

static char *get_app_config_file_path(const char *filename) {
    char *dir = g_build_filename(g_get_user_config_dir(), "diction", NULL);
    g_mkdir_with_parents(dir, 0755);
    char *path = g_build_filename(dir, filename, NULL);
    g_free(dir);
    return path;
}

static void free_word_list(GPtrArray **list_ptr) {
    if (list_ptr && *list_ptr) {
        g_ptr_array_free(*list_ptr, TRUE);
        *list_ptr = NULL;
    }
}

static void sidebar_search_state_free(SidebarSearchState *state) {
    if (!state) {
        return;
    }
    g_free(state->query);
    g_free(state->query_key);
    if (state->best) {
        g_hash_table_unref(state->best);
    }
    g_free(state);
}

static gboolean word_list_contains_ci(GPtrArray *list, const char *word) {
    if (!list || !word) {
        return FALSE;
    }
    for (guint i = 0; i < list->len; i++) {
        const char *item = g_ptr_array_index(list, i);
        if (g_ascii_strcasecmp(item, word) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static GPtrArray *load_word_list(const char *filename, guint limit) {
    GPtrArray *words = g_ptr_array_new_with_free_func(g_free);
    char *path = get_app_config_file_path(filename);
    if (!g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        return words;
    }

    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    if (!json_parser_load_from_file(parser, path, &error)) {
        if (error) {
            g_error_free(error);
        }
        g_object_unref(parser);
        g_free(path);
        return words;
    }

    JsonNode *root = json_parser_get_root(parser);
    if (root && JSON_NODE_HOLDS_ARRAY(root)) {
        JsonArray *array = json_node_get_array(root);
        for (guint i = 0; i < json_array_get_length(array); i++) {
            char *word = sanitize_user_word(json_array_get_string_element(array, i));
            if (!word) {
                continue;
            }
            if (word_list_contains_ci(words, word)) {
                g_free(word);
                continue;
            }
            g_ptr_array_add(words, word);
            if (limit > 0 && words->len >= limit) {
                break;
            }
        }
    }

    g_object_unref(parser);
    g_free(path);
    return words;
}

static void save_word_list(GPtrArray *words, const char *filename) {
    if (!words) {
        return;
    }

    char *path = get_app_config_file_path(filename);
    JsonArray *array = json_array_new();
    for (guint i = 0; i < words->len; i++) {
        json_array_add_string_element(array, g_ptr_array_index(words, i));
    }

    JsonNode *root = json_node_alloc();
    json_node_init_array(root, array);
    JsonGenerator *gen = json_generator_new();
    json_generator_set_pretty(gen, TRUE);
    json_generator_set_root(gen, root);
    json_generator_to_file(gen, path, NULL);
    g_object_unref(gen);
    json_node_free(root);
    json_array_unref(array);
    g_free(path);
}

static void clear_listbox_rows(GtkListBox *listbox) {
    if (!listbox) {
        return;
    }
    GtkWidget *child = NULL;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(listbox)))) {
        gtk_list_box_remove(listbox, child);
    }
}

typedef struct {
    char *word;
} SidebarWordData;

static void sidebar_word_data_free(SidebarWordData *data) {
    if (!data) {
        return;
    }
    g_free(data->word);
    g_free(data);
}

static void sidebar_word_data_destroy(gpointer data, GClosure *closure) {
    (void)closure;
    sidebar_word_data_free(data);
}

static void refresh_favorite_button_state(void);
static void populate_history_sidebar(void);
static void populate_favorites_sidebar(void);
static void populate_groups_sidebar(void);
static void populate_search_sidebar(const char *query);
static gboolean dict_entry_in_active_scope(DictEntry *entry);
static GtkWidget *create_word_sidebar_row(const char *word,
                                          const char *subtitle,
                                          gboolean add_favorite_button);

static void cancel_sidebar_search(void) {
    if (sidebar_search_state && sidebar_search_state->source_id != 0) {
        g_source_remove(sidebar_search_state->source_id);
        sidebar_search_state->source_id = 0;
    }
    g_clear_pointer(&sidebar_search_state, sidebar_search_state_free);
}

static void populate_search_sidebar_status(const char *title, const char *subtitle) {
    clear_listbox_rows(related_listbox);
    if (!related_listbox) {
        return;
    }
    GtkWidget *row = create_word_sidebar_row(title, subtitle, FALSE);
    gtk_widget_set_sensitive(row, FALSE);
    gtk_list_box_append(related_listbox, row);
}

static void render_sidebar_search_results(SidebarSearchState *state) {
    clear_listbox_rows(related_listbox);
    if (!related_listbox || !state) {
        return;
    }

    GPtrArray *ordered = g_ptr_array_new();
    GHashTableIter iter;
    gpointer key = NULL;
    gpointer value = NULL;
    g_hash_table_iter_init(&iter, state->best);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        g_ptr_array_add(ordered, value);
    }
    g_ptr_array_sort(ordered, compare_search_candidates);

    guint total = 0;
    for (int bucket = SEARCH_BUCKET_EXACT; bucket <= SEARCH_BUCKET_FUZZY; bucket++) {
        guint appended = 0;
        for (guint i = 0; i < ordered->len; i++) {
            SearchCandidate *candidate = g_ptr_array_index(ordered, i);
            if ((int)candidate->bucket != bucket) {
                continue;
            }
            if (appended == 0) {
                gtk_list_box_append(related_listbox,
                    create_search_section_row(search_bucket_label((SearchBucket)bucket)));
            }
            gtk_list_box_append(related_listbox,
                create_word_sidebar_row(candidate->word, candidate->dict_name, FALSE));
            appended++;
            total++;
        }
    }

    if (total == 0) {
        GtkWidget *row = create_word_sidebar_row("No suggestions", state->query, FALSE);
        gtk_widget_set_sensitive(row, FALSE);
        gtk_list_box_append(related_listbox, row);
    }

    g_ptr_array_free(ordered, TRUE);
}

static void populate_search_sidebar_exact_only(const char *query, const char *empty_hint) {
    clear_listbox_rows(related_listbox);
    if (!related_listbox || !query || !*query) {
        return;
    }

    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    guint total = 0;
    size_t q_len = strlen(query);

    for (DictEntry *entry = all_dicts; entry; entry = entry->next) {
        if (!entry->dict || !entry->dict->index || !dict_entry_in_active_scope(entry)) {
            continue;
        }

        guint per_dict = 0;
        for (SplayNode *node = splay_tree_search_first(entry->dict->index, query);
             node;
             node = splay_tree_successor(node)) {
            const char *candidate_ptr = entry->dict->data + node->key_offset;
            if (node->key_length != q_len ||
                strncasecmp(candidate_ptr, query, q_len) != 0) {
                break;
            }

            char *word = g_strndup(candidate_ptr, node->key_length);
            char *word_key = g_utf8_casefold(word, -1);
            if (!g_hash_table_contains(seen, word_key)) {
                if (total == 0) {
                    gtk_list_box_append(related_listbox, create_search_section_row("Exact"));
                }
                g_hash_table_add(seen, word_key);
                gtk_list_box_append(related_listbox,
                    create_word_sidebar_row(word, entry->name, FALSE));
                word_key = NULL;
                total++;
                per_dict++;
            }
            g_free(word_key);
            g_free(word);

            if (per_dict >= 32) {
                break;
            }
        }
    }

    if (total == 0) {
        GtkWidget *row = create_word_sidebar_row(
            empty_hint ? empty_hint : "No suggestions",
            "Type more characters for broader matches.",
            FALSE);
        gtk_widget_set_sensitive(row, FALSE);
        gtk_list_box_append(related_listbox, row);
    }

    g_hash_table_unref(seen);
}

static gboolean continue_sidebar_search(gpointer user_data) {
    SidebarSearchState *state = user_data;
    if (!state || state != sidebar_search_state) {
        return G_SOURCE_REMOVE;
    }

    guint processed = 0;
    const guint max_batch_size = 1500;
    gint64 deadline_us = g_get_monotonic_time() + 8000;

    while (processed < max_batch_size && g_get_monotonic_time() < deadline_us) {
        if (!state->current_entry) {
            render_sidebar_search_results(state);
            state->source_id = 0;
            g_clear_pointer(&sidebar_search_state, sidebar_search_state_free);
            return G_SOURCE_REMOVE;
        }

        if (!state->current_node) {
            gboolean found_dict = FALSE;
            while (state->current_entry) {
                DictEntry *entry = state->current_entry;
                state->current_entry = state->current_entry->next;
                if (!entry->dict || !entry->dict->index || !entry->dict->index->root ||
                    !dict_entry_in_active_scope(entry)) {
                    continue;
                }
                state->current_node = splay_tree_min(entry->dict->index->root);
                if (state->current_node) {
                    state->current_dict = entry;
                    found_dict = TRUE;
                    break;
                }
            }

            if (!found_dict) {
                continue;
            }

            state->dict_rank++;
        }

        if (!state->current_node) {
            continue;
        }
        if (!state->current_dict) {
            state->current_node = NULL;
            processed++;
            continue;
        }

        SplayNode *node = state->current_node;
        state->current_node = splay_tree_successor(node);

        char *word = g_strndup(state->current_dict->dict->data + node->key_offset, node->key_length);
        char *word_key = g_utf8_casefold(word, -1);
        SearchBucket bucket;
        double fuzzy_score = 0.0;

        if (classify_search_candidate(state->query_key, state->query_len, word_key, &bucket, &fuzzy_score)) {
            guint candidate_len = utf8_length_or_bytes(word_key);
            guint length_delta = candidate_len > state->query_len
                ? candidate_len - state->query_len
                : state->query_len - candidate_len;
            SearchCandidate *current = g_hash_table_lookup(state->best, word_key);
            if (search_candidate_should_replace(current, bucket, fuzzy_score, state->dict_rank, length_delta, word)) {
                SearchCandidate *candidate = g_new0(SearchCandidate, 1);
                candidate->word = word;
                candidate->word_key = g_strdup(word_key);
                candidate->dict_name = g_strdup(state->current_dict->name);
                candidate->dict_rank = state->dict_rank;
                candidate->bucket = bucket;
                candidate->fuzzy_score = fuzzy_score;
                candidate->length_delta = length_delta;
                g_hash_table_replace(state->best, g_strdup(word_key), candidate);
                word = NULL;
            }
        }

        if (!state->current_node) {
            state->current_dict = NULL;
        }

        g_free(word_key);
        g_free(word);
        processed++;
    }

    return G_SOURCE_CONTINUE;
}

static void populate_search_sidebar(const char *query) {
    cancel_sidebar_search();

    char *clean = sanitize_user_word(query);
    if (!clean) {
        clear_listbox_rows(related_listbox);
        if (history_words && history_words->len > 0) {
            guint limit = MIN(history_words->len, 25);
            for (guint i = 0; i < limit; i++) {
                const char *word = g_ptr_array_index(history_words, i);
                gtk_list_box_append(related_listbox, create_word_sidebar_row(word, "Recent search", FALSE));
            }
        } else {
            GtkWidget *row = create_word_sidebar_row("Start typing to search", "Candidate headwords will appear here.", FALSE);
            gtk_widget_set_sensitive(row, FALSE);
            gtk_list_box_append(related_listbox, row);
        }
        g_free(clean);
        return;
    }

    guint query_len = utf8_length_or_bytes(clean);
    if (query_len < SEARCH_SIDEBAR_MIN_BROAD_QUERY_LEN) {
        populate_search_sidebar_exact_only(
            clean,
            "Type at least 3 characters for prefix, suffix, substring, and fuzzy suggestions.");
        g_free(clean);
        return;
    }

    sidebar_search_state = g_new0(SidebarSearchState, 1);
    sidebar_search_state->query = clean;
    sidebar_search_state->query_key = g_utf8_casefold(clean, -1);
    sidebar_search_state->query_len = utf8_length_or_bytes(sidebar_search_state->query_key);
    sidebar_search_state->best = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)search_candidate_free);
    sidebar_search_state->current_entry = all_dicts;

    populate_search_sidebar_status("Searching suggestions…", clean);
    sidebar_search_state->source_id = g_idle_add_full(
        G_PRIORITY_LOW, continue_sidebar_search, sidebar_search_state, NULL);
}

static void update_favorites_word(const char *word, gboolean add) {
    char *clean = sanitize_user_word(word);
    if (!clean) {
        return;
    }

    if (!favorite_words) {
        favorite_words = g_ptr_array_new_with_free_func(g_free);
    }

    for (guint i = 0; i < favorite_words->len; i++) {
        const char *item = g_ptr_array_index(favorite_words, i);
        if (g_ascii_strcasecmp(item, clean) == 0) {
            if (!add) {
                g_ptr_array_remove_index(favorite_words, i);
            }
            save_word_list(favorite_words, FAVORITES_FILE_NAME);
            populate_favorites_sidebar();
            populate_history_sidebar();
            refresh_favorite_button_state();
            g_free(clean);
            return;
        }
    }

    if (add) {
        g_ptr_array_insert(favorite_words, 0, clean);
        save_word_list(favorite_words, FAVORITES_FILE_NAME);
        populate_favorites_sidebar();
        populate_history_sidebar();
        refresh_favorite_button_state();
        return;
    }

    g_free(clean);
    refresh_favorite_button_state();
}

static void update_history_word(const char *word) {
    char *clean = sanitize_user_word(word);
    if (!clean) {
        return;
    }

    if (!history_words) {
        history_words = g_ptr_array_new_with_free_func(g_free);
    }

    for (guint i = 0; i < history_words->len; i++) {
        const char *item = g_ptr_array_index(history_words, i);
        if (g_ascii_strcasecmp(item, clean) == 0) {
            g_ptr_array_remove_index(history_words, i);
            break;
        }
    }

    g_ptr_array_insert(history_words, 0, clean);
    while (history_words->len > 200) {
        g_ptr_array_remove_index(history_words, history_words->len - 1);
    }

    save_word_list(history_words, HISTORY_FILE_NAME);
    populate_history_sidebar();
}

static void on_sidebar_favorite_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    SidebarWordData *data = user_data;
    if (!data || !data->word) {
        return;
    }
    gboolean is_favorite = word_list_contains_ci(favorite_words, data->word);
    update_favorites_word(data->word, !is_favorite);
}

static GtkWidget *create_word_sidebar_row(const char *word,
                                          const char *subtitle,
                                          gboolean add_favorite_button) {
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *action = adw_action_row_new();
    adw_preferences_row_set_use_markup(ADW_PREFERENCES_ROW(action), FALSE);
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(action), word ? word : "");
    if (subtitle && *subtitle) {
        adw_action_row_set_subtitle(ADW_ACTION_ROW(action), subtitle);
    }
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), action);

    char *clean = sanitize_user_word(word);
    if (clean) {
        g_object_set_data_full(G_OBJECT(row), "lookup-word", clean, g_free);
    }

    if (add_favorite_button && clean) {
        gboolean is_favorite = word_list_contains_ci(favorite_words, clean);
        GtkWidget *fav_btn = gtk_button_new_from_icon_name(
            is_favorite ? "starred-symbolic" : "non-starred-symbolic");
        gtk_widget_add_css_class(fav_btn, "flat");
        gtk_widget_set_valign(fav_btn, GTK_ALIGN_CENTER);
        gtk_widget_set_tooltip_text(fav_btn, is_favorite ? "Unfavorite" : "Favorite");

        SidebarWordData *data = g_new0(SidebarWordData, 1);
        data->word = g_strdup(clean);
        g_signal_connect_data(fav_btn, "clicked", G_CALLBACK(on_sidebar_favorite_clicked),
            data, sidebar_word_data_destroy, 0);
        adw_action_row_add_suffix(ADW_ACTION_ROW(action), fav_btn);
    }

    return row;
}

static gboolean dict_entry_enabled(DictEntry *entry) {
    if (!entry || !app_settings) {
        return TRUE;
    }
    DictConfig *cfg = settings_find_dictionary_by_path(app_settings, entry->path);
    return !cfg || cfg->enabled;
}

static gboolean dict_entry_in_scope(DictEntry *entry, const char *scope_id) {
    if (!entry || !dict_entry_enabled(entry)) {
        return FALSE;
    }
    if (!scope_id || g_strcmp0(scope_id, "all") == 0 || !app_settings) {
        return TRUE;
    }

    char *dict_id = settings_make_dictionary_id(entry->path);
    gboolean allowed = FALSE;
    for (guint i = 0; i < app_settings->dictionary_groups->len; i++) {
        DictGroup *grp = g_ptr_array_index(app_settings->dictionary_groups, i);
        if (g_strcmp0(grp->id, scope_id) != 0) {
            continue;
        }
        for (guint j = 0; j < grp->members->len; j++) {
            const char *member = g_ptr_array_index(grp->members, j);
            if (g_strcmp0(member, dict_id) == 0) {
                allowed = TRUE;
                break;
            }
        }
        break;
    }
    g_free(dict_id);
    return allowed;
}

static gboolean dict_entry_in_active_scope(DictEntry *entry) {
    return dict_entry_in_scope(entry, active_scope_id);
}

static void sync_settings_dictionaries_from_loaded(void) {
    if (!app_settings) {
        return;
    }

    GHashTable *loaded_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (DictEntry *entry = all_dicts; entry; entry = entry->next) {
        if (!entry->dict || !entry->path) {
            continue;
        }
        settings_upsert_dictionary(app_settings, entry->name, entry->path, "directory");
        g_hash_table_add(loaded_paths, g_strdup(entry->path));
    }

    settings_prune_directory_dictionaries(app_settings, loaded_paths);
    g_hash_table_unref(loaded_paths);
    settings_save(app_settings);
}

static void refresh_favorite_button_state(void) {
    if (!favorite_toggle_btn || !search_entry) {
        return;
    }
    char *clean = sanitize_user_word(gtk_editable_get_text(GTK_EDITABLE(search_entry)));
    gboolean is_favorite = clean && word_list_contains_ci(favorite_words, clean);
    gtk_button_set_icon_name(GTK_BUTTON(favorite_toggle_btn),
        is_favorite ? "starred-symbolic" : "non-starred-symbolic");
    gtk_widget_set_tooltip_text(favorite_toggle_btn,
        is_favorite ? "Remove from Favorites" : "Add to Favorites");
    g_free(clean);
}

static void populate_history_sidebar(void) {
    clear_listbox_rows(history_listbox);
    if (!history_listbox || !history_words || history_words->len == 0) {
        if (history_listbox) {
            GtkWidget *row = create_word_sidebar_row("No history yet", "Successful searches will appear here.", FALSE);
            gtk_widget_set_sensitive(row, FALSE);
            gtk_list_box_append(history_listbox, row);
        }
        return;
    }

    for (guint i = 0; i < history_words->len; i++) {
        const char *word = g_ptr_array_index(history_words, i);
        gtk_list_box_append(history_listbox, create_word_sidebar_row(word, NULL, TRUE));
    }
}

static void populate_favorites_sidebar(void) {
    clear_listbox_rows(favorites_listbox);
    if (!favorites_listbox || !favorite_words || favorite_words->len == 0) {
        if (favorites_listbox) {
            GtkWidget *row = create_word_sidebar_row("No favorites yet", "Use the star button to save words.", FALSE);
            gtk_widget_set_sensitive(row, FALSE);
            gtk_list_box_append(favorites_listbox, row);
        }
        return;
    }

    for (guint i = 0; i < favorite_words->len; i++) {
        const char *word = g_ptr_array_index(favorite_words, i);
        gtk_list_box_append(favorites_listbox, create_word_sidebar_row(word, NULL, TRUE));
    }
}

static void populate_groups_sidebar(void) {
    clear_listbox_rows(groups_listbox);
    if (!groups_listbox) {
        return;
    }
    GtkListBoxRow *active_row = NULL;

    guint all_count = 0;
    for (DictEntry *entry = all_dicts; entry; entry = entry->next) {
        if (dict_entry_enabled(entry)) {
            all_count++;
        }
    }

    GtkWidget *all_row = create_word_sidebar_row("All Dictionaries", NULL, FALSE);
    g_object_set_data_full(G_OBJECT(all_row), "scope-id", g_strdup("all"), g_free);
    {
        GtkWidget *child = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(all_row));
        if (ADW_IS_ACTION_ROW(child)) {
            char subtitle[64];
            g_snprintf(subtitle, sizeof(subtitle), "%u dictionaries", all_count);
            adw_action_row_set_subtitle(ADW_ACTION_ROW(child), subtitle);
        }
    }
    gtk_list_box_append(groups_listbox, all_row);
    if (!active_scope_id || g_strcmp0(active_scope_id, "all") == 0) {
        active_row = GTK_LIST_BOX_ROW(all_row);
    }

    if (!app_settings) {
        if (active_row) {
            gtk_list_box_select_row(groups_listbox, active_row);
        }
        return;
    }

    for (guint i = 0; i < app_settings->dictionary_groups->len; i++) {
        DictGroup *grp = g_ptr_array_index(app_settings->dictionary_groups, i);
        guint member_count = 0;
        for (DictEntry *entry = all_dicts; entry; entry = entry->next) {
            if (!dict_entry_enabled(entry)) {
                continue;
            }
            char *dict_id = settings_make_dictionary_id(entry->path);
            for (guint j = 0; j < grp->members->len; j++) {
                const char *member = g_ptr_array_index(grp->members, j);
                if (g_strcmp0(member, dict_id) == 0) {
                    member_count++;
                    break;
                }
            }
            g_free(dict_id);
        }

        GtkWidget *row = create_word_sidebar_row(grp->name, NULL, FALSE);
        g_object_set_data_full(G_OBJECT(row), "scope-id", g_strdup(grp->id), g_free);
        GtkWidget *child = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
        if (ADW_IS_ACTION_ROW(child)) {
            char subtitle[64];
            g_snprintf(subtitle, sizeof(subtitle), "%u dictionaries", member_count);
            adw_action_row_set_subtitle(ADW_ACTION_ROW(child), subtitle);
        }
        gtk_list_box_append(groups_listbox, row);
        if (active_scope_id && g_strcmp0(active_scope_id, grp->id) == 0) {
            active_row = GTK_LIST_BOX_ROW(row);
        }
    }

    if (active_row) {
        gtk_list_box_select_row(groups_listbox, active_row);
    }
}

static void on_favorite_toggle_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn;
    (void)user_data;
    char *word = sanitize_user_word(gtk_editable_get_text(GTK_EDITABLE(search_entry)));
    if (!word) {
        return;
    }
    gboolean is_favorite = word_list_contains_ci(favorite_words, word);
    update_favorites_word(word, !is_favorite);
    g_free(word);
}

static gboolean dict_list_filter_func(GtkListBoxRow *row, gpointer user_data) {
    (void)user_data;
    if (!search_entry) return TRUE;
    const char *query = gtk_editable_get_text(GTK_EDITABLE(search_entry));
    if (!query || strlen(query) == 0) return TRUE;

    DictEntry *e = g_object_get_data(G_OBJECT(row), "dict-entry");
    if (!e) return TRUE;
    
    return e->has_matches;
}

static void on_decide_policy(WebKitWebView *v, WebKitPolicyDecision *d, WebKitPolicyDecisionType t, gpointer user_data) {
    (void)v;
    if (t == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        WebKitNavigationPolicyDecision *nd = WEBKIT_NAVIGATION_POLICY_DECISION(d);
        WebKitNavigationAction *na = webkit_navigation_policy_decision_get_navigation_action(nd);
        WebKitURIRequest *req = webkit_navigation_action_get_request(na);
        const char *uri = webkit_uri_request_get_uri(req);
        fprintf(stderr, "[LINK CLICKED] URI: %s\n", uri);
        if (g_str_has_prefix(uri, "dict://")) {
            const char *word = uri + 7;
            char *unescaped = g_uri_unescape_string(word, NULL);
            fprintf(stderr, "[DICT LINK] Searching for: %s\n", unescaped ? unescaped : word);
            gtk_editable_set_text(GTK_EDITABLE(user_data), unescaped ? unescaped : word);
            g_free(unescaped);
            webkit_policy_decision_ignore(d);
            return;
        } else if (g_str_has_prefix(uri, "sound://")) {
            if (!try_play_encoded_sound_uri(uri)) {
                const char *sound_file = uri + 8; // Skip "sound://"
                fprintf(stderr, "[AUDIO CLICKED] Clicked: %s\n", sound_file);
                
                /* Backward-compatible fallback */
                if (active_entry && active_entry->dict && active_entry->dict->resource_dir) {
                    char *audio_path = g_build_filename(active_entry->dict->resource_dir, sound_file, NULL);
                    if (g_file_test(audio_path, G_FILE_TEST_EXISTS)) {
                        play_audio_file(audio_path);
                    } else {
                        fprintf(stderr, "[AUDIO ERROR] File not found: %s\n", audio_path);
                    }
                    g_free(audio_path);
                } else {
                    fprintf(stderr, "[AUDIO ERROR] No active dictionary or resource directory\n");
                }
            }
            
            webkit_policy_decision_ignore(d);
            return;
        }
    }
    webkit_policy_decision_use(d);
}

static void on_history_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);
static void on_related_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);
static void on_favorites_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);
static void on_groups_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data);

static void on_history_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box; (void)user_data;
    if (!row) return;
    const char *text = g_object_get_data(G_OBJECT(row), "lookup-word");
    if (text) {
        gtk_editable_set_text(GTK_EDITABLE(search_entry), text);
    }
    gtk_list_box_unselect_all(box);
}

static void on_related_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box; (void)user_data;
    if (!row) return;
    const char *text = g_object_get_data(G_OBJECT(row), "lookup-word");
    if (text) {
        gtk_editable_set_text(GTK_EDITABLE(search_entry), text);
    }
    gtk_list_box_unselect_all(box);
}

static void on_favorites_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box; (void)user_data;
    if (!row) return;
    const char *text = g_object_get_data(G_OBJECT(row), "lookup-word");
    if (text) {
        gtk_editable_set_text(GTK_EDITABLE(search_entry), text);
    }
    gtk_list_box_unselect_all(box);
}

static void on_groups_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box; (void)user_data;
    if (!row) return;
    const char *scope_id = g_object_get_data(G_OBJECT(row), "scope-id");
    g_free(active_scope_id);
    active_scope_id = g_strdup(scope_id ? scope_id : "all");
    refresh_search_results();
}

static gboolean run_debounced_search(gpointer user_data) {
    (void)user_data;
    search_execute_source_id = 0;
    execute_search_now();
    return G_SOURCE_REMOVE;
}

static void schedule_execute_search(void) {
    if (search_execute_source_id != 0) {
        g_source_remove(search_execute_source_id);
    }
    search_execute_source_id = g_timeout_add(200, run_debounced_search, NULL);
}

static void execute_search_now(void) {
    if (search_execute_source_id != 0) {
        g_source_remove(search_execute_source_id);
        search_execute_source_id = 0;
    }

    if (!search_entry) {
        return;
    }

    const char *query = gtk_editable_get_text(GTK_EDITABLE(search_entry));

    refresh_favorite_button_state();

    if (!query || strlen(query) == 0) {
        cancel_sidebar_search();
        populate_search_sidebar(NULL);
        webkit_web_view_load_html(web_view, "<h2>Diction</h2><p>Start typing to search...</p>", "file:///");
        for (DictEntry *e = all_dicts; e; e = e->next) {
            e->has_matches = FALSE;
        }
        gtk_list_box_invalidate_filter(dict_listbox);
        return;
    }

    GString *html_res = g_string_new("<html><body style='font-family: sans-serif; padding: 10px;'>");
    int found_count = 0;
    for (DictEntry *e = all_dicts; e; e = e->next) e->has_matches = FALSE;

    int dict_idx = 0;
    for (DictEntry *e = all_dicts; e; e = e->next) {
        if (!e->dict || !dict_entry_in_active_scope(e)) continue;

        SplayNode *res = splay_tree_search_first(e->dict->index, query);
        size_t q_len = strlen(query);
        int dict_header_shown = 0;

        while (res != NULL) {
            // Check if this node is still a match (case-insensitive and same length)
            if (res->key_length != q_len || 
                strncasecmp(e->dict->data + res->key_offset, query, q_len) != 0) {
                break;
            }

            const char *def_ptr = e->dict->data + res->val_offset;
            size_t def_len = res->val_length;

            /* Handle MDX @@@LINK= redirect */
            if (e->format == DICT_FORMAT_MDX && def_len > 8 && g_str_has_prefix(def_ptr, "@@@LINK=")) {
                char link_target[1024];
                const char *lp = def_ptr + 8;
                size_t l = 0;
                while (l < sizeof(link_target)-1 && l < (def_len - 8) && lp[l] != '\r' && lp[l] != '\n') {
                    link_target[l] = lp[l];
                    l++;
                }
                link_target[l] = '\0';

                // For redirects, we just take the first match for simplicity, 
                // or we could recursively handle it. But usually redirects are 1-to-1 or 1-to-many.
                // Here we just use the first match of the target.
                SplayNode *red_res = splay_tree_search_first(e->dict->index, link_target);
                if (red_res) {
                    def_ptr = e->dict->data + red_res->val_offset;
                    def_len = red_res->val_length;
                }
            }

            // Get current theme
            int dark_mode = style_manager && adw_style_manager_get_dark(style_manager) ? 1 : 0;

            char *rendered = dsl_render_to_html(
                def_ptr, def_len,
                e->dict->data + res->key_offset, res->key_length,
                e->format, e->dict->resource_dir, e->dict->source_dir, e->dict->mdx_stylesheet, dark_mode);
            if (rendered) {
                // Theme-aware dict source bar colors
                const char *bar_bg = dark_mode ? "#2d2d2d" : "#f0f0f0";
                const char *bar_fg = dark_mode ? "#aaaaaa" : "#555555";
                const char *bar_border = dark_mode ? "#444444" : "#dddddd";

                if (!dict_header_shown) {
                    g_string_append_printf(html_res,
                        "<div id='dict-%d' class='dict-source' style='background: %s; color: %s; "
                        "padding: 4px 12px; margin: 20px -10px 10px -10px; border-bottom: 1px solid %s; "
                        "font-size: 0.85em; font-weight: bold; text-transform: uppercase; letter-spacing: 0.05em;'>"
                        "%s</div>",
                        dict_idx, bar_bg, bar_fg, bar_border, e->name);
        
                    dict_header_shown = 1;
                    e->has_matches = TRUE;
                    found_count++;
                }

                g_string_append(html_res, rendered);
                free(rendered);
            }
            
            res = splay_tree_successor(res);
        }

        dict_idx++;
    }

    if (found_count > 0) {
        g_string_append(html_res, "</body></html>");
        webkit_web_view_load_html(web_view, html_res->str, "file:///");
        update_history_word(query);
    } else {
        // Theme-aware no results message
        int dark_mode = style_manager && adw_style_manager_get_dark(style_manager) ? 1 : 0;
        const char *text_color = dark_mode ? "#aaaaaa" : "#666666";

        char buf[512];
        snprintf(buf, sizeof(buf),
            "<div style='padding: 20px; color: %s; font-style: italic;'>"
            "No exact match for <b>%s</b> in any dictionary.</div>", text_color, query);
        webkit_web_view_load_html(web_view, buf, "file:///");
    }
    g_string_free(html_res, TRUE);

    gtk_list_box_invalidate_filter(dict_listbox);
    populate_search_sidebar(query);
}

static void on_search_changed(GtkSearchEntry *entry, gpointer user_data) {
    (void)user_data;
    const char *query = gtk_editable_get_text(GTK_EDITABLE(entry));

    if (last_search_query && strcmp(query, last_search_query) == 0) return;

    g_free(last_search_query);
    last_search_query = g_strdup(query);

    if (!query || strlen(query) == 0) {
        execute_search_now();
        return;
    }

    schedule_execute_search();
}

static void on_random_clicked(GtkButton *btn, gpointer user_data) {
    (void)btn; (void)user_data;
    if (!all_dicts) return;

    // Count dicts
    int count = 0;
    for (DictEntry *e = all_dicts; e; e = e->next) if (e->dict && e->dict->index->root) count++;
    if (count == 0) return;

    // Pick random dict
    int target = rand() % count;
    DictEntry *e = all_dicts;
    int cur = 0;
    while (e) {
        if (e->dict && e->dict->index->root) {
            if (cur == target) break;
            cur++;
        }
        e = e->next;
    }

    if (e && e->dict && e->dict->index->root) {
        SplayNode *node = splay_tree_get_random(e->dict->index);
        if (node) {
            const char *word = e->dict->data + node->key_offset;
            size_t len = node->key_length;
            char *word_str = g_strndup(word, len);
            gtk_editable_set_text(GTK_EDITABLE(search_entry), word_str);
            g_free(word_str);
            // Search will be triggered by "search-changed" signal
        }
    }
}

static void on_dict_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box; (void)user_data;
    if (!row) return;
    DictEntry *e = g_object_get_data(G_OBJECT(row), "dict-entry");
    if (!e) return;

    int idx = -1;
    int current = 0;
    for (DictEntry *cursor = all_dicts; cursor; cursor = cursor->next) {
        if (!dict_entry_enabled(cursor)) {
            continue;
        }
        if (cursor == e) {
            idx = current;
            break;
        }
        current++;
    }
    if (idx < 0) {
        return;
    }

    char js[256];
    snprintf(js, sizeof(js),
        "var el = document.getElementById('dict-%d'); "
        "if (el) { el.scrollIntoView({behavior: 'smooth', block: 'start'}); }",
        idx);
    webkit_web_view_evaluate_javascript(web_view, js, -1, NULL, NULL, NULL, NULL, NULL);
    active_entry = e;
}

// Refresh the current search results when theme changes
static void refresh_search_results(void) {
    if (!search_entry) return;

    const char *query = gtk_editable_get_text(GTK_EDITABLE(search_entry));
    if (!query || strlen(query) == 0) {
        // Refresh placeholder
        int dark_mode = style_manager && adw_style_manager_get_dark(style_manager) ? 1 : 0;
        const char *bg = dark_mode ? "#1e1e1e" : "#ffffff";
        const char *fg = dark_mode ? "#dddddd" : "#222222";
        char html[256];
        snprintf(html, sizeof(html),
            "<html><body style='font-family: sans-serif; background: %s; color: %s; "
            "text-align: center; margin-top: 2em; opacity: 0.7;'>"
            "<h2>Diction</h2><p>Start typing to search...</p></body></html>",
            bg, fg);
        webkit_web_view_load_html(web_view, html, "file:///");
        return;
    }

    execute_search_now();
}

// Theme change handler
static void on_theme_changed(AdwStyleManager *manager, GParamSpec *pspec, gpointer user_data) {
    (void)manager; (void)pspec; (void)user_data;

    // Update webview background color
    GdkRGBA bg_color;
    int dark_mode = style_manager && adw_style_manager_get_dark(style_manager) ? 1 : 0;

    if (dark_mode) {
        gdk_rgba_parse(&bg_color, "#1e1e1e");
    } else {
        gdk_rgba_parse(&bg_color, "#ffffff");
    }
    webkit_web_view_set_background_color(web_view, &bg_color);

    // Refresh current content
    refresh_search_results();
}

static void reload_dictionaries_from_settings(void *user_data) {
    (void)user_data;
    if (search_execute_source_id != 0) {
        g_source_remove(search_execute_source_id);
        search_execute_source_id = 0;
    }
    cancel_sidebar_search();

    // Free existing dicts
    dict_loader_free(all_dicts);
    all_dicts = NULL;
    active_entry = NULL;

    // Clear sidebar
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(dict_listbox))))
        gtk_list_box_remove(dict_listbox, child);
    clear_listbox_rows(related_listbox);
    clear_listbox_rows(groups_listbox);

    // Show "Reloading..." and start async scan
    webkit_web_view_load_html(web_view,
        "<html><body style='font-family: sans-serif; text-align: center; margin-top: 3em; opacity: 0.6;'>"
        "<h2>Reloading dictionaries\u2026</h2><p>Please wait.</p>"
        "</body></html>", "file:///");

    if (!active_scope_id) {
        active_scope_id = g_strdup("all");
    }
    populate_history_sidebar();
    populate_favorites_sidebar();
    populate_groups_sidebar();
    populate_search_sidebar(gtk_editable_get_text(GTK_EDITABLE(search_entry)));
    start_async_dict_loading();
}

static void show_settings_dialog(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action; (void)parameter;
    GtkWindow *window = gtk_application_get_active_window(GTK_APPLICATION(user_data));
    if (window) {
        GtkWidget *dialog = settings_dialog_new(window, app_settings, style_manager,
            reload_dictionaries_from_settings, NULL);
        adw_dialog_present(ADW_DIALOG(dialog), GTK_WIDGET(window));
    }
}

static void show_about_dialog(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action; (void)parameter;
    GtkWindow *window = gtk_application_get_active_window(GTK_APPLICATION(user_data));
    if (window) {
        const char *developers[] = { "Diction Contributors", NULL };
        AdwDialog *dialog = adw_about_dialog_new();
        adw_about_dialog_set_application_name(ADW_ABOUT_DIALOG(dialog), "Diction");
        adw_about_dialog_set_version(ADW_ABOUT_DIALOG(dialog), "0.1.0");
        adw_about_dialog_set_developer_name(ADW_ABOUT_DIALOG(dialog), "Diction Contributors");
        adw_about_dialog_set_developers(ADW_ABOUT_DIALOG(dialog), developers);
        adw_about_dialog_set_copyright(ADW_ABOUT_DIALOG(dialog), "© 2024 Diction Contributors");
        adw_about_dialog_set_license(ADW_ABOUT_DIALOG(dialog), "GPL-3.0-or-later");
        adw_dialog_present(dialog, GTK_WIDGET(window));
    }
}

static void on_sidebar_tab_toggled(GtkToggleButton *btn, gpointer user_data) {
    (void)user_data;
    if (gtk_toggle_button_get_active(btn)) {
        AdwViewStack *stack = g_object_get_data(G_OBJECT(btn), "stack-widget");
        const char *name = g_object_get_data(G_OBJECT(btn), "stack-name");
        if (stack && name) {
            adw_view_stack_set_visible_child_name(stack, name);
        }
    }
}

static void append_entry_to_sidebar(DictEntry *e) {
    fprintf(stderr, "[SIDEBAR] Adding dictionary: '%s' (format: %d)\n", e->name, e->format);
    GtkWidget *row = gtk_list_box_row_new();
    GtkWidget *label = gtk_label_new(e->name);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_widget_set_margin_start(label, 8);
    gtk_widget_set_margin_end(label, 8);
    gtk_widget_set_margin_top(label, 4);
    gtk_widget_set_margin_bottom(label, 4);
    
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), label);
    g_object_set_data(G_OBJECT(row), "dict-entry", e);
    gtk_list_box_append(dict_listbox, row);
}

static void populate_dict_sidebar(void) {
    for (DictEntry *e = all_dicts; e; e = e->next) {
        if (dict_entry_enabled(e)) {
            append_entry_to_sidebar(e);
        }
    }
}

// ------- Async loading infrastructure -------
typedef struct {
    char **dirs;          // NULL-terminated array of directory paths to scan
    int   n_dirs;
    char **manual_paths;  // NULL-terminated array of manually-added dictionary files
    int   n_manual;
} LoadThreadArgs;

// Payload passed from thread to main thread via g_idle_add
typedef struct {
    DictEntry *entry; // single loaded entry (next == NULL on delivery)
    gboolean   done;  // TRUE = loading finished
} LoadIdleData;

static gboolean on_dict_loaded_idle(gpointer user_data) {
    LoadIdleData *ld = user_data;

    if (!ld->done && ld->entry) {
        DictEntry *e = ld->entry;
        e->next = NULL;

        DictConfig *cfg = app_settings ? settings_find_dictionary_by_path(app_settings, e->path) : NULL;
        if (cfg && !cfg->enabled) {
            dict_loader_free(e);
            g_free(ld);
            return G_SOURCE_REMOVE;
        }

        // Check for duplicate in global list (might exist if reload/re-scan happened)
        DictEntry *existing = NULL;
        for (DictEntry *curr = all_dicts; curr; curr = curr->next) {
            if (curr->path && strcmp(curr->path, e->path) == 0) {
                existing = curr;
                break;
            }
        }

        if (existing) {
            // Already there, just update the loaded dict data
            // (The sidebar row already points to this 'existing' entry)
            if (existing->dict) dict_mmap_close(existing->dict);
            existing->dict = e->dict;
            if (e->name) {
                free(existing->name);
                existing->name = strdup(e->name);
            }
            // We can free the 'e' shell now as 'e->dict' is transferred
            e->dict = NULL;
            dict_loader_free(e);
        } else {
            // New unique entry
            if (!all_dicts) {
                all_dicts = e;
            } else {
                DictEntry *last = all_dicts;
                while (last->next) last = last->next;
                last->next = e;
            }
            // Add to sidebar
            if (dict_entry_enabled(e)) {
                append_entry_to_sidebar(e);
            }
        }

        // Auto-select the very first dictionary
        if (all_dicts == e && !active_entry) {
            active_entry = e;
            GtkListBoxRow *first = gtk_list_box_get_row_at_index(dict_listbox, 0);
            if (first) gtk_list_box_select_row(dict_listbox, first);
        }
    }

    if (ld->done) {
        sync_settings_dictionaries_from_loaded();
        populate_groups_sidebar();
        populate_history_sidebar();
        populate_favorites_sidebar();
        populate_search_sidebar(gtk_editable_get_text(GTK_EDITABLE(search_entry)));

        // Loading complete — update welcome page if no dicts found
        if (!all_dicts) {
            webkit_web_view_load_html(web_view,
                "<h2>No Dictionaries Found</h2>"
                "<p>Open <b>Preferences</b> and add a dictionary directory.</p>",
                "file:///");
        } else {
            // Do a random search on startup if nothing is there
            const char *current = gtk_editable_get_text(GTK_EDITABLE(search_entry));
            if (strlen(current) == 0) {
                on_random_clicked(NULL, NULL);
            }
        }
    }

    g_free(ld);
    return G_SOURCE_REMOVE;
}

static void on_dict_found_streaming(DictEntry *e, void *user_data) {
    (void)user_data;
    LoadIdleData *ld = g_new0(LoadIdleData, 1);
    ld->entry = e;
    ld->done  = FALSE;
    g_idle_add(on_dict_loaded_idle, ld);
}

static gpointer dict_load_thread(gpointer user_data) {
    LoadThreadArgs *args = user_data;

    for (int i = 0; i < args->n_manual; i++) {
        const char *path = args->manual_paths[i];
        DictFormat fmt = dict_detect_format(path);
        DictMmap *dict = dict_load_any(path, fmt);
        if (!dict) {
            continue;
        }

        DictEntry *entry = calloc(1, sizeof(DictEntry));
        entry->path = strdup(path);
        entry->format = fmt;
        entry->dict = dict;
        entry->name = dict->name ? strdup(dict->name) : g_path_get_basename(path);
        on_dict_found_streaming(entry, NULL);
    }

    for (int i = 0; i < args->n_dirs; i++) {
        dict_loader_scan_directory_streaming(args->dirs[i], on_dict_found_streaming, NULL);
    }

    // Signal completion
    LoadIdleData *done_ld = g_new0(LoadIdleData, 1);
    done_ld->done = TRUE;
    g_idle_add(on_dict_loaded_idle, done_ld);

    // Free args
    for (int i = 0; i < args->n_dirs; i++)
        g_free(args->dirs[i]);
    g_free(args->dirs);
    for (int i = 0; i < args->n_manual; i++)
        g_free(args->manual_paths[i]);
    g_free(args->manual_paths);
    g_free(args);
    return NULL;
}

static void start_async_dict_loading(void) {
    if (!app_settings)
        return;

    LoadThreadArgs *args = g_new0(LoadThreadArgs, 1);
    args->n_dirs = (int)app_settings->dictionary_dirs->len;
    args->dirs   = g_new(char *, args->n_dirs + 1);
    for (int i = 0; i < args->n_dirs; i++)
        args->dirs[i] = g_strdup(g_ptr_array_index(app_settings->dictionary_dirs, i));
    args->dirs[args->n_dirs] = NULL;

    GPtrArray *manual_paths = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < app_settings->dictionaries->len; i++) {
        DictConfig *cfg = g_ptr_array_index(app_settings->dictionaries, i);
        if (g_strcmp0(cfg->source, "manual") == 0 && cfg->enabled && cfg->path) {
            g_ptr_array_add(manual_paths, g_strdup(cfg->path));
        }
    }
    args->n_manual = (int)manual_paths->len;
    args->manual_paths = g_new0(char *, args->n_manual + 1);
    for (int i = 0; i < args->n_manual; i++) {
        args->manual_paths[i] = g_ptr_array_index(manual_paths, i);
    }
    g_ptr_array_free(manual_paths, FALSE);

    if (args->n_dirs == 0 && args->n_manual == 0) {
        g_free(args->dirs);
        g_free(args->manual_paths);
        g_free(args);
        return;
    }

    GThread *thread = g_thread_new("dict-loader", dict_load_thread, args);
    g_thread_unref(thread); // fire-and-forget
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;
    AdwApplicationWindow *window = ADW_APPLICATION_WINDOW(adw_application_window_new(app));
    gtk_window_set_title(GTK_WINDOW(window), "Diction");
    gtk_window_set_default_size(GTK_WINDOW(window), 1000, 650);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(window), main_box);

    AdwOverlaySplitView *split_view = ADW_OVERLAY_SPLIT_VIEW(adw_overlay_split_view_new());
    gtk_widget_set_vexpand(GTK_WIDGET(split_view), TRUE);
    gtk_box_append(GTK_BOX(main_box), GTK_WIDGET(split_view));

    /* --- Sidebar --- */
    GtkWidget *sidebar_vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);

    /* Sidebar Header */
    GtkWidget *sidebar_header = adw_header_bar_new();
    gtk_widget_add_css_class(sidebar_header, "flat");
    GtkWidget *title_label = gtk_label_new("Diction");
    gtk_widget_add_css_class(title_label, "title");
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(sidebar_header), title_label);

    GtkWidget *random_btn = gtk_button_new_from_icon_name("media-playlist-shuffle-symbolic");
    gtk_widget_add_css_class(random_btn, "flat");
    gtk_widget_set_tooltip_text(random_btn, "Random Headword");
    g_signal_connect(random_btn, "clicked", G_CALLBACK(on_random_clicked), NULL);
    adw_header_bar_pack_start(ADW_HEADER_BAR(sidebar_header), random_btn);

    GtkWidget *settings_btn = gtk_menu_button_new();
    gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(settings_btn), "open-menu-symbolic");
    gtk_widget_add_css_class(settings_btn, "flat");
    GMenu *menu = g_menu_new();
    g_menu_append(menu, "Preferences", "app.settings");
    g_menu_append(menu, "About", "app.about");
    gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(settings_btn), G_MENU_MODEL(menu));
    g_object_unref(menu);
    adw_header_bar_pack_end(ADW_HEADER_BAR(sidebar_header), settings_btn);

    gtk_box_append(GTK_BOX(sidebar_vbox), sidebar_header);

    /* Sidebar Stack */
    AdwViewStack *sidebar_stack = ADW_VIEW_STACK(adw_view_stack_new());
    gtk_widget_set_vexpand(GTK_WIDGET(sidebar_stack), TRUE);

    /* Search/Related Tab */
    GtkWidget *related_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(related_scroll, TRUE);
    gtk_widget_set_hexpand(related_scroll, TRUE);
    related_listbox = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_add_css_class(GTK_WIDGET(related_listbox), "navigation-sidebar");
    gtk_list_box_set_selection_mode(related_listbox, GTK_SELECTION_SINGLE);
    g_signal_connect(related_listbox, "row-selected", G_CALLBACK(on_related_selected), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(related_scroll), GTK_WIDGET(related_listbox));
    adw_view_stack_add_titled_with_icon(sidebar_stack, related_scroll, "search", "Search", "system-search-symbolic");

    /* Dictionaries Tab */
    GtkWidget *dict_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(dict_scroll, TRUE);
    gtk_widget_set_hexpand(dict_scroll, TRUE);
    dict_listbox = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_add_css_class(GTK_WIDGET(dict_listbox), "navigation-sidebar");
    gtk_list_box_set_filter_func(dict_listbox, dict_list_filter_func, NULL, NULL);
    gtk_list_box_set_selection_mode(dict_listbox, GTK_SELECTION_SINGLE);
    g_signal_connect(dict_listbox, "row-selected", G_CALLBACK(on_dict_selected), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(dict_scroll), GTK_WIDGET(dict_listbox));
    adw_view_stack_add_titled_with_icon(sidebar_stack, dict_scroll, "dictionaries", "Dictionaries", "accessories-dictionary-symbolic");

    /* History Tab */
    GtkWidget *history_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(history_scroll, TRUE);
    gtk_widget_set_hexpand(history_scroll, TRUE);
    history_listbox = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_add_css_class(GTK_WIDGET(history_listbox), "navigation-sidebar");
    gtk_list_box_set_selection_mode(history_listbox, GTK_SELECTION_SINGLE);
    g_signal_connect(history_listbox, "row-selected", G_CALLBACK(on_history_selected), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(history_scroll), GTK_WIDGET(history_listbox));
    adw_view_stack_add_titled_with_icon(sidebar_stack, history_scroll, "history", "History", "document-open-recent-symbolic");

    /* Favorites Tab */
    GtkWidget *favorites_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(favorites_scroll, TRUE);
    gtk_widget_set_hexpand(favorites_scroll, TRUE);
    favorites_listbox = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_add_css_class(GTK_WIDGET(favorites_listbox), "navigation-sidebar");
    gtk_list_box_set_selection_mode(favorites_listbox, GTK_SELECTION_SINGLE);
    g_signal_connect(favorites_listbox, "row-selected", G_CALLBACK(on_favorites_selected), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(favorites_scroll), GTK_WIDGET(favorites_listbox));
    adw_view_stack_add_titled_with_icon(sidebar_stack, favorites_scroll, "favorites", "Favorites", "starred-symbolic");

    /* Groups Tab */
    GtkWidget *groups_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(groups_scroll, TRUE);
    gtk_widget_set_hexpand(groups_scroll, TRUE);
    groups_listbox = GTK_LIST_BOX(gtk_list_box_new());
    gtk_widget_add_css_class(GTK_WIDGET(groups_listbox), "navigation-sidebar");
    gtk_list_box_set_selection_mode(groups_listbox, GTK_SELECTION_SINGLE);
    g_signal_connect(groups_listbox, "row-selected", G_CALLBACK(on_groups_selected), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(groups_scroll), GTK_WIDGET(groups_listbox));
    adw_view_stack_add_titled_with_icon(sidebar_stack, groups_scroll, "groups", "Groups", "folder-symbolic");

    gtk_box_append(GTK_BOX(sidebar_vbox), GTK_WIDGET(sidebar_stack));

    /* Custom Bottom Tabs (Python style) */
    GtkWidget *tabs_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_margin_top(tabs_box, 2);
    gtk_widget_set_margin_bottom(tabs_box, 3);
    gtk_widget_set_margin_start(tabs_box, 2);
    gtk_widget_set_margin_end(tabs_box, 2);
    gtk_widget_set_halign(tabs_box, GTK_ALIGN_FILL);
    gtk_widget_add_css_class(tabs_box, "linked");

    const char *tabs[][3] = {
        {"system-search-symbolic", "search", "Search"},
        {"starred-symbolic", "favorites", "Favorites"},
        {"document-open-recent-symbolic", "history", "History"},
        {"folder-symbolic", "groups", "Groups"},
        {"accessories-dictionary-symbolic", "dictionaries", "Dictionaries"}
    };

    GtkWidget *first_btn = NULL;
    for (int i = 0; i < 5; i++) {
        GtkWidget *btn = gtk_toggle_button_new();
        gtk_button_set_icon_name(GTK_BUTTON(btn), tabs[i][0]);
        gtk_widget_set_tooltip_text(btn, tabs[i][2]);
        gtk_widget_add_css_class(btn, "flat");
        gtk_widget_set_hexpand(btn, TRUE);
        
        if (i == 0) {
            first_btn = btn;
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(btn), TRUE);
        } else {
            gtk_toggle_button_set_group(GTK_TOGGLE_BUTTON(btn), GTK_TOGGLE_BUTTON(first_btn));
        }

        g_object_set_data_full(G_OBJECT(btn), "stack-name", g_strdup(tabs[i][1]), g_free);
        g_object_set_data(G_OBJECT(btn), "stack-widget", sidebar_stack);
        
        g_signal_connect(btn, "toggled", G_CALLBACK(on_sidebar_tab_toggled), NULL);

        gtk_box_append(GTK_BOX(tabs_box), btn);
    }
    gtk_widget_add_css_class(tabs_box, "sidebar-tabs");
    gtk_box_append(GTK_BOX(sidebar_vbox), tabs_box);

    adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(split_view), sidebar_vbox);

    /* --- Content --- */
    AdwToolbarView *toolbar_view = ADW_TOOLBAR_VIEW(adw_toolbar_view_new());
    
    GtkWidget *content_header = adw_header_bar_new();
    gtk_widget_add_css_class(content_header, "content-header");
    adw_toolbar_view_add_top_bar(toolbar_view, content_header);

    /* Sidebar toggle */
    GtkWidget *sidebar_toggle = gtk_toggle_button_new();
    gtk_button_set_icon_name(GTK_BUTTON(sidebar_toggle), "sidebar-show-symbolic");
    g_object_bind_property(split_view, "show-sidebar", sidebar_toggle, "active", G_BINDING_BIDIRECTIONAL | G_BINDING_SYNC_CREATE);
    adw_header_bar_pack_start(ADW_HEADER_BAR(content_header), sidebar_toggle);

    search_entry = GTK_SEARCH_ENTRY(gtk_search_entry_new());
    gtk_widget_set_size_request(GTK_WIDGET(search_entry), 350, -1);
    adw_header_bar_set_title_widget(ADW_HEADER_BAR(content_header), GTK_WIDGET(search_entry));
    g_signal_connect(search_entry, "search-changed", G_CALLBACK(on_search_changed), NULL);

    favorite_toggle_btn = gtk_button_new_from_icon_name("non-starred-symbolic");
    gtk_widget_add_css_class(favorite_toggle_btn, "flat");
    g_signal_connect(favorite_toggle_btn, "clicked", G_CALLBACK(on_favorite_toggle_clicked), NULL);
    adw_header_bar_pack_end(ADW_HEADER_BAR(content_header), favorite_toggle_btn);

    /* WebKit view */
    web_view = WEBKIT_WEB_VIEW(webkit_web_view_new());
    WebKitSettings *web_settings = webkit_web_view_get_settings(web_view);
    webkit_settings_set_auto_load_images(web_settings, TRUE);
    webkit_settings_set_allow_file_access_from_file_urls(web_settings, TRUE);
    webkit_settings_set_allow_universal_access_from_file_urls(web_settings, TRUE);

    /* Handle internal dict:// links */
    g_signal_connect(web_view, "decide-policy", G_CALLBACK(on_decide_policy), search_entry);

    GtkWidget *web_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(web_scroll, TRUE);
    gtk_widget_set_hexpand(web_scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(web_scroll), GTK_WIDGET(web_view));
    
    adw_toolbar_view_set_content(toolbar_view, web_scroll);
    adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(split_view), GTK_WIDGET(toolbar_view));

    /* Populate sidebar */
    populate_dict_sidebar();
    populate_history_sidebar();
    populate_favorites_sidebar();
    populate_groups_sidebar();
    populate_search_sidebar(NULL);
    refresh_favorite_button_state();

    /* Auto-select first dictionary */
    if (all_dicts) {
        active_entry = all_dicts;
        GtkListBoxRow *first = gtk_list_box_get_row_at_index(dict_listbox, 0);
        if (first) gtk_list_box_select_row(dict_listbox, first);
    }

    // Initialize style manager for theme support
    style_manager = adw_style_manager_get_default();
    g_signal_connect(style_manager, "notify::dark", G_CALLBACK(on_theme_changed), NULL);

    GtkCssProvider *css_provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(css_provider,
        ".sidebar-tabs { border-top: 0px solid alpha(@theme_fg_color, 0.1); }"
        ".sidebar-tabs button { padding-left: 12px; padding-right: 12px; padding-top: 8px; padding-bottom: 8px; margin-left:  0.5px; margin-right: 0.5px; min-height: 0; min-width: 0; border: none; border-radius: 10px; }"
        ".sidebar-tabs button image { opacity: 0.7; }"
        ".sidebar-tabs button:checked { background: alpha(@theme_fg_color, 0.1); }"
        ".sidebar-tabs button:checked image { opacity: 1.0; }"
        ".navigation-sidebar { background: transparent; }"
        ".navigation-sidebar listitem:hover, .navigation-sidebar row:hover { background: alpha(@theme_fg_color, 0.05); }"
        ".navigation-sidebar listitem:selected, .navigation-sidebar row:selected { background: alpha(@theme_fg_color, 0.1); color: inherit; }"
        ".content-header { background: @window_bg_color; }"
        ".menu-item { font-weight: normal; padding: 4px 8px; min-height: 0; }"
    );
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(css_provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    // Apply saved theme preference
    if (app_settings && app_settings->theme) {
        if (strcmp(app_settings->theme, "light") == 0) {
            adw_style_manager_set_color_scheme(style_manager, ADW_COLOR_SCHEME_FORCE_LIGHT);
        } else if (strcmp(app_settings->theme, "dark") == 0) {
            adw_style_manager_set_color_scheme(style_manager, ADW_COLOR_SCHEME_FORCE_DARK);
        } else {
            adw_style_manager_set_color_scheme(style_manager, ADW_COLOR_SCHEME_DEFAULT);
        }
    }

    // Apply initial theme to webview
    GdkRGBA bg_color;
    int dark_mode = adw_style_manager_get_dark(style_manager) ? 1 : 0;
    if (dark_mode) {
        gdk_rgba_parse(&bg_color, "#1e1e1e");
    } else {
        gdk_rgba_parse(&bg_color, "#ffffff");
    }
    webkit_web_view_set_background_color(web_view, &bg_color);

    /* Show the window FIRST, then start background loading */
    webkit_web_view_load_html(web_view,
        "<html><body style='font-family: sans-serif; text-align: center; margin-top: 3em; opacity: 0.6;'>"
        "<h2>Loading dictionaries…</h2><p>Please wait.</p>"
        "</body></html>", "file:///");

    gtk_window_present(GTK_WINDOW(window));

    // Start async loading if we have settings-based dirs
    if (!all_dicts) {
        start_async_dict_loading();
    } else {
        // CLI-mode: dicts already loaded synchronously, just populate
        populate_dict_sidebar();
        if (all_dicts) {
            active_entry = all_dicts;
            GtkListBoxRow *first = gtk_list_box_get_row_at_index(dict_listbox, 0);
            if (first) gtk_list_box_select_row(dict_listbox, first);
        }
        webkit_web_view_load_html(web_view,
            "<h2>Welcome to Diction</h2>"
            "<p>Select a dictionary from the sidebar and start searching.</p>", "file:///");
    }
}

int main(int argc, char *argv[]) {
    // Disable compositing to fix rendering issues
    setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", 1);
    // Seed random number generator
    srand(time(NULL));
    // Load settings first
    app_settings = settings_load();
    active_scope_id = g_strdup("all");
    history_words = load_word_list(HISTORY_FILE_NAME, 200);
    favorite_words = load_word_list(FAVORITES_FILE_NAME, 0);

    /* Load dictionaries only in CLI mode (single file or directory argument).      *
     * When running with no arguments, loading happens async after window is shown. */
    if (argc > 1) {
        struct stat st;
        if (stat(argv[1], &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                all_dicts = dict_loader_scan_directory(argv[1]);
            } else {
                /* Single file mode */
                DictFormat fmt = dict_detect_format(argv[1]);
                DictMmap *d = dict_load_any(argv[1], fmt);
                if (d) {
                    DictEntry *e = calloc(1, sizeof(DictEntry));
                    const char *slash = strrchr(argv[1], '/');
                    const char *base = slash ? slash + 1 : argv[1];
                    e->name = strdup(base);
                    e->path = strdup(argv[1]);
                    e->format = fmt;
                    e->dict = d;
                    all_dicts = e;
                }
            }
        }
    }
    /* No else: settings-based dirs are loaded async in on_activate */

    AdwApplication *app = adw_application_new("org.diction.App", G_APPLICATION_DEFAULT_FLAGS);

    // Add settings and about actions
    GSimpleAction *settings_action = g_simple_action_new("settings", NULL);
    g_signal_connect(settings_action, "activate", G_CALLBACK(show_settings_dialog), app);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(settings_action));
    g_object_unref(settings_action);

    GSimpleAction *about_action = g_simple_action_new("about", NULL);
    g_signal_connect(about_action, "activate", G_CALLBACK(show_about_dialog), app);
    g_action_map_add_action(G_ACTION_MAP(app), G_ACTION(about_action));
    g_object_unref(about_action);

    g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);

    char *empty[] = { argv[0], NULL };
    int status = g_application_run(G_APPLICATION(app), 1, empty);

    // Save settings on exit
    if (app_settings) {
        settings_save(app_settings);
        settings_free(app_settings);
    }
    if (search_execute_source_id != 0) {
        g_source_remove(search_execute_source_id);
    }
    cancel_sidebar_search();
    free_word_list(&history_words);
    free_word_list(&favorite_words);
    g_free(active_scope_id);
    g_free(last_search_query);

    g_object_unref(app);
    dict_loader_free(all_dicts);

    return status;
}
