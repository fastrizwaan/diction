static gboolean mdd_find_resource(MddFileState *fs, const char *query, uint64_t *out_start_off, uint64_t *out_end_off) {
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
