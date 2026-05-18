import re

with open("dict-mdx.c", "r") as f:
    content = f.read()

# 1. Replace MDDRes and MddFileState
old_structs = """typedef struct { uint64_t off; uint64_t end_off; char *name; } MDDRes;
static int mdd_res_cmp(const void *a, const void *b) {
    const char *na = ((MDDRes*)a)->name;
    const char *nb = ((MDDRes*)b)->name;
    return strcasecmp(na, nb);
}

typedef struct { 
    uint64_t comp; 
    uint64_t decomp; 
    uint64_t file_offset; 
    uint64_t decomp_offset; 
} MDD_RBI;

typedef struct {
    char *mdd_path;
    int is_v2;
    int num_size;
    int encoding_is_utf16;
    int encrypted;
    long kb_data_pos;
    uint64_t kb_data_size;
    
    MDD_RBI *rbis;
    uint64_t nrb;
    uint64_t total_decomp;

    MDDRes *resources;
    size_t res_count;
} MddFileState;"""

new_structs = """typedef struct { 
    uint64_t comp; 
    uint64_t decomp; 
    uint64_t file_offset; 
    uint64_t decomp_offset; 
} MDD_RBI;

typedef struct {
    uint64_t comp_size;
    uint64_t decomp_size;
    char *head_word;
    char *tail_word;
    uint64_t file_offset;
} MDD_KBI;

typedef struct {
    char *mdd_path;
    int is_v2;
    int num_size;
    int encoding_is_utf16;
    int encrypted;
    long kb_data_pos;
    uint64_t kb_data_size;
    
    MDD_RBI *rbis;
    uint64_t nrb;
    uint64_t total_decomp;

    MDD_KBI *kbis;
    size_t nkb;
} MddFileState;"""

content = content.replace(old_structs, new_structs)

# 2. Replace mdd_get, mdd_has, mdd_close
old_methods = re.search(r"static char\* mdd_get.*?static void mdd_close\(ResourceReader \*reader\) \{.*?\}", content, re.DOTALL).group(0)

