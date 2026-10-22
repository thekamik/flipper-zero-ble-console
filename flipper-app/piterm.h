#pragma once

#include <furi.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/submenu.h>

#include "history.h"
#include "transport/transport.h"
#include "views/terminal_view.h"
#include "views/keyboard_view.h"
#include "scenes/piterm_scene.h"

typedef enum {
    PitermViewSettings,
    PitermViewTerminal,
    PitermViewKeyboard,
    PitermViewHistory,
} PitermView;

typedef enum {
    PitermEventStatusChanged,
    PitermEventOpenKeyboard,
    PitermEventOpenHistory,
    PitermEventSendEnter,
    PitermEventSendCtrlC,
    PitermEventLineEnter,
    PitermEventLineTab,
    PitermEventLineCtrlC,
    PitermEventLineCtrlD,
    PitermEventSettingsTerminal,
    PitermEventSettingsForget,
} PitermEvent;

typedef enum {
    PitermEolLf,
    PitermEolCrLf,
} PitermEol;

typedef struct {
    uint8_t eol; /* PitermEol */
    uint8_t local_echo;
    uint8_t paired; /* a server session has happened (data requires an authenticated link) */
} PitermSettings;

/* Connection state shown to the user. */
typedef enum {
    PitermStateOff,
    PitermStateWaitPairing, /* advertising, no session yet */
    PitermStatePaired, /* advertising, paired, waiting for the server */
    PitermStateConnected, /* BLE link up, nothing received from the server yet */
    PitermStateSession, /* data received from the server */
} PitermState;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    VariableItemList* settings_list;
    VariableItem* status_item;
    TerminalView* terminal;
    KeyboardView* keyboard;
    Submenu* history_menu;

    Transport transport;
    FuriStreamBuffer* rx_stream;
    FuriThread* worker;
    volatile TransportStatus status;
    volatile bool session; /* data received from the server on this connection */

    History* history;
    PitermSettings settings;
    char pending_line[KEYBOARD_TEXT_MAX + 1]; /* keyboard line waiting to be sent */
} PitermApp;

/* Sends bytes to the server (echoing them locally if enabled). */
bool piterm_send(PitermApp* app, const uint8_t* data, size_t len);
/* Sends a line followed by the configured line ending. */
bool piterm_send_line(PitermApp* app, const char* line);
void piterm_settings_save(PitermApp* app);
PitermState piterm_state(const PitermApp* app);
const char* piterm_state_text(PitermState state);
/* Updates the terminal status bar (hidden while a session is active). */
void piterm_refresh_status(PitermApp* app);
