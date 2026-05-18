import re

with open('src/dict-loader.c', 'r') as f:
    content = f.read()

# Replace "const char* dict_get_definition(" with "static const char* dict_get_definition_raw("
content = content.replace(
    "const char* dict_get_definition(DictMmap *dict, const FlatTreeEntry *entry, size_t *out_len, char **out_to_free) {",
    "static const char* dict_get_definition_raw(DictMmap *dict, const FlatTreeEntry *entry, size_t *out_len, char **out_to_free) {"
)

# Insert the wrapper function just before the next function definition (which is dict_fts_query_candidates, or dict_search_fts)
# Or we can just insert it at the end of dict_get_definition_raw.
# The original dict_get_definition ends at:
#     /* 4. Fallback (if using old in-memory ->data logic) */
#     return dict->data + entry->d_off;
# }
# So we can search for that.

end_marker = "    return dict->data + entry->d_off;\n}"

wrapper = """    return dict->data + entry->d_off;
}

const char* dict_get_definition(DictMmap *dict, const FlatTreeEntry *entry, size_t *out_len, char **out_to_free) {
    size_t raw_len = 0;
    char *raw_to_free = NULL;
    const char *raw_ptr = dict_get_definition_raw(dict, entry, &raw_len, &raw_to_free);
    
    if (!raw_ptr) {
        if (out_len) *out_len = 0;
        if (out_to_free) *out_to_free = NULL;
        return NULL;
    }
    
    if (g_utf8_validate(raw_ptr, raw_len, NULL)) {
        if (out_len) *out_len = raw_len;
        if (out_to_free) *out_to_free = raw_to_free;
        return raw_ptr;
    }
    
    /* Enforce valid UTF-8 to prevent WebKit and SQLite crashes */
    char *valid_str = g_utf8_make_valid(raw_ptr, raw_len);
    if (raw_to_free) g_free(raw_to_free);
    
    if (out_len) *out_len = strlen(valid_str);
    if (out_to_free) *out_to_free = valid_str;
    return valid_str;
}
"""

if end_marker in content:
    content = content.replace(end_marker, wrapper)
    with open('src/dict-loader.c', 'w') as f:
        f.write(content)
    print("Patched dict_get_definition to enforce valid UTF-8.")
else:
    print("Failed to find end_marker.")
