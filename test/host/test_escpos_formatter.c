#include "escpos_formatter.h"
#include "unity.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define ESC 0x1B
#define AT  0x40
#define LF  0x0A

static void test_empty_string(void)
{
    uint8_t buf[16];
    const uint8_t expected[] = {ESC, AT, LF};

    size_t written = escpos_format("", 0, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(sizeof(expected), written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));
}

static void test_simple_text(void)
{
    uint8_t buf[16];
    const uint8_t expected[] = {ESC, AT, 'H', 'e', 'l', 'l', 'o', LF};

    size_t written = escpos_format("Hello", 5, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(sizeof(expected), written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));
}

static void test_embedded_newline(void)
{
    uint8_t buf[32];
    const char *text = "line1\nline2";
    const uint8_t expected[] = {
        ESC, AT,
        'l', 'i', 'n', 'e', '1', LF,
        'l', 'i', 'n', 'e', '2',
        LF,
    };

    size_t written = escpos_format(text, strlen(text), buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(sizeof(expected), written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));
}

static void test_punctuation_and_digits(void)
{
    uint8_t buf[32];
    const char *text = "Order #42!";
    const uint8_t expected[] = {
        ESC, AT,
        'O', 'r', 'd', 'e', 'r', ' ', '#', '4', '2', '!',
        LF,
    };

    size_t written = escpos_format(text, strlen(text), buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(sizeof(expected), written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));
}

static void test_null_text_with_zero_len_matches_empty_string(void)
{
    uint8_t buf[16];
    const uint8_t expected[] = {ESC, AT, LF};

    size_t written = escpos_format(NULL, 0, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(sizeof(expected), written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));
}

static void test_null_text_with_nonzero_len_rejected(void)
{
    uint8_t buf[16];
    TEST_ASSERT_EQUAL_UINT(0, escpos_format(NULL, 5, buf, sizeof(buf)));
}

static void test_null_out_buf_rejected(void)
{
    TEST_ASSERT_EQUAL_UINT(0, escpos_format("Hello", 5, NULL, 16));
}

