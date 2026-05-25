#include "search-provider-generated.h"
#include "flat-index.h"
#include "dict-loader.h"
#include "settings.h"
#include "search-utils.h"
#include "text-utils.h"
#include <gio/gio.h>
#include <string.h>
#include <ctype.h>
#include <sys/resource.h>

static GPtrArray *active_dicts = NULL;
static AppSettings *app_settings = NULL;
GMutex dict_loader_mutex;
static GMainLoop *loop = NULL;
static guint idle_timeout_id = 0;

void settings_scan_progress_notify(const char *path, int percent) {
    (void)path; (void)percent;
}

void settings_scan_notify(const char *name, const char *path, int event_type) {
    (void)name; (void)path; (void)event_type;
}

static gboolean on_idle_timeout(gpointer data) {
    (void)data;
    if (loop) g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static void reset_idle_timeout(void) {
    if (idle_timeout_id != 0) {
        g_source_remove(idle_timeout_id);
    }
    idle_timeout_id = g_timeout_add_seconds(10, on_idle_timeout, NULL);
}

static void on_settings_changed(GFileMonitor *monitor, GFile *file, GFile *other_file, GFileMonitorEvent event_type, gpointer user_data) {
    (void)monitor; (void)file; (void)other_file; (void)user_data;
    if (event_type == G_FILE_MONITOR_EVENT_CHANGED || 
        event_type == G_FILE_MONITOR_EVENT_CREATED || 
        event_type == G_FILE_MONITOR_EVENT_DELETED) {
        if (active_dicts) {
            g_ptr_array_free(active_dicts, TRUE);
            active_dicts = NULL;
        }
        if (app_settings) {
            settings_free(app_settings);
            app_settings = NULL;
        }
    }
}

static void load_dictionaries(void) {
    if (active_dicts) return;
    
    app_settings = settings_load();
    active_dicts = g_ptr_array_new_with_free_func((GDestroyNotify)dict_loader_free_list);
    
    if (!app_settings) return;
    
    if (app_settings->dictionary_dirs) {
        for (guint i = 0; i < app_settings->dictionary_dirs->len; i++) {
            const char *dir_path = g_ptr_array_index(app_settings->dictionary_dirs, i);
            DictEntry *head = dict_loader_scan_directory(dir_path);
            if (head) {
                DictEntry *curr = head;
                while (curr) {
                    if (settings_dictionary_enabled_by_path(app_settings, curr->path, TRUE)) {
                        if (!curr->dict) {
                            curr->dict = dict_load_any(curr->path, curr->format, NULL, 0);
                        }
                    }
                    curr = curr->next;
                }
                g_ptr_array_add(active_dicts, head);
            }
        }
    }
    
    // Load manual/imported dictionaries not covered by directory scans
    if (app_settings->dictionaries) {
        for (guint i = 0; i < app_settings->dictionaries->len; i++) {
            DictConfig *cfg = g_ptr_array_index(app_settings->dictionaries, i);
            if (cfg && cfg->enabled && (g_strcmp0(cfg->source, "manual") == 0 || g_strcmp0(cfg->source, "imported") == 0)) {
                DictEntry *entry = g_new0(DictEntry, 1);
                entry->path = g_strdup(cfg->path);
                entry->dict_id = g_strdup(cfg->id);
                entry->name = g_strdup(cfg->name);
                entry->format = cfg->format;
                entry->dict = dict_load_any(entry->path, entry->format, NULL, 0);
                g_ptr_array_add(active_dicts, entry);
            }
        }
    }
}

// Helper to strip HTML tags for snippet description
static char* create_snippet(const char *html_def, size_t max_len) {
    if (!html_def) return g_strdup("");
    
    char *clean = g_malloc(strlen(html_def) + 1);
    char *dst = clean;
    const char *src = html_def;
    gboolean in_tag = FALSE;
    
    while (*src) {
        if (*src == '<') in_tag = TRUE;
        else if (*src == '>') in_tag = FALSE;
        else if (!in_tag) {
            if (*src == '\n' || *src == '\r' || *src == '\t') {
                *dst++ = ' ';
            } else {
                *dst++ = *src;
            }
        }
        src++;
    }
    *dst = '\0';
    
    g_strstrip(clean);
    
    if (strlen(clean) > max_len) {
        clean[max_len - 3] = '.';
        clean[max_len - 2] = '.';
        clean[max_len - 1] = '.';
        clean[max_len] = '\0';
    }
    
    return clean;
}

static gboolean handle_get_initial_result_set(
    DictionSearchProviderSearchProvider2 *object,
    GDBusMethodInvocation *invocation,
    const gchar *const *terms,
    gpointer user_data)
{
    (void)object;
    (void)user_data;
    
    reset_idle_timeout();
    load_dictionaries();
    
    char *query = g_strjoinv(" ", (gchar **)terms);
    g_strstrip(query);
    
    GPtrArray *results = g_ptr_array_new_with_free_func(g_free);
    
    if (strlen(query) >= 2) {
        gboolean limit_reached = FALSE;
        
        for (guint i = 0; i < active_dicts->len && !limit_reached; i++) {
            DictEntry *curr = g_ptr_array_index(active_dicts, i);
            while (curr && !limit_reached) {
                if (curr->dict && curr->dict->index) {
                    FlatIndex *idx = curr->dict->index;
                    size_t match_pos = flat_index_search_fast(idx, query);
                    if (match_pos != (size_t)-1) {
                        size_t term_len = 0;
                        char *term = flat_index_get_headword(idx, match_pos, &term_len);
                        if (term) {
                            // Add result ID as dictionary_id::term
                            char *result_id = g_strdup_printf("%s::%s", curr->dict_id ? curr->dict_id : "unknown", term);
                            g_ptr_array_add(results, result_id);
                            g_free(term);
                            
                            if (results->len >= 5) {
                                limit_reached = TRUE;
                            }
                        }
                    }
                }
                curr = curr->next;
            }
        }
    }
    
    g_ptr_array_add(results, NULL); // NULL-terminate for strv
    
    gchar **results_strv = (gchar **)g_ptr_array_free(results, FALSE);
    diction_search_provider_search_provider2_complete_get_initial_result_set(
        object, invocation, (const gchar *const *)results_strv);
        
    g_strfreev(results_strv);
    g_free(query);
    
    return TRUE;
}

static gboolean handle_get_subsearch_result_set(
    DictionSearchProviderSearchProvider2 *object,
    GDBusMethodInvocation *invocation,
    const gchar *const *previous_results,
    const gchar *const *terms,
    gpointer user_data)
{
    (void)previous_results;
    return handle_get_initial_result_set(object, invocation, terms, user_data);
}

static gboolean handle_get_result_metas(
    DictionSearchProviderSearchProvider2 *object,
    GDBusMethodInvocation *invocation,
    const gchar *const *identifiers,
    gpointer user_data)
{
    (void)object;
    (void)user_data;
    
    reset_idle_timeout();
    load_dictionaries();
    
    GVariantBuilder builder;
    g_variant_builder_init(&builder, G_VARIANT_TYPE("aa{sv}"));
    
    for (int i = 0; identifiers[i] != NULL; i++) {
        const char *id_str = identifiers[i];
        char **parts = g_strsplit(id_str, "::", 2);
        if (g_strv_length(parts) == 2) {
            const char *dict_id = parts[0];
            const char *term = parts[1];
            
            // Find definition
            const char *snippet = "";
            char *to_free = NULL;
            char *clean_snippet = NULL;
            
            for (guint j = 0; j < active_dicts->len; j++) {
                DictEntry *curr = g_ptr_array_index(active_dicts, j);
                while (curr) {
                    if (g_strcmp0(curr->dict_id, dict_id) == 0 && curr->dict && curr->dict->index) {
                        size_t match_pos = flat_index_search_fast(curr->dict->index, term);
                        if (match_pos != (size_t)-1) {
                            const FlatTreeEntry *entry = flat_index_get(curr->dict->index, match_pos);
                            if (entry) {
                                size_t def_len = 0;
                                const char *def = dict_get_definition(curr->dict, entry, &def_len, &to_free);
                                if (def) {
                                    clean_snippet = create_snippet(def, 150);
                                    snippet = clean_snippet;
                                }
                            }
                        }
                        break;
                    }
                    curr = curr->next;
                }
                if (clean_snippet) break;
            }
            
            g_variant_builder_open(&builder, G_VARIANT_TYPE("a{sv}"));
            g_variant_builder_add(&builder, "{sv}", "id", g_variant_new_string(id_str));
            g_variant_builder_add(&builder, "{sv}", "name", g_variant_new_string(term));
            g_variant_builder_add(&builder, "{sv}", "description", g_variant_new_string(snippet));
            
            // Icon
            GIcon *icon = g_themed_icon_new("io.github.fastrizwaan.diction");
            g_variant_builder_add(&builder, "{sv}", "icon", g_icon_serialize(icon));
            g_object_unref(icon);
            
            g_variant_builder_close(&builder);
            
            g_free(to_free);
            g_free(clean_snippet);
        }
        g_strfreev(parts);
    }
    
    GVariant *metas = g_variant_builder_end(&builder);
    diction_search_provider_search_provider2_complete_get_result_metas(object, invocation, metas);
    
    return TRUE;
}

static gboolean handle_activate_result(
    DictionSearchProviderSearchProvider2 *object,
    GDBusMethodInvocation *invocation,
    const gchar *identifier,
    const gchar *const *terms,
    guint timestamp,
    gpointer user_data)
{
    (void)object;
    (void)terms;
    (void)timestamp;
    (void)user_data;
    
    reset_idle_timeout();
    
    char **parts = g_strsplit(identifier, "::", 2);
    if (g_strv_length(parts) == 2) {
        const char *term = parts[1];
        char *command = g_strdup_printf("diction --search \"%s\"", term);
        g_spawn_command_line_async(command, NULL);
        g_free(command);
    }
    g_strfreev(parts);
    
    diction_search_provider_search_provider2_complete_activate_result(object, invocation);
    return TRUE;
}

static gboolean handle_launch_search(
    DictionSearchProviderSearchProvider2 *object,
    GDBusMethodInvocation *invocation,
    const gchar *const *terms,
    guint timestamp,
    gpointer user_data)
{
    (void)object;
    (void)timestamp;
    (void)user_data;
    
    reset_idle_timeout();
    
    char *query = g_strjoinv(" ", (gchar **)terms);
    char *command = g_strdup_printf("diction --scan \"%s\"", query);
    g_spawn_command_line_async(command, NULL);
    g_free(command);
    g_free(query);
    
    diction_search_provider_search_provider2_complete_launch_search(object, invocation);
    return TRUE;
}

static void on_bus_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data) {
    (void)name;
    (void)user_data;
    
    DictionSearchProviderSearchProvider2 *skeleton = diction_search_provider_search_provider2_skeleton_new();
    
    g_signal_connect(skeleton, "handle-get-initial-result-set", G_CALLBACK(handle_get_initial_result_set), NULL);
    g_signal_connect(skeleton, "handle-get-subsearch-result-set", G_CALLBACK(handle_get_subsearch_result_set), NULL);
    g_signal_connect(skeleton, "handle-get-result-metas", G_CALLBACK(handle_get_result_metas), NULL);
    g_signal_connect(skeleton, "handle-activate-result", G_CALLBACK(handle_activate_result), NULL);
    g_signal_connect(skeleton, "handle-launch-search", G_CALLBACK(handle_launch_search), NULL);
    
    GError *error = NULL;
    if (!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(skeleton), connection, "/io/github/fastrizwaan/diction/SearchProvider", &error)) {
        g_printerr("Failed to export SearchProvider skeleton: %s\n", error->message);
        g_error_free(error);
    }
}

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    
    // Maximize file descriptor limit
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
    
    loop = g_main_loop_new(NULL, FALSE);
    reset_idle_timeout();
    
    /* Setup file monitor to clear cache on settings change */
    char *settings_path = g_build_filename(g_get_user_data_dir(), "diction", "settings.json", NULL);
    GFile *settings_file = g_file_new_for_path(settings_path);
    GFileMonitor *settings_monitor = g_file_monitor_file(settings_file, G_FILE_MONITOR_NONE, NULL, NULL);
    if (settings_monitor) {
        g_signal_connect(settings_monitor, "changed", G_CALLBACK(on_settings_changed), NULL);
    }
    
    g_bus_own_name(G_BUS_TYPE_SESSION,
                   "io.github.fastrizwaan.diction.SearchProvider",
                   G_BUS_NAME_OWNER_FLAGS_NONE,
                   on_bus_acquired,
                   NULL,
                   NULL,
                   NULL,
                   NULL);
                   
    g_main_loop_run(loop);
    
    if (settings_monitor) g_object_unref(settings_monitor);
    g_object_unref(settings_file);
    g_free(settings_path);
    
    if (active_dicts) {
        g_ptr_array_free(active_dicts, TRUE);
    }
    if (app_settings) {
        settings_free(app_settings);
    }
    
    return 0;
}
