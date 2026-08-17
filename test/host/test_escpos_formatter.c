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

    return UNITY_END();
}