static void test_buffer_too_small_rejected(void)
{
    uint8_t buf[4];
    uint8_t sentinel[4];
    memset(sentinel, 0xAA, sizeof(sentinel));
    memcpy(buf, sentinel, sizeof(buf));

    /* "Hi" needs ESCPOS_FRAME_OVERHEAD_LEN(3) + 2 = 5 bytes; buf only holds 4. */
    size_t written = escpos_format("Hi", 2, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(0, written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(sentinel, buf, sizeof(buf));
}

static void test_buffer_exact_fit_succeeds(void)
{
    uint8_t buf[8];
    const uint8_t expected[] = {ESC, AT, 'H', 'e', 'l', 'l', 'o', LF};

    /* Exactly ESCPOS_FRAME_OVERHEAD_LEN(3) + 5 = 8 bytes -- no slack. */
    size_t written = escpos_format("Hello", 5, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(sizeof(expected), written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));
}

#define GS 0x1D
#define BANG 0x21

static void test_sized_normal_1x1(void)
{
    uint8_t buf[32];
    const uint8_t expected[] = {
        ESC, AT,
        GS, BANG, 0x00,
        'H', 'i',
        GS, BANG, 0x00,
        LF,
    };

    size_t written = escpos_format_sized("Hi", 2, 1, 1, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(sizeof(expected), written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));
}

static void test_sized_double_width_and_height(void)
{
    uint8_t buf[32];
    /* width_mult=2, height_mult=2 -> n = (1 << 4) | 1 = 0x11 */
    const uint8_t expected[] = {
        ESC, AT,
        GS, BANG, 0x11,
        'B', 'i', 'g',
        GS, BANG, 0x00,
        LF,
    };

    size_t written = escpos_format_sized("Big", 3, 2, 2, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(sizeof(expected), written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));
}

static void test_sized_asymmetric_width_and_height(void)
{
    uint8_t buf[32];
    /* width_mult=3, height_mult=5 -> n = (2 << 4) | 4 = 0x24 */
    const uint8_t expected[] = {
        ESC, AT,
        GS, BANG, 0x24,
        'X',
        GS, BANG, 0x00,
        LF,
    };

    size_t written = escpos_format_sized("X", 1, 3, 5, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(sizeof(expected), written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));
}

static void test_sized_max_size_8x8(void)
{
    uint8_t buf[32];
    /* width_mult=8, height_mult=8 -> n = (7 << 4) | 7 = 0x77 */
    const uint8_t expected[] = {
        ESC, AT,
        GS, BANG, 0x77,
        'Y',
        GS, BANG, 0x00,
        LF,
    };

    size_t written = escpos_format_sized("Y", 1, 8, 8, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(sizeof(expected), written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));
}

static void test_sized_empty_string(void)
{
    uint8_t buf[32];
    const uint8_t expected[] = {
        ESC, AT,
        GS, BANG, 0x11,
        GS, BANG, 0x00,
        LF,
    };

    size_t written = escpos_format_sized("", 0, 2, 2, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(sizeof(expected), written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));
}

static void test_sized_width_zero_rejected(void)
{
    uint8_t buf[32];
    TEST_ASSERT_EQUAL_UINT(0, escpos_format_sized("Hi", 2, 0, 2, buf, sizeof(buf)));
}

static void test_sized_height_zero_rejected(void)
{
    uint8_t buf[32];
    TEST_ASSERT_EQUAL_UINT(0, escpos_format_sized("Hi", 2, 2, 0, buf, sizeof(buf)));
}

static void test_sized_width_too_large_rejected(void)
{
    uint8_t buf[32];
    TEST_ASSERT_EQUAL_UINT(0, escpos_format_sized("Hi", 2, 9, 2, buf, sizeof(buf)));
}

static void test_sized_height_too_large_rejected(void)
{
    uint8_t buf[32];
    TEST_ASSERT_EQUAL_UINT(0, escpos_format_sized("Hi", 2, 2, 9, buf, sizeof(buf)));
}

static void test_sized_null_text_with_nonzero_len_rejected(void)
{
    uint8_t buf[32];
    TEST_ASSERT_EQUAL_UINT(0, escpos_format_sized(NULL, 5, 2, 2, buf, sizeof(buf)));
}

static void test_sized_null_out_buf_rejected(void)
{
    TEST_ASSERT_EQUAL_UINT(0, escpos_format_sized("Hi", 2, 2, 2, NULL, 32));
}

static void test_sized_buffer_too_small_rejected(void)
{
    uint8_t buf[10];
    uint8_t sentinel[10];
    memset(sentinel, 0xAA, sizeof(sentinel));
    memcpy(buf, sentinel, sizeof(buf));

    /* "Hi" needs ESCPOS_SIZED_FRAME_OVERHEAD_LEN(9) + 2 = 11 bytes; buf only holds 10. */
    size_t written = escpos_format_sized("Hi", 2, 2, 2, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(0, written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(sentinel, buf, sizeof(buf));
}

static void test_sized_buffer_exact_fit_succeeds(void)
{
    uint8_t buf[11];
    const uint8_t expected[] = {
        ESC, AT,
        GS, BANG, 0x11,
        'H', 'i',
        GS, BANG, 0x00,
        LF,
    };

    /* Exactly ESCPOS_SIZED_FRAME_OVERHEAD_LEN(9) + 2 = 11 bytes -- no slack. */
    size_t written = escpos_format_sized("Hi", 2, 2, 2, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(sizeof(expected), written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));
}

static void test_raster_small_bitmap(void)
{
    uint8_t buf[32];
    /* 8x2 image: 1 byte/row, 2 rows -- row0 all black, row1 all white. */
    const uint8_t bitmap[] = {0xFF, 0x00};
    const uint8_t expected[] = {
        ESC, AT,
        GS, 'v', '0', 0x00, /* header */
        0x01, 0x00,         /* width_bytes = 1, little-endian */
        0x02, 0x00,         /* height_px = 2, little-endian */
        0xFF, 0x00,         /* bitmap data */
        LF,
    };

    size_t written = escpos_format_raster(bitmap, 1, 2, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(sizeof(expected), written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));
}

static void test_raster_multi_byte_width(void)
{
    uint8_t buf[32];
    /* 24x1 image: 3 bytes/row, 1 row. */
    const uint8_t bitmap[] = {0xAA, 0xBB, 0xCC};
    const uint8_t expected[] = {
        ESC, AT,
        GS, 'v', '0', 0x00,
        0x03, 0x00,
        0x01, 0x00,
        0xAA, 0xBB, 0xCC,
        LF,
    };

    size_t written = escpos_format_raster(bitmap, 3, 1, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(sizeof(expected), written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));
}

static void test_raster_width_high_byte_encoding(void)
{
    uint8_t buf[600];
    /* width_bytes = 300 (0x012C) -> xL=0x2C, xH=0x01; confirms the
     * little-endian 16-bit split, not just the common single-byte case. */
    uint8_t bitmap[300];
    memset(bitmap, 0x55, sizeof(bitmap));

    size_t written = escpos_format_raster(bitmap, 300, 1, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(ESCPOS_RASTER_FRAME_OVERHEAD_LEN + 300, written);
    TEST_ASSERT_EQUAL_UINT8(0x2C, buf[6]);
    TEST_ASSERT_EQUAL_UINT8(0x01, buf[7]);
}

static void test_raster_zero_width_rejected(void)
{
    uint8_t buf[32];
    const uint8_t bitmap[] = {0xFF};
    TEST_ASSERT_EQUAL_UINT(0, escpos_format_raster(bitmap, 0, 1, buf, sizeof(buf)));
}

static void test_raster_zero_height_rejected(void)
{
    uint8_t buf[32];
    const uint8_t bitmap[] = {0xFF};
    TEST_ASSERT_EQUAL_UINT(0, escpos_format_raster(bitmap, 1, 0, buf, sizeof(buf)));
}

static void test_raster_width_too_large_rejected(void)
{
    uint8_t buf[32];
    const uint8_t bitmap[] = {0xFF};
    /* Rejected before ever reading width_bytes*height_px bytes from
     * bitmap, so a tiny dummy buffer is safe to pass here. */
    TEST_ASSERT_EQUAL_UINT(0, escpos_format_raster(bitmap, 0x10000, 1, buf, sizeof(buf)));
}

static void test_raster_height_too_large_rejected(void)
{
    uint8_t buf[32];
    const uint8_t bitmap[] = {0xFF};
    TEST_ASSERT_EQUAL_UINT(0, escpos_format_raster(bitmap, 1, 0x10000, buf, sizeof(buf)));
}

static void test_raster_null_bitmap_rejected(void)
{
    uint8_t buf[32];
    TEST_ASSERT_EQUAL_UINT(0, escpos_format_raster(NULL, 1, 1, buf, sizeof(buf)));
}

static void test_raster_null_out_buf_rejected(void)
{
    const uint8_t bitmap[] = {0xFF};
    TEST_ASSERT_EQUAL_UINT(0, escpos_format_raster(bitmap, 1, 1, NULL, 32));
}

static void test_raster_buffer_too_small_rejected(void)
{
    uint8_t buf[11];
    uint8_t sentinel[11];
    memset(sentinel, 0xAA, sizeof(sentinel));
    memcpy(buf, sentinel, sizeof(buf));

    const uint8_t bitmap[] = {0xFF, 0x00};
    /* Needs ESCPOS_RASTER_FRAME_OVERHEAD_LEN(11) + 2 = 13 bytes; buf only holds 11. */
    size_t written = escpos_format_raster(bitmap, 1, 2, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(0, written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(sentinel, buf, sizeof(buf));
}

static void test_raster_buffer_exact_fit_succeeds(void)
{
    uint8_t buf[13];
    const uint8_t bitmap[] = {0xFF, 0x00};
    const uint8_t expected[] = {
        ESC, AT,
        GS, 'v', '0', 0x00,
        0x01, 0x00,
        0x02, 0x00,
        0xFF, 0x00,
        LF,
    };

    /* Exactly ESCPOS_RASTER_FRAME_OVERHEAD_LEN(11) + 2 = 13 bytes -- no slack. */
    size_t written = escpos_format_raster(bitmap, 1, 2, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_UINT(sizeof(expected), written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, buf, sizeof(expected));
}

int main(void)
{
    UNITY_BEGIN();

    RUN_TEST(test_empty_string);
    RUN_TEST(test_simple_text);
    RUN_TEST(test_embedded_newline);
    RUN_TEST(test_punctuation_and_digits);
    RUN_TEST(test_null_text_with_zero_len_matches_empty_string);
    RUN_TEST(test_null_text_with_nonzero_len_rejected);
    RUN_TEST(test_null_out_buf_rejected);
    RUN_TEST(test_buffer_too_small_rejected);
    RUN_TEST(test_buffer_exact_fit_succeeds);

    RUN_TEST(test_sized_normal_1x1);
    RUN_TEST(test_sized_double_width_and_height);
    RUN_TEST(test_sized_asymmetric_width_and_height);
    RUN_TEST(test_sized_max_size_8x8);
    RUN_TEST(test_sized_empty_string);
    RUN_TEST(test_sized_width_zero_rejected);
    RUN_TEST(test_sized_height_zero_rejected);
    RUN_TEST(test_sized_width_too_large_rejected);
    RUN_TEST(test_sized_height_too_large_rejected);
    RUN_TEST(test_sized_null_text_with_nonzero_len_rejected);
    RUN_TEST(test_sized_null_out_buf_rejected);
    RUN_TEST(test_sized_buffer_too_small_rejected);
    RUN_TEST(test_sized_buffer_exact_fit_succeeds);

    RUN_TEST(test_raster_small_bitmap);
    RUN_TEST(test_raster_multi_byte_width);
    RUN_TEST(test_raster_width_high_byte_encoding);
    RUN_TEST(test_raster_zero_width_rejected);
    RUN_TEST(test_raster_zero_height_rejected);
    RUN_TEST(test_raster_width_too_large_rejected);
    RUN_TEST(test_raster_height_too_large_rejected);
    RUN_TEST(test_raster_null_bitmap_rejected);
    RUN_TEST(test_raster_null_out_buf_rejected);
    RUN_TEST(test_raster_buffer_too_small_rejected);
    RUN_TEST(test_raster_buffer_exact_fit_succeeds);

    return UNITY_END();
}
