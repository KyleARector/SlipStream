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
