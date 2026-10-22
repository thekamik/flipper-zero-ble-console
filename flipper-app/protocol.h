#pragma once

/*
 * PiTerm protocol constants. Single source of truth: docs/PROTOCOL.md.
 * Any change here requires the same change in pi-server/src/protocol.rs.
 *
 * Service and characteristic UUIDs (TX/RX/flow control) are defined by the
 * firmware: targets/f7/ble_glue/services/serial_service.*
 *   serwis  8fe5b3d5-2e7f-4a98-2a48-7acc60fe0000
 *   TX      19ed82ae-ed21-4c9d-4145-228e61fe0000  Flipper -> Pi, indicate
 *   RX      19ed82ae-ed21-4c9d-4145-228e62fe0000  Pi -> Flipper, write
 *   FLOW    19ed82ae-ed21-4c9d-4145-228e63fe0000  u32 big-endian, notify
 */

/* Advertised name: "PiTerm <Flipper name>". */
#define PITERM_ADV_NAME_PREFIX "PiTerm"

/* Offset of MAC byte [2]: default profile +0, HID +1, PiTerm +2. */
#define PITERM_MAC_OFFSET 2

/* Flow control window = size of the Flipper receive buffer. */
#define PITERM_RX_BUF_SIZE 1024

/* Size of a single TX indication. Without knowing the MTU, 20 B is safe
 * (ATT MTU 23 - 3); commands are short anyway. */
#define PITERM_TX_CHUNK 20

/* Control characters sent as-is. */
#define PITERM_CTRL_C 0x03
#define PITERM_CTRL_D 0x04
#define PITERM_TAB    0x09
