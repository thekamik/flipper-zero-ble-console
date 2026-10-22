#pragma once

#include <gui/view.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Scrollable shell output. */

typedef struct TerminalView TerminalView;

typedef enum {
    TerminalViewEventKeyboard, /* OK */
    TerminalViewEventHistory, /* Left */
    TerminalViewEventEnter, /* Right: empty line */
    TerminalViewEventCtrlC, /* long OK */
} TerminalViewEvent;

typedef void (*TerminalViewCallback)(TerminalViewEvent event, void* context);

TerminalView* terminal_view_alloc(void);
void terminal_view_free(TerminalView* view);
View* terminal_view_get_view(TerminalView* view);
void terminal_view_set_callback(TerminalView* view, TerminalViewCallback cb, void* context);

/* Safe to call from any thread (locks the view model). */
void terminal_view_feed(TerminalView* view, const uint8_t* data, size_t len);
/* Status bar at the top of the screen; NULL hides it (session active). */
void terminal_view_set_status(TerminalView* view, const char* status);
void terminal_view_clear(TerminalView* view);
