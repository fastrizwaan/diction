#include "dict-mmap.h"
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <string.h>
#include <glib.h>

char* wiki_http_get(const char *url, GError **error) {
    SoupSession *session = soup_session_new();
    soup_session_set_user_agent(session, "Diction/0.1.0 (https://github.com/fastrizwaan/diction)");

    SoupMessage *msg = soup_message_new("GET", url);
    if (!msg) {
        g_object_unref(session);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Failed to create SoupMessage");
        return NULL;
    }

    GBytes *bytes = soup_session_send_and_read(session, msg, NULL, error);
    g_object_unref(msg);
    g_object_unref(session);

    if (!bytes) {
        return NULL;
    }

    gsize size;
    const char *data = g_bytes_get_data(bytes, &size);
    char *res = g_strndup(data, size);
    g_bytes_unref(bytes);
    return res;
}

DictMmap* parse_wiki_file(const char *path, volatile gint *cancel_flag, gint expected) {
    (void)cancel_flag;
    (void)expected;

    GKeyFile *key_file = g_key_file_new();
    GError *error = NULL;
    if (!g_key_file_load_from_file(key_file, path, G_KEY_FILE_NONE, &error)) {
        g_printerr("Failed to load wiki file %s: %s\n", path, error->message);
        g_clear_error(&error);
        g_key_file_free(key_file);
        return NULL;
    }

    char *name = g_key_file_get_string(key_file, "Wiki", "Name", NULL);
    char *url = g_key_file_get_string(key_file, "Wiki", "Url", NULL);
    char *lang = g_key_file_get_string(key_file, "Wiki", "Lang", NULL);
    g_key_file_free(key_file);

    if (!url) {
        g_printerr("Wiki file %s is missing Url field\n", path);
        g_free(name);
        g_free(lang);
        return NULL;
    }

    DictMmap *dict = g_new0(DictMmap, 1);
    dict->name = name ? name : g_path_get_basename(path);
    dict->wiki_url = url;
    dict->wiki_lang = lang ? lang : g_strdup("en");
    dict->index = NULL;

    return dict;
}

GList* wiki_prefix_search(DictMmap *dict, const char *prefix, GError **error) {
    if (!dict || !dict->wiki_url || !prefix || !*prefix) return NULL;

    char *escaped_prefix = g_uri_escape_string(prefix, NULL, TRUE);
    
    char *api_base;
    if (g_str_has_suffix(dict->wiki_url, "/api.php")) {
        api_base = g_strdup(dict->wiki_url);
    } else if (g_str_has_suffix(dict->wiki_url, "/w/api.php")) {
        api_base = g_strdup(dict->wiki_url);
    } else {
        char *base = g_strdup(dict->wiki_url);
        gsize len = strlen(base);
        if (len > 0 && base[len-1] == '/') {
            base[len-1] = '\0';
        }
        api_base = g_strconcat(base, "/w/api.php", NULL);
        g_free(base);
    }

    char *url = g_strdup_printf("%s?action=query&list=allpages&aplimit=40&format=json&apprefix=%s", api_base, escaped_prefix);
    g_free(api_base);
    g_free(escaped_prefix);

    char *response = wiki_http_get(url, error);
    g_free(url);

    if (!response) {
        return NULL;
    }

    GList *results = NULL;
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, response, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *root_obj = json_node_get_object(root);
            JsonObject *query_obj = json_object_get_object_member(root_obj, "query");
            if (query_obj) {
                JsonArray *ap_array = json_object_get_array_member(query_obj, "allpages");
                if (ap_array) {
                    for (guint i = 0; i < json_array_get_length(ap_array); i++) {
                        JsonObject *page_obj = json_array_get_object_element(ap_array, i);
                        const char *title = json_object_get_string_member(page_obj, "title");
                        if (title) {
                            results = g_list_append(results, g_strdup(title));
                        }
                    }
                }
            }
        }
    }
    g_object_unref(parser);
    g_free(response);

    return results;
}

