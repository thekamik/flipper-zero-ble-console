#pragma once

#include <gui/view.h>

/* On-screen keyboard with full printable ASCII (the built-in TextInput lacks
 * e.g. | > $) and shell control keys. */

#define KEYBOARD_TEXT_MAX 128

typedef struct KeyboardView KeyboardView;

typedef enum {
    KeyboardViewEventEnter,
    KeyboardViewEventTab,
    KeyboardViewEventCtrlC,
    KeyboardViewEventCtrlD,
} KeyboardViewEvent;

/* `text` is the typed line; the view clears the line after the event. */
typedef void (*KeyboardViewCallback)(KeyboardViewEvent event, const char* text, void* context);

KeyboardView* keyboard_view_alloc(void);
void keyboard_view_free(KeyboardView* kb);
View* keyboard_view_get_view(KeyboardView* kb);
void keyboard_view_set_callback(KeyboardView* kb, KeyboardViewCallback cb, void* context);

/* Sets the text (e.g. from history) and moves the cursor to the Enter key. */
void keyboard_view_set_text(KeyboardView* kb, const char* text);
