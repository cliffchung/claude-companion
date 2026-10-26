#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"
#include "ota/fw_rules.h"

void setUp(void) {}
void tearDown(void) {}

static void put32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }

/* A synthetic image: header (chip 18, revs 0..199, hash appended), `segments` segments of seg_len bytes (the first
   starts with the app descriptor: project claude_companion, version 1.6.0+abc), 0x5A elsewhere, the padding, the
   XOR checksum and an appended "hash" of 32 0x11 bytes (the pure walk doesn't hash). Returns the length. */
static size_t make_image(int segments, uint32_t seg_len, uint8_t *out, size_t cap)
{
    size_t need = 24 + (size_t)segments * (8 + seg_len) + 16 + 32;
    TEST_ASSERT_TRUE(need <= cap);
    memset(out, 0, cap);
    out[0] = FW_IMG_MAGIC;
    out[1] = (uint8_t)segments;
    out[2] = 2; out[3] = 0x40;
    put32(out + 4, 0x40000000);
    out[8] = 0xEE;
    put16(out + 12, 18);
    put16(out + 15, 0);
    put16(out + 17, 199);
    out[23] = 1;
    size_t off = 24;
    uint8_t ck = 0xEF;
    for (int s = 0; s < segments; s++) {
        put32(out + off, 0x40000000 + (uint32_t)s * 0x10000);
        put32(out + off + 4, seg_len);
        off += 8;
        uint8_t *d = out + off;
        memset(d, 0x5A, seg_len);
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
        for (uint32_t i = 0; i < seg_len; i++) ck ^= d[i];
        off += seg_len;
    }
    size_t padded = (off + 1 + 15) & ~(size_t)15;
    out[padded - 1] = ck;
    memset(out + padded, 0x11, 32);
    return padded + 32;
}

static uint8_t img[200000];

static size_t good(void) { return make_image(3, 1024, img, sizeof img); }

/* ---- info ---- */

static size_t info_buf(uint8_t *b, const char *line, size_t hdr)
{
    size_t l = strlen(line);
    memcpy(b, line, l);
    for (size_t i = 0; i < hdr; i++) b[l + i] = (uint8_t)i;
    return l + hdr;
}

#define HEX64 "3f2a9c1e0123456789abcdef0123456789abcdef0123456789abcdef01234567"

static void test_info_ok(void)
{
    uint8_t b[600];
    uint32_t size; char sha[65]; const uint8_t *hdr; size_t hl;
    size_t n = info_buf(b, "6374957 " HEX64 "\n", 288);
    TEST_ASSERT_TRUE(fw_parse_info(b, n, &size, sha, &hdr, &hl));
    TEST_ASSERT_EQUAL_UINT32(6374957, size);
    TEST_ASSERT_EQUAL_STRING(HEX64, sha);
    TEST_ASSERT_EQUAL(288, hl);
    TEST_ASSERT_EQUAL_PTR(b + strlen("6374957 " HEX64 "\n"), hdr);
    n = info_buf(b, "100 " HEX64 "\n", 100);
    TEST_ASSERT_TRUE(fw_parse_info(b, n, &size, sha, &hdr, &hl));
    TEST_ASSERT_EQUAL_UINT32(100, size);
    TEST_ASSERT_EQUAL(100, hl);
}

static void test_info_rejects(void)
{
    uint8_t b[600];
    uint32_t size; char sha[65]; const uint8_t *hdr; size_t hl;
    size_t n = info_buf(b, "6374957 " HEX64, 0);
    TEST_ASSERT_FALSE(fw_parse_info(b, n, &size, sha, &hdr, &hl));                 /* no newline */
    n = info_buf(b, "63749x7 " HEX64 "\n", 288);
    TEST_ASSERT_FALSE(fw_parse_info(b, n, &size, sha, &hdr, &hl));                 /* a letter */
    n = info_buf(b, "12345678901 " HEX64 "\n", 288);
    TEST_ASSERT_FALSE(fw_parse_info(b, n, &size, sha, &hdr, &hl));                 /* 11 digits */
    n = info_buf(b, "6374957 " "3f2a9c1e0123456789abcdef0123456789abcdef0123456789abcdef0123456" "\n", 288);
    TEST_ASSERT_FALSE(fw_parse_info(b, n, &size, sha, &hdr, &hl));                 /* 63 */
    n = info_buf(b, "6374957 " HEX64 "8\n", 288);
    TEST_ASSERT_FALSE(fw_parse_info(b, n, &size, sha, &hdr, &hl));                 /* 65 */
    n = info_buf(b, "6374957 " "3F2A9C1E0123456789abcdef0123456789abcdef0123456789abcdef01234567" "\n", 288);
    TEST_ASSERT_FALSE(fw_parse_info(b, n, &size, sha, &hdr, &hl));                 /* uppercase */
    n = info_buf(b, "6374957" HEX64 "\n", 288);
    TEST_ASSERT_FALSE(fw_parse_info(b, n, &size, sha, &hdr, &hl));                 /* no space */
    n = info_buf(b, "7000000 " HEX64 "\n", 287);
    TEST_ASSERT_FALSE(fw_parse_info(b, n, &size, sha, &hdr, &hl));                 /* short header */
    n = info_buf(b, "7000000 " HEX64 "\n", 289);
    TEST_ASSERT_FALSE(fw_parse_info(b, n, &size, sha, &hdr, &hl));                 /* one too many */
    n = info_buf(b, " " HEX64 "\n", 0);
    TEST_ASSERT_FALSE(fw_parse_info(b, n, &size, sha, &hdr, &hl));                 /* no size */
    n = info_buf(b, "4294967296 " HEX64 "\n", 288);
    TEST_ASSERT_FALSE(fw_parse_info(b, n, &size, sha, &hdr, &hl));                 /* over 32 bits */
    TEST_ASSERT_FALSE(fw_parse_info(NULL, 0, &size, sha, &hdr, &hl));
}

