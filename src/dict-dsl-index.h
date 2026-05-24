#ifndef DICT_DSL_INDEX_H
#define DICT_DSL_INDEX_H

#include <glib.h>
#include <stdint.h>

/* Build a SQLite headword index for the given DSL dictionary.
 * The index database is stored at ~/.cache/diction/hw/<sha1-of-dsl_path>.
 * Returns TRUE on success. */
gboolean build_dsl_index_only_cache(const char *dsl_path, volatile gint *cancel_flag, gint expected);

/* Parse name and languages from DSL header without building a full index. */
void dsl_parse_header_internal(const char *path, char **out_name, char **out_source_lang, char **out_target_lang);

#endif
