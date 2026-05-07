
#include <stdio.h>
#include <stdint.h>

typedef struct {
    char signature[4];
    char inputLang[3];
    char outputLang[3];
    uint8_t compression;
    uint32_t wordCount;
    uint32_t shortIndexLength;
    uint32_t titleOffset;
    uint32_t copyrightOffset;
    uint32_t versionOffset;
    uint32_t shortIndexOffset;
    uint32_t fullIndexOffset;
    uint32_t articlesOffset;
} __attribute__((packed)) DCTHeader;

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 1;
    DCTHeader h;
    fread(&h, 1, sizeof(h), f);
    printf("Signature: %.4s\n", h.signature);
    printf("WordCount: %u\n", h.wordCount);
    printf("Compression: %u\n", h.compression);
    printf("TitleOffset: %u\n", h.titleOffset);
    printf("FullIndexOffset: %u\n", h.fullIndexOffset);
    printf("ArticlesOffset: %u\n", h.articlesOffset);
    fseek(f, h.fullIndexOffset, SEEK_SET);
    unsigned char buf[64];
    fread(buf, 1, 64, f);
    for(int i=0; i<64; i++) printf("%02x ", buf[i]);
    printf("\n");
    fclose(f);
    return 0;
}