new_methods = """static gboolean mdd_find_resource(MddFileState *fs, const char *query, uint64_t *out_start_off, uint64_t *out_end_off) {
    if (!fs || !fs->kbis || fs->nkb == 0) return FALSE;
    
    int l = 0, r = (int)fs->nkb - 1;
    int found_kb = -1;
    while (l <= r) {
        int m = l + (r - l) / 2;
        int cmp_head = strcasecmp(query, fs->kbis[m].head_word);
        int cmp_tail = strcasecmp(query, fs->kbis[m].tail_word);
        
        if (cmp_head >= 0 && cmp_tail <= 0) {
            found_kb = m;
            break;
        } else if (cmp_head < 0) {
            r = m - 1;
        } else {
            l = m + 1;
        }
    }
    
    if (found_kb < 0) return FALSE;
    
    FILE *f = fopen(fs->mdd_path, "rb");
    if (!f) return FALSE;
    
    fseek(f, fs->kbis[found_kb].file_offset, SEEK_SET);
    unsigned char *comp = g_malloc(fs->kbis[found_kb].comp_size);
    if (!comp || fread(comp, 1, fs->kbis[found_kb].comp_size, f) != fs->kbis[found_kb].comp_size) {
        g_free(comp); fclose(f); return FALSE;
    }
    
    size_t dlen = 0;
    unsigned char *data = mdx_block_decompress(comp, fs->kbis[found_kb].comp_size, fs->kbis[found_kb].decomp_size, &dlen);
    g_free(comp);
    if (!data) { fclose(f); return FALSE; }
    
    gboolean found = FALSE;
    gboolean got_end = FALSE;
    const unsigned char *hp = data, *he = data + dlen;
    
    while (hp < he) {
        if (hp + fs->num_size > he) break;
        uint64_t current_off = (fs->num_size == 8) ? ru64be(hp) : ru32be(hp);
        hp += fs->num_size;
        
        char word[1024];
        if (fs->encoding_is_utf16) {
            const unsigned char *ws = hp;
            while (hp + 1 < he && !(hp[0] == 0 && hp[1] == 0)) hp += 2;
            utf16le_to_utf8(ws, hp - ws, word, sizeof(word)-1);
            if (hp + 1 < he) hp += 2;
        } else {
            const unsigned char *ws = hp;
            while (hp < he && *hp != '\0') hp++;
            size_t wl = hp - ws; if (wl > 1023) wl = 1023;
            memcpy(word, ws, wl); word[wl] = '\0';
            if (hp < he) hp++;
        }
        mdx_normalize_resource_path(word);
        
        if (found) {
            if (out_end_off) *out_end_off = current_off;
            got_end = TRUE;
            break;
        }
        
        if (strcasecmp(query, word) == 0) {
            found = TRUE;
            if (out_start_off) *out_start_off = current_off;
        }
    }
    g_free(data);
    
    if (found && !got_end) {
        if (found_kb + 1 < fs->nkb) {
            fseek(f, fs->kbis[found_kb + 1].file_offset, SEEK_SET);
            unsigned char *next_comp = g_malloc(fs->kbis[found_kb + 1].comp_size);
            if (next_comp && fread(next_comp, 1, fs->kbis[found_kb + 1].comp_size, f) == fs->kbis[found_kb + 1].comp_size) {
                size_t ndlen = 0;
                unsigned char *next_data = mdx_block_decompress(next_comp, fs->kbis[found_kb + 1].comp_size, fs->kbis[found_kb + 1].decomp_size, &ndlen);
                if (next_data && ndlen >= (size_t)fs->num_size) {
                    if (out_end_off) *out_end_off = (fs->num_size == 8) ? ru64be(next_data) : ru32be(next_data);
                }
                g_free(next_data);
            }
            g_free(next_comp);
        } else {
            if (out_end_off) *out_end_off = fs->total_decomp;
        }
    }
    
    fclose(f);
    return found;
}

static char* mdd_get(ResourceReader *reader, const char *name) {
    if (!reader || !name) return NULL;
    MddBackend *mdd = resource_reader_get_backend(reader);
    if (!mdd) return NULL;

    char *query = g_strdup(name);
    mdx_normalize_resource_path(query);

    for (size_t f = 0; f < mdd->num_files; f++) {
        MddFileState *fs = mdd->files[f];
        uint64_t start_off = 0, end_off = 0;
        
        if (mdd_find_resource(fs, query, &start_off, &end_off)) {
            char *dest_path = g_build_filename(resource_reader_get_dir(reader), query, NULL);
            if (g_file_test(dest_path, G_FILE_TEST_EXISTS)) {
                g_free(query);
                return dest_path;
            }

            char *parent_dir = g_path_get_dirname(dest_path);
            g_mkdir_with_parents(parent_dir, 0755);
            g_free(parent_dir);

            FILE *in = fopen(fs->mdd_path, "rb");
            if (!in) { g_free(dest_path); continue; }

            FILE *out = fopen(dest_path, "wb");
            if (!out) { fclose(in); g_free(dest_path); continue; }

            for (uint64_t i = 0; i < fs->nrb; i++) {
                uint64_t b_dstart = fs->rbis[i].decomp_offset;
                uint64_t b_dend = b_dstart + fs->rbis[i].decomp;
                
                if (end_off <= b_dstart || start_off >= b_dend) continue;
                
                fseek(in, fs->rbis[i].file_offset, SEEK_SET);
                unsigned char *comp = g_malloc(fs->rbis[i].comp);
                if (!comp || fread(comp, 1, fs->rbis[i].comp, in) != fs->rbis[i].comp) {
                    g_free(comp);
                    continue;
                }
                
                size_t dlen = 0;
                unsigned char *decomp = mdx_block_decompress(comp, fs->rbis[i].comp, fs->rbis[i].decomp, &dlen);
                g_free(comp);
                
                if (decomp) {
                    uint64_t slice_start = (start_off > b_dstart) ? start_off : b_dstart;
                    uint64_t slice_end = (end_off < b_dend) ? end_off : b_dend;
                    
                    size_t offset_in_block = slice_start - b_dstart;
                    size_t slice_len = slice_end - slice_start;
                    
                    fwrite(decomp + offset_in_block, 1, slice_len, out);
                    g_free(decomp);
                }
            }
            fclose(in);
            fclose(out);
            g_free(query);
            return dest_path;
        }
    }
    g_free(query);
    return NULL;
}

static gboolean mdd_has(ResourceReader *reader, const char *name) {
    if (!reader || !name) return FALSE;
    MddBackend *mdd = resource_reader_get_backend(reader);
    if (!mdd) return FALSE;

    char *query = g_strdup(name);
    mdx_normalize_resource_path(query);
    gboolean res = FALSE;

    for (size_t f = 0; f < mdd->num_files; f++) {
        MddFileState *fs = mdd->files[f];
        if (mdd_find_resource(fs, query, NULL, NULL)) {
            res = TRUE;
            break;
        }
    }
    g_free(query);
    return res;
}

static void mdd_close(ResourceReader *reader) {
    MddBackend *mdd = resource_reader_get_backend(reader);
    if (!mdd) return;
    for (size_t f = 0; f < mdd->num_files; f++) {
        MddFileState *fs = mdd->files[f];
        g_free(fs->mdd_path);
        g_free(fs->rbis);
        if (fs->kbis) {
            for (size_t i = 0; i < fs->nkb; i++) {
                g_free(fs->kbis[i].head_word);
                g_free(fs->kbis[i].tail_word);
            }
            g_free(fs->kbis);
        }
        g_free(fs);
    }
    g_free(mdd->files);
    g_free(mdd);
}"""

