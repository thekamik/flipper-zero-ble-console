/* Screen 1: connection status and settings. */
#include "../piterm.h"

enum {
    ItemStatus,
    ItemEol,
    ItemEcho,
    ItemTerminal,
    ItemForget,
};

static const char* const eol_names[] = {"\\n", "\\r\\n"};
static const char* const echo_names[] = {"Off", "On"};

static void eol_changed(VariableItem* item) {
    PitermApp* app = variable_item_get_context(item);
    app->settings.eol = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, eol_names[app->settings.eol]);
    piterm_settings_save(app);
}

static void echo_changed(VariableItem* item) {
    PitermApp* app = variable_item_get_context(item);
    app->settings.local_echo = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, echo_names[app->settings.local_echo]);
    piterm_settings_save(app);
}

static void settings_enter(void* context, uint32_t index) {
    PitermApp* app = context;
    if(index == ItemTerminal) {
        view_dispatcher_send_custom_event(app->view_dispatcher, PitermEventSettingsTerminal);
    } else if(index == ItemForget) {
        view_dispatcher_send_custom_event(app->view_dispatcher, PitermEventSettingsForget);
    }
}

static void settings_update_status(PitermApp* app) {
    variable_item_set_current_value_text(app->status_item, piterm_state_text(piterm_state(app)));
}

void piterm_scene_settings_on_enter(void* context) {
    PitermApp* app = context;
    VariableItemList* list = app->settings_list;
    VariableItem* item;

    app->status_item = variable_item_list_add(list, "Status", 1, NULL, app);

    item = variable_item_list_add(list, "Line ending", COUNT_OF(eol_names), eol_changed, app);
    variable_item_set_current_value_index(item, app->settings.eol);
    variable_item_set_current_value_text(item, eol_names[app->settings.eol]);

    item = variable_item_list_add(list, "Local echo", COUNT_OF(echo_names), echo_changed, app);
    variable_item_set_current_value_index(item, app->settings.local_echo);
    variable_item_set_current_value_text(item, echo_names[app->settings.local_echo]);

    variable_item_list_add(list, "Terminal >", 0, NULL, app);
    variable_item_list_add(list, "Forget pairing", 0, NULL, app);

    variable_item_list_set_enter_callback(list, settings_enter, app);
    variable_item_list_set_selected_item(
        list, scene_manager_get_scene_state(app->scene_manager, PitermSceneSettings));
    settings_update_status(app);
    view_dispatcher_switch_to_view(app->view_dispatcher, PitermViewSettings);
}

bool piterm_scene_settings_on_event(void* context, SceneManagerEvent event) {
    PitermApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;

    switch(event.event) {
    case PitermEventStatusChanged:
        settings_update_status(app);
        /* jump to the terminal as soon as the server starts a session */
        if(piterm_state(app) == PitermStateSession) {
            scene_manager_next_scene(app->scene_manager, PitermSceneTerminal);
        }
        return true;
    case PitermEventSettingsTerminal:
        scene_manager_set_scene_state(app->scene_manager, PitermSceneSettings, ItemTerminal);
        scene_manager_next_scene(app->scene_manager, PitermSceneTerminal);
        return true;
    case PitermEventSettingsForget:
        app->transport.api->forget_paired(app->transport.inst);
        app->settings.paired = 0;
        piterm_settings_save(app);
        settings_update_status(app);
        return true;
    default:
        return false;
    }
}

void piterm_scene_settings_on_exit(void* context) {
    PitermApp* app = context;
    variable_item_list_reset(app->settings_list);
}