/* ---- header ---- */

#define BIG 7000000u

static fw_hdr_t hdr_check(const uint8_t *b, uint32_t size) { return fw_check_header(b, FW_HDR_LEN, size, FW_SLOT_SIZE, 100, NULL); }

static void test_header_ok(void)
{
    good();
    fw_desc_t d;
    TEST_ASSERT_EQUAL(FW_HDR_OK, fw_check_header(img, FW_HDR_LEN, BIG, FW_SLOT_SIZE, 100, &d));
    TEST_ASSERT_EQUAL_STRING("1.6.0+abc", d.version);
    TEST_ASSERT_EQUAL_STRING(FW_PROJECT, d.project);
    TEST_ASSERT_EQUAL_STRING("12:34:56", d.time);
    TEST_ASSERT_EQUAL_STRING("Oct 25 2026", d.date);
    TEST_ASSERT_EQUAL_STRING("v5.5.6", d.idf);
    for (int i = 0; i < 32; i++) TEST_ASSERT_EQUAL_HEX8(0xA0 + i, d.elf_sha[i]);
    TEST_ASSERT_EQUAL(18, d.chip_id);
    TEST_ASSERT_EQUAL(0, d.min_rev);
    TEST_ASSERT_EQUAL(199, d.max_rev);
    TEST_ASSERT_EQUAL(3, d.segments);
    TEST_ASSERT_TRUE(d.hash_appended);
    TEST_ASSERT_EQUAL(FW_HDR_OK, hdr_check(img, FW_SLOT_SIZE));
    TEST_ASSERT_EQUAL(FW_HDR_OK, hdr_check(img, FW_MIN_SIZE));
}

static void test_header_each_refusal(void)
{
    uint8_t b[FW_HDR_LEN];
    good();
#define FRESH() memcpy(b, img, FW_HDR_LEN)
    FRESH(); b[0] = 0xE8;          TEST_ASSERT_EQUAL(FW_HDR_MAGIC, hdr_check(b, BIG));
    FRESH(); b[1] = 0;             TEST_ASSERT_EQUAL(FW_HDR_SEGMENTS, hdr_check(b, BIG));
    FRESH(); b[1] = 17;            TEST_ASSERT_EQUAL(FW_HDR_SEGMENTS, hdr_check(b, BIG));
    FRESH(); put16(b + 12, 0x0009); TEST_ASSERT_EQUAL(FW_HDR_CHIP, hdr_check(b, BIG));
    FRESH();
    TEST_ASSERT_EQUAL(FW_HDR_CHIP_REV, fw_check_header(b, FW_HDR_LEN, BIG, FW_SLOT_SIZE, 300, NULL));
    FRESH(); put16(b + 15, 101);   TEST_ASSERT_EQUAL(FW_HDR_CHIP_REV, hdr_check(b, BIG));   /* min above 100 */
    FRESH(); b[32] ^= 1;           TEST_ASSERT_EQUAL(FW_HDR_DESC_MAGIC, hdr_check(b, BIG));
    FRESH(); b[80 + 15] = 'N';     TEST_ASSERT_EQUAL(FW_HDR_PROJECT, hdr_check(b, BIG));
    FRESH(); memset(b + 80, 'c', 32); memcpy(b + 80, FW_PROJECT, 16);
                                   TEST_ASSERT_EQUAL(FW_HDR_PROJECT, hdr_check(b, BIG));
    FRESH(); memset(b + 48, 0, 32); TEST_ASSERT_EQUAL(FW_HDR_NO_VERSION, hdr_check(b, BIG));
    FRESH(); memset(b + 48, '1', 32); TEST_ASSERT_EQUAL(FW_HDR_NO_VERSION, hdr_check(b, BIG));
    FRESH();                       TEST_ASSERT_EQUAL(FW_HDR_TOO_SMALL, hdr_check(b, 65535));
    FRESH();                       TEST_ASSERT_EQUAL(FW_HDR_TOO_BIG, hdr_check(b, FW_SLOT_SIZE + 1));
    FRESH();
    TEST_ASSERT_EQUAL(FW_HDR_SHORT, fw_check_header(b, 100, BIG, FW_SLOT_SIZE, 100, NULL));
    TEST_ASSERT_EQUAL(FW_HDR_TOO_SMALL, fw_check_header(b, 100, 100, FW_SLOT_SIZE, 100, NULL));
#undef FRESH
}

