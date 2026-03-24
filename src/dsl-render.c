#include "dsl-render.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <glib.h>

typedef struct {
    char *str;
    size_t len;
    size_t cap;
} StrBuf;

static void buf_append(StrBuf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) {
        b->cap = b->cap == 0 ? 1024 : b->cap * 2;
        while (b->len + n + 1 > b->cap) b->cap *= 2;
        b->str = realloc(b->str, b->cap);
    }
    memcpy(b->str + b->len, s, n);
    b->len += n;
    b->str[b->len] = '\0';
}

static void buf_append_str(StrBuf *b, const char *s) {
    buf_append(b, s, strlen(s));
}


char* dsl_render_to_html(const char *dsl_text, size_t length, const char *headword, size_t hw_length, DictFormat format, const char *resource_dir) {
    StrBuf b = {NULL, 0, 0};
    
    // Add GoldenDict-like styling
    buf_append_str(&b, 
        "<style>"
        "body{font-family: sans-serif; line-height: 1.4; color: #333;}"
        ".dict-link {color: #0b5394; text-decoration: none;}"
        ".dict-link:hover {text-decoration: underline;}"
        ".trn{color: #1a73e8;}"
        ".ex{color: #5f6368; font-style: italic;}"
        ".com{color: #006600;}"
        ".pos{color: #c90016; font-weight: bold;}"
        ".m-line{margin-top: 2px; margin-bottom: 2px;}"
        "</style>"
    );
    
    buf_append_str(&b, "<h2 style='color:#0b5394; margin-bottom: 0.5em;'>");
    buf_append(&b, headword, hw_length);
    buf_append_str(&b, "</h2>\n<div>");

    if (format == DICT_FORMAT_MDX) {
        if (!resource_dir) {
            buf_append(&b, dsl_text, length);
        } else {
            /* Robust HTML attribute rewriting for MDX */
            size_t head = 0;
            while (head < length) {
                const char *s1 = strstr(dsl_text + head, "src=\"");
                const char *s2 = strstr(dsl_text + head, "href=\"");
                const char *hit = NULL;
                
                if (s1 && s2) hit = (s1 < s2) ? s1 : s2;
                else if (s1) hit = s1;
                else if (s2) hit = s2;
                
                if (!hit) {
                    buf_append(&b, dsl_text + head, length - head);
                    break;
                }
                
                /* Append everything before the attribute */
                buf_append(&b, dsl_text + head, hit - (dsl_text + head));
                
                size_t attr_name_len = (hit == s1) ? 3 : 4;
                buf_append(&b, hit, attr_name_len + 2); // "src=\"" or "href=\""
                head = (hit - dsl_text) + attr_name_len + 2;
                
                /* Find the end quote */
                const char *q_end = strchr(dsl_text + head, '\"');
                if (!q_end) {
                    buf_append(&b, dsl_text + head, length - head);
                    break;
                }
                
                size_t val_len = q_end - (dsl_text + head);
                char *val = strndup(dsl_text + head, val_len);
                
                if (g_str_has_prefix(val, "entry://") || g_str_has_prefix(val, "bword://")) {
                    buf_append_str(&b, "dict://");
                    const char *target = strstr(val, "://") + 3;
                    buf_append_str(&b, target);
                } else if (g_str_has_prefix(val, "sound://")) {
                    buf_append_str(&b, "file://");
                    buf_append_str(&b, resource_dir);
                    buf_append_str(&b, "/");
                    buf_append_str(&b, val + 8);
                } else if (strstr(val, "://") || val[0] == '#' || g_str_has_prefix(val, "data:")) {
                    /* Keep absolute or special links */
                    buf_append_str(&b, val);
                } else {
                    /* Assume local resource */
                    buf_append_str(&b, "file://");
                    buf_append_str(&b, resource_dir);
                    buf_append_str(&b, "/");
                    buf_append_str(&b, val);
                }
                free(val);
                head = q_end - dsl_text; // point to the second quote
            }
        }
        buf_append_str(&b, "</div>");
        return b.str;
    }

    size_t i = 0;
    
    // State tracking
    int in_sound = 0;
    int m_open = 0;
    
    while (i < length) {
        // Strip out {{ macros }} entirely
        if (dsl_text[i] == '{' && i + 1 < length && dsl_text[i+1] == '{') {
            size_t end = i + 2;
            while (end + 1 < length && !(dsl_text[end] == '}' && dsl_text[end+1] == '}')) {
                end++;
            }
            if (end + 1 < length) {
                i = end + 2;
                continue;
            }
        }
        
        // Escape brackets
        if (dsl_text[i] == '\\' && i + 1 < length) {
            char next = dsl_text[i+1];
            if (next == '[' || next == ']' || next == '(' || next == ')' || next == '{' || next == '}' || next == '~') {
                if (!in_sound) buf_append(&b, &dsl_text[i+1], 1);
                i += 2;
                continue;
            }
        }

        if (dsl_text[i] == '[') {
             size_t end = i + 1;
             while (end < length && dsl_text[end] != ']') end++;
             if (end < length) {
                 size_t tag_len = end - i - 1;
                 const char *tag = dsl_text + i + 1;
                 
                 // Handle color tags like [c darkblue]
                 if (tag_len > 2 && tag[0] == 'c' && tag[1] == ' ') {
                     if (!in_sound) {
                         buf_append_str(&b, "<span style='color:");
                         buf_append(&b, tag + 2, tag_len - 2);
                         buf_append_str(&b, "'>");
                     }
                 }
                 else if (tag_len == 2 && strncmp(tag, "/c", 2) == 0) {
                     if (!in_sound) buf_append_str(&b, "</span>");
                 }
                 
                 // M-Line handling [m1], [m2]...
                 else if (tag_len >= 1 && tag[0] == 'm' && (tag_len == 1 || isdigit(tag[1]))) {
                     if (!in_sound) {
                         if (m_open) buf_append_str(&b, "</div>");
                         
                         int level = (tag_len > 1 && isdigit(tag[1])) ? (tag[1] - '0') : 1;
                         char mbuf[128];
                         snprintf(mbuf, sizeof(mbuf), "<div class='m-line' style='margin-left: %.1fem'>", (double)level * 1.2);
                         buf_append_str(&b, mbuf);
                         m_open = 1;
                     }
                 }
                 else if (tag_len == 2 && strncmp(tag, "/m", 2) == 0) {
                     if (!in_sound && m_open) {
                         buf_append_str(&b, "</div>");
                         m_open = 0;
                     }
                 }
                 
                 // Links handling [ref]...[/ref]
                 else if (tag_len == 3 && strncmp(tag, "ref", 3) == 0) {
                     if (!in_sound) buf_append_str(&b, "<a class='dict-link' href='#'>");
                 }
                 else if (tag_len == 4 && strncmp(tag, "/ref", 4) == 0) {
                     if (!in_sound) buf_append_str(&b, "</a>");
                 }
                 
                 // Sound tags [s]...[/s] - hide for now
                 else if (tag_len == 1 && tag[0] == 's') in_sound = 1;
                 else if (tag_len == 2 && tag[0] == '/' && tag[1] == 's') in_sound = 0;
                 
                 // Transcription [t]...[/t]
                 else if (tag_len == 1 && tag[0] == 't') {
                     if (!in_sound) buf_append_str(&b, "<span style='color: #1e8e3e; font-family: sans-serif;'>");
                 }
                 else if (tag_len == 2 && tag[0] == '/' && tag[1] == 't') {
                     if (!in_sound) buf_append_str(&b, "</span>");
                 }
                 
                 // Basics
                 else if (tag_len == 1 && tag[0] == 'b') { if(!in_sound) buf_append_str(&b, "<b>"); }
                 else if (tag_len == 2 && strncmp(tag, "/b", 2) == 0) { if(!in_sound) buf_append_str(&b, "</b>"); }
                 else if (tag_len == 1 && tag[0] == 'i') { if(!in_sound) buf_append_str(&b, "<i>"); }
                 else if (tag_len == 2 && strncmp(tag, "/i", 2) == 0) { if(!in_sound) buf_append_str(&b, "</i>"); }
                 else if (tag_len == 1 && tag[0] == 'u') { if(!in_sound) buf_append_str(&b, "<u>"); }
                 else if (tag_len == 2 && strncmp(tag, "/u", 2) == 0) { if(!in_sound) buf_append_str(&b, "</u>"); }
                 
                 // Superscript/Subscript
                 else if (tag_len == 3 && strncmp(tag, "sup", 3) == 0) { if(!in_sound) buf_append_str(&b, "<sup>"); }
                 else if (tag_len == 4 && strncmp(tag, "/sup", 4) == 0) { if(!in_sound) buf_append_str(&b, "</sup>"); }
                 else if (tag_len == 3 && strncmp(tag, "sub", 3) == 0) { if(!in_sound) buf_append_str(&b, "<sub>"); }
                 else if (tag_len == 4 && strncmp(tag, "/sub", 4) == 0) { if(!in_sound) buf_append_str(&b, "</sub>"); }
                 
                 // Bullet [*]
                 else if (tag_len == 1 && tag[0] == '*') {
                     if(!in_sound) buf_append_str(&b, "<span style='color: #888;'>• </span>");
                 }
                 else if (tag_len == 2 && strncmp(tag, "/*", 2) == 0) { /* ignore */ }
                 
                 else if (tag_len == 1 && tag[0] == 'p') { if(!in_sound) buf_append_str(&b, "<span class='pos'>"); }
                 else if (tag_len == 2 && strncmp(tag, "/p", 2) == 0) { if(!in_sound) buf_append_str(&b, "</span>"); }
                 else if (tag_len == 3 && strncmp(tag, "trn", 3) == 0) { if(!in_sound) buf_append_str(&b, "<span class='trn'>"); }
                 else if (tag_len == 4 && strncmp(tag, "/trn", 4) == 0) { if(!in_sound) buf_append_str(&b, "</span>"); }
                 else if (tag_len == 2 && strncmp(tag, "ex", 2) == 0) { if(!in_sound) buf_append_str(&b, "<span class='ex'>"); }
                 else if (tag_len == 3 && strncmp(tag, "/ex", 3) == 0) { if(!in_sound) buf_append_str(&b, "</span>"); }
                 else if (tag_len == 3 && strncmp(tag, "com", 3) == 0) { if(!in_sound) buf_append_str(&b, "<span class='com'>"); }
                 else if (tag_len == 4 && strncmp(tag, "/com", 4) == 0) { if(!in_sound) buf_append_str(&b, "</span>"); }
                 
                 else {
                     // Potential transcription if unknown tag with no space (e.g. [frait])
                     int has_space = 0;
                     for(size_t j=0; j<tag_len; j++) if(isspace(tag[j])) has_space = 1;
                     
                     if (!in_sound) {
                         if (!has_space && tag_len > 0 && tag[0] != '/') {
                             buf_append_str(&b, "<span style='color: #1e8e3e;'>[");
                             buf_append(&b, tag, tag_len);
                             buf_append_str(&b, "]</span>");
                         } else {
                             // Pass through as text but escaped
                             buf_append_str(&b, "[");
                             buf_append(&b, tag, tag_len);
                             buf_append_str(&b, "]");
                         }
                     }
                 }
                 
                 i = end + 1;
                 continue;
             }
        } 
        else if (dsl_text[i] == '<' && i + 1 < length && dsl_text[i+1] == '<') {
            size_t end = i + 2;
            while (end + 1 < length && !(dsl_text[end] == '>' && dsl_text[end+1] == '>')) end++;
            if (end + 1 < length) {
                if (!in_sound) {
                    size_t word_len = end - i - 2;
                    buf_append_str(&b, "<a class='dict-link' href='#'>");
                    buf_append(&b, dsl_text + i + 2, word_len);
                    buf_append_str(&b, "</a>");
                }
                i = end + 2;
                continue;
            }
        }
        else if (dsl_text[i] == '<') {
            if (!in_sound) {
                // Potential sense identifier like <A>
                size_t end = i + 1;
                while (end < length && dsl_text[end] != '>' && (end - i < 10)) end++;
                if (end < length && dsl_text[end] == '>') {
                    buf_append_str(&b, "<b style='color: #c90016;'>&lt;");
                    buf_append(&b, dsl_text + i + 1, end - i - 1);
                    buf_append_str(&b, "&gt;</b>");
                    i = end + 1;
                    continue;
                } else {
                    buf_append_str(&b, "&lt;");
                    i++;
                    continue;
                }
            } else { i++; continue; }
        }
        else if (dsl_text[i] == '\n') {
            if (!in_sound) buf_append_str(&b, "\n");
            i++;
            continue;
        } 
        else if (dsl_text[i] == '~') {
            if (!in_sound) buf_append(&b, headword, hw_length);
            i++;
            continue;
        } 
        else if (dsl_text[i] == '>') {
            if (!in_sound) buf_append_str(&b, "&gt;");
            i++;
            continue;
        } 
        else if (dsl_text[i] == '&') {
            if (!in_sound) buf_append_str(&b, "&amp;");
            i++;
            continue;
        }
        
        if (!in_sound) buf_append(&b, &dsl_text[i], 1);
        i++;
    }
    
    if (m_open) buf_append_str(&b, "</div>");
    
    if (b.str == NULL) {
        buf_append_str(&b, " ");
    } else {
        buf_append_str(&b, "</div>");
    }
    
    return b.str;
}
