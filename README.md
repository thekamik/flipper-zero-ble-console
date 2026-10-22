# PiTerm

An interactive Linux shell on the Flipper Zero screen, over Bluetooth LE. No Wi-Fi, no cable, no TCP/IP.

PiTerm has two parts:

- **Flipper app** (`flipper-app/`, C): a `.fap` with a scrollable terminal, a full-ASCII on-screen keyboard and command history.
- **Server** (`pi-server/`, Rust): a small daemon for a Raspberry Pi or any other Linux machine with BlueZ. It connects to the Flipper, runs a login shell in a pseudo-terminal and relays bytes in both directions.

Tested with the official Flipper firmware 1.5.1 (API 88.2) and a Debian server with BlueZ.

## Features

- Shell output on the Flipper with ANSI escape filtering, line wrapping and a 192-line scrollback.
- On-screen keyboard covering all printable ASCII (including `| > $ ~ \`), plus `tab`, `^C` and `^D` keys.
- Command history (last 20 commands, stored on the SD card).
- Encrypted, PIN-authenticated BLE connection. Pairing uses the standard system Bluetooth tools.
- PiTerm runs its own BLE profile with a separate MAC address and bonding keys, so pairing with the Flipper mobile app is not affected.
- Connection status on the Flipper: waiting to pair, paired, connected, session active.
- The server works without configuration (it picks the paired PiTerm device), reconnects automatically and starts a new shell after `exit`.
- systemd service and install script for the server.

## How it works

```
 Flipper Zero (flipper-app)                        Linux / Raspberry Pi (pi-server)
┌─────────────────────────┐                       ┌──────────────────────────────┐
│ keyboard ──► TX  (fe61) │ ──── indication ────► │ ──► pty ──► /bin/bash -l     │
│ screen ◄──── RX  (fe62) │ ◄─── write ────────── │ ◄── pty ◄───┘                │
│ buffer ────► FLOW(fe63) │ ──── notify ────────► │ send window (flow control)   │
└─────────────────────────┘                       └──────────────────────────────┘
```

The Flipper firmware does not let apps act as a BLE central, so the roles are reversed compared to a typical terminal: the **Flipper is the BLE peripheral** (GATT server) and the **Linux machine is the central** (GATT client). From the user's point of view the Linux machine is still the server, because that is where the shell runs.

1. The Flipper app starts a custom BLE profile with the firmware's serial service and advertises as `PiTerm <Flipper name>`.
2. The server connects to the paired Flipper, reads the flow control window and subscribes to TX and FLOW.
3. Keys typed on the Flipper are sent as indications on TX and written to the shell's pty.
4. Shell output is written to RX in chunks no larger than the MTU and the Flipper's free buffer space. The Flipper advertises its free space on FLOW.

The data is a raw byte stream without framing. The full specification, including flow control and the session lifecycle, is in [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

## Quick start

1. Build the Flipper app and install it on the Flipper ([Flipper app](#flipper-app)).
2. Build the server and install it on the Linux machine ([Server](#server)).
3. Start PiTerm on the Flipper and pair it with the Linux machine ([Pairing](#pairing)).
4. Start the server ([Running](#running)). The Flipper switches to the terminal and shows the shell prompt.

## Flipper app

### Requirements

- Python 3 with `venv`. On Debian/Ubuntu: `sudo apt install python3-venv`. If `python3 -m venv` still reports that `ensurepip` is unavailable, install the package for your Python version, e.g. `sudo apt install python3.13-venv` (check the version with `python3 --version`).
- Access to the Flipper's USB serial port (`/dev/ttyACM0`, group `dialout`):
  ```bash
  sudo usermod -aG dialout $USER     # then log out and back in
  ```

You don't need to install an ARM compiler or the Flipper SDK yourself. [`ufbt`](https://github.com/flipperdevices/flipperzero-ufbt) downloads both to `~/.ufbt` the first time it runs.

### Installing ufbt

Install `ufbt` into a virtual environment in the repository root (`.venv/` is ignored by git):

```bash
cd flipper-zero-ble-console

python3 -m venv .venv
source .venv/bin/activate            # bash/zsh
source .venv/bin/activate.fish       # fish

pip install --upgrade pip
pip install ufbt
ufbt --version
```

Leave the environment with `deactivate`. You can also call `ufbt` without activating it, as `../.venv/bin/ufbt` from `flipper-app/`.

### Building

```bash
cd flipper-app
ufbt update      # download the SDK matching the Flipper firmware
ufbt             # build -> flipper-app/dist/piterm.fap
```

`application.fam` is the app manifest (name, entry point, sources, icon). The resulting binary is `dist/piterm.fap`. It is linked against the firmware at load time, so you have to build it with the SDK for the firmware version on your Flipper (shown in `Settings > About`):

```bash
ufbt update --channel=release                 # official firmware, latest release (default)
ufbt update --channel=dev                     # official firmware, development branch
ufbt update --index-url=<SDK index URL>       # custom firmware: see its documentation
```

### Installing on the Flipper

Connect the Flipper over USB and close qFlipper (it locks the serial port), then run:

```bash
ufbt launch      # build, copy to the SD card and start the app
```

You can also copy `dist/piterm.fap` to `apps/Bluetooth/` on the SD card with qFlipper or a card reader. On the Flipper, the app is in **Apps → Bluetooth → PiTerm**.

When PiTerm starts, it switches the Flipper's Bluetooth to its own profile. On exit it restores the default profile, so the Flipper mobile app works again.

App logs: run `ufbt cli`, then `log debug`.

## Server

The server has nothing Raspberry Pi specific. It runs on any Linux machine with BlueZ and a Bluetooth LE adapter.

### Requirements

- Rust ([rustup.rs](https://rustup.rs)).
- D-Bus headers (`bluer` links against `libdbus-1`): `sudo apt install libdbus-1-dev pkg-config`.
- On the target machine: BlueZ (`bluetoothd`). Raspberry Pi OS includes it by default.

### Building

Natively, e.g. directly on the Pi:

```bash
cd pi-server
cargo build --release            # -> target/release/piterm-server
```

To cross-compile from a PC, use [`cross`](https://github.com/cross-rs/cross). It needs Docker or Podman; `Cross.toml` installs `libdbus-1-dev` for the target.

```bash
cargo install cross
cross build --release --target aarch64-unknown-linux-gnu      # Raspberry Pi OS 64-bit
cross build --release --target armv7-unknown-linux-gnueabihf  # Raspberry Pi OS 32-bit
cross build --release --target arm-unknown-linux-gnueabihf    # Raspberry Pi Zero (v1)
# -> target/<target>/release/piterm-server
```

### Installing as a service

Copy the binary and the files from `pi-server/dist/` into one directory on the target machine and run the installer:

```bash
scp target/aarch64-unknown-linux-gnu/release/piterm-server dist/* pi@raspberrypi.local:~/piterm/
ssh pi@raspberrypi.local
cd ~/piterm
sudo ./install.sh pi     # the user whose shell is exposed on the Flipper (not root)
```

`install.sh` does the following:
- installs the binary to `/usr/local/bin/piterm-server`,
- creates `/etc/piterm/config.toml` if it doesn't exist,
- installs `/etc/systemd/system/piterm.service` running as the given user,
- adds that user to the `bluetooth` group (access to BlueZ over D-Bus).

### Configuration

The configuration file is optional. Without it, the server uses the defaults below. The default path is `/etc/piterm/config.toml`; an example is in [`pi-server/dist/config.toml`](pi-server/dist/config.toml).

| Key | Default | Description |
|---|---|---|
| `flipper_addr` | unset | MAC of the Flipper in the PiTerm profile. The server connects only to this address. If unset, the server uses the only paired device named `PiTerm …` |
| `adapter` | default adapter | BlueZ adapter, e.g. `"hci1"` |
| `shell`, `shell_args` | `/bin/bash`, `["-l"]` | shell to run |
| `term` | `dumb` | value of `TERM` |
| `term_cols`, `term_rows` | `21`, `7` | terminal size reported to the shell (matches the Flipper screen) |
| `reconnect_delay_secs` | `5` | delay between connection attempts |

## Pairing

The connection is encrypted and requires a one-time pairing with a PIN shown on the Flipper. Pair with the usual system Bluetooth tools; the server finds the paired Flipper on its own.

1. Start **PiTerm** on the Flipper. The status shows `Waiting to pair`.
2. Pair from the Linux machine with `bluetoothctl`:
   ```
   bluetoothctl
   scan on                       # wait until "PiTerm <name>" shows up
   scan off
   pair 80:E1:27:F2:36:CE        # MAC of the PiTerm device
                                 # enter the 6-digit PIN shown on the Flipper at "Enter passkey"
   trust 80:E1:27:F2:36:CE
   disconnect 80:E1:27:F2:36:CE
   exit
   ```
   If `pair` doesn't ask for a PIN, register an agent first: `agent KeyboardOnly`, then `default-agent`.

You can also pair from the GNOME or KDE Bluetooth settings: choose `PiTerm <name>` and enter the PIN from the Flipper.

**Pair the PiTerm device, not the regular Flipper.** PiTerm uses its own MAC address. For example, `Flipper Re0pl0p` is `…:F0:36:CE`, while `PiTerm Re0pl0p` is `…:F2:36:CE`.

Alternatively, the server can pair and store the address in its configuration:

```bash
piterm-server scan                                   # find the PiTerm MAC
sudo piterm-server pair 80:E1:27:F2:36:CE --save --config /etc/piterm/config.toml
```

To remove a pairing (e.g. when switching Flippers or after encryption errors), remove it on both sides:
- Flipper: in PiTerm settings, select **Forget pairing**.
- Linux: `bluetoothctl remove 80:E1:27:F2:36:CE`.

The Flipper stores the PiTerm bonding keys separately (`apps_data/piterm/.bt_term.keys`), so this does not affect pairing with your phone.

## Running

As a systemd service:

```bash
sudo systemctl enable --now piterm
journalctl -u piterm -f
```

Manually, e.g. on a laptop:

```bash
cd pi-server
cargo run --release                                   # same as `piterm-server run`
RUST_LOG=debug cargo run --release                    # verbose logs (flow control window, MTU)
cargo run --release -- --config dist/config.toml run  # with a configuration file
```

A session works as follows:
- Start PiTerm on the Flipper. The server connects within a few seconds, and the Flipper switches to the terminal screen with the shell prompt.
- `exit` or `^D` ends the shell. The server prints `[piterm] new shell` and starts a new one.
- Leaving the app on the Flipper ends the session. The server waits for the next connection.

Example server log:

```
INFO piterm_server: PiTerm: adapter hci0
INFO piterm_server::ble: using paired device 80:E1:27:F2:36:CE (PiTerm Re0pl0p)
INFO piterm_server::session: connected to 80:E1:27:F2:36:CE
INFO piterm_server::session: started shell /bin/bash
INFO piterm_server::session: Flipper disconnected
```

### Server commands

| Command | Description |
|---|---|
| `run` (default) | daemon: connect to the Flipper, run a shell, reconnect after a disconnect |
| `scan [--secs N]` | list nearby Flippers running PiTerm |
| `inspect [MAC]` | connect and print the Flipper's services and characteristics |
| `pair [MAC] [--save]` | pair (with the PIN from the Flipper) and mark the device as trusted |

All commands accept `--config <file>`. Without a MAC, `run` and `inspect` use `flipper_addr` or, if it's unset, the only paired `PiTerm …` device. The daemon keeps waiting until such a device exists.

## Using the Flipper app

### Connection status

The status is shown in the **Status** row of the settings screen and in a bar at the top of the terminal screen. The bar disappears once a session is active.

| Status | Meaning |
|---|---|
| Waiting to pair | advertising, no server session has happened yet |
| Paired | advertising, a session has happened before; waiting for the server |
| Connected | BLE link is up, nothing has been received from the server yet (pairing happens at this point) |
| Session active | the server is sending data; the app switches to the terminal |
| Off | Bluetooth could not be started |

The firmware doesn't tell apps whether a device is bonded, so the app marks itself as `Paired` after its first successful session. **Forget pairing** clears this flag. If you remove the pairing on the Linux side only, also use **Forget pairing** on the Flipper.

### Controls

| Screen | Button | Action |
|---|---|---|
| Settings | OK on **Terminal >** | open the terminal (happens automatically when a session starts) |
| | **Line ending** | send `\n` or `\r\n` after a command |
| | **Local echo** (`Off`/`On`) | also print sent bytes locally (usually not needed, the shell echoes) |
| | OK on **Forget pairing** | delete PiTerm's bonding keys |
| Terminal | Up / Down (hold: fast) | scroll |
| | OK | open the keyboard |
| | long OK | send Ctrl+C |
| | Right | send Enter (empty line) |
| | Left | command history |
| | Back | settings |
| Keyboard | arrows + OK | type; long OK on a letter types it uppercase |
| | `aA` / `#@` | uppercase (one-shot) / symbols |
| | `del` (long OK: clear the line) | backspace |
| | `ent` / `tab` / `^C` / `^D` | send the line / the line + Tab / Ctrl+C / Ctrl+D |
| | Back | return without sending (the line is kept) |
| History | OK | put the command into the keyboard with the cursor on `ent` |

## Security

Anyone who pairs with PiTerm gets a shell on the server.

- Pairing requires the PIN shown on the Flipper, and the link is encrypted.
- The server connects only to `flipper_addr` or to a device you have paired yourself.
- Run the service as a regular, preferably dedicated, user, never as root.
- The server never writes to the firmware's RPC status characteristic (writing it resets the Flipper's BLE stack).

## Known limitations

- Supports one Flipper per server.
- The terminal is a dumb terminal (`TERM=dumb`, 21×7 characters). Full-screen programs such as `vim` or `htop` are not usable.
- Non-ASCII characters are shown as `?`.
- The Flipper sends typed input in 20-byte packets because apps can't read the negotiated MTU. This only matters for very long commands.
- When the connection drops, the shell is closed and a new one is started on the next connection.

## Troubleshooting

| Symptom | Cause and fix |
|---|---|
| `source .venv/bin/activate` fails in fish | use `source .venv/bin/activate.fish` |
| `python3 -m venv`: "ensurepip is not available" | `sudo apt install python3-venv` (or `python3.X-venv`), then `rm -rf .venv` and start again |
| `ufbt launch`: permission denied on `/dev/ttyACM0` | add yourself to the `dialout` group, close qFlipper |
| the app shows "API mismatch" or doesn't start | the `.fap` was built with a different SDK: `ufbt update` for your firmware and rebuild |
| server: "no paired PiTerm device" or "is not paired" | see [Pairing](#pairing); pair `PiTerm …`, not the regular `Flipper …` device |
| server: "… not found within 10s" | PiTerm isn't running on the Flipper, or the Flipper is out of range; the server retries |
| server: D-Bus "not authorized" | the service user must be in the `bluetooth` group (see `/etc/dbus-1/system.d/bluetooth.conf`) |
| pairing fails or encryption errors after re-pairing | remove the pairing on both sides and pair again |

## Development

### Repository layout

```
├── docs/
│   ├── PROTOCOL.md           # BLE protocol specification
│   ├── CLIENT_CONCEPT.md     # Flipper app design notes
│   └── SERVER_CONCEPT.md     # server design notes
├── flipper-app/              # Flipper app (C, ufbt)
│   ├── application.fam       # app manifest
│   ├── piterm.c/.h           # app, worker thread, settings
│   ├── term_profile.c/.h     # custom BLE profile
│   ├── term_buffer.c/.h      # output ring buffer and ANSI filter
│   ├── history.c/.h          # command history on the SD card
│   ├── transport/            # transport interface and BLE implementation
│   ├── views/                # terminal and keyboard views
│   ├── scenes/               # settings, terminal, keyboard, history
│   └── tests/                # host tests
├── pi-server/                # server (Rust, bluer + tokio)
│   ├── src/                  # ble, session, flow, shell, config, protocol
│   └── dist/                 # config.toml, piterm.service, install.sh
└── .github/workflows/ci.yml
```

Design notes and the reasoning behind them: [`docs/CLIENT_CONCEPT.md`](docs/CLIENT_CONCEPT.md) and [`docs/SERVER_CONCEPT.md`](docs/SERVER_CONCEPT.md).

### Tests

```bash
# Flipper terminal buffer (ANSI filter, wrapping) on the host
cc -Iflipper-app flipper-app/tests/term_buffer_test.c flipper-app/term_buffer.c -o /tmp/term_buffer_test && /tmp/term_buffer_test

# server
cd pi-server && cargo test && cargo clippy --all-targets -- -D warnings
```

To test the Flipper app without the server, use nRF Connect on a phone. A write to RX (`…62fe0000`) appears on the terminal screen, and a command from the keyboard arrives as an indication on TX (`…61fe0000`).

### CI

GitHub Actions runs path-filtered jobs:
- `flipper-app/**`: the host test and a `.fap` build with `flipperdevices/flipperzero-ufbt-action`.
- `pi-server/**`: `cargo fmt --check`, `cargo clippy`, `cargo test` and an `aarch64` build with `cross`.
- Changes to `docs/PROTOCOL.md` run both jobs.

### Conventions

- Protocol changes start in `docs/PROTOCOL.md` and update both projects in the same change. The protocol constants live in `flipper-app/protocol.h` and `pi-server/src/protocol.rs`.
- Commit messages are prefixed with the project: `flipper:`, `server:`, `protocol:`, `ci:`.

## License

GPL-2.0, see [LICENSE](LICENSE).
