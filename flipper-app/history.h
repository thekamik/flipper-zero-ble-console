#pragma once

#include <stddef.h>

#define HISTORY_MAX      20
#define HISTORY_LINE_MAX 128

typedef struct History History;

History* history_alloc(void); /* loads APP_DATA_PATH("history.txt") */
void history_free(History* history);

/* Adds a command (skipping empty lines and repeats of the last one) and saves the file. */
void history_add(History* history, const char* line);

size_t history_count(const History* history);
/* 0 = newest */
const char* history_get(const History* history, size_t index);
