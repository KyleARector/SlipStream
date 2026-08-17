#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESC/POS "initialize printer" command: ESC @ */
#define ESCPOS_CMD_INIT_LEN 2

/* Trailing line feed appended after the text body. */
#define ESCPOS_TRAILER_LEN 1

#define ESCPOS_FRAME_OVERHEAD_LEN (ESCPOS_CMD_INIT_LEN + ESCPOS_TRAILER_LEN)

/* Formats text into an ESC/POS byte stream: init sequence + text verbatim +
 * one trailing line feed. Embedded '\n' bytes in text pass through
 * untouched -- ESC/POS printers treat a bare LF (0x0A) as "advance a line"
 * natively, no translation needed.
 *
 * No cut command is emitted: cut-command support is unconfirmed against the
 * physical printer (see spec's "Notes for Implementation"), so it's
 * deliberately left out until that's verified in a later milestone.
 *
 * text may be NULL only if text_len is 0. Writes at most out_buf_capacity
 * bytes to out_buf. Returns the number of bytes written on success, or 0 if
 * out_buf is too small or arguments are invalid. */
size_t escpos_format(const char *text, size_t text_len, uint8_t *out_buf, size_t out_buf_capacity);

#ifdef __cplusplus
}
#endif
