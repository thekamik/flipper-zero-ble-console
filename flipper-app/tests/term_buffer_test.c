/* Host test: cc -I. tests/term_buffer_test.c term_buffer.c && ./a.out (from flipper-app/) */
#include "../term_buffer.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static TermBuffer tb;
static char line[TERM_COLS + 1];

static void feed(const char* s) {
    term_buffer_feed(&tb, (const uint8_t*)s, strlen(s));
}

static const char* get(size_t i) {
    term_buffer_get_line(&tb, i, line);
    return line;
}

int main(void) {
    term_buffer_reset(&tb);
    feed("hello\r\nworld");
    assert(term_buffer_line_count(&tb) == 2);
    assert(strcmp(get(0), "hello") == 0);
    assert(strcmp(get(1), "world") == 0);

    /* ANSI: colors, OSC window title, two-byte ESC */
    term_buffer_reset(&tb);
    feed("\x1b[01;34mdir\x1b[0m \x1b]0;title\x07ok\x1b=!");
    assert(strcmp(get(0), "dir ok!") == 0);

    /* wrap at exactly 21 characters without an empty line after \r\n */
    term_buffer_reset(&tb);
    feed("123456789012345678901\r\nx");
    assert(term_buffer_line_count(&tb) == 2);
    assert(strcmp(get(1), "x") == 0);
    feed("1234567890123456789012");
    assert(strcmp(get(2), "12") == 0); /* "x" + 20 characters, the rest on a new line */

    /* backspace and overwrite after \r */
    term_buffer_reset(&tb);
    feed("abc\b\bX\rZ");
    assert(strcmp(get(0), "ZXc") == 0);

    /* UTF-8 -> '?' */
    term_buffer_reset(&tb);
    feed("z\xc5\xbc\xc3\xb3\xc5\x82w");
    assert(strcmp(get(0), "z???w") == 0);

    /* ring buffer overflow */
    term_buffer_reset(&tb);
    for(int i = 0; i < TERM_LINES + 10; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d\n", i);
        feed(buf);
    }
    assert(term_buffer_line_count(&tb) == TERM_LINES);
    snprintf(line, sizeof(line), "%d", 11);
    char expected[8];
    strcpy(expected, line);
    assert(strcmp(get(0), expected) == 0);
    assert(strcmp(get(TERM_LINES - 1), "") == 0);

    puts("term_buffer: OK");
    return 0;
}
