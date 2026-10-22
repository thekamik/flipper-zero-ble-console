#include "term_buffer.h"

#include <string.h>

enum {
    EscNone,
    EscStart, /* after ESC */
    EscCsi, /* ESC [ ... final byte */
    EscOsc, /* ESC ] ... BEL or ESC \ */
    EscOscEsc,
};

void term_buffer_reset(TermBuffer* tb) {
    memset(tb, 0, sizeof(TermBuffer));
    tb->count = 1;
}

static size_t new_line(TermBuffer* tb) {
    tb->head = (tb->head + 1) % TERM_LINES;
    tb->len[tb->head] = 0;
    if(tb->count < TERM_LINES) tb->count++;
    tb->col = 0;
    return 1;
}

static size_t put_char(TermBuffer* tb, char c) {
    size_t added = 0;
    if(tb->col >= TERM_COLS) added = new_line(tb); /* wrap lazily, on the next character */
    tb->text[tb->head][tb->col++] = c;
    if(tb->col > tb->len[tb->head]) tb->len[tb->head] = tb->col;
    return added;
}

/* Returns true if the byte belongs to an escape sequence and must be skipped. */
static bool filter_escape(TermBuffer* tb, uint8_t b) {
    switch(tb->esc_state) {
    case EscStart:
        if(b == '[') {
            tb->esc_state = EscCsi;
        } else if(b == ']') {
            tb->esc_state = EscOsc;
        } else {
            tb->esc_state = EscNone; /* two-byte sequence, e.g. ESC = */
        }
        return true;
    case EscCsi:
        if(b >= 0x40 && b <= 0x7E) tb->esc_state = EscNone;
        return true;
    case EscOsc:
        if(b == 0x07) tb->esc_state = EscNone;
        if(b == 0x1B) tb->esc_state = EscOscEsc;
        return true;
    case EscOscEsc:
        tb->esc_state = (b == '\\') ? EscNone : EscOsc;
        return true;
    default:
        if(b == 0x1B) {
            tb->esc_state = EscStart;
            return true;
        }
        return false;
    }
}

size_t term_buffer_feed(TermBuffer* tb, const uint8_t* data, size_t len) {
    size_t added = 0;
    for(size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if(filter_escape(tb, b)) continue;

        if(b == '\n') {
            added += new_line(tb);
        } else if(b == '\r') {
            tb->col = 0;
        } else if(b == '\b') {
            if(tb->col > 0) tb->col--;
        } else if(b == '\t') {
            do {
                added += put_char(tb, ' ');
            } while(tb->col % 4 != 0 && tb->col < TERM_COLS);
        } else if(b >= 0x20 && b < 0x7F) {
            added += put_char(tb, (char)b);
        } else if(b >= 0xC0) {
            added += put_char(tb, '?'); /* start of a UTF-8 character */
        }
        /* other control bytes and UTF-8 continuation bytes are dropped */
    }
    return added;
}

size_t term_buffer_line_count(const TermBuffer* tb) {
    return tb->count;
}

void term_buffer_get_line(const TermBuffer* tb, size_t index, char* out) {
    out[0] = '\0';
    if(index >= tb->count) return;
    size_t line = (tb->head + TERM_LINES - (tb->count - 1 - index)) % TERM_LINES;
    memcpy(out, tb->text[line], tb->len[line]);
    out[tb->len[line]] = '\0';
}
