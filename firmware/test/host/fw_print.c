/* A host tool over ota/fw_rules.c, for firmware/tools/test_fw_cmd.py and test_stage_update.py (not a test itself).
   usage: fw_print check <bin> [chip_rev]   prints header, layout, version, project, hashed_len and hash lines;
                                            exit 0 only when the header, the layout and the appended hash are all OK
          fw_print make-test-image <out>    writes a synthetic image (Task 1's shape, 3 segments, a real appended
                                            SHA-256, about 96 KB, version 1.6.0+abc) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ota/fw_rules.h"
#include "sha256_tiny.h"

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

static int make_test_image(const char *path)
{
    enum { SEGS = 3, SEG_LEN = 32000 };
    size_t cap = 24 + SEGS * (8 + SEG_LEN) + 16 + 32;
    uint8_t *b = calloc(1, cap);
    if (!b) return 1;
    b[0] = FW_IMG_MAGIC; b[1] = SEGS; b[2] = 2; b[3] = 0x40;
    put32(b + 4, 0x40000000);
    b[8] = 0xEE;
    put16(b + 12, FW_CHIP_ESP32P4);
    put16(b + 15, 0);
    put16(b + 17, 199);
    b[23] = 1;
    size_t off = 24;
    uint8_t ck = 0xEF;
    for (int s = 0; s < SEGS; s++) {
        put32(b + off, 0x40000000 + (uint32_t)s * 0x10000);
        put32(b + off + 4, SEG_LEN);
        off += 8;
        uint8_t *d = b + off;
        memset(d, 0x5A, SEG_LEN);
        if (s == 0) {
            memset(d, 0, 256);
            put32(d, FW_DESC_MAGIC);
            strcpy((char *)d + 16, "1.6.0+abc");
            strcpy((char *)d + 48, FW_PROJECT);
            strcpy((char *)d + 80, "12:34:56");
            strcpy((char *)d + 96, "Oct 25 2026");
            strcpy((char *)d + 112, "v5.5.6");
            for (int i = 0; i < 32; i++) d[144 + i] = (uint8_t)(0xA0 + i);
        }
        for (int i = 0; i < SEG_LEN; i++) ck ^= d[i];
        off += SEG_LEN;
    }
    size_t padded = (off + 1 + 15) & ~(size_t)15;
    b[padded - 1] = ck;
    sha256_tiny(b, padded, b + padded);
    FILE *f = fopen(path, "wb");
    if (!f) { free(b); return 1; }
    size_t w = fwrite(b, 1, padded + 32, f);
    int bad = fclose(f) != 0 || w != padded + 32;
    free(b);
    return bad;
}

static int check(const char *path, int chip_rev)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "fw_print: can't open %s\n", path); return 2; }
    size_t cap = 16u * 1024 * 1024, n = 0;
    uint8_t *b = malloc(cap);
    if (!b) { fclose(f); return 2; }
    n = fread(b, 1, cap, f);
    fclose(f);
    fw_desc_t d;
    memset(&d, 0, sizeof d);
    fw_hdr_t h = fw_check_header(b, n, (uint32_t)n, FW_SLOT_SIZE, (uint16_t)chip_rev, &d);
    fw_layout_t l;
    fw_layout_res_t r = fw_image_layout(b, n, &l);
    printf("header %s\n", fw_hdr_name(h));
    printf("layout %s\n", fw_layout_name(r));
    printf("version %s\n", d.version);
    printf("project %s\n", d.project);
    printf("hashed_len %u\n", (unsigned)l.hashed_len);
    bool hash_ok = false;
    if (r == FW_LAYOUT_OK && l.hash_appended) {
        uint8_t s[32];
        sha256_tiny(b, l.hashed_len, s);
        hash_ok = memcmp(s, b + l.hashed_len, 32) == 0;
        printf("hash %s\n", hash_ok ? "ok" : "bad");
    } else {
        printf("hash none\n");
    }
    free(b);
    return h == FW_HDR_OK && r == FW_LAYOUT_OK && hash_ok ? 0 : 1;
}

int main(int argc, char **argv)
{
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "check")) return check(argv[2], argc == 4 ? atoi(argv[3]) : 100);
    if (argc == 3 && !strcmp(argv[1], "make-test-image")) return make_test_image(argv[2]);
    fprintf(stderr, "usage: %s check <bin> [chip_rev] | make-test-image <out>\n", argv[0]);
    return 2;
}
