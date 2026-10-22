#include "history.h"

#include <furi.h>
#include <storage/storage.h>
#include <toolbox/stream/file_stream.h>

#define HISTORY_PATH APP_DATA_PATH("history.txt")

struct History {
    char lines[HISTORY_MAX][HISTORY_LINE_MAX + 1]; /* oldest first */
    size_t count;
};

static void history_push(History* h, const char* line) {
    if(h->count == HISTORY_MAX) {
        memmove(h->lines[0], h->lines[1], sizeof(h->lines[0]) * (HISTORY_MAX - 1));
        h->count--;
    }
    strlcpy(h->lines[h->count++], line, HISTORY_LINE_MAX + 1);
}

static void history_load(History* h) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);
    FuriString* line = furi_string_alloc();
    if(file_stream_open(stream, HISTORY_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        while(stream_read_line(stream, line)) {
            furi_string_trim(line, "\r\n");
            if(!furi_string_empty(line)) history_push(h, furi_string_get_cstr(line));
        }
    }
    furi_string_free(line);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}

static void history_save(const History* h) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);
    if(file_stream_open(stream, HISTORY_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        for(size_t i = 0; i < h->count; i++) {
            stream_write_cstring(stream, h->lines[i]);
            stream_write_char(stream, '\n');
        }
    }
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}

History* history_alloc(void) {
    History* h = malloc(sizeof(History));
    h->count = 0;
    history_load(h);
    return h;
}

void history_free(History* h) {
    free(h);
}

void history_add(History* h, const char* line) {
    if(line[0] == '\0') return;
    if(h->count > 0 && strcmp(h->lines[h->count - 1], line) == 0) return;
    history_push(h, line);
    history_save(h);
}

size_t history_count(const History* h) {
    return h->count;
}

const char* history_get(const History* h, size_t index) {
    furi_check(index < h->count);
    return h->lines[h->count - 1 - index];
}
