#pragma once

#include <gui/scene_manager.h>

#define ADD_SCENE(prefix, name, id) PitermScene##id,
typedef enum {
#include "piterm_scene_config.h"
    PitermSceneNum,
} PitermScene;
#undef ADD_SCENE

extern const SceneManagerHandlers piterm_scene_handlers;

#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_enter(void*);
#include "piterm_scene_config.h"
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) \
    bool prefix##_scene_##name##_on_event(void* context, SceneManagerEvent event);
#include "piterm_scene_config.h"
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_exit(void* context);
#include "piterm_scene_config.h"
#undef ADD_SCENE
