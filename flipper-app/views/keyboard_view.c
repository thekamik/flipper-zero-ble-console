#include "keyboard_view.h"

#include <furi.h>
#include <gui/elements.h>

#define CHAR_ROWS   4
#define CHAR_COLS   11
#define FUNC_COLS   8
#define ROWS        (CHAR_ROWS + 1)
#define KEY_W       11
#define KEY_H       10
#define FUNC_W      16
#define GRID_X      3
#define GRID_Y      11
#define PREVIEW_MAX 19 /* must match "%.19s" in keyboard_draw */

typedef enum {
    LayerLower,
    LayerUpper,
    LayerSymbols,
    LayerCount,
} Layer;

/* Together the layers cover every printable ASCII character. */
static const char* const layers[LayerCount][CHAR_ROWS] = {
    [LayerLower] = {"1234567890-", "qwertyuiop/", "asdfghjkl._", "zxcvbnm,|~:"},
    [LayerUpper] = {"1234567890-", "QWERTYUIOP/", "ASDFGHJKL._", "ZXCVBNM,|~:"},
    [LayerSymbols] = {"!@#$%^&*()=", "[]{}<>|\\/+-", "'\"`;:,.?_~&", "0123456789 "},
};

typedef enum {
    FuncShift,
    FuncSymbols,
    FuncSpace,
    FuncDelete,
    FuncTab,
    FuncCtrlC,
    FuncCtrlD,
    FuncEnter,
} FuncKey;

static const char* const func_labels[FUNC_COLS] = {"aA", "#@", "spc", "del", "tab", "^C", "^D", "ent"};

struct KeyboardView {
    View* view;
    KeyboardViewCallback callback;
    void* context;
};

typedef struct {
    char text[KEYBOARD_TEXT_MAX + 1];
    size_t len;
    uint8_t row;
    uint8_t col;
    Layer layer;
} KeyboardModel;

static void draw_key(Canvas* canvas, int x, int y, int w, const char* label, bool selected) {
    if(selected) {
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_box(canvas, x, y, w, KEY_H);
        canvas_set_color(canvas, ColorWhite);
    }
    canvas_draw_str_aligned(canvas, x + w / 2, y + KEY_H / 2, AlignCenter, AlignCenter, label);
    canvas_set_color(canvas, ColorBlack);
}

static void keyboard_draw(Canvas* canvas, void* model_ptr) {
    KeyboardModel* m = model_ptr;
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

    /* line preview: the last characters + cursor */
    canvas_set_font(canvas, FontKeyboard);
    const char* tail = m->len > PREVIEW_MAX ? m->text + m->len - PREVIEW_MAX : m->text;
    char preview[PREVIEW_MAX + 3];
    snprintf(preview, sizeof(preview), "%s%.19s_", m->len > PREVIEW_MAX ? "<" : "", tail);
    canvas_draw_str(canvas, 1, 8, preview);
    canvas_draw_line(canvas, 0, 10, 127, 10);

    for(uint8_t r = 0; r < CHAR_ROWS; r++) {
        const char* row = layers[m->layer][r];
        for(uint8_t c = 0; c < CHAR_COLS; c++) {
            char label[2] = {row[c], '\0'};
            if(label[0] == ' ') label[0] = '_';
            draw_key(
                canvas,
                GRID_X + c * KEY_W,
                GRID_Y + r * KEY_H,
                KEY_W,
                label,
                m->row == r && m->col == c);
        }
    }

    canvas_set_font(canvas, FontSecondary);
    for(uint8_t c = 0; c < FUNC_COLS; c++) {
        const char* label = func_labels[c];
        if(c == FuncShift && m->layer == LayerUpper) label = "Aa";
        if(c == FuncSymbols && m->layer == LayerSymbols) label = "ab";
        draw_key(
            canvas,
            c * FUNC_W,
            GRID_Y + CHAR_ROWS * KEY_H,
            FUNC_W,
            label,
            m->row == CHAR_ROWS && m->col == c);
    }
}

static void insert_char(KeyboardModel* m, char c) {
    if(m->len < KEYBOARD_TEXT_MAX) {
        m->text[m->len++] = c;
        m->text[m->len] = '\0';
    }
}