char* wiki_clean_html(const char *base_url, const char *raw_html) {
    if (!raw_html) return NULL;

    GRegex *regex = g_regex_new("<a\\s+[^>]*href=\"/wiki/(?!(?:File|Image|Special|Media|Fichier|Datei|Archivo|Ficheiro):)([^\"]+)\"", G_REGEX_CASELESS, 0, NULL);
    char *step1 = g_regex_replace(regex, raw_html, -1, 0, "<a href=\"dict://\\1\"", 0, NULL);
    g_regex_unref(regex);

    GRegex *redlink_regex = g_regex_new("<a\\s+[^>]*href=\"/w/index\\.php\\?title=([^&\"]+)([^&\"]*redlink=1[^\"]*)\"", G_REGEX_CASELESS, 0, NULL);
    char *step1b = g_regex_replace(redlink_regex, step1, -1, 0, "<a href=\"dict://\\1\\2\"", 0, NULL);
    g_regex_unref(redlink_regex);
    g_free(step1);

    GRegex *proto_regex = g_regex_new("(href|src|srcset)=\"//", G_REGEX_CASELESS, 0, NULL);
    char *step2 = g_regex_replace(proto_regex, step1b, -1, 0, "\\1=\"https://", 0, NULL);
    g_regex_unref(proto_regex);
    g_free(step1b);

    char *base_domain = g_strdup(base_url);
    char *proto_end = strstr(base_domain, "://");
    if (proto_end) {
        char *slash = strchr(proto_end + 3, '/');
        if (slash) *slash = '\0';
    }
    gsize bd_len = strlen(base_domain);
    if (bd_len > 0 && base_domain[bd_len-1] == '/') {
        base_domain[bd_len-1] = '\0';
    }

    char *domain_prefix = g_strdup_printf("\\1=\"%s/", base_domain);
    GRegex *rel_url_regex = g_regex_new("(href|src|srcset)=\"/(?!/)", G_REGEX_CASELESS, 0, NULL);
    char *step3 = g_regex_replace(rel_url_regex, step2, -1, 0, domain_prefix, 0, NULL);
    g_regex_unref(rel_url_regex);
    g_free(domain_prefix);
    g_free(base_domain);
    g_free(step2);

    char *wrapped = g_strdup_printf("<div class=\"mwiki\">%s</div>", step3);
    g_free(step3);

    return wrapped;
}

char* wiki_fetch_article(DictMmap *dict, const char *query, GError **error) {
    if (!dict || !dict->wiki_url || !query || !*query) return NULL;

    char *escaped_page = g_uri_escape_string(query, NULL, TRUE);
    
    char *api_base;
    if (g_str_has_suffix(dict->wiki_url, "/api.php")) {
        api_base = g_strdup(dict->wiki_url);
    } else if (g_str_has_suffix(dict->wiki_url, "/w/api.php")) {
        api_base = g_strdup(dict->wiki_url);
    } else {
        char *base = g_strdup(dict->wiki_url);
        gsize len = strlen(base);
        if (len > 0 && base[len-1] == '/') {
            base[len-1] = '\0';
        }
        api_base = g_strconcat(base, "/w/api.php", NULL);
        g_free(base);
    }

    char *url = g_strdup_printf("%s?action=parse&prop=text&format=json&redirects=1&page=%s", api_base, escaped_page);
    g_free(api_base);
    g_free(escaped_page);

    char *response = wiki_http_get(url, error);
    g_free(url);

    if (!response) {
        return NULL;
    }

    char *html = NULL;
    JsonParser *parser = json_parser_new();
    if (json_parser_load_from_data(parser, response, -1, NULL)) {
        JsonNode *root = json_parser_get_root(parser);
        if (JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject *root_obj = json_node_get_object(root);
            JsonObject *parse_obj = json_object_get_object_member(root_obj, "parse");
            if (parse_obj) {
                JsonObject *text_obj = json_object_get_object_member(parse_obj, "text");
                if (text_obj) {
                    const char *raw_html = json_object_get_string_member(text_obj, "*");
                    if (raw_html) {
                        html = wiki_clean_html(dict->wiki_url, raw_html);
                    }
                }
            }
        }
    }
    g_object_unref(parser);
    g_free(response);

    return html;
}