static void test_header_order(void)
{
    uint8_t b[FW_HDR_LEN];
    memset(b, 0, sizeof b);                       /* everything wrong: size first, then magic */
    TEST_ASSERT_EQUAL(FW_HDR_TOO_BIG, hdr_check(b, FW_SLOT_SIZE + 1));
    TEST_ASSERT_EQUAL(FW_HDR_MAGIC, hdr_check(b, BIG));
}

/* ---- layout ---- */

static void test_layout_ok_one_and_nine_segments(void)
{
    fw_layout_t l;
    size_t n = make_image(1, 300, img, sizeof img);
    TEST_ASSERT_EQUAL(FW_LAYOUT_OK, fw_image_layout(img, n, &l));
    TEST_ASSERT_EQUAL_UINT32(n - 32, l.hashed_len);
    TEST_ASSERT_EQUAL_HEX8(l.checksum_calc, l.checksum_stored);
    TEST_ASSERT_TRUE(l.hash_appended);
    TEST_ASSERT_EQUAL(0, l.hashed_len % 16);
    n = make_image(9, 4099, img, sizeof img);
    TEST_ASSERT_EQUAL(FW_LAYOUT_OK, fw_image_layout(img, n, &l));
    TEST_ASSERT_EQUAL_UINT32(n - 32, l.hashed_len);
    TEST_ASSERT_EQUAL_HEX8(l.checksum_calc, l.checksum_stored);
}

static void test_layout_no_hash(void)
{
    fw_layout_t l;
    size_t n = make_image(2, 512, img, sizeof img);
    img[23] = 0;
    TEST_ASSERT_EQUAL(FW_LAYOUT_TRAILING, fw_image_layout(img, n, &l));
    TEST_ASSERT_EQUAL(FW_LAYOUT_OK, fw_image_layout(img, n - 32, &l));
    TEST_ASSERT_FALSE(l.hash_appended);
    TEST_ASSERT_EQUAL_UINT32(n - 32, l.hashed_len);
}

static void test_layout_truncated(void)
{
    fw_layout_t l;
    size_t n = make_image(3, 1024, img, sizeof img);
    TEST_ASSERT_EQUAL(FW_LAYOUT_TRUNCATED, fw_image_layout(img, 24 + 8 + 500, &l));   /* inside a segment */
    TEST_ASSERT_EQUAL(FW_LAYOUT_TRUNCATED, fw_image_layout(img, n - 33, &l));         /* inside the checksum block */
    TEST_ASSERT_EQUAL(FW_LAYOUT_TRUNCATED, fw_image_layout(img, n - 1, &l));          /* inside the hash */
    TEST_ASSERT_EQUAL(FW_LAYOUT_TRUNCATED, fw_image_layout(img, 10, &l));             /* inside the header */
    TEST_ASSERT_EQUAL(FW_LAYOUT_TRUNCATED, fw_image_layout(img, 24 + 4, &l));         /* inside a segment header */
}

static void test_layout_bad_checksum(void)
{
    fw_layout_t l;
    size_t n = make_image(3, 1024, img, sizeof img);
    img[24 + 8 + 600] ^= 0x01;
    TEST_ASSERT_EQUAL(FW_LAYOUT_CHECKSUM, fw_image_layout(img, n, &l));
    TEST_ASSERT_NOT_EQUAL(l.checksum_calc, l.checksum_stored);
}

static void test_layout_trailing(void)
{
    fw_layout_t l;
    size_t n = make_image(3, 1024, img, sizeof img);
    img[n] = 0;
    TEST_ASSERT_EQUAL(FW_LAYOUT_TRAILING, fw_image_layout(img, n + 1, &l));
}

static void test_layout_segment_count(void)
{
    fw_layout_t l;
    size_t n = make_image(3, 1024, img, sizeof img);
    img[1] = 0;  TEST_ASSERT_EQUAL(FW_LAYOUT_SEGMENTS, fw_image_layout(img, n, &l));
    img[1] = 17; TEST_ASSERT_EQUAL(FW_LAYOUT_SEGMENTS, fw_image_layout(img, n, &l));
    n = make_image(3, 1024, img, sizeof img);
    put32(img + 24 + 4, 0xFFFFFFF0u);
    TEST_ASSERT_EQUAL(FW_LAYOUT_TRUNCATED, fw_image_layout(img, n, &l));
}

/* ---- versions and the offer ---- */

