#include "terminal_view.h"
#include "../term_buffer.h"

#include <furi.h>
#include <gui/elements.h>

#define TERM_ROWS        7
#define TERM_LINE_HEIGHT 9
#define SCROLL_FAST      TERM_ROWS

struct TerminalView {
    View* view;
    TerminalViewCallback callback;
    void* context;
};

typedef struct {
    TermBuffer buffer;
    size_t scroll; /* lines from the bottom; 0 = follow the newest output */
    char status[24]; /* empty = session active, no status bar */
} TerminalViewModel;

static size_t max_scroll(const TerminalViewModel* model) {
    size_t count = term_buffer_line_count(&model->buffer);
    return count > TERM_ROWS ? count - TERM_ROWS : 0;
}

static void terminal_view_draw(Canvas* canvas, void* model_ptr) {
    TerminalViewModel* model = model_ptr;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontKeyboard);

    size_t count = term_buffer_line_count(&model->buffer);
    size_t first = count > TERM_ROWS ? count - TERM_ROWS - model->scroll : 0;
    char line[TERM_COLS + 1];
    for(size_t row = 0; row < TERM_ROWS && first + row < count; row++) {
        term_buffer_get_line(&model->buffer, first + row, line);
        canvas_draw_str(canvas, 0, (row + 1) * TERM_LINE_HEIGHT - 1, line);
    }

    if(model->scroll > 0) {
        elements_scrollbar_pos(canvas, 128, 0, 64, first, count - TERM_ROWS + 1);
    }
    if(model->status[0]) {
        /* status bar (there is no new output without a session anyway) */
        canvas_set_font(canvas, FontSecondary);
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_box(canvas, 0, 0, 128, 10);
        canvas_set_color(canvas, ColorWhite);
        canvas_draw_str_aligned(canvas, 64, 1, AlignCenter, AlignTop, model->status);
        canvas_set_color(canvas, ColorBlack);
    }
}

static bool terminal_view_input(InputEvent* event, void* context) {
    TerminalView* tv = context;

    if(event->key == InputKeyUp || event->key == InputKeyDown) {
        if(event->type != InputTypeShort && event->type != InputTypeRepeat) return true;
        size_t step = event->type == InputTypeRepeat ? SCROLL_FAST : 1;
        with_view_model(
            tv->view,
            TerminalViewModel * model,
            {
                size_t limit = max_scroll(model);
                if(event->key == InputKeyUp) {
                    model->scroll = MIN(model->scroll + step, limit);
                } else {
                    model->scroll = model->scroll > step ? model->scroll - step : 0;
                }
            },
            true);
        return true;
    }

    TerminalViewEvent out;
    if(event->key == InputKeyOk && event->type == InputTypeShort) {
        out = TerminalViewEventKeyboard;
    } else if(event->key == InputKeyOk && event->type == InputTypeLong) {
        out = TerminalViewEventCtrlC;
    } else if(event->key == InputKeyLeft && event->type == InputTypeShort) {
        out = TerminalViewEventHistory;
    } else if(event->key == InputKeyRight && event->type == InputTypeShort) {
        out = TerminalViewEventEnter;
    } else {
        return event->key != InputKeyBack; /* Back is handled by the SceneManager */
    }
    if(tv->callback) tv->callback(out, tv->context);
    return true;
}

TerminalView* terminal_view_alloc(void) {
    TerminalView* tv = malloc(sizeof(TerminalView));
    tv->callback = NULL;
    tv->context = NULL;
    tv->view = view_alloc();
    view_allocate_model(tv->view, ViewModelTypeLocking, sizeof(TerminalViewModel));
    view_set_context(tv->view, tv);
    view_set_draw_callback(tv->view, terminal_view_draw);
    view_set_input_callback(tv->view, terminal_view_input);
    terminal_view_clear(tv);
    return tv;
}

void terminal_view_free(TerminalView* tv) {
    view_free(tv->view);
    free(tv);
}

View* terminal_view_get_view(TerminalView* tv) {
    return tv->view;
}

void terminal_view_set_callback(TerminalView* tv, TerminalViewCallback cb, void* context) {
    tv->callback = cb;
    tv->context = context;
}

void terminal_view_feed(TerminalView* tv, const uint8_t* data, size_t len) {
    with_view_model(
        tv->view,
        TerminalViewModel * model,
        {
            size_t added = term_buffer_feed(&model->buffer, data, len);
            /* a scrolled-up view stays on the same lines */
            if(model->scroll > 0) model->scroll = MIN(model->scroll + added, max_scroll(model));
        },
        true);
}

void terminal_view_set_status(TerminalView* tv, const char* status) {
    with_view_model(
        tv->view,
        TerminalViewModel * model,
        { strlcpy(model->status, status ? status : "", sizeof(model->status)); },
        true);
}

void terminal_view_clear(TerminalView* tv) {
    with_view_model(
        tv->view,
        TerminalViewModel * model,
        {
            term_buffer_reset(&model->buffer);
            model->scroll = 0;
        },
        true);
}
