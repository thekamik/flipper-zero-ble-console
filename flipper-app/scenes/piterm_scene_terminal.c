/* Screen 3: scrollable shell output. */
#include "../piterm.h"
#include "../protocol.h"

static void terminal_callback(TerminalViewEvent event, void* context) {
    PitermApp* app = context;
    static const PitermEvent map[] = {
        [TerminalViewEventKeyboard] = PitermEventOpenKeyboard,
        [TerminalViewEventHistory] = PitermEventOpenHistory,
        [TerminalViewEventEnter] = PitermEventSendEnter,
        [TerminalViewEventCtrlC] = PitermEventSendCtrlC,
    };
    view_dispatcher_send_custom_event(app->view_dispatcher, map[event]);
}

void piterm_scene_terminal_on_enter(void* context) {
    PitermApp* app = context;
    terminal_view_set_callback(app->terminal, terminal_callback, app);
    piterm_refresh_status(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, PitermViewTerminal);
}

bool piterm_scene_terminal_on_event(void* context, SceneManagerEvent event) {
    PitermApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    switch(event.event) {
    case PitermEventOpenKeyboard:
        scene_manager_next_scene(app->scene_manager, PitermSceneKeyboard);
        return true;
    case PitermEventOpenHistory:
        scene_manager_next_scene(app->scene_manager, PitermSceneHistory);
        return true;
    case PitermEventSendEnter:
        piterm_send_line(app, "");
        return true;
    case PitermEventSendCtrlC: {
        uint8_t c = PITERM_CTRL_C;
        piterm_send(app, &c, 1);
        return true;
    }
    case PitermEventStatusChanged:
        return true; /* the app-level callback updates the status bar */
    default:
        return false;
    }
}

void piterm_scene_terminal_on_exit(void* context) {
    PitermApp* app = context;
    terminal_view_set_callback(app->terminal, NULL, NULL);
}