static void test_version_cmp(void)
{
    TEST_ASSERT_EQUAL(FW_VER_NEWER, fw_version_cmp("1.6.1", "1.6.0"));
    TEST_ASSERT_EQUAL(FW_VER_OLDER, fw_version_cmp("1.6.0", "1.6.1"));
    TEST_ASSERT_EQUAL(FW_VER_SAME, fw_version_cmp("1.6.0+abc", "1.6.0+def"));
    TEST_ASSERT_EQUAL(FW_VER_SAME, fw_version_cmp("1.6.0+80ab00e.dirty", "1.6.0+80ab00e"));
    TEST_ASSERT_EQUAL(FW_VER_NEWER, fw_version_cmp("v1.7", "1.6.9"));
    TEST_ASSERT_EQUAL(FW_VER_NEWER, fw_version_cmp("V1.7", "1.6.9"));
    TEST_ASSERT_EQUAL(FW_VER_SAME, fw_version_cmp("1.6", "1.6.0"));
    TEST_ASSERT_EQUAL(FW_VER_SAME, fw_version_cmp("1", "1.0.0"));
    TEST_ASSERT_EQUAL(FW_VER_OLDER, fw_version_cmp("1.6.0-rc1", "1.6.0"));
    TEST_ASSERT_EQUAL(FW_VER_NEWER, fw_version_cmp("1.6.0", "1.6.0-rc1"));
    TEST_ASSERT_EQUAL(FW_VER_NEWER, fw_version_cmp("1.6.0-rc2", "1.6.0-rc1"));
    TEST_ASSERT_EQUAL(FW_VER_NEWER, fw_version_cmp("1.10.0", "1.9.0"));
    TEST_ASSERT_EQUAL(FW_VER_UNKNOWN, fw_version_cmp("80ab00e", "1.6.0"));
    TEST_ASSERT_EQUAL(FW_VER_UNKNOWN, fw_version_cmp("1.6.0", "80ab00e"));
    TEST_ASSERT_EQUAL(FW_VER_UNKNOWN, fw_version_cmp("", "1.6.0"));
    TEST_ASSERT_EQUAL(FW_VER_UNKNOWN, fw_version_cmp("1.6.0.1", "1.6.0"));
    TEST_ASSERT_EQUAL(FW_VER_UNKNOWN, fw_version_cmp("1..6", "1.6.0"));
    TEST_ASSERT_EQUAL(FW_VER_UNKNOWN, fw_version_cmp(NULL, "1.6.0"));
}

static void test_offer(void)
{
    fw_desc_t h;
    memset(&h, 0, sizeof h);
    uint8_t same[32], other[32];
    memset(same, 0x42, 32); memset(other, 0x43, 32);
    memcpy(h.elf_sha, same, 32);
    strcpy(h.version, "1.7.0+aaa");
    TEST_ASSERT_EQUAL(FW_OFFER_UP_TO_DATE, fw_offer(&h, "1.6.0+bbb", same));
    TEST_ASSERT_EQUAL(FW_OFFER_NEWER, fw_offer(&h, "1.6.0+bbb", other));
    strcpy(h.version, "1.5.0+aaa");
    TEST_ASSERT_EQUAL(FW_OFFER_OLDER, fw_offer(&h, "1.6.0+bbb", other));
    strcpy(h.version, "1.6.0+aaa");
    TEST_ASSERT_EQUAL(FW_OFFER_OTHER_BUILD, fw_offer(&h, "1.6.0+bbb", other));
    strcpy(h.version, "3f2a9c1");
    TEST_ASSERT_EQUAL(FW_OFFER_OTHER_BUILD, fw_offer(&h, "1.6.0+bbb", other));
}

/* ---- progress ---- */

static void test_pct(void)
{
    TEST_ASSERT_EQUAL(0, fw_pct(0, 0));
    TEST_ASSERT_EQUAL(0, fw_pct(0, 100));
    TEST_ASSERT_EQUAL(0, fw_pct(1, 6374957));
    TEST_ASSERT_EQUAL(50, fw_pct(50, 100));
    TEST_ASSERT_EQUAL(99, fw_pct(6374956, 6374957));
    TEST_ASSERT_EQUAL(100, fw_pct(6374957, 6374957));
    TEST_ASSERT_EQUAL(100, fw_pct(7, 3));
}

static void test_mb_text(void)
{
    char s[32];
    fw_mb_text(3150000, 6374957, s, sizeof s);
    TEST_ASSERT_EQUAL_STRING("3.2 of 6.4 MB", s);
    fw_mb_text(0, 0, s, sizeof s);
    TEST_ASSERT_EQUAL_STRING("0.0 of 0.0 MB", s);
    fw_mb_text(49999, 50000, s, sizeof s);
    TEST_ASSERT_EQUAL_STRING("0.0 of 0.1 MB", s);
    fw_mb_text(12000000, 12000000, s, sizeof s);
    TEST_ASSERT_EQUAL_STRING("12.0 of 12.0 MB", s);
    char t[8];
    memset(t, 'Z', sizeof t);
    fw_mb_text(3150000, 6374957, t, 4);
    TEST_ASSERT_EQUAL_STRING("3.2", t);
    TEST_ASSERT_EQUAL('Z', t[4]);
    fw_mb_text(1, 1, t, 0);
    TEST_ASSERT_EQUAL('3', t[0]);
}

