#include "transport.h"
#include "../protocol.h"
#include "../term_profile.h"

#include <furi.h>
#include <furi_hal_bt.h>
#include <bt/bt_service/bt.h>
#include <storage/storage.h>

#define TAG "PitermBle"

#define BLE_KEYS_PATH    APP_DATA_PATH(".bt_term.keys")
#define TX_ACK_TIMEOUT_MS 2000

typedef struct {
    Bt* bt;
    FuriHalBleProfileBase* profile;
    BleServiceSerial* serial;
    FuriSemaphore* tx_done;
    TransportRxCallback rx_cb;
    TransportStatusCallback status_cb;
    void* ctx;
    volatile TransportStatus status;
} TransportBle;

/* Runs on the BLE thread holding the service mutex: only copy and signal. */
static uint16_t ble_serial_event(SerialServiceEvent event, void* context) {
    TransportBle* t = context;
    if(event.event == SerialServiceEventTypeDataReceived) {
        if(t->rx_cb) return t->rx_cb(event.data.buffer, event.data.size, t->ctx);
    } else if(event.event == SerialServiceEventTypeDataSent) {
        furi_semaphore_release(t->tx_done);
    } else if(event.event == SerialServiceEventTypesBleResetRequest) {
        FURI_LOG_W(TAG, "Server wrote RPC status, ignoring");
    }
    return 0;
}

static void ble_status_changed(BtStatus bt_status, void* context) {
    TransportBle* t = context;
    TransportStatus status = TransportStatusIdle;
    if(bt_status == BtStatusConnected) {
        status = TransportStatusConnected;
    } else if(bt_status == BtStatusAdvertising) {
        status = TransportStatusWaiting;
    }
    t->status = status;
    if(t->status_cb) t->status_cb(status, t->ctx);
}

static bool ble_open(void* inst) {
    TransportBle* t = inst;
    furi_check(!t->profile);

    t->bt = furi_record_open(RECORD_BT);
    bt_disconnect(t->bt);
    furi_delay_ms(200); /* let the radio core finish writing NVM */

    bt_keys_storage_set_storage_path(t->bt, BLE_KEYS_PATH);
    t->profile = bt_profile_start(t->bt, term_profile, NULL);
    if(!t->profile) {
        FURI_LOG_E(TAG, "Failed to start the BLE profile");
        bt_keys_storage_set_default_path(t->bt);
        furi_check(bt_profile_restore_default(t->bt));
        furi_record_close(RECORD_BT);
        t->bt = NULL;
        return false;
    }
    t->serial = term_profile_get_serial(t->profile);
    ble_svc_serial_set_callbacks(t->serial, PITERM_RX_BUF_SIZE, ble_serial_event, t);

    furi_hal_bt_start_advertising();
    t->status = TransportStatusWaiting;
    bt_set_status_changed_callback(t->bt, ble_status_changed, t);
    return true;
}

static void ble_close(void* inst) {
    TransportBle* t = inst;
    if(!t->profile) return;

    bt_set_status_changed_callback(t->bt, NULL, NULL);
    ble_svc_serial_set_callbacks(t->serial, 0, NULL, NULL);
    bt_disconnect(t->bt);
    furi_delay_ms(200);
    bt_keys_storage_set_default_path(t->bt);
    furi_check(bt_profile_restore_default(t->bt));
    furi_record_close(RECORD_BT);

    t->bt = NULL;
    t->profile = NULL;
    t->serial = NULL;
    t->status = TransportStatusIdle;
}

static bool ble_send(void* inst, const uint8_t* data, size_t len) {
    TransportBle* t = inst;
    if(!t->serial || t->status != TransportStatusConnected) return false;

    while(len > 0) {
        uint16_t chunk = MIN(len, (size_t)PITERM_TX_CHUNK);
        uint8_t buf[PITERM_TX_CHUNK];
        memcpy(buf, data, chunk);

        furi_semaphore_acquire(t->tx_done, 0); /* drop a stale confirmation */
        if(!ble_svc_serial_update_tx(t->serial, buf, chunk)) return false;
        /* Indication: send the next chunk only after the server confirms this one. */
        if(furi_semaphore_acquire(t->tx_done, TX_ACK_TIMEOUT_MS) != FuriStatusOk) {
            FURI_LOG_W(TAG, "Indication not confirmed");
            return false;
        }
        data += chunk;
        len -= chunk;
    }
    return true;
}

static void ble_set_callbacks(
    void* inst,
    TransportRxCallback rx,
    TransportStatusCallback status,
    void* ctx) {
    TransportBle* t = inst;
    t->rx_cb = rx;
    t->status_cb = status;
    t->ctx = ctx;
}

static void ble_rx_drained(void* inst) {
    TransportBle* t = inst;
    /* The firmware restores the window only when it reached 0, otherwise a no-op. */
    if(t->serial) ble_svc_serial_notify_buffer_is_empty(t->serial);
}

static TransportStatus ble_status(void* inst) {
    TransportBle* t = inst;
    return t->status;
}

static void ble_forget_paired(void* inst) {
    TransportBle* t = inst;
    if(t->bt) bt_forget_bonded_devices(t->bt);
}

static void ble_free(void* inst) {
    TransportBle* t = inst;
    ble_close(t);
    furi_semaphore_free(t->tx_done);
    free(t);
}

static const TransportApi transport_ble_api = {
    .open = ble_open,
    .close = ble_close,
    .send = ble_send,
    .set_callbacks = ble_set_callbacks,
    .rx_drained = ble_rx_drained,
    .status = ble_status,
    .forget_paired = ble_forget_paired,
    .free = ble_free,
};

Transport transport_ble_alloc(void) {
    TransportBle* t = malloc(sizeof(TransportBle));
    memset(t, 0, sizeof(TransportBle));
    t->tx_done = furi_semaphore_alloc(1, 0);
    return (Transport){.api = &transport_ble_api, .inst = t};
}
