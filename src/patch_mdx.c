static int cmp_tree_entry_doff(const void *a, const void *b) {
    const FlatTreeEntry *ea = a;
    const FlatTreeEntry *eb = b;
    if (ea->d_off < eb->d_off) return -1;
    if (ea->d_off > eb->d_off) return 1;
    return 0;
}

static int cmp_tree_entry_hoff(const void *a, const void *b) {
    const FlatTreeEntry *ea = a;
    const FlatTreeEntry *eb = b;
    if (ea->h_off < eb->h_off) return -1;
    if (ea->h_off > eb->h_off) return 1;
    return 0;
}
