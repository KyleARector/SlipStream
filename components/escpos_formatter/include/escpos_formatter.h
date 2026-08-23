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

/* ESC/POS "set character size" command: GS ! n */
#define ESCPOS_CMD_TEXT_SIZE_LEN 3

/* The printer's native width/height multiplier range -- GS !'s single
 * data byte packs (width_mult - 1) into the high nibble and
 * (height_mult - 1) into the low nibble, so 1-8 is the largest range a
 * nibble can express. 1 means normal (unscaled) size. */
#define ESCPOS_TEXT_SIZE_MIN 1
#define ESCPOS_TEXT_SIZE_MAX 8

/* init + GS!(set size) + text + GS!(reset to normal) + trailing LF. */
#define ESCPOS_SIZED_FRAME_OVERHEAD_LEN \
    (ESCPOS_CMD_INIT_LEN + ESCPOS_CMD_TEXT_SIZE_LEN + ESCPOS_CMD_TEXT_SIZE_LEN + ESCPOS_TRAILER_LEN)

/* Same framing as escpos_format() (init sequence + text verbatim + trailing
 * line feed), but wraps the text in a GS ! width/height-multiplier pair:
 * one command setting the requested size right before the text, and one
 * resetting to normal size (1x1) right after it, before the trailing LF.
 *
 * The reset-to-normal is unconditional and always emitted, even when
 * width_mult == height_mult == 1 -- GS ! is printer state that persists
 * across jobs on the same physical printer (the print job FSM reuses one
 * persistent instance, not a fresh one per job), so a formatter that ever
 * left the printer in an enlarged state would silently corrupt the size of
 * every following job, including ones built with plain escpos_format().
 * Emitting the reset unconditionally means this function's output is
 * always self-contained regardless of what came before it.
 *
 * width_mult and height_mult must each be in
 * [ESCPOS_TEXT_SIZE_MIN, ESCPOS_TEXT_SIZE_MAX]; text may be NULL only if
 * text_len is 0. Writes at most out_buf_capacity bytes to out_buf. Returns
 * the number of bytes written on success, or 0 if out_buf is too small or
 * any argument is invalid. */
size_t escpos_format_sized(const char *text, size_t text_len, uint8_t width_mult, uint8_t height_mult,
                            uint8_t *out_buf, size_t out_buf_capacity);

/* ESC/POS "print raster bit image" command header: GS v 0 m xL xH yL yH
 * (the bitmap data itself follows separately, not counted in this length). */
#define ESCPOS_CMD_RASTER_HEADER_LEN 8

/* init + GS v 0 header + trailing LF (bitmap data length is added by the
 * caller: width_bytes * height_px). */
#define ESCPOS_RASTER_FRAME_OVERHEAD_LEN (ESCPOS_CMD_INIT_LEN + ESCPOS_CMD_RASTER_HEADER_LEN + ESCPOS_TRAILER_LEN)

/* Formats a pre-rendered 1-bit-per-pixel bitmap into an ESC/POS byte
 * stream: init sequence + GS v 0 raster header + bitmap data verbatim +
 * one trailing line feed.
 *
 * bitmap_data must already be in the exact format GS v 0 expects: MSB-first
 * packed bits, one row after another, each row exactly width_bytes bytes
 * (so a width not a multiple of 8 is padded to the next byte boundary by
 * whoever produced the bitmap -- this formatter does no bit-packing or
 * padding itself). Per the spec's architecture decision, rendering
 * (dithering, scaling, packing) happens server-side; this formatter's only
 * job is wrapping already-correct bytes in the right command, mirroring
 * escpos_format_sized()'s relationship to GS !.
 *
 * width_bytes and height_px must each be in [1, 0xFFFF] (GS v 0's header
 * fields are 16-bit); bitmap_data must be non-NULL and exactly
 * width_bytes * height_px bytes long. Writes at most out_buf_capacity
 * bytes to out_buf. Returns the number of bytes written on success, or 0
 * if out_buf is too small or any argument is invalid. */
size_t escpos_format_raster(const uint8_t *bitmap_data, size_t width_bytes, size_t height_px, uint8_t *out_buf,
                             size_t out_buf_capacity);

#ifdef __cplusplus
}
#endif
