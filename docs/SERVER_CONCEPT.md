# PiTerm server design (Rust)

Design notes for `pi-server/`. For building and usage see the [README](../README.md); for the wire protocol see [PROTOCOL.md](PROTOCOL.md). The Flipper side is described in [CLIENT_CONCEPT.md](CLIENT_CONCEPT.md).

## Goal

A daemon that connects to the Flipper over Bluetooth LE, runs a shell in a pseudo-terminal and relays bytes in both directions. It ships as a single binary with no runtime dependencies beyond BlueZ, which Raspberry Pi OS and most Linux distributions already include.

Nothing in the server is specific to the Raspberry Pi. It runs on any Linux machine with BlueZ and a BLE adapter.

## BLE roles

The Linux machine is the **BLE central (GATT client)** and the Flipper is the **peripheral (GATT server)**. The Flipper firmware does not give apps the central role, so the Linux side scans for the advertising Flipper and connects to it. In terminal terms the Linux machine is still the server, because the shell runs there.

What the protocol requires from the server:
- Subscribe to **TX** (user input) and **FLOW** (flow control window); write shell output to **RX**.
- Never send more than the current window, and keep sending until the window reaches exactly zero, because only then does the Flipper restore it.
- Connect only to the configured or explicitly paired Flipper, and never write the RPC status characteristic.

## Stack

| Concern | Crate |
|---|---|
| Async runtime | `tokio` |
| BLE (BlueZ over D-Bus) | `bluer` (feature `bluetoothd`) |
| Pseudo-terminal | `pty-process` (feature `async`) |
| Configuration | `serde` + `toml` |
| CLI | `clap` |
| Errors and logging | `anyhow`, `tracing` + `tracing-subscriber` (stdout, collected by journald) |

`btleplug` was considered as a cross-platform alternative to `bluer`. It was rejected: the shell and pty part is Linux-specific anyway, and `bluer` exposes BlueZ features PiTerm relies on (`AcquireWrite` with the MTU, a pairing agent).

## Modules

```
pi-server/
├── Cargo.toml          # package "piterm-server"
├── Cross.toml          # cross-compilation images with libdbus-1-dev
├── src/
│   ├── main.rs         # CLI, configuration, daemon loop (resolve → session → wait)
│   ├── config.rs       # optional /etc/piterm/config.toml
│   ├── protocol.rs     # UUIDs and constants from docs/PROTOCOL.md
│   ├── ble.rs          # adapter, discovery, characteristics, RX writer, scan/inspect/pair
│   ├── flow.rs         # send window (tokio watch channel)
│   ├── shell.rs        # shell in a pty
│   └── session.rs      # one connection: relay loop and shell restarts
└── dist/
    ├── config.toml     # example configuration
    ├── piterm.service  # systemd unit
    └── install.sh      # installer
```

## Daemon loop

`main.rs` runs a loop that never exits on its own:

1. **Resolve the Flipper address:** `flipper_addr` from the configuration, or else the only paired device whose name starts with `PiTerm`. If there is none (or several), log why and retry later. The service can therefore start before the Flipper is paired.
2. **Run a session** (`session::run`) until the Flipper disconnects or an error occurs.
3. **Wait** `reconnect_delay_secs` (default 5 s) and start again.

A malformed `flipper_addr` is the only configuration error that stops the daemon immediately.

## Session

`session::run` handles one BLE connection:

