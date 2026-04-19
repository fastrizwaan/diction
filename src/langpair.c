#include "langpair.h"

#include <string.h>

typedef struct {
    const char *alias;
    const char *canonical;
} LanguageAlias;

typedef struct {
    int han;
    int hiragana;
    int katakana;
    int hangul;
    int arabic;
    int hebrew;
    int devanagari;
    int cyrillic;
    int greek;
    int latin;
} ScriptCounts;

typedef struct {
    const char *canonical;
    gssize pos;
} LanguageHit;

static const LanguageAlias language_aliases[] = {
    { "english", "English" },
    { "en", "English" },
    { "eng", "English" },
    { "american english", "English" },
    { "british english", "English" },
    { "hindi", "Hindi" },
    { "hi", "Hindi" },
    { "hin", "Hindi" },
    { "हिन्दी", "Hindi" },
    { "हिंदी", "Hindi" },
    { "arabic", "Arabic" },
    { "ar", "Arabic" },
    { "ara", "Arabic" },
    { "عربي", "Arabic" },
    { "عربى", "Arabic" },
    { "hebrew", "Hebrew" },
    { "he", "Hebrew" },
    { "heb", "Hebrew" },
    { "iw", "Hebrew" },
    { "עברית", "Hebrew" },
    { "japanese", "Japanese" },
    { "ja", "Japanese" },
    { "jpn", "Japanese" },
    { "nihongo", "Japanese" },
    { "日本語", "Japanese" },
    { "chinese", "Chinese" },
    { "zh", "Chinese" },
    { "zho", "Chinese" },
    { "chi", "Chinese" },
    { "mandarin", "Chinese" },
    { "simplified chinese", "Chinese" },
    { "traditional chinese", "Chinese" },
    { "中文", "Chinese" },
    { "汉语", "Chinese" },
    { "漢語", "Chinese" },
    { "korean", "Korean" },
    { "ko", "Korean" },
    { "kor", "Korean" },
    { "hangul", "Korean" },
    { "한국어", "Korean" },
    { "russian", "Russian" },
    { "ru", "Russian" },
    { "rus", "Russian" },
    { "русский", "Russian" },
    { "greek", "Greek" },
    { "el", "Greek" },
    { "ell", "Greek" },
    { "ελληνικά", "Greek" },
    { "french", "French" },
    { "fr", "French" },
    { "fra", "French" },
    { "fre", "French" },
    { "français", "French" },
    { "spanish", "Spanish" },
    { "es", "Spanish" },
    { "spa", "Spanish" },
    { "español", "Spanish" },
    { "german", "German" },
    { "de", "German" },
    { "deu", "German" },
    { "ger", "German" },
    { "deutsch", "German" },
    { "turkish", "Turkish" },
    { "tr", "Turkish" },
    { "tur", "Turkish" },
    { NULL, NULL }
};

static gboolean is_token_char(gunichar ch) {
    return g_unichar_isalnum(ch) || ch == '_';
}

static gboolean is_boundary_before(const char *text_start, const char *pos) {
    if (pos <= text_start) {
        return TRUE;
    }

    const char *prev = g_utf8_prev_char(pos);
    return !is_token_char(g_utf8_get_char(prev));
}

static gboolean is_boundary_after(const char *after_pos) {
    if (!after_pos || *after_pos == '\0') {
        return TRUE;
    }

    return !is_token_char(g_utf8_get_char(after_pos));
}

static gssize find_alias_position(const char *haystack_cf, const char *alias_cf) {
    if (!haystack_cf || !alias_cf || !*haystack_cf || !*alias_cf) {
        return -1;
    }

    const char *scan = haystack_cf;
    while ((scan = strstr(scan, alias_cf)) != NULL) {
        const char *after = scan + strlen(alias_cf);
        if (is_boundary_before(haystack_cf, scan) && is_boundary_after(after)) {
            return (gssize)(scan - haystack_cf);
        }
        scan = g_utf8_next_char(scan);
    }

    return -1;
}