/* ---- gates ---- */

static fw_gate_in_t gate_ok(void)
{
    fw_gate_in_t g = { .layout_ok = true, .rollback_ok = true, .pending = false, .link_up = true, .session_open = false, .voice_active = false, .updating = false,
                       .src = PR_SRC_BATTERY, .battery_pct = 80, .psram_largest = 20u * 1024 * 1024,
                       .image_size = 6374957 };
    return g;
}

static void test_gate_each(void)
{
    fw_gate_in_t g = gate_ok();
    TEST_ASSERT_EQUAL(FW_GATE_OK, fw_gate(&g));
    g = gate_ok(); g.layout_ok = false;    TEST_ASSERT_EQUAL(FW_GATE_LAYOUT, fw_gate(&g));
    g = gate_ok(); g.rollback_ok = false;  TEST_ASSERT_EQUAL(FW_GATE_NO_ROLLBACK, fw_gate(&g));
    g = gate_ok(); g.updating = true;      TEST_ASSERT_EQUAL(FW_GATE_BUSY, fw_gate(&g));
    g = gate_ok(); g.pending = true;       TEST_ASSERT_EQUAL(FW_GATE_PENDING, fw_gate(&g));
    g = gate_ok(); g.link_up = false;      TEST_ASSERT_EQUAL(FW_GATE_NO_LINK, fw_gate(&g));
    g = gate_ok(); g.session_open = true;  TEST_ASSERT_EQUAL(FW_GATE_SESSION, fw_gate(&g));
    g = gate_ok(); g.voice_active = true;  TEST_ASSERT_EQUAL(FW_GATE_VOICE, fw_gate(&g));
    g = gate_ok(); g.src = PR_SRC_UNKNOWN; TEST_ASSERT_EQUAL(FW_GATE_BATTERY_UNKNOWN, fw_gate(&g));
    g = gate_ok(); g.battery_pct = 29;     TEST_ASSERT_EQUAL(FW_GATE_BATTERY, fw_gate(&g));
    g = gate_ok(); g.battery_pct = 30;     TEST_ASSERT_EQUAL(FW_GATE_OK, fw_gate(&g));
    g = gate_ok(); g.src = PR_SRC_USB; g.battery_pct = 5; TEST_ASSERT_EQUAL(FW_GATE_OK, fw_gate(&g));
    g = gate_ok(); g.src = PR_SRC_USB; g.battery_pct = -1; TEST_ASSERT_EQUAL(FW_GATE_OK, fw_gate(&g));
    g = gate_ok(); g.psram_largest = g.image_size + FW_PSRAM_MARGIN - 1; TEST_ASSERT_EQUAL(FW_GATE_MEMORY, fw_gate(&g));
    g = gate_ok(); g.psram_largest = g.image_size + FW_PSRAM_MARGIN;     TEST_ASSERT_EQUAL(FW_GATE_OK, fw_gate(&g));
}

static void test_gate_order(void)
{
    fw_gate_in_t g = { .layout_ok = false, .rollback_ok = false, .pending = true, .link_up = false, .session_open = true, .voice_active = true,
                       .updating = true, .src = PR_SRC_UNKNOWN, .battery_pct = 1, .psram_largest = 0, .image_size = 6374957 };
    TEST_ASSERT_EQUAL(FW_GATE_LAYOUT, fw_gate(&g));
    g.layout_ok = true;     TEST_ASSERT_EQUAL(FW_GATE_NO_ROLLBACK, fw_gate(&g));
    g.rollback_ok = true;   TEST_ASSERT_EQUAL(FW_GATE_BUSY, fw_gate(&g));
    g.updating = false;     TEST_ASSERT_EQUAL(FW_GATE_PENDING, fw_gate(&g));
    g.pending = false;      TEST_ASSERT_EQUAL(FW_GATE_NO_LINK, fw_gate(&g));
    g.link_up = true;       TEST_ASSERT_EQUAL(FW_GATE_SESSION, fw_gate(&g));
    g.session_open = false; TEST_ASSERT_EQUAL(FW_GATE_VOICE, fw_gate(&g));
    g.voice_active = false; TEST_ASSERT_EQUAL(FW_GATE_BATTERY_UNKNOWN, fw_gate(&g));
    g.src = PR_SRC_BATTERY; TEST_ASSERT_EQUAL(FW_GATE_BATTERY, fw_gate(&g));
    g.battery_pct = 50;     TEST_ASSERT_EQUAL(FW_GATE_MEMORY, fw_gate(&g));
}

/* ---- validity and the boot-loop guard ---- */