/* Returns true and sets `event` if the key finishes editing the line. */
static bool press(KeyboardModel* m, bool long_press, KeyboardViewEvent* event) {
    if(m->row < CHAR_ROWS) {
        char c = layers[m->layer][m->row][m->col];
        if(long_press && c >= 'a' && c <= 'z') c = c - 'a' + 'A';
        insert_char(m, c);
        if(m->layer == LayerUpper) m->layer = LayerLower; /* one-shot shift */
        return false;
    }
    switch((FuncKey)m->col) {
    case FuncShift:
        m->layer = m->layer == LayerUpper ? LayerLower : LayerUpper;
        return false;
    case FuncSymbols:
        m->layer = m->layer == LayerSymbols ? LayerLower : LayerSymbols;
        return false;
    case FuncSpace:
        insert_char(m, ' ');
        return false;
    case FuncDelete:
        if(long_press) {
            m->len = 0;
        } else if(m->len > 0) {
            m->len--;
        }
        m->text[m->len] = '\0';
        return false;
    case FuncTab:
        *event = KeyboardViewEventTab;
        return true;
    case FuncCtrlC:
        *event = KeyboardViewEventCtrlC;
        return true;
    case FuncCtrlD:
        *event = KeyboardViewEventCtrlD;
        return true;
    case FuncEnter:
        *event = KeyboardViewEventEnter;
        return true;
    }
    return false;
}

static void move(KeyboardModel* m, InputKey key) {
    uint8_t cols = m->row == CHAR_ROWS ? FUNC_COLS : CHAR_COLS;
    switch(key) {
    case InputKeyLeft:
        m->col = (m->col + cols - 1) % cols;
        break;
    case InputKeyRight:
        m->col = (m->col + 1) % cols;
        break;
    case InputKeyUp:
    case InputKeyDown: {
        uint8_t row = key == InputKeyUp ? (m->row + ROWS - 1) % ROWS : (m->row + 1) % ROWS;
        /* map the column between the character rows and the function row */
        if(m->row == CHAR_ROWS && row != CHAR_ROWS) {
            m->col = (m->col * FUNC_W + FUNC_W / 2 - GRID_X) / KEY_W;
        } else if(m->row != CHAR_ROWS && row == CHAR_ROWS) {
            m->col = (GRID_X + m->col * KEY_W + KEY_W / 2) / FUNC_W;
        }
        m->row = row;
        break;
    }
    default:
        break;
    }
}

static bool keyboard_input(InputEvent* event, void* context) {
    KeyboardView* kb = context;
    if(event->key == InputKeyBack) return false; /* go back, keep the line */

    bool done = false;
    KeyboardViewEvent out = KeyboardViewEventEnter;
    char text[KEYBOARD_TEXT_MAX + 1];

    if(event->type != InputTypeShort && event->type != InputTypeRepeat &&
       event->type != InputTypeLong) {
        return true;
    }
    with_view_model(
        kb->view,
        KeyboardModel * m,
        {
            if(event->key == InputKeyOk) {
                if(event->type != InputTypeRepeat) {
                    done = press(m, event->type == InputTypeLong, &out);
                }
            } else if(event->type != InputTypeLong) {
                move(m, event->key);
            }
            if(done) {
                strlcpy(text, m->text, sizeof(text));
                m->len = 0;
                m->text[0] = '\0';
                m->layer = LayerLower;
            }
        },
        true);

    if(done && kb->callback) kb->callback(out, text, kb->context);
    return true;
}

KeyboardView* keyboard_view_alloc(void) {
    KeyboardView* kb = malloc(sizeof(KeyboardView));
    kb->callback = NULL;
    kb->context = NULL;
    kb->view = view_alloc();
    view_allocate_model(kb->view, ViewModelTypeLocking, sizeof(KeyboardModel));
    view_set_context(kb->view, kb);
    view_set_draw_callback(kb->view, keyboard_draw);
    view_set_input_callback(kb->view, keyboard_input);
    with_view_model(
        kb->view,
        KeyboardModel * m,
        {
            memset(m, 0, sizeof(KeyboardModel));
            m->row = 1; /* start on "q" */
        },
        false);
    return kb;
}

void keyboard_view_free(KeyboardView* kb) {
    view_free(kb->view);
    free(kb);
}

View* keyboard_view_get_view(KeyboardView* kb) {
    return kb->view;
}

void keyboard_view_set_callback(KeyboardView* kb, KeyboardViewCallback cb, void* context) {
    kb->callback = cb;
    kb->context = context;
}

void keyboard_view_set_text(KeyboardView* kb, const char* text) {
    with_view_model(
        kb->view,
        KeyboardModel * m,
        {
            strlcpy(m->text, text, sizeof(m->text));
            m->len = strlen(m->text);
            m->row = CHAR_ROWS;
            m->col = FuncEnter;
        },
        true);
}