1. Find the device (start a scan if BlueZ doesn't know it yet) and refuse it if it isn't paired.
2. Connect and look up the PiTerm service and its TX, RX and FLOW characteristics.
3. Read FLOW for the initial window and subscribe to FLOW and TX.
4. Open RX for writing. `AcquireWrite` is preferred (write without response, with a known MTU); plain writes of 20 bytes are the fallback.
5. Start a shell and relay data until it exits or the Flipper disconnects. When the shell exits, send `[piterm] new shell` and start a new one on the same connection.

Inside the relay loop, three event sources are polled with `tokio::select!`:

| Source | Action |
|---|---|
| shell output (`pump_output` future) | read from the pty and send it to RX through the window; finishing means the shell exited |
| TX notifications | write the bytes to the pty |
| device events | `Connected(false)` ends the session |

On Linux, reading the pty after the shell exits fails with `EIO` rather than returning EOF; `shell::is_eof` treats both the same way.

## Flow control

`flow::Window` wraps a `tokio::sync::watch` channel holding the current window:

- `acquire(max)` waits for a non-zero window, then atomically takes `min(window, max)` bytes from it.
- `set(value)` stores a value from a FLOW notification and wakes waiting senders.

FLOW notifications are handled by **a separate task**, not by the relay loop. The first design processed them inside the same `select!` loop that also sent output. A send waiting for the window would then block the loop that delivers the notification it is waiting for: a deadlock as soon as the window reached zero. With a separate task, a blocked sender can always be woken up. The task is aborted when the session ends.

Each RX write is `min(MTU, 243, window, remaining data)`, so the window is always driven to exactly zero, as the Flipper requires.

## Shell

`shell::spawn` opens a pty, sets its size to `term_cols × term_rows` (default 21 × 7, the Flipper screen) and starts the configured shell (default `/bin/bash -l`) as a session leader with `TERM=dumb`, in the user's home directory. The child is killed when the session drops it.

The shell runs as the service user. Use a regular, preferably dedicated, account, never root.

## Pairing

The Flipper uses passkey-entry pairing: it shows a 6-digit PIN and the Linux side types it in. There are two ways to pair:

1. **System tools** (preferred): `bluetoothctl` or a desktop Bluetooth dialog. The server then finds the paired `PiTerm …` device without any configuration.
2. **`piterm-server pair [MAC] [--save]`:** registers a `bluer` agent whose `request_passkey` reads the PIN from stdin, pairs, marks the device as trusted and optionally stores the MAC as `flipper_addr`.

The daemon itself never pairs. Unpaired devices are rejected, so pairing is always an explicit action of the administrator.

## Deployment

- **Build** natively with `cargo build --release`, or cross-compile with `cross`. `bluer` links against `libdbus-1`, so `Cross.toml` installs `libdbus-1-dev` for the target architecture (`aarch64`, `armv7` and `arm` are configured).
- **Install** with `dist/install.sh`. It installs the binary to `/usr/local/bin`, creates `/etc/piterm/config.toml` if missing, installs the systemd unit for the given user and adds that user to the `bluetooth` group, which grants access to BlueZ over D-Bus on Raspberry Pi OS and Debian.
- The unit starts after `bluetooth.service` and restarts on failure.

Copying the binary onto the Pi from the Flipper itself (Mass Storage app with the files on the SD card, plus a Bad USB script that mounts the drive and runs `install.sh`) was considered. It is not part of the project: the binary is several megabytes, and the keyboard layout used by Bad USB often doesn't match the Pi's.

## Security

- Anyone who pairs with the Flipper gets a shell on the server. The protections are the PIN at pairing time, the encrypted link, and connecting only to `flipper_addr` or an explicitly paired device.
- The server doesn't run as root.
- The server never writes the RPC status characteristic, which would reset the Flipper's BLE stack.

## Resolved design questions

| Question | Outcome |
|---|---|
| `bluer` API for notifications, `write_io` and the MTU | `notify()` for TX and FLOW; `write_io()` returns a writer with `mtu()` (already reduced by BlueZ overhead) |
| Do indications from TX work through `notify()`? | yes, BlueZ handles indications and notifications the same way and confirms indications itself |
| D-Bus permissions for a non-root user | membership in the `bluetooth` group is enough on Debian and Raspberry Pi OS |
| Keep the shell across disconnects (like `tmux`)? | no: the shell is closed on disconnect and a new one starts on the next connection |
| Require `flipper_addr`? | no: without it the server uses the only paired `PiTerm …` device, so pairing with system tools is enough |