static void count_scripts(const char *utf8_text, ScriptCounts *counts) {
    memset(counts, 0, sizeof(*counts));
    if (!utf8_text) {
        return;
    }

    const char *p = utf8_text;
    while (*p) {
        gunichar ch = g_utf8_get_char(p);
        p = g_utf8_next_char(p);

        if (ch >= 0x4E00 && ch <= 0x9FFF) counts->han++;
        else if (ch >= 0x3040 && ch <= 0x309F) counts->hiragana++;
        else if (ch >= 0x30A0 && ch <= 0x30FF) counts->katakana++;
        else if (ch >= 0xAC00 && ch <= 0xD7AF) counts->hangul++;
        else if (ch >= 0x0600 && ch <= 0x06FF) counts->arabic++;
        else if (ch >= 0x0590 && ch <= 0x05FF) counts->hebrew++;
        else if (ch >= 0x0900 && ch <= 0x097F) counts->devanagari++;
        else if (ch >= 0x0400 && ch <= 0x04FF) counts->cyrillic++;
        else if (ch >= 0x0370 && ch <= 0x03FF) counts->greek++;
        else if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= 0x00C0 && ch <= 0x024F)) counts->latin++;
    }
}

static const char *dominant_script_language(const char *utf8_text) {
    ScriptCounts counts;
    count_scripts(utf8_text, &counts);

    if (counts.hiragana > 8 || counts.katakana > 8) return "Japanese";
    if (counts.hangul > 8) return "Korean";
    if (counts.han > 12) return "Chinese";
    if (counts.arabic > 8) return "Arabic";
    if (counts.hebrew > 8) return "Hebrew";
    if (counts.devanagari > 8) return "Hindi";
    if (counts.cyrillic > 8) return "Russian";
    if (counts.greek > 8) return "Greek";
    if (counts.latin > 20) return "English";
    return NULL;
}

static char *path_basename_without_ext(const char *path) {
    if (!path || !*path) {
        return NULL;
    }

    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    char *copy = g_strdup(base);
    char *dot = strrchr(copy, '.');
    if (dot) {
        *dot = '\0';
    }
    return copy;
}

static gboolean detect_language_pair_from_text(const char *metadata_text,
                                               const char *path,
                                               char **out_source,
                                               char **out_target) {
    *out_source = NULL;
    *out_target = NULL;

    GString *combined = g_string_new("");
    if (metadata_text && *metadata_text) {
        g_string_append(combined, metadata_text);
    }

    char *basename = path_basename_without_ext(path);
    if (basename && *basename) {
        if (combined->len > 0) {
            g_string_append_c(combined, ' ');
        }
        g_string_append(combined, basename);
    }

    if (combined->len == 0) {
        g_free(basename);
        g_string_free(combined, TRUE);
        return FALSE;
    }

    char *casefolded = g_utf8_casefold(combined->str, -1);
    LanguageHit hits[16];
    guint hit_count = 0;

    for (guint i = 0; language_aliases[i].alias; i++) {
        char *alias_cf = g_utf8_casefold(language_aliases[i].alias, -1);
        gssize pos = find_alias_position(casefolded, alias_cf);
        g_free(alias_cf);
        if (pos < 0) {
            continue;
        }

        gboolean updated = FALSE;
        for (guint j = 0; j < hit_count; j++) {
            if (g_strcmp0(hits[j].canonical, language_aliases[i].canonical) == 0) {
                if (pos < hits[j].pos) {
                    hits[j].pos = pos;
                }
                updated = TRUE;
                break;
            }
        }

        if (!updated && hit_count < G_N_ELEMENTS(hits)) {
            hits[hit_count].canonical = language_aliases[i].canonical;
            hits[hit_count].pos = pos;
            hit_count++;
        }
    }

    guint first_idx = G_MAXUINT;
    guint second_idx = G_MAXUINT;
    for (guint i = 0; i < hit_count; i++) {
        if (first_idx == G_MAXUINT || hits[i].pos < hits[first_idx].pos) {
            second_idx = first_idx;
            first_idx = i;
        } else if (second_idx == G_MAXUINT || hits[i].pos < hits[second_idx].pos) {
            second_idx = i;
        }
    }

    const char *script_lang = dominant_script_language(combined->str);
    const char *source = (first_idx != G_MAXUINT) ? hits[first_idx].canonical : NULL;
    const char *target = (second_idx != G_MAXUINT) ? hits[second_idx].canonical : NULL;

    if (source && !target && script_lang && g_strcmp0(source, script_lang) != 0) {
        target = script_lang;
    }

    if (!source && script_lang) {
        source = script_lang;
    }

    if (source && !target) {
        target = source;
    }

    if (source && target) {
        *out_source = g_strdup(source);
        *out_target = g_strdup(target);
    }

    g_free(casefolded);
    g_free(basename);
    g_string_free(combined, TRUE);
    return *out_source != NULL && *out_target != NULL;
}

