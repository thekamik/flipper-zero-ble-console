# PiTerm protocol (Bluetooth LE)

This document is the single source of truth for the communication between the Flipper app (`flipper-app/`) and the server (`pi-server/`). Any change here must be applied to both projects in the same change. The constants live in [`flipper-app/protocol.h`](../flipper-app/protocol.h) and [`pi-server/src/protocol.rs`](../pi-server/src/protocol.rs).

## Overview

PiTerm carries a raw terminal byte stream between a shell on a Linux machine and the Flipper Zero screen. It uses the serial GATT service that is built into the Flipper firmware, so no custom GATT service is needed.

```
 Flipper Zero: BLE peripheral, GATT server        Linux: BLE central, GATT client
┌──────────────────────────────────────┐         ┌─────────────────────────────────┐
│ keyboard ─► TX   (…61fe0000)         │ ─ ind ─►│ ─► pty ─► shell                 │
│ screen  ◄── RX   (…62fe0000)         │ ◄ write ─│ ◄─ pty ◄─┘                      │
│ buffer  ─► FLOW  (…63fe0000)         │ ─ ntf ─►│ send window                     │
└──────────────────────────────────────┘         └─────────────────────────────────┘
```

The protocol has three layers:

1. **Link:** a bonded, encrypted BLE connection (see [Security](#security)).
2. **Transport:** two byte streams over GATT, one per direction, with flow control in the Linux → Flipper direction (see [Data transfer](#data-transfer)).
3. **Terminal:** unframed terminal bytes, interpreted as a dumb terminal (see [Terminal semantics](#terminal-semantics)).

## Roles

| Device | BLE role | GATT role | Terminal role |
|---|---|---|---|
| Flipper Zero | peripheral (advertises, accepts connections) | server | input and display |
| Linux machine | central (scans, connects) | client | runs the shell |

The Flipper firmware does not expose the BLE central role to apps, so the Flipper cannot connect to the Linux machine. The Linux side always initiates the connection.

## Advertising and identification

When the app starts, it stops the active BLE profile, starts its own profile (`flipper-app/term_profile.c`) and begins advertising.

| Field | Value |
|---|---|
| Local name | `PiTerm <Flipper name>`, e.g. `PiTerm Re0pl0p` |
| Advertised 16-bit service UUID | `0x3080` OR the hardware color code (`0x3080`–`0x3082`), same as the stock Flipper profile |
| Appearance | `0x8600` |
| Address | public Flipper BLE MAC with byte `[2]` increased by **2** |
| Requested connection interval | 7.5 ms to 45 ms |

The MAC offset keeps the PiTerm identity separate from the Flipper's other profiles: the default profile (used by the mobile app) uses offset 0 and the HID profile uses offset 1. For example, if `Flipper Re0pl0p` is `80:E1:27:F0:36:CE`, then `PiTerm Re0pl0p` is `80:E1:27:F2:36:CE`.

The PiTerm service and characteristic UUIDs are the same ones the official mobile app RPC uses. A UUID match therefore does not prove that a device runs PiTerm. The server only connects to:
- the address in `flipper_addr`, or, if it's unset,
- the only **paired** device whose name starts with `PiTerm`.

## Security

- The profile enables bonding and uses the `PinCodeShow` pairing method: the Flipper displays a 6-digit passkey, and the Linux side enters it (BLE passkey entry).
- Pairing happens once, using `bluetoothctl`, a desktop Bluetooth dialog or `piterm-server pair`. The Linux side marks the device as trusted.
- The serial service characteristics are protected by the firmware: they require an encrypted link from a bonded device. Any data exchanged therefore implies successful pairing.
- The Flipper keeps PiTerm bonding keys in a separate file, `/ext/apps_data/piterm/.bt_term.keys`. Bonds with phones, which are stored in the default key file, are not affected.
- When the app exits, it restores the default key storage path and the default BLE profile.

## GATT layout

Service UUID: `8fe5b3d5-2e7f-4a98-2a48-7acc60fe0000` (firmware: `targets/f7/ble_glue/services/serial_service.*`)

| Name | UUID | Properties | Direction | Content |
|---|---|---|---|---|
| TX | `19ed82ae-ed21-4c9d-4145-228e61fe0000` | read, **indicate** | Flipper → Linux | bytes typed by the user |
| RX | `19ed82ae-ed21-4c9d-4145-228e62fe0000` | write, write without response | Linux → Flipper | shell output |
| FLOW | `19ed82ae-ed21-4c9d-4145-228e63fe0000` | read, notify | Flipper → Linux | free space in the Flipper receive buffer, `uint32` **big-endian** |
| RPC status | `19ed82ae-ed21-4c9d-4145-228e64fe0000` | read, write, notify | not used | **must never be written** |

Writing `0` to RPC status makes the firmware raise a BLE reset request. PiTerm does not use this characteristic; the server never writes it, and the app only logs the event if it happens.

Firmware limits:

| Limit | Value |
|---|---|
| Maximum characteristic value length | 243 bytes |
| Maximum data per TX update call | 486 bytes |
| RX value length | variable, up to the ATT MTU − 3 for a single write |

## Data transfer

### Linux → Flipper (RX, shell output)

The Linux side writes shell output to RX, preferably as **write without response**. The server acquires a write socket from BlueZ (`AcquireWrite`), which also reports the usable MTU. If BlueZ doesn't provide one, it falls back to regular writes of 20 bytes.

Each write is at most:

```
min(usable MTU, 243, current window, remaining data)
```

where the window is described in [Flow control](#flow-control).

On the Flipper, the firmware passes each write to the app's callback on the BLE thread. The app only copies the bytes into a 1024-byte stream buffer and returns. A separate worker thread reads the stream buffer and feeds the terminal.

### Flipper → Linux (TX, user input)

The Flipper sends input as **indications** on TX:

- Data is split into **20-byte** chunks. Apps can't read the negotiated ATT MTU, and 20 bytes (default ATT MTU 23 − 3) always fits.
- After each chunk, the app waits for the indication confirmation (`SerialServiceEventTypeDataSent`) before sending the next chunk. Indications are therefore acknowledged and arrive in order.
- If no confirmation arrives within **2 seconds**, the send is abandoned and logged.
- The app only sends while connected.

The Linux side subscribes to TX (`StartNotify`; BlueZ handles indications and their confirmations the same way as notifications) and writes every received value to the shell's pty unchanged.

### Flow control

Flow control applies only to Linux → Flipper. Its purpose is to keep shell output from overflowing the Flipper's 1024-byte receive buffer.

**Window.** FLOW holds the number of bytes the Linux side may still send, as a big-endian `uint32`. The firmware maintains it as follows:

- When the app registers its buffer, the window is set to the buffer size (**1024**).
- Every RX write decreases the window by the number of bytes written (not below 0). If a write is larger than the window, the firmware logs a warning; data may be lost.
- When the app has drained its buffer, it calls `ble_svc_serial_notify_buffer_is_empty()`. **Only if the window is exactly 0**, the firmware resets it to 1024 and sends a FLOW notification. Otherwise the call does nothing.

**Linux side rules:**

1. After connecting, read FLOW to get the initial window, then subscribe to FLOW notifications.
2. Before each write, take `n = min(chunk, window)` bytes and subtract `n` from the local copy of the window.
3. If the window is 0, wait for a FLOW notification and set the window to the notified value.
4. Never stop at a small non-zero window and wait: the Flipper only restores the window after it reaches exactly 0. Keep sending until the window is used up.
5. Handle FLOW notifications independently of sending (the server uses a separate task), so a sender waiting for the window cannot block the notification it is waiting for.

**Flipper side rules:**

1. The RX callback runs on the BLE thread while the service mutex is held. It must only copy data and must not call `ble_svc_serial_notify_buffer_is_empty()` (the mutex is not recursive, so this would deadlock).
2. The worker thread calls `ble_svc_serial_notify_buffer_is_empty()` every time it finds the stream buffer empty after processing data. Calling it while the window is non-zero is harmless.

**Example** (MTU large enough, 1500 bytes of output):

```
Linux                                   Flipper (window in firmware)
read FLOW ─────────────────────────────► 1024
write 243 B  (local window 781)  ──────► 781     worker drains, notify_buffer_is_empty: no-op
write 243 B  (538)               ──────► 538
write 243 B  (295)               ──────► 295
write 243 B  (52)                ──────► 52
write 52 B   (0)                 ──────► 0       worker drains, notify_buffer_is_empty:
                                                 window reset to 1024
                       ◄────────────────── FLOW notify 1024
write 243 B  (781)               ──────► 781
write 233 B  (548)               ──────► 548     all 1500 bytes sent
```

## Terminal semantics

The byte stream has no framing, encoding or escaping. Both directions carry ordinary terminal bytes.

**Input (Flipper → Linux)**

| Action on the Flipper | Bytes sent |
|---|---|
| send a line | the line, then `\n` (or `\r\n`, an app setting) |
| Enter on an empty line | `\n` (or `\r\n`) |
| Tab | the current line, then `0x09` (completion happens in the remote shell) |
| Ctrl+C | `0x03` |
| Ctrl+D | `0x04` |

The Flipper edits the line locally and sends it as a whole, except for Tab: after a Tab, the partial line is in the remote shell's line editor and editing continues there. The remote pty echoes input, so the app's local echo is off by default.

**Output (Linux → Flipper)**

The server runs the shell with `TERM=dumb` and reports a 21 × 7 terminal to it, so programs avoid escape sequences and wrap to the Flipper screen. The Flipper still interprets the output as follows:

| Byte(s) | Handling |
|---|---|
| printable ASCII (`0x20`–`0x7E`) | drawn; the line wraps after 21 characters |
| `\n` | new line |
| `\r` | cursor to column 0 (following characters overwrite) |
| `\b` | cursor one column left |
| `\t` | spaces up to the next multiple of 4 columns |
| `ESC [ … final byte` (CSI), `ESC ] … BEL` or `ESC ] … ESC \` (OSC), `ESC x` | removed |
| UTF-8 lead byte (`0xC0`–`0xFF`) | drawn as `?` |
| UTF-8 continuation bytes and other control bytes | dropped |

The Flipper keeps the last 192 lines of output.

## Session lifecycle

```
Flipper app                         Linux server
───────────                         ────────────
start: own profile, advertise
  status: Waiting to pair / Paired
                                    find device (flipper_addr or paired "PiTerm …")
                                    connect (pairing with PIN the first time)
  status: Connected
                                    read FLOW, subscribe FLOW and TX
                                    start shell in pty (TERM=dumb, 21×7)
                                    shell prompt ──► RX
  first RX data
  status: Session active
  ◄────────────── terminal bytes in both directions ──────────────►
```

1. **Start.** The app disconnects any current BLE connection, switches to the PiTerm profile and key file, and advertises.
2. **Connect.** The server finds the device and connects. On the first connection the devices pair (PIN on the Flipper). The server rejects devices that aren't paired.
3. **Set up.** The server reads FLOW, subscribes to FLOW and TX, and starts the shell in a pty.
4. **Session.** The first RX data marks the session as active on the Flipper. After the first session, the app remembers that it is paired.
5. **Shell exit** (`exit`, Ctrl+D). The server sends `\r\n[piterm] new shell\r\n` and starts a new shell on the same connection.
6. **Disconnect** (out of range, app exit). The server closes the shell, waits `reconnect_delay_secs` (default 5 s) and tries again. The Flipper returns to advertising.
7. **App exit.** The Flipper disconnects, restores the default key storage path and the default BLE profile, so the mobile app works again.

### Flipper status

| Status | Condition |
|---|---|
| Off | the BLE profile could not be started |
| Waiting to pair | advertising, no session has happened yet |
| Paired | advertising, a session has happened before |
| Connected | connected, no RX data received yet on this connection |
| Session active | RX data received on this connection |

The firmware doesn't tell apps whether a bond exists, so `Paired` is a flag the app stores after the first session (`/ext/apps_data/piterm/settings.bin`). **Forget pairing** removes the bonding keys and clears the flag.

## Constants

| Constant | Value | Flipper (`protocol.h`) | Server (`protocol.rs`) |
|---|---|---|---|
| Name prefix | `PiTerm` | `PITERM_ADV_NAME_PREFIX` | `ADV_NAME_PREFIX` |
| MAC offset | 2 | `PITERM_MAC_OFFSET` | — |
| Receive buffer / initial window | 1024 B | `PITERM_RX_BUF_SIZE` | read from FLOW |
| TX chunk | 20 B | `PITERM_TX_CHUNK` | — |
| RX chunk without known MTU | 20 B | — | `DEFAULT_CHUNK` |
| Maximum RX value | 243 B | firmware | `RX_VALUE_LEN_MAX` |
| Control characters | `0x03`, `0x04`, `0x09` | `PITERM_CTRL_C`, `PITERM_CTRL_D`, `PITERM_TAB` | — |

## Versioning

The protocol is not versioned and has no handshake: both sides must come from the same revision of this repository. If the protocol ever needs incompatible changes (framing, terminal resize, status messages), the first step is a short handshake at the start of a session, e.g. the server sending a `PITERM/1` line. That handshake must be specified here before it is implemented.