content = content.replace(old_methods, new_methods)

# 3. Replace mdx_open_mdd_reader parsing logic

old_parse_kb = re.search(r"        long kb_data_pos = ftell\(f\);.*?        MDD_RBI \*rbis = g_malloc0_n\(nrb, sizeof\(MDD_RBI\)\);", content, re.DOTALL).group(0)

new_parse_kb = """        long kb_data_pos = ftell(f);
        MDD_KBI *kbis = g_malloc0_n(num_key_blocks, sizeof(MDD_KBI));
        if (!kbis) { g_free(kbi_raw); fclose(f); continue; }
        
        size_t kbc = 0;
        uint64_t current_file_offset = kb_data_pos;
        
        if (mdd_is_v2) {
            if (mdd_encrypted & 2) mdx_decrypt_key_block_info(kbi_raw, kbi_comp);
            size_t dlen = 0;
            unsigned char *data = mdx_block_decompress(kbi_raw, kbi_comp, kbi_decomp, &dlen);
            if (data) {
                const unsigned char *ip = data, *ie = data + dlen;
                while (ip < ie && kbc < num_key_blocks) {
                    if (ip + mdd_num_size > ie) break;
                    ip += mdd_num_size;
                    
                    uint32_t head_size = read_u8or16(&ip, 1);
                    char *head_word = NULL;
                    if (head_size > 0) {
                        char word[1024];
                        if (mdd_encoding_is_utf16) {
                            utf16le_to_utf8(ip, head_size * 2, word, sizeof(word)-1);
                        } else {
                            size_t wl = head_size; if (wl > 1023) wl = 1023;
                            memcpy(word, ip, wl); word[wl] = '\\0';
                        }
                        mdx_normalize_resource_path(word);
                        head_word = g_strdup(word);
                    }
                    ip += (mdd_encoding_is_utf16 ? (head_size + 1) * 2 : (head_size + 1));
                    
                    uint32_t tail_size = read_u8or16(&ip, 1);
                    char *tail_word = NULL;
                    if (tail_size > 0) {
                        char word[1024];
                        if (mdd_encoding_is_utf16) {
                            utf16le_to_utf8(ip, tail_size * 2, word, sizeof(word)-1);
                        } else {
                            size_t wl = tail_size; if (wl > 1023) wl = 1023;
                            memcpy(word, ip, wl); word[wl] = '\\0';
                        }
                        mdx_normalize_resource_path(word);
                        tail_word = g_strdup(word);
                    }
                    ip += (mdd_encoding_is_utf16 ? (tail_size + 1) * 2 : (tail_size + 1));
                    
                    if (ip + mdd_num_size * 2 > ie) {
                        g_free(head_word); g_free(tail_word); break;
                    }
                    kbis[kbc].comp_size = read_num(&ip, mdd_num_size);
                    kbis[kbc].decomp_size = read_num(&ip, mdd_num_size);
                    kbis[kbc].head_word = head_word ? head_word : g_strdup("");
                    kbis[kbc].tail_word = tail_word ? tail_word : g_strdup("");
                    kbis[kbc].file_offset = current_file_offset;
                    current_file_offset += kbis[kbc].comp_size;
                    
                    kbc++;
                }
                g_free(data);
            }
        } else {
            const unsigned char *ip = kbi_raw, *ie = kbi_raw + kbi_comp;
            while (ip < ie && kbc < num_key_blocks) {
                ip += mdd_num_size;
                
                uint32_t head_size = read_u8or16(&ip, 0);
                char *head_word = NULL;
                if (head_size > 0) {
                    char word[1024];
                    if (mdd_encoding_is_utf16) {
                        utf16le_to_utf8(ip, head_size * 2, word, sizeof(word)-1);
                    } else {
                        size_t wl = head_size; if (wl > 1023) wl = 1023;
                        memcpy(word, ip, wl); word[wl] = '\\0';
                    }
                    mdx_normalize_resource_path(word);
                    head_word = g_strdup(word);
                }
                ip += head_size; // v1 doesn't have trailing NUL
                
                uint32_t tail_size = read_u8or16(&ip, 0);
                char *tail_word = NULL;
                if (tail_size > 0) {
                    char word[1024];
                    if (mdd_encoding_is_utf16) {
                        utf16le_to_utf8(ip, tail_size * 2, word, sizeof(word)-1);
                    } else {
                        size_t wl = tail_size; if (wl > 1023) wl = 1023;
                        memcpy(word, ip, wl); word[wl] = '\\0';
                    }
                    mdx_normalize_resource_path(word);
                    tail_word = g_strdup(word);
                }
                ip += tail_size; // v1 doesn't have trailing NUL
                
                if (ip + mdd_num_size * 2 > ie) {
                    g_free(head_word); g_free(tail_word); break;
                }
                kbis[kbc].comp_size = read_num(&ip, mdd_num_size);
                kbis[kbc].decomp_size = read_num(&ip, mdd_num_size);
                kbis[kbc].head_word = head_word ? head_word : g_strdup("");
                kbis[kbc].tail_word = tail_word ? tail_word : g_strdup("");
                kbis[kbc].file_offset = current_file_offset;
                current_file_offset += kbis[kbc].comp_size;
                
                kbc++;
            }
        }
        g_free(kbi_raw);
        
        fseek(f, kb_data_pos + kb_data_size, SEEK_SET);
        unsigned char rbh[64]; if(fread(rbh, 1, mdd_num_size * 4, f) != (size_t)(mdd_num_size * 4)) {}
        const unsigned char *rp = rbh;
        uint64_t nrb = read_num(&rp, mdd_num_size);
        read_num(&rp, mdd_num_size);
        read_num(&rp, mdd_num_size);
        
        MDD_RBI *rbis = g_malloc0_n(nrb, sizeof(MDD_RBI));"""