static void test_validity(void)
{
    TEST_ASSERT_EQUAL(FW_V_NONE, fw_validity(false, false, 0));
    TEST_ASSERT_EQUAL(FW_V_NONE, fw_validity(false, true, 500000));
    TEST_ASSERT_EQUAL(FW_V_MARK_VALID, fw_validity(true, true, 5000));
    TEST_ASSERT_EQUAL(FW_V_MARK_VALID, fw_validity(true, true, 200000));
    TEST_ASSERT_EQUAL(FW_V_WAIT, fw_validity(true, false, 0));
    TEST_ASSERT_EQUAL(FW_V_WAIT, fw_validity(true, false, 179999));
    TEST_ASSERT_EQUAL(FW_V_ROLLBACK, fw_validity(true, false, 180000));
}

static void test_bootloop(void)
{
    TEST_ASSERT_EQUAL(1, fw_bootloop_next(0, true, true));
    TEST_ASSERT_EQUAL(3, fw_bootloop_next(2, true, true));
    TEST_ASSERT_EQUAL(0, fw_bootloop_next(2, true, false));
    TEST_ASSERT_EQUAL(0, fw_bootloop_next(2, false, true));
    TEST_ASSERT_EQUAL(0, fw_bootloop_next(2, false, false));
    TEST_ASSERT_TRUE(fw_bootloop_trip(3, true));
    TEST_ASSERT_TRUE(fw_bootloop_trip(4, true));
    TEST_ASSERT_FALSE(fw_bootloop_trip(3, false));
    TEST_ASSERT_FALSE(fw_bootloop_trip(2, true));
}

/* core I1: a switch by the guard or Go back skips the probation */
static void test_boot_state(void)
{
    TEST_ASSERT_EQUAL(FW_BOOT_NONE, fw_boot_state(false, false, false));
    TEST_ASSERT_EQUAL(FW_BOOT_NONE, fw_boot_state(false, true, true));     /* a stale flag on a valid image: nothing */
    TEST_ASSERT_EQUAL(FW_BOOT_PROBATION, fw_boot_state(true, false, false)); /* a wireless update */
    TEST_ASSERT_EQUAL(FW_BOOT_TRUSTED, fw_boot_state(true, true, true));     /* the guard's or Go back's target */
    TEST_ASSERT_EQUAL(FW_BOOT_PROBATION, fw_boot_state(true, true, false));  /* N2: the flag names another slot */
}

/* round 2, N2: an entry-less complete slot (a build that never ran) is never trusted and never offered */
static void test_trust_and_previous(void)
{
    TEST_ASSERT_TRUE(fw_switch_trusted(true));
    TEST_ASSERT_FALSE(fw_switch_trusted(false));
    TEST_ASSERT_TRUE(fw_previous_ok(true, true, true));
    TEST_ASSERT_FALSE(fw_previous_ok(false, true, true));   /* no VALID entry: a cut before the switch */
    TEST_ASSERT_FALSE(fw_previous_ok(true, false, true));   /* another project */
    TEST_ASSERT_FALSE(fw_previous_ok(true, true, false));   /* no marker for this build: it never ran valid */
    /* so the guard's target is never an entry-less slot either, and a switch to one would not be trusted */
    bool entry_valid = false;
    TEST_ASSERT_FALSE(fw_previous_ok(entry_valid, true, true) && fw_switch_trusted(entry_valid));
}

/* round 2, N1: Go back on a pending image rolls back; one claim on the pending flag wins */
static void test_go_back_and_claim(void)
{
    TEST_ASSERT_EQUAL(FW_GB_ROLLBACK, fw_go_back_mode(true));
    TEST_ASSERT_EQUAL(FW_GB_SWITCH, fw_go_back_mode(false));
    bool pending = true;
    bool go_back = fw_pending_claim(&pending);    /* Go back first */
    bool rollback = fw_pending_claim(&pending);   /* then the 180 s task */
    TEST_ASSERT_TRUE(go_back);
    TEST_ASSERT_FALSE(rollback);
    pending = true;
    rollback = fw_pending_claim(&pending);        /* the other order */
    go_back = fw_pending_claim(&pending);
    TEST_ASSERT_TRUE(rollback ^ go_back);
    TEST_ASSERT_FALSE(pending);
    TEST_ASSERT_FALSE(fw_pending_claim(NULL));
}

