/* Screen 2: keyboard and command line. */
#include "../piterm.h"
#include "../protocol.h"

static void keyboard_callback(KeyboardViewEvent event, const char* text, void* context) {
    PitermApp* app = context;
    static const PitermEvent map[] = {
        [KeyboardViewEventEnter] = PitermEventLineEnter,
        [KeyboardViewEventTab] = PitermEventLineTab,
        [KeyboardViewEventCtrlC] = PitermEventLineCtrlC,
        [KeyboardViewEventCtrlD] = PitermEventLineCtrlD,
    };
    strlcpy(app->pending_line, text, sizeof(app->pending_line));
    view_dispatcher_send_custom_event(app->view_dispatcher, map[event]);
}

void piterm_scene_keyboard_on_enter(void* context) {
    PitermApp* app = context;
    keyboard_view_set_callback(app->keyboard, keyboard_callback, app);
    view_dispatcher_switch_to_view(app->view_dispatcher, PitermViewKeyboard);
}

bool piterm_scene_keyboard_on_event(void* context, SceneManagerEvent event) {
    PitermApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    uint8_t ctrl;
    switch(event.event) {
    case PitermEventLineEnter:
        history_add(app->history, app->pending_line);
        piterm_send_line(app, app->pending_line);
        break;
    case PitermEventLineTab: {
        /* the line goes to readline on the server; editing continues there */
        size_t len = strlen(app->pending_line);
        app->pending_line[len++] = PITERM_TAB;
        piterm_send(app, (const uint8_t*)app->pending_line, len);
        break;
    }
    case PitermEventLineCtrlC:
    case PitermEventLineCtrlD:
        ctrl = event.event == PitermEventLineCtrlC ? PITERM_CTRL_C : PITERM_CTRL_D;
        piterm_send(app, &ctrl, 1);
        break;
    case PitermEventStatusChanged:
        return true;
    default:
        return false;
    }
    scene_manager_search_and_switch_to_previous_scene(app->scene_manager, PitermSceneTerminal);
    return true;
}

void piterm_scene_keyboard_on_exit(void* context) {
    PitermApp* app = context;
    keyboard_view_set_callback(app->keyboard, NULL, NULL);
}