content = content.replace(old_parse_kb, new_parse_kb)


# 4. Remove the `kb_data_pos` loop entirely and `qsort`, and fix UTF-16 always True
old_post_rbi = re.search(r"                current_decomp_offset \+= rbis\[i\]\.decomp;\n            \}\n.*?\n            mdd->files\[mdd->num_files\+\+\] = fs;", content, re.DOTALL).group(0)

new_post_rbi = """                current_decomp_offset += rbis[i].decomp;
            }
            
            MddFileState *fs = g_new0(MddFileState, 1);
            fs->mdd_path = g_strdup(mdd_path);
            fs->is_v2 = mdd_is_v2;
            fs->num_size = mdd_num_size;
            fs->encoding_is_utf16 = mdd_encoding_is_utf16;
            fs->encrypted = mdd_encrypted;
            fs->kb_data_pos = kb_data_pos;
            fs->kb_data_size = kb_data_size;
            fs->rbis = rbis;
            fs->nrb = nrb;
            fs->total_decomp = current_decomp_offset;
            fs->kbis = kbis;
            fs->nkb = kbc;
            
            mdd->files[mdd->num_files++] = fs;"""

content = content.replace(old_post_rbi, new_post_rbi)


# 5. Fix `} else { for(size_t i=0; i<res_count; i++) g_free(resources[i].name); g_free(resources); }`
old_cleanup = """        } else {
            for(size_t i=0; i<res_count; i++) g_free(resources[i].name);
            g_free(resources);
        }
        
        g_free(kbis);"""

new_cleanup = """        } else {
            for(size_t i=0; i<kbc; i++) {
                g_free(kbis[i].head_word);
                g_free(kbis[i].tail_word);
            }
            g_free(kbis);
        }"""

content = content.replace(old_cleanup, new_cleanup)


# 6. Fix UTF-16 parsing for MDD ALWAYS UTF-16
old_utf16 = """                            } else {
                                mdd_encoding_is_utf16 = mdd_is_v2 ? 1 : encoding_is_utf16;
                            }
                            g_free(encoding);
                        } else mdd_encoding_is_utf16 = mdd_is_v2 ? 1 : encoding_is_utf16;
                    } else mdd_encoding_is_utf16 = mdd_is_v2 ? 1 : encoding_is_utf16;"""

new_utf16 = """                            }
                            g_free(encoding);
                        }
                    }
                    mdd_encoding_is_utf16 = 1; // MDD keywords are ALWAYS UTF-16"""

content = content.replace(old_utf16, new_utf16)

with open("dict-mdx.c", "w") as f:
    f.write(content)