/* core I1 and M7: the guard's state machine over boots */
static void test_bootloop_action(void)
{
    TEST_ASSERT_EQUAL(FW_LOOP_NONE, fw_bootloop_action(2, true, 0));
    TEST_ASSERT_EQUAL(FW_LOOP_SWITCH, fw_bootloop_action(3, true, 0));
    TEST_ASSERT_EQUAL(FW_LOOP_GIVE_UP, fw_bootloop_action(3, false, 0));   /* nothing to go to */
    TEST_ASSERT_EQUAL(FW_LOOP_GIVE_UP, fw_bootloop_action(3, true, 1));    /* already fell back once */
    TEST_ASSERT_EQUAL(FW_LOOP_NONE, fw_bootloop_action(0, true, 1));

    /* Simulated boots. A crash-loops; the guard switches to B; B (trusted) is marked valid at once. Ping-pong: B
       crash-loops too: after three more early crashes the guard gives up instead of switching back. */
    int count = 0, fallbacks = 0, switches = 0, gave_up = 0;
    bool trusted = false;
    for (int boot = 0; boot < 12; boot++) {
        count = fw_bootloop_next(count, boot > 0, true);   /* every boot after the first follows an early panic */
        fw_loop_t a = fw_bootloop_action(count, true, fallbacks);
        if (a == FW_LOOP_SWITCH) { switches++; fallbacks++; count = 0; trusted = true; continue; }
        if (a == FW_LOOP_GIVE_UP) gave_up++;
        if (trusted) { TEST_ASSERT_EQUAL(FW_BOOT_TRUSTED, fw_boot_state(true, trusted, true)); trusted = false; }
    }
    TEST_ASSERT_EQUAL(1, switches);
    TEST_ASSERT_TRUE(gave_up >= 1);

    /* a boot that lasts the early window resets both: a later crash loop may fall back again */
    count = 3; fallbacks = 1;
    count = fw_bootloop_next(count, false, false); fallbacks = 0;   /* early_cb */
    for (int i = 0; i < 3; i++) count = fw_bootloop_next(count, true, true);
    TEST_ASSERT_EQUAL(FW_LOOP_SWITCH, fw_bootloop_action(count, true, fallbacks));
}

static void test_confirm_matches(void)
{
    TEST_ASSERT_TRUE(fw_confirm_matches(HEX64, HEX64));
    TEST_ASSERT_FALSE(fw_confirm_matches(HEX64, "3f2a9c1e0123456789abcdef0123456789abcdef0123456789abcdef01234568"));
    TEST_ASSERT_FALSE(fw_confirm_matches("", ""));
    TEST_ASSERT_FALSE(fw_confirm_matches(NULL, HEX64));
    TEST_ASSERT_FALSE(fw_confirm_matches(HEX64, NULL));
}

static void test_attempt_rolled_back(void)
{
    TEST_ASSERT_TRUE(fw_attempt_rolled_back("1.6.1+3f2a9c1", true, "1.6.1+3f2a9c1"));
    TEST_ASSERT_FALSE(fw_attempt_rolled_back("1.6.1+3f2a9c1", false, NULL));          /* power cut before the switch */
    TEST_ASSERT_FALSE(fw_attempt_rolled_back("1.6.1+3f2a9c1", true, "1.5.0+0c1d2e3")); /* an older rollback */
    TEST_ASSERT_FALSE(fw_attempt_rolled_back("", true, ""));
    TEST_ASSERT_FALSE(fw_attempt_rolled_back(NULL, true, "x"));
}

/* ---- outcomes ---- */

static void test_host_rc(void)
{
    TEST_ASSERT_EQUAL(FW_OUT_OK, fw_host_rc(0));
    TEST_ASSERT_EQUAL(FW_OUT_NO_FILE, fw_host_rc(120));
    TEST_ASSERT_EQUAL(FW_OUT_HOST_READ, fw_host_rc(121));
    TEST_ASSERT_EQUAL(FW_OUT_HOST_READ, fw_host_rc(122));
    TEST_ASSERT_EQUAL(FW_OUT_HOST_READ, fw_host_rc(126));
    TEST_ASSERT_EQUAL(FW_OUT_HOST_READ, fw_host_rc(127));
    TEST_ASSERT_EQUAL(FW_OUT_HOST_READ, fw_host_rc(1));
    TEST_ASSERT_EQUAL(FW_OUT_HOST_READ, fw_host_rc(-1));
}

static void test_hdr_out(void)
{
    TEST_ASSERT_EQUAL(FW_OUT_OK, fw_hdr_out(FW_HDR_OK));
    TEST_ASSERT_EQUAL(FW_OUT_CHANGED, fw_hdr_out(FW_HDR_SHORT));
    TEST_ASSERT_EQUAL(FW_OUT_TOO_BIG, fw_hdr_out(FW_HDR_TOO_BIG));
    const fw_hdr_t rest[] = { FW_HDR_MAGIC, FW_HDR_SEGMENTS, FW_HDR_CHIP, FW_HDR_CHIP_REV, FW_HDR_DESC_MAGIC,
                              FW_HDR_PROJECT, FW_HDR_NO_VERSION, FW_HDR_TOO_SMALL };
    for (size_t i = 0; i < sizeof rest / sizeof rest[0]; i++) TEST_ASSERT_EQUAL(FW_OUT_NOT_APP, fw_hdr_out(rest[i]));
}