char *langpair_normalize_language_name(const char *raw) {
    if (!raw) {
        return NULL;
    }

    char *trimmed = g_strdup(raw);
    g_strstrip(trimmed);
    if (*trimmed == '\0') {
        g_free(trimmed);
        return NULL;
    }

    char *casefolded = g_utf8_casefold(trimmed, -1);
    if (g_strcmp0(casefolded, "unknown") == 0 ||
        g_strcmp0(casefolded, "mixed") == 0 ||
        g_strcmp0(casefolded, "und") == 0 ||
        g_strcmp0(casefolded, "??") == 0) {
        g_free(casefolded);
        g_free(trimmed);
        return NULL;
    }
    g_free(casefolded);

    char *source = NULL;
    char *target = NULL;
    if (detect_language_pair_from_text(trimmed, NULL, &source, &target)) {
        char *result = g_strdup(source);
        g_free(source);
        g_free(target);
        g_free(trimmed);
        return result;
    }

    return trimmed;
}

void langpair_fill_missing(char **source_lang,
                           char **target_lang,
                           const char *metadata_text,
                           const char *path) {
    if (!source_lang || !target_lang) {
        return;
    }

    char *normalized_source = langpair_normalize_language_name(*source_lang);
    char *normalized_target = langpair_normalize_language_name(*target_lang);

    g_free(*source_lang);
    g_free(*target_lang);
    *source_lang = normalized_source;
    *target_lang = normalized_target;

    char *guessed_source = NULL;
    char *guessed_target = NULL;
    if (detect_language_pair_from_text(metadata_text, path, &guessed_source, &guessed_target)) {
        if (!*source_lang && guessed_source) {
            *source_lang = g_strdup(guessed_source);
        }
        if (!*target_lang && guessed_target) {
            *target_lang = g_strdup(guessed_target);
        }
    }

    if (!*source_lang && *target_lang) {
        *source_lang = g_strdup(*target_lang);
    }
    if (!*target_lang && *source_lang) {
        *target_lang = g_strdup(*source_lang);
    }

    g_free(guessed_source);
    g_free(guessed_target);
}

char *langpair_guess_group_from_metadata(const char *metadata_text, const char *path) {
    char *source = NULL;
    char *target = NULL;
    if (!detect_language_pair_from_text(metadata_text, path, &source, &target)) {
        g_free(source);
        g_free(target);
        return NULL;
    }

    char *group = g_strdup_printf("%s->%s", source, target);
    g_free(source);
    g_free(target);
    return group;
}

char *langpair_build_group_name(const char *source_lang, const char *target_lang) {
    char *source = langpair_normalize_language_name(source_lang);
    char *target = langpair_normalize_language_name(target_lang);

    if (!source && target) {
        source = g_strdup(target);
    }
    if (!target && source) {
        target = g_strdup(source);
    }

    if (!source || !target) {
        g_free(source);
        g_free(target);
        return NULL;
    }

    char *group = g_strdup_printf("%s->%s", source, target);
    g_free(source);
    g_free(target);
    return group;
}
