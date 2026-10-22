# PiTerm client design (Flipper Zero app, C)

Design notes for `flipper-app/`. For building and usage see the [README](../README.md); for the wire protocol see [PROTOCOL.md](PROTOCOL.md). The Linux side is described in [SERVER_CONCEPT.md](SERVER_CONCEPT.md).

## Goal

A `.fap` that gives the Flipper Zero an interactive terminal to a shell running on a Linux machine. The connection uses Bluetooth LE: the Flipper is the peripheral (GATT server) and the Linux machine connects to it as the central.

There is no SSH or TCP/IP on the Flipper. It exchanges a raw terminal byte stream over the serial GATT service that is built into the firmware. Security comes from BLE pairing with a PIN.

What the protocol requires from the app:
- Send typed bytes as **TX** indications and receive shell output on **RX**.
- After draining the receive buffer, call `ble_svc_serial_notify_buffer_is_empty()` so the flow control window can be restored.
- Advertise as `PiTerm <name>` with a shifted MAC address and a separate bonding keys file.

## Findings from the firmware

These findings are based on the official firmware 1.x (API 88.x).

### Don't use `ble_profile_serial` directly

On every connection, the `bt` service (`applications/services/bt/bt_service/bt.c`) checks whether the active profile is `ble_profile_serial`. If so, it opens a mobile app RPC session and replaces the serial service callback with its own, so PiTerm's data would end up in RPC.

The app therefore defines **its own profile** (`FuriHalBleProfileTemplate`, in `term_profile.c`) whose `start()` calls the exported `ble_svc_serial_start()`. Because the template pointer is different, the `bt` service doesn't attach RPC. The structure follows `lib/ble_profile/extra_profiles/hid_profile.c`.

The serial service functions are public in `api_symbols.csv`:

```c
BleServiceSerial* ble_svc_serial_start(void);
void ble_svc_serial_stop(BleServiceSerial* service);
void ble_svc_serial_set_callbacks(BleServiceSerial* service, uint16_t buff_size,
                                  SerialServiceEventCallback callback, void* context);
void ble_svc_serial_notify_buffer_is_empty(BleServiceSerial* service);
bool ble_svc_serial_update_tx(BleServiceSerial* service, uint8_t* data, uint16_t data_len);
```

Limits: `BLE_SVC_SERIAL_DATA_LEN_MAX = 486` per `update_tx` call, `BLE_SVC_SERIAL_CHAR_VALUE_LEN_MAX = 243` per characteristic value.

### The profile

`term_profile.c` returns this GAP configuration:

| Field | Value | Why |
|---|---|---|
| advertised service | 16-bit `0x3080` OR the hardware color | same as the stock profile |
| appearance | `0x8600` | same as the stock profile |
| bonding / pairing | bonding on, `GapPairingPinCodeShow` | PIN shown on the Flipper, passkey entry on Linux |
| connection interval | 7.5 ms to 45 ms | low latency for interactive use |
| MAC | BLE MAC with byte `[2]` + 2 | separate identity: default profile +0, HID +1 |
| name | `PiTerm <Flipper name>` | the first byte is the AD record type, as in the stock profile |

`stop()` only stops the serial service. The core frees the profile memory, just as with `hid_profile.c`; freeing it in the profile would cause a double free.

### Switching profiles

The sequence follows the Bad USB BLE helper (`applications/main/bad_usb/helpers/bad_usb_hid.c`) and is implemented in `transport/transport_ble.c`.

Start:

```c
Bt* bt = furi_record_open(RECORD_BT);
bt_disconnect(bt);
furi_delay_ms(200);                               // let the radio core finish writing NVM
bt_keys_storage_set_storage_path(bt, APP_DATA_PATH(".bt_term.keys")); // separate bonding keys
FuriHalBleProfileBase* profile = bt_profile_start(bt, term_profile, NULL);
ble_svc_serial_set_callbacks(term_profile_get_serial(profile),
                             PITERM_RX_BUF_SIZE, ble_serial_event, transport);
furi_hal_bt_start_advertising();
bt_set_status_changed_callback(bt, ble_status_changed, transport);
```

Exit. This is required, otherwise the Flipper stays in the PiTerm profile and the mobile app stops working:

```c
bt_set_status_changed_callback(bt, NULL, NULL);
bt_disconnect(bt);
furi_delay_ms(200);
bt_keys_storage_set_default_path(bt);
bt_profile_restore_default(bt);
furi_record_close(RECORD_BT);
```

### Receiving and sending

- The serial event callback runs on the **BLE thread while holding the service mutex**. On `DataReceived` it only copies the data into a `FuriStreamBuffer` and returns the free space.
- `ble_svc_serial_notify_buffer_is_empty()` is called **from the worker thread** after draining the buffer. Calling it from the callback would deadlock, because the mutex is not recursive.
- `ble_svc_serial_update_tx()` sends an indication. The confirmation arrives as `SerialServiceEventTypeDataSent` and releases a semaphore; the next chunk is sent only after that (2 s timeout).
- `SerialServiceEventTypesBleResetRequest` is only logged.
- The pairing PIN screen is drawn by the `bt` service (it handles `GapEventTypePinCodeShow`), so it also works with a custom profile.

