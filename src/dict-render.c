#include "dict-render.h"
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

// Color name mapping for dark mode (too dark -> lighter)
static const char* get_dark_mode_color(const char *color_name) {
    struct { const char *dark; const char *light; } map[] = {
        {"darkblue", "dodgerblue"},
        {"darkcyan", "cyan"},
        {"darkviolet", "orchid"},
        {"darkmagenta", "violet"},
        {"darkred", "tomato"},
        {"darkorange", "orange"},
        {"darkgreen", "seagreen"},
        {"darkolivegreen", "yellowgreen"},
        {"darkslategray", "cadetblue"},
        {"darkslategrey", "cadetblue"},
        {"darkslateblue", "slateblue"},
        {"navy", "steelblue"},
        {NULL, NULL}
    };
    for (int i = 0; map[i].dark; i++) {
        if (g_ascii_strcasecmp(color_name, map[i].dark) == 0) {
            return map[i].light;
        }
    }
    return color_name;
}

// Color name mapping for light mode (too light -> darker)
static const char* get_light_mode_color(const char *color_name) {
    struct { const char *light; const char *dark; } map[] = {
        {"ivory", "darkkhaki"},
        {"lightgray", "gray"},
        {"lightgrey", "gray"},
        {"lightyellow", "goldenrod"},
        {"white", "beige"},
        {"yellow", "darkgoldenrod"},
        {NULL, NULL}
    };
    for (int i = 0; map[i].light; i++) {
        if (g_ascii_strcasecmp(color_name, map[i].light) == 0) {
            return map[i].dark;
        }
    }
    return color_name;
}

// Simple hex color lightening helper
static void lighten_hex_color(char *output, const char *hex, size_t output_size) {
    char *endptr;
    unsigned long r = 0, g = 0, b = 0;

    if (hex[0] != '#') {
        g_strlcpy(output, hex, output_size);
        return;
    }

    const char *c = hex + 1;
    size_t len = strlen(c);

    if (len == 3) {
        char tmp[4] = {c[0], c[0], '\0'};
        r = strtoul(tmp, &endptr, 16);
        tmp[0] = c[1]; tmp[1] = c[1];
        g = strtoul(tmp, &endptr, 16);
        tmp[0] = c[2]; tmp[1] = c[2];
        b = strtoul(tmp, &endptr, 16);
    } else if (len >= 6) {
        char tmp[3] = {c[0], c[1], '\0'};
        r = strtoul(tmp, &endptr, 16);
        tmp[0] = c[2]; tmp[1] = c[3];
        g = strtoul(tmp, &endptr, 16);
        tmp[0] = c[4]; tmp[1] = c[5];
        b = strtoul(tmp, &endptr, 16);
    } else {
        g_strlcpy(output, hex, output_size);
        return;
    }

    // Lighten by 30%
    r = (size_t)(r + (255 - r) * 0.3);
    g = (size_t)(g + (255 - g) * 0.3);
    b = (size_t)(b + (255 - b) * 0.3);

    g_snprintf(output, output_size, "#%02lx%02lx%02lx", r, g, b);
}


char* dsl_render_to_html(const char *dsl_text, size_t length, const char *headword, size_t hw_length, DictFormat format, const char *resource_dir, int dark_mode) {
    StrBuf b = {NULL, 0, 0};

    // Theme colors based on Python Diction's _build_theme_css
    const char *body_color = dark_mode ? "#dddddd" : "#222222";
    const char *bg_color = dark_mode ? "#1e1e1e" : "#ffffff";
    const char *link_color = dark_mode ? "#89b4ff" : "#005bbb";
    const char *trn_color = dark_mode ? "#89b4ff" : "#1a73e8";
    const char *ex_color = dark_mode ? "#9ae59a" : "#5f6368";
    const char *com_color = dark_mode ? "#9ae59a" : "#006600";
    const char *pos_color = dark_mode ? "#9ae59a" : "#c90016";
    const char *translit_color = dark_mode ? "#888888" : "#808080";
    const char *heading_color = dark_mode ? "#89b4ff" : "#0b5394";
    const char *border_color = dark_mode ? "#444444" : "#cccccc";

    // Add GoldenDict-like styling with theme-aware colors
    buf_append_str(&b,
        "<style>"
        "body{font-family: system-ui, sans-serif; line-height: 1.4; color: ");
    buf_append_str(&b, body_color);
    buf_append_str(&b, "; background: ");
    buf_append_str(&b, bg_color);
    buf_append_str(&b, ";}"
        ".dict-link {color: ");
    buf_append_str(&b, link_color);
    buf_append_str(&b, "; text-decoration: none;}"
        ".dict-link:hover {text-decoration: underline;}"
        ".trn{color: ");
    buf_append_str(&b, trn_color);
    buf_append_str(&b, ";}"
        ".ex{color: ");
    buf_append_str(&b, ex_color);
    buf_append_str(&b, "; font-style: italic;}"
        ".com{color: ");
    buf_append_str(&b, com_color);
    buf_append_str(&b, ";}"
        ".pos{color: ");
    buf_append_str(&b, pos_color);
    buf_append_str(&b, "; font-weight: bold;}"
        ".translit{color: ");
    buf_append_str(&b, translit_color);
    buf_append_str(&b, "; font-style: italic;}"
        ".m-line{margin-top: 2px; margin-bottom: 2px;}"
        "hr{border: none; border-top: 1px solid ");
    buf_append_str(&b, border_color);
    buf_append_str(&b, "; margin: 10px 0;}"
        "</style>"
    );

    buf_append_str(&b, "<h2 style='color:");
    buf_append_str(&b, heading_color);
    buf_append_str(&b, "; margin-bottom: 0.5em;'>");
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

                         // Extract color name/value
                         char color_name[64];
                         size_t color_len = tag_len - 2;
                         if (color_len >= sizeof(color_name)) color_len = sizeof(color_name) - 1;
                         memcpy(color_name, tag + 2, color_len);
                         color_name[color_len] = '\0';

                         // Apply theme-aware color adjustment
                         const char *final_color = color_name;
                         char adjusted_color[64];

                         if (dark_mode) {
                             final_color = get_dark_mode_color(color_name);
                             // If it's a hex color, lighten it
                             if (final_color == color_name && final_color[0] == '#') {
                                 lighten_hex_color(adjusted_color, final_color, sizeof(adjusted_color));
                                 final_color = adjusted_color;
                             }
                         } else {
                             final_color = get_light_mode_color(color_name);
                         }

                         buf_append_str(&b, final_color);
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
                     const char *t_color = dark_mode ? "#9ae59a" : "#1e8e3e";
                     if (!in_sound) {
                         buf_append_str(&b, "<span style='color: ");
                         buf_append_str(&b, t_color);
                         buf_append_str(&b, "; font-family: sans-serif;'>");
                     }
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
                     const char *bullet_color = dark_mode ? "#888888" : "#888888";
                     if(!in_sound) {
                         buf_append_str(&b, "<span style='color: ");
                         buf_append_str(&b, bullet_color);
                         buf_append_str(&b, ";'>• </span>");
                     }
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
                             const char *trans_color = dark_mode ? "#9ae59a" : "#1e8e3e";
                             buf_append_str(&b, "<span style='color: ");
                             buf_append_str(&b, trans_color);
                             buf_append_str(&b, ";'>[");
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
                    const char *sense_color = dark_mode ? "#9ae59a" : "#c90016";
                    buf_append_str(&b, "<b style='color: ");
                    buf_append_str(&b, sense_color);
                    buf_append_str(&b, ";'>&lt;");
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