static void test_gate_out(void)
{
    TEST_ASSERT_EQUAL(FW_OUT_OK, fw_gate_out(FW_GATE_OK));
    TEST_ASSERT_EQUAL(FW_OUT_NO_ROLLBACK, fw_gate_out(FW_GATE_NO_ROLLBACK));
    TEST_ASSERT_EQUAL(FW_OUT_LAYOUT, fw_gate_out(FW_GATE_LAYOUT));
    TEST_ASSERT_EQUAL(FW_OUT_PENDING, fw_gate_out(FW_GATE_PENDING));
    TEST_ASSERT_EQUAL(FW_OUT_BUSY, fw_gate_out(FW_GATE_BUSY));
    TEST_ASSERT_EQUAL(FW_OUT_NO_LINK, fw_gate_out(FW_GATE_NO_LINK));
    TEST_ASSERT_EQUAL(FW_OUT_SESSION, fw_gate_out(FW_GATE_SESSION));
    TEST_ASSERT_EQUAL(FW_OUT_VOICE, fw_gate_out(FW_GATE_VOICE));
    TEST_ASSERT_EQUAL(FW_OUT_BATTERY_UNKNOWN, fw_gate_out(FW_GATE_BATTERY_UNKNOWN));
    TEST_ASSERT_EQUAL(FW_OUT_BATTERY, fw_gate_out(FW_GATE_BATTERY));
    TEST_ASSERT_EQUAL(FW_OUT_NO_MEMORY, fw_gate_out(FW_GATE_MEMORY));
}

static void test_out_text_ids(void)
{
    TEST_ASSERT_EQUAL(STR_COUNT, fw_out_text(FW_OUT_OK));
    const struct { fw_out_t o; str_id_t s; } m[] = {
        { FW_OUT_CANCELLED, STR_FW_CANCELLED }, { FW_OUT_NO_LINK, STR_SES_NOT_CONNECTED },
        { FW_OUT_LINK_LOST, STR_FW_ERR_LINK }, { FW_OUT_TIMEOUT, STR_FW_ERR_TIMEOUT },
        { FW_OUT_NO_FILE, STR_FW_ERR_NO_FILE }, { FW_OUT_HOST_READ, STR_FW_ERR_HOST_READ },
        { FW_OUT_NOT_APP, STR_FW_ERR_NOT_APP }, { FW_OUT_TOO_BIG, STR_FW_ERR_TOO_BIG },
        { FW_OUT_CORRUPT, STR_FW_ERR_CORRUPT }, { FW_OUT_CHANGED, STR_FW_ERR_CHANGED },
        { FW_OUT_WRITE, STR_FW_ERR_WRITE }, { FW_OUT_BATTERY, STR_FW_ERR_BATTERY },
        { FW_OUT_BATTERY_UNKNOWN, STR_FW_ERR_BATTERY_UNKNOWN }, { FW_OUT_SESSION, STR_FW_ERR_BUSY_SESSION },
        { FW_OUT_VOICE, STR_FW_ERR_BUSY_VOICE }, { FW_OUT_BUSY, STR_FW_ERR_BUSY },
        { FW_OUT_NO_MEMORY, STR_FW_ERR_NO_MEMORY }, { FW_OUT_PENDING, STR_FW_ERR_PENDING },
        { FW_OUT_NO_ROLLBACK, STR_FW_ERR_NO_ROLLBACK }, { FW_OUT_LAYOUT, STR_FW_ERR_LAYOUT },
    };
    TEST_ASSERT_EQUAL(FW_OUT_LAYOUT, sizeof m / sizeof m[0]);   /* every value but OK */
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) {
        TEST_ASSERT_EQUAL(m[i].s, fw_out_text(m[i].o));
        const char *t = tr(fw_out_text(m[i].o));
        TEST_ASSERT_TRUE(t[0] != 0);
        TEST_ASSERT_NULL(strstr(t, "Mac"));
    }
    TEST_ASSERT_EQUAL_STRING("No firmware file on the Host at the firmware path (Settings \xe2\x80\xba Host).",
                             tr(STR_FW_ERR_NO_FILE));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_info_ok);
    RUN_TEST(test_info_rejects);
    RUN_TEST(test_header_ok);
    RUN_TEST(test_header_each_refusal);
    RUN_TEST(test_header_order);
    RUN_TEST(test_layout_ok_one_and_nine_segments);
    RUN_TEST(test_layout_no_hash);
    RUN_TEST(test_layout_truncated);
    RUN_TEST(test_layout_bad_checksum);
    RUN_TEST(test_layout_trailing);
    RUN_TEST(test_layout_segment_count);
    RUN_TEST(test_version_cmp);
    RUN_TEST(test_offer);
    RUN_TEST(test_pct);
    RUN_TEST(test_mb_text);
    RUN_TEST(test_gate_each);
    RUN_TEST(test_gate_order);
    RUN_TEST(test_validity);
    RUN_TEST(test_bootloop);
    RUN_TEST(test_boot_state);
    RUN_TEST(test_trust_and_previous);
    RUN_TEST(test_go_back_and_claim);
    RUN_TEST(test_bootloop_action);
    RUN_TEST(test_confirm_matches);
    RUN_TEST(test_attempt_rolled_back);
    RUN_TEST(test_host_rc);
    RUN_TEST(test_hdr_out);
    RUN_TEST(test_gate_out);
    RUN_TEST(test_out_text_ids);
    return UNITY_END();
}