## Architecture

```
flipper-app/
├── application.fam            # manifest (host tests excluded from the build)
├── piterm.c / piterm.h        # app struct, ViewDispatcher, SceneManager, worker, settings, status
├── protocol.h                 # constants from docs/PROTOCOL.md
├── term_profile.c / .h        # custom BLE profile
├── term_buffer.c / .h         # output ring buffer, wrapping, ANSI filter (no furi dependency)
├── history.c / .h             # command history on the SD card
├── transport/
│   ├── transport.h            # transport interface
│   └── transport_ble.c        # BLE implementation
├── views/
│   ├── terminal_view.c / .h   # scrollable output with a status bar
│   └── keyboard_view.c / .h   # full-ASCII on-screen keyboard
├── scenes/                    # settings, terminal, keyboard, history
└── tests/term_buffer_test.c   # host test
```

### Transport interface

The screens never talk to BLE directly. `transport.h` defines a small interface (a struct of function pointers):

| Function | Purpose |
|---|---|
| `open` / `close` / `free` | start and stop the transport |
| `send` | blocking send from the app thread |
| `set_callbacks` | RX callback (returns free buffer space) and status callback |
| `rx_drained` | the app has emptied its receive buffer (flow control) |
| `status` | idle / waiting / connected |
| `forget_paired` | delete the bonding keys |

Only BLE is implemented. The interface was designed so that a UART transport (3 wires to the Pi's serial console) could be added to work on the UI without BLE, without changing any screen.

### Threads

| Thread | Work |
|---|---|
| GUI | `ViewDispatcher`, scenes, input handling, sending |
| BLE (firmware) | serial event callback: copy into the stream buffer, signal TX confirmations |
| `bt` service (firmware) | status callback: store the status, post a custom event |
| Worker | read the stream buffer, feed the terminal view, call `rx_drained`, detect the start of a session |

The terminal view's model is locked (`ViewModelTypeLocking`), so the worker can safely update it while the GUI draws it.

### Connection status

The firmware doesn't tell apps whether a bond exists. The app infers the status:

| Status | Condition |
|---|---|
| Waiting to pair | advertising, no session has happened yet |
| Paired | advertising, a session has happened before |
| Connected | link up, no RX data on this connection yet |
| Session active | RX data received on this connection |
| Off | the profile could not be started |

RX data can only arrive over an authenticated link, so the first data marks the app as paired. The flag is stored in `settings.bin`, and **Forget pairing** clears it.

## Screens

1. **Settings** (`VariableItemList`): status, line ending (`\n` or `\r\n`), local echo, **Terminal >**, **Forget pairing** (`bt_forget_bonded_devices`). When a session starts, the app switches to the terminal automatically.
2. **Keyboard** (custom view): a one-line preview of the text being typed and a key grid. Four character rows and three layers (lowercase, uppercase, symbols) cover all printable ASCII. A function row has `aA`, `#@`, `spc`, `del`, `tab`, `^C`, `^D` and `ent`. Long OK types an uppercase letter.
3. **Terminal** (custom view): 21 × 7 characters in `FontKeyboard` (6 px wide), 192 lines of scrollback, Up/Down to scroll (hold to scroll fast), and a status bar while there is no session. OK opens the keyboard, long OK sends Ctrl+C, Right sends Enter, Left opens the history.
4. **History** (`Submenu`): the last 20 commands from `APP_DATA_PATH("history.txt")`. Selecting one puts it into the keyboard with the cursor on `ent`, so sending takes one click and editing is still possible.

The built-in `TextInput` was planned for the keyboard, but it lacks characters a shell needs (`|`, `>`, `$` and others), so a custom keyboard view replaced it.

## Testing

- `term_buffer.c` has no furi dependency and is tested on the host (`tests/term_buffer_test.c`): ANSI filtering, wrapping, backspace and carriage return, UTF-8 replacement and ring buffer overflow.
- Without the server, nRF Connect on a phone can act as the central: a write to RX appears on the terminal screen, and a command from the keyboard arrives as a TX indication.
- Logs: `ufbt cli`, then `log debug`.

## Resolved design questions

| Question | Outcome |
|---|---|
| Who frees the profile memory in `stop`? | the core, as with `hid_profile.c`; the profile only stops its services |
| Is the pairing PIN shown with a custom profile? | yes, the `bt` service draws it |
| API compatibility across firmware versions | built and tested on official 1.5.1 (API 88.2); other firmware needs its own SDK (`ufbt update`) |
| Is the `TextInput` character set enough? | no, a custom full-ASCII keyboard is used |
| TX chunk size | 20 bytes, because apps can't read the negotiated ATT MTU |
