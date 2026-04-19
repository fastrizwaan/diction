#pragma once

#include <glib.h>

char *langpair_normalize_language_name(const char *raw);
void langpair_fill_missing(char **source_lang,
                           char **target_lang,
                           const char *metadata_text,
                           const char *path);
char *langpair_guess_group_from_metadata(const char *metadata_text, const char *path);
char *langpair_build_group_name(const char *source_lang, const char *target_lang);
