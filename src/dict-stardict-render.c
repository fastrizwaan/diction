#include "dict-render.h"
#include <glib.h>
#include <ctype.h>
#include <string.h>
#include <stdio.h>

static uint32_t read_u32be(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

void append_html_escaped_text(GString *article, const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        switch (s[i]) {
            case '&': g_string_append(article, "&amp;"); break;
            case '<': g_string_append(article, "&lt;"); break;
            case '>': g_string_append(article, "&gt;"); break;
            case '"': g_string_append(article, "&quot;"); break;
            default: g_string_append_c(article, s[i]); break;
        }
    }
}

static void append_stardict_resource_html(GString *article, char type, const char *data, size_t size) {
    switch (type) {
        case 'h':
        case 'g':
        case 'x':
            g_string_append_len(article, data, size);
            break;
        case 'm':
        case 'l':
        case 't':
        case 'y':
        case 'w':
            append_html_escaped_text(article, data, size);
            break;
        default:
            append_html_escaped_text(article, data, size);
            break;
    }
}

gboolean append_stardict_article(GString *article, const unsigned char *data, size_t size, const char *sametypesequence) {
    const unsigned char *ptr = data;
    size_t remaining = size;

    if (sametypesequence && *sametypesequence) {
        size_t seq_len = strlen(sametypesequence);

        for (size_t i = 0; i < seq_len && remaining > 0; i++) {
            char type = sametypesequence[i];
            gboolean last = (i + 1 == seq_len);

            if (islower((unsigned char)type)) {
                size_t entry_size = 0;
                if (last) {
                    entry_size = remaining;
                } else {
                    while (entry_size < remaining && ptr[entry_size] != '\0') {
                        entry_size++;
                    }
                    if (entry_size == remaining) return FALSE;
                }
                append_stardict_resource_html(article, type, (const char *)ptr, entry_size);
                ptr += entry_size;
                remaining -= entry_size;
                if (!last && remaining > 0) { ptr++; remaining--; }
            } else if (isupper((unsigned char)type)) {
                size_t entry_size = 0;
                if (last) {
                    entry_size = remaining;
                } else {
                    if (remaining < 4) return FALSE;
                    entry_size = read_u32be(ptr);
                    ptr += 4;
                    remaining -= 4;
                    if (entry_size > remaining) return FALSE;
                }
                append_stardict_resource_html(article, type, (const char *)ptr, entry_size);
                ptr += entry_size;
                remaining -= entry_size;
            } else { return FALSE; }
        }
        return TRUE;
    }

    while (remaining > 0) {
        char type = (char)*ptr++;
        remaining--;
        if (islower((unsigned char)type)) {
            size_t entry_size = 0;
            while (entry_size < remaining && ptr[entry_size] != '\0') entry_size++;
            if (entry_size == remaining) return FALSE;
            append_stardict_resource_html(article, type, (const char *)ptr, entry_size);
            ptr += entry_size + 1;
            remaining -= entry_size + 1;
        } else if (isupper((unsigned char)type)) {
            if (remaining < 4) return FALSE;
            uint32_t entry_size = read_u32be(ptr);
            ptr += 4; remaining -= 4;
            if (entry_size > remaining) return FALSE;
            append_stardict_resource_html(article, type, (const char *)ptr, (size_t)entry_size);
            ptr += entry_size;
            remaining -= entry_size;
        } else { return FALSE; }
    }
    return TRUE;
}
