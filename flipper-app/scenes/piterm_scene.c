#include "piterm_scene.h"

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_enter,
static void (*const piterm_on_enter_handlers[])(void*) = {
#include "piterm_scene_config.h"
};
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_event,
static bool (*const piterm_on_event_handlers[])(void* context, SceneManagerEvent event) = {
#include "piterm_scene_config.h"
};
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_exit,
static void (*const piterm_on_exit_handlers[])(void* context) = {
#include "piterm_scene_config.h"
};
#undef ADD_SCENE

const SceneManagerHandlers piterm_scene_handlers = {
    .on_enter_handlers = piterm_on_enter_handlers,
    .on_event_handlers = piterm_on_event_handlers,
    .on_exit_handlers = piterm_on_exit_handlers,
    .scene_num = PitermSceneNum,
};
