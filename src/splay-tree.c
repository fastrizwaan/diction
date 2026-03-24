#include "splay-tree.h"
#include <stdlib.h>
#include <string.h>

static SplayNode* create_node(size_t key_offset, size_t key_length, size_t val_offset, size_t val_length) {
    SplayNode *node = (SplayNode*)malloc(sizeof(SplayNode));
    if (node) {
        node->left = node->right = node->parent = NULL;
        node->key_offset = key_offset;
        node->key_length = key_length;
        node->val_offset = val_offset;
        node->val_length = val_length;
    }
    return node;
}

static void left_rotate(SplayTree *tree, SplayNode *x) {
    SplayNode *y = x->right;
    x->right = y->left;
    if (y->left != NULL) {
        y->left->parent = x;
    }
    y->parent = x->parent;
    if (x->parent == NULL) {
        tree->root = y;
    } else if (x == x->parent->left) {
        x->parent->left = y;
    } else {
        x->parent->right = y;
    }
    y->left = x;
    x->parent = y;
}

static void right_rotate(SplayTree *tree, SplayNode *x) {
    SplayNode *y = x->left;
    x->left = y->right;
    if (y->right != NULL) {
        y->right->parent = x;
    }
    y->parent = x->parent;
    if (x->parent == NULL) {
        tree->root = y;
    } else if (x == x->parent->right) {
        x->parent->right = y;
    } else {
        x->parent->left = y;
    }
    y->right = x;
    x->parent = y;
}

static void splay(SplayTree *tree, SplayNode *x) {
    while (x->parent != NULL) {
        if (x->parent->parent == NULL) {
            if (x == x->parent->left) {
                // zig
                right_rotate(tree, x->parent);
            } else {
                // zag
                left_rotate(tree, x->parent);
            }
        } else if (x == x->parent->left && x->parent == x->parent->parent->left) {
            // zig-zig
            right_rotate(tree, x->parent->parent);
            right_rotate(tree, x->parent);
        } else if (x == x->parent->right && x->parent == x->parent->parent->right) {
            // zag-zag
            left_rotate(tree, x->parent->parent);
            left_rotate(tree, x->parent);
        } else if (x == x->parent->right && x->parent == x->parent->parent->left) {
            // zig-zag
            left_rotate(tree, x->parent);
            right_rotate(tree, x->parent);
        } else {
            // zag-zig
            right_rotate(tree, x->parent);
            left_rotate(tree, x->parent);
        }
    }
}

SplayTree* splay_tree_new(const char *mmap_data, size_t mmap_size) {
    SplayTree *tree = (SplayTree*)malloc(sizeof(SplayTree));
    if (tree) {
        tree->root = NULL;
        tree->mmap_data = mmap_data;
        tree->mmap_size = mmap_size;
    }
    return tree;
}

// Case-insensitive comparison helper
static int compare_keys(const char *mmap_data, size_t ko_a, size_t kl_a, const char *key_b, size_t kl_b) {
    size_t min_len = kl_a < kl_b ? kl_a : kl_b;
    int res = strncasecmp(mmap_data + ko_a, key_b, min_len);
    if (res == 0) {
        if (kl_a < kl_b) return -1;
        if (kl_a > kl_b) return 1;
        return 0;
    }
    return res;
}

static int compare_nodes(const char *mmap_data, SplayNode *a, SplayNode *b) {
    return compare_keys(mmap_data, a->key_offset, a->key_length, mmap_data + b->key_offset, b->key_length);
}

void splay_tree_insert(SplayTree *tree, size_t key_offset, size_t key_length, size_t val_offset, size_t val_length) {
    SplayNode *z = create_node(key_offset, key_length, val_offset, val_length);
    SplayNode *y = NULL;
    SplayNode *x = tree->root;

    while (x != NULL) {
        y = x;
        int cmp = compare_nodes(tree->mmap_data, z, x);
        if (cmp < 0) {
            x = x->left;
        } else if (cmp >= 0) {
            x = x->right; // allow duplicates or stable insert
        }
    }

    z->parent = y;
    if (y == NULL) {
        tree->root = z;
    } else {
        int cmp = compare_nodes(tree->mmap_data, z, y);
        if (cmp < 0) {
            y->left = z;
        } else {
            y->right = z;
        }
    }
    splay(tree, z);
}

SplayNode* splay_tree_search(SplayTree *tree, const char *query) {
    SplayNode *x = tree->root;
    SplayNode *prev = NULL;
    size_t q_len = strlen(query);

    while (x != NULL) {
        prev = x;
        int cmp = compare_keys(tree->mmap_data, x->key_offset, x->key_length, query, q_len);
        if (cmp == 0) {
            splay(tree, x);
            return x;
        } else if (cmp > 0) {
            x = x->left;
        } else {
            x = x->right;
        }
    }
    
    // If not found, splay the last accessed node anyway
    if (prev != NULL) {
        splay(tree, prev);
    }
    return NULL;
}

SplayNode* splay_tree_min(SplayNode *node) {
    while (node->left != NULL) {
        node = node->left;
    }
    return node;
}

SplayNode* splay_tree_successor(SplayNode *node) {
    if (node->right != NULL) {
        return splay_tree_min(node->right);
    }
    SplayNode *y = node->parent;
    while (y != NULL && node == y->right) {
        node = y;
        y = y->parent;
    }
    return y;
}

static void free_nodes(SplayNode *node) {
    if (node != NULL) {
        free_nodes(node->left);
        free_nodes(node->right);
        free(node);
    }
}

void splay_tree_free(SplayTree *tree) {
    if (tree) {
        free_nodes(tree->root);
        free(tree);
    }
}
