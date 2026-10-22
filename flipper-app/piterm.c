#include "piterm.h"
#include "protocol.h"

#include <storage/storage.h>

#define TAG "Piterm"

#define SETTINGS_PATH  APP_DATA_PATH("settings.bin")
#define WORKER_STOP    (1 << 0)
#define WORKER_CHUNK   64
#define WORKER_POLL_MS 50

/* Runs on the BLE thread: only copy into the stream buffer. */
static size_t piterm_rx_callback(const uint8_t* data, size_t len, void* ctx) {
    PitermApp* app = ctx;
    size_t sent = furi_stream_buffer_send(app->rx_stream, data, len, 0);
    if(sent < len) FURI_LOG_W(TAG, "RX buffer overflow: %zu B dropped", len - sent);
    return furi_stream_buffer_spaces_available(app->rx_stream);
}

static void piterm_status_callback(TransportStatus status, void* ctx) {
    PitermApp* app = ctx;
    app->status = status;
    app->session = false;
    view_dispatcher_send_custom_event(app->view_dispatcher, PitermEventStatusChanged);
}

/* Worker: RX buffer -> terminal, then restore the flow control window. */
static int32_t piterm_worker(void* ctx) {
    PitermApp* app = ctx;
    uint8_t buf[WORKER_CHUNK];
    while(!(furi_thread_flags_get() & WORKER_STOP)) {
        size_t n = furi_stream_buffer_receive(app->rx_stream, buf, sizeof(buf), WORKER_POLL_MS);
        if(n == 0) continue;
        terminal_view_feed(app->terminal, buf, n);
        if(!app->session) {
            app->session = true;
            view_dispatcher_send_custom_event(app->view_dispatcher, PitermEventStatusChanged);
        }
        if(furi_stream_buffer_is_empty(app->rx_stream)) {
            app->transport.api->rx_drained(app->transport.inst);
        }
    }
    return 0;
}

bool piterm_send(PitermApp* app, const uint8_t* data, size_t len) {
    if(app->settings.local_echo) terminal_view_feed(app->terminal, data, len);
    bool ok = app->transport.api->send(app->transport.inst, data, len);
    if(!ok) FURI_LOG_W(TAG, "Send failed");
    return ok;
}

bool piterm_send_line(PitermApp* app, const char* line) {
    char buf[KEYBOARD_TEXT_MAX + 3];
    size_t len = strlcpy(buf, line, KEYBOARD_TEXT_MAX + 1);
    if(app->settings.eol == PitermEolCrLf) buf[len++] = '\r';
    buf[len++] = '\n';
    return piterm_send(app, (const uint8_t*)buf, len);
}

PitermState piterm_state(const PitermApp* app) {
    switch(app->status) {
    case TransportStatusConnected:
        return app->session ? PitermStateSession : PitermStateConnected;
    case TransportStatusWaiting:
        return app->settings.paired ? PitermStatePaired : PitermStateWaitPairing;
    default:
        return PitermStateOff;
    }
}

const char* piterm_state_text(PitermState state) {
    switch(state) {
    case PitermStateWaitPairing:
        return "Waiting to pair";
    case PitermStatePaired:
        return "Paired";
    case PitermStateConnected:
        return "Connected";
    case PitermStateSession:
        return "Session active";
    default:
        return "Off";
    }
}

void piterm_refresh_status(PitermApp* app) {
    PitermState state = piterm_state(app);
    terminal_view_set_status(
        app->terminal, state == PitermStateSession ? NULL : piterm_state_text(state));
}

static void piterm_settings_load(PitermApp* app) {
    app->settings = (PitermSettings){.eol = PitermEolLf, .local_echo = 0, .paired = 0};
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    PitermSettings loaded;
    if(storage_file_open(file, SETTINGS_PATH, FSAM_READ, FSOM_OPEN_EXISTING) &&
       storage_file_read(file, &loaded, sizeof(loaded)) == sizeof(loaded)) {
        app->settings = loaded;
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

void piterm_settings_save(PitermApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, SETTINGS_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(file, &app->settings, sizeof(app->settings));
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static bool piterm_custom_event_callback(void* ctx, uint32_t event) {
    PitermApp* app = ctx;
    if(event == PitermEventStatusChanged) {
        if(piterm_state(app) == PitermStateSession && !app->settings.paired) {
            app->settings.paired = 1;
            piterm_settings_save(app);
        }
        piterm_refresh_status(app);
    }
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool piterm_back_event_callback(void* ctx) {
    PitermApp* app = ctx;
    return scene_manager_handle_back_event(app->scene_manager);
}

static PitermApp* piterm_alloc(void) {
    PitermApp* app = malloc(sizeof(PitermApp));
    memset(app, 0, sizeof(PitermApp));
    piterm_settings_load(app);
    app->history = history_alloc();

    app->gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&piterm_scene_handlers, app);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->view_dispatcher, piterm_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, piterm_back_event_callback);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->settings_list = variable_item_list_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher,
        PitermViewSettings,
        variable_item_list_get_view(app->settings_list));
    app->terminal = terminal_view_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, PitermViewTerminal, terminal_view_get_view(app->terminal));
    app->keyboard = keyboard_view_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, PitermViewKeyboard, keyboard_view_get_view(app->keyboard));
    app->history_menu = submenu_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, PitermViewHistory, submenu_get_view(app->history_menu));

    app->rx_stream = furi_stream_buffer_alloc(PITERM_RX_BUF_SIZE, 1);
    app->worker = furi_thread_alloc_ex("PitermWorker", 1024, piterm_worker, app);

    app->transport = transport_ble_alloc();
    app->transport.api->set_callbacks(
        app->transport.inst, piterm_rx_callback, piterm_status_callback, app);
    return app;
}

static void piterm_free(PitermApp* app) {
    /* Transport first (no more callbacks), then the worker. */
    app->transport.api->free(app->transport.inst);
    furi_thread_flags_set(furi_thread_get_id(app->worker), WORKER_STOP);
    furi_thread_join(app->worker);
    furi_thread_free(app->worker);
    furi_stream_buffer_free(app->rx_stream);

    view_dispatcher_remove_view(app->view_dispatcher, PitermViewSettings);
    view_dispatcher_remove_view(app->view_dispatcher, PitermViewTerminal);
    view_dispatcher_remove_view(app->view_dispatcher, PitermViewKeyboard);
    view_dispatcher_remove_view(app->view_dispatcher, PitermViewHistory);
    variable_item_list_free(app->settings_list);
    terminal_view_free(app->terminal);
    keyboard_view_free(app->keyboard);
    submenu_free(app->history_menu);
    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);
    furi_record_close(RECORD_GUI);

    history_free(app->history);
    free(app);
}

int32_t piterm_app(void* p) {
    UNUSED(p);
    PitermApp* app = piterm_alloc();

    furi_thread_start(app->worker);
    if(!app->transport.api->open(app->transport.inst)) {
        FURI_LOG_E(TAG, "Failed to open the BLE transport");
    }
    app->status = app->transport.api->status(app->transport.inst);

    scene_manager_next_scene(app->scene_manager, PitermSceneSettings);
    view_dispatcher_run(app->view_dispatcher);

    piterm_free(app);
    return 0;
}
