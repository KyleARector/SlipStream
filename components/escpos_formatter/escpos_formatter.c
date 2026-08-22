#include "escpos_formatter.h"

#include <string.h>

static const uint8_t k_init_cmd[ESCPOS_CMD_INIT_LEN] = {0x1B, 0x40}; /* ESC @ */
static const uint8_t k_trailer[ESCPOS_TRAILER_LEN] = {0x0A};         /* LF */

size_t escpos_format(const char *text, size_t text_len, uint8_t *out_buf, size_t out_buf_capacity)
{
    if (out_buf == NULL) {
        return 0;
    }
    if (text == NULL && text_len != 0) {
        return 0;
    }

    size_t total_len = ESCPOS_FRAME_OVERHEAD_LEN + text_len;
    if (total_len > out_buf_capacity) {
        return 0;
    }

    size_t offset = 0;

    memcpy(out_buf + offset, k_init_cmd, ESCPOS_CMD_INIT_LEN);
    offset += ESCPOS_CMD_INIT_LEN;

    if (text_len > 0) {
        memcpy(out_buf + offset, text, text_len);
        offset += text_len;
    }

    memcpy(out_buf + offset, k_trailer, ESCPOS_TRAILER_LEN);
    offset += ESCPOS_TRAILER_LEN;

    return offset;
}

size_t escpos_format_sized(const char *text, size_t text_len, uint8_t width_mult, uint8_t height_mult,
                            uint8_t *out_buf, size_t out_buf_capacity)
{
    if (out_buf == NULL) {
        return 0;
    }
    if (text == NULL && text_len != 0) {
        return 0;
    }
    if (width_mult < ESCPOS_TEXT_SIZE_MIN || width_mult > ESCPOS_TEXT_SIZE_MAX || height_mult < ESCPOS_TEXT_SIZE_MIN ||
        height_mult > ESCPOS_TEXT_SIZE_MAX) {
        return 0;
    }

    size_t total_len = ESCPOS_SIZED_FRAME_OVERHEAD_LEN + text_len;
    if (total_len > out_buf_capacity) {
        return 0;
    }

    uint8_t size_cmd[ESCPOS_CMD_TEXT_SIZE_LEN] = {0x1D, 0x21, (uint8_t)(((width_mult - 1) << 4) | (height_mult - 1))};
    uint8_t reset_cmd[ESCPOS_CMD_TEXT_SIZE_LEN] = {0x1D, 0x21, 0x00};

    size_t offset = 0;

    memcpy(out_buf + offset, k_init_cmd, ESCPOS_CMD_INIT_LEN);
    offset += ESCPOS_CMD_INIT_LEN;

    memcpy(out_buf + offset, size_cmd, ESCPOS_CMD_TEXT_SIZE_LEN);
    offset += ESCPOS_CMD_TEXT_SIZE_LEN;

    if (text_len > 0) {
        memcpy(out_buf + offset, text, text_len);
        offset += text_len;
    }

    memcpy(out_buf + offset, reset_cmd, ESCPOS_CMD_TEXT_SIZE_LEN);
    offset += ESCPOS_CMD_TEXT_SIZE_LEN;

    memcpy(out_buf + offset, k_trailer, ESCPOS_TRAILER_LEN);
    offset += ESCPOS_TRAILER_LEN;

    return offset;
}
