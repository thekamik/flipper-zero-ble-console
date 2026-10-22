#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Transport layer separated from the UI: screens do not know whether BLE or
 * (in the future) UART is underneath. */

typedef enum {
    TransportStatusIdle,
    TransportStatusWaiting, /* advertising, waiting for the server */
    TransportStatusConnected,
} TransportStatus;

/* Called on the transport thread. Must only copy the data and return the free
 * space left in the receive buffer. */
typedef size_t (*TransportRxCallback)(const uint8_t* data, size_t len, void* ctx);
/* Called on the transport thread when the status changes. */
typedef void (*TransportStatusCallback)(TransportStatus status, void* ctx);

typedef struct {
    bool (*open)(void* inst);
    void (*close)(void* inst);
    /* Blocks until the data is confirmed. Call from the application thread. */
    bool (*send)(void* inst, const uint8_t* data, size_t len);
    void (*set_callbacks)(
        void* inst,
        TransportRxCallback rx,
        TransportStatusCallback status,
        void* ctx);
    /* The app has drained its receive buffer (flow control). Never call from the RX callback. */
    void (*rx_drained)(void* inst);
    TransportStatus (*status)(void* inst);
    /* Removes paired devices (if the transport has any). */
    void (*forget_paired)(void* inst);
    void (*free)(void* inst);
} TransportApi;

typedef struct {
    const TransportApi* api;
    void* inst;
} Transport;

Transport transport_ble_alloc(void);
