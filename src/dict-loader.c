#include "dict-loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

/* ── helpers ─────────────────────────────────────────────── */

static int ends_with_ci(const char *s, const char *suffix) {
    size_t sl = strlen(s), xl = strlen(suffix);
    if (sl < xl) return 0;
    return strcasecmp(s + sl - xl, suffix) == 0;
}

static char *path_join(const char *dir, const char *name) {
    size_t dl = strlen(dir), nl = strlen(name);
    int need_sep = (dl > 0 && dir[dl-1] != '/');
    char *out = malloc(dl + nl + 2);
    memcpy(out, dir, dl);
    if (need_sep) out[dl++] = '/';
    memcpy(out + dl, name, nl + 1);
    return out;
}

static char *basename_noext(const char *path) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    size_t len = dot ? (size_t)(dot - base) : strlen(base);
    char *out = malloc(len + 1);
    memcpy(out, base, len);
    out[len] = '\0';
    return out;
}

/* ── format detection ────────────────────────────────────── */

DictFormat dict_detect_format(const char *path) {
    if (ends_with_ci(path, ".dsl.dz") || ends_with_ci(path, ".dsl"))
        return DICT_FORMAT_DSL;
    if (ends_with_ci(path, ".ifo"))
        return DICT_FORMAT_STARDICT;
    if (ends_with_ci(path, ".mdx"))
        return DICT_FORMAT_MDX;
    if (ends_with_ci(path, ".bgl"))
        return DICT_FORMAT_BGL;
    return DICT_FORMAT_UNKNOWN;
}

/* ── single dictionary loader ────────────────────────────── */

/* Forward declarations for format-specific parsers */
extern DictMmap* parse_bgl_file(const char *path);
extern DictMmap* parse_mdx_file(const char *path);
extern DictMmap* parse_stardict(const char *ifo_path);

DictMmap* dict_load_any(const char *path, DictFormat fmt) {
    switch (fmt) {
        case DICT_FORMAT_DSL:
            return dict_mmap_open(path);

        case DICT_FORMAT_STARDICT:
            return parse_stardict(path);

        case DICT_FORMAT_MDX:
            return parse_mdx_file(path);

        case DICT_FORMAT_BGL:
            return parse_bgl_file(path);

        default:
            return NULL;
    }
}

/* ── directory scanner ───────────────────────────────────── */

static void scan_recursive(const char *dirpath, DictEntry **head, 
                            DictLoaderCallback callback, void *user_data) {
    DIR *d = opendir(dirpath);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char *full = path_join(dirpath, ent->d_name);
        struct stat st;
        if (stat(full, &st) != 0) { free(full); continue; }

        if (S_ISDIR(st.st_mode)) {
            /* Skip extracted resource folders */
            if (!ends_with_ci(full, ".files") &&
                !ends_with_ci(full, ".dsl.files") &&
                !ends_with_ci(full, ".dsl.dz.files")) {
                scan_recursive(full, head, callback, user_data);
            }
            free(full);
            continue;
        }

        DictFormat fmt = dict_detect_format(full);
        if (fmt == DICT_FORMAT_UNKNOWN) {
            free(full);
            continue;
        }

        DictMmap *loaded = dict_load_any(full, fmt);
        if (!loaded) {
            free(full);
            continue;
        }

        DictEntry *entry = calloc(1, sizeof(DictEntry));
        if (loaded->name) {
            entry->name = strdup(loaded->name);
        } else {
            entry->name = basename_noext(full);
        }
        entry->path = full;
        entry->format = fmt;
        entry->dict = loaded;

        if (callback) {
            callback(entry, user_data);
        }

        if (head) {
            entry->next = *head;
            *head = entry;
        }
    }

    closedir(d);
}

DictEntry* dict_loader_scan_directory(const char *dirpath) {
    DictEntry *head = NULL;
    scan_recursive(dirpath, &head, NULL, NULL);
    return head;
}

void dict_loader_scan_directory_streaming(const char *dirpath, DictLoaderCallback callback, void *user_data) {
    scan_recursive(dirpath, NULL, callback, user_data);
}

void dict_loader_free(DictEntry *head) {
    while (head) {
        DictEntry *next = head->next;
        if (head->dict) dict_mmap_close(head->dict);
        free(head->name);
        free(head->path);
        free(head);
        head = next;
    }
}
