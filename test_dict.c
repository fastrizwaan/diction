#include "src/dict-mmap.h"
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    DictMmap *d = dict_mmap_open(argv[1]);
    if (d) {
        printf("Successfully mapped dictionary.\n");
        dict_mmap_close(d);
    } else {
        printf("Failed to map dictionary.\n");
    }
    return 0;
}
