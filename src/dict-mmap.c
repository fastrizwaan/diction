#include "dict-mmap.h"
#include "flat-index.h"
#include "resource-reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <zlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdint.h>
#include <utime.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <archive.h>
#include <archive_entry.h>
#include "dict-cache.h"
#include "settings.h"
#include "dict-chunked.h"

void dict_mmap_close(DictMmap *dict) {
    if (dict) {
        flat_index_close(dict->index);
        resource_reader_close(dict->resource_reader);
        if (dict->chunk_reader) dict_chunk_reader_free(dict->chunk_reader);
        if (dict->data) munmap((void*)dict->data, dict->size);
        if (dict->fd >= 0) close(dict->fd);
        if (dict->tmp_file) fclose(dict->tmp_file);
        g_free(dict->name);
        g_free(dict->source_dir);
        g_free(dict->resource_dir);
        g_free(dict->mdx_stylesheet);
        g_free(dict->source_lang);
        g_free(dict->target_lang);
        g_free(dict->icon_path);
        g_free(dict);
    }
}
