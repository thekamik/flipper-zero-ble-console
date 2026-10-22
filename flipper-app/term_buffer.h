#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Ring buffer of shell output with line wrapping and an ANSI filter. It has no
 * furi dependencies, so it can be tested on the host (tests/term_buffer_test.c). */

#define TERM_COLS  21 /* FontKeyboard: 6 px per glyph, 128 px screen */
#define TERM_LINES 192 /* about 4 KB */

typedef struct {
    char text[TERM_LINES][TERM_COLS];
    uint8_t len[TERM_LINES];
    size_t head; /* index of the current (last) line */
    size_t count; /* number of lines in use, >= 1 */
    uint8_t col;
    uint8_t esc_state;
} TermBuffer;

void term_buffer_reset(TermBuffer* tb);

/* Appends raw bytes from the shell. Returns the number of new lines. */
size_t term_buffer_feed(TermBuffer* tb, const uint8_t* data, size_t len);

size_t term_buffer_line_count(const TermBuffer* tb);

/* Line `index`, counting from the oldest. Writes it as a C string to `out`
 * (at least TERM_COLS + 1 bytes). */
void term_buffer_get_line(const TermBuffer* tb, size_t index, char* out);
