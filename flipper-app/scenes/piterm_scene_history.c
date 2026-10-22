/* Screen 4: command history. Selecting an entry puts it into the keyboard with
 * the cursor on "ent", so sending takes one click and editing is still possible. */
#include "../piterm.h"

static void history_selected(void* context, uint32_t index) {
    PitermApp* app = context;
    keyboard_view_set_text(app->keyboard, history_get(app->history, index));
    view_dispatcher_send_custom_event(app->view_dispatcher, PitermEventOpenKeyboard);
}

void piterm_scene_history_on_enter(void* context) {
    PitermApp* app = context;
    Submenu* menu = app->history_menu;
    submenu_set_header(menu, history_count(app->history) ? "History" : "History (empty)");
    for(size_t i = 0; i < history_count(app->history); i++) {
        submenu_add_item(menu, history_get(app->history, i), i, history_selected, app);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, PitermViewHistory);
}

bool piterm_scene_history_on_event(void* context, SceneManagerEvent event) {
    PitermApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;
    if(event.event == PitermEventOpenKeyboard) {
        /* history -> keyboard; Back from the keyboard returns to the terminal */
        scene_manager_previous_scene(app->scene_manager);
        scene_manager_next_scene(app->scene_manager, PitermSceneKeyboard);
        return true;
    }
    return event.event == PitermEventStatusChanged;
}

void piterm_scene_history_on_exit(void* context) {
    PitermApp* app = context;
    submenu_reset(app->history_menu);
}
