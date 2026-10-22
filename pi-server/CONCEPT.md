# PiTerm: serwer na Raspberry Pi (Rust)

> Dokument koncepcyjny sprzed implementacji. Aktualny stan, budowanie i obsługa: [`README.md`](../README.md).

Folder `pi-server/` w repozytorium PiTerm. Pierwsza część: [`../flipper-app/CONCEPT.md`](../flipper-app/CONCEPT.md) (klient na Flipper Zero w C). Wspólny protokół: [`../docs/PROTOCOL.md`](../docs/PROTOCOL.md).

## Cel

Demon na Raspberry Pi, który łączy się z Flipperem po Bluetooth LE, uruchamia powłokę w pseudoterminalu i przekazuje bajty w obie strony. Jedna binarka, bez Pythona i zależności poza BlueZ, które są już w Raspberry Pi OS.

## Role BLE

Pi jest **centralą (klientem GATT)**, a Flipper **peryferium (serwerem GATT)**. Firmware Flippera nie udostępnia aplikacjom trybu central, więc to Pi skanuje i łączy się z Flipperem, gdy ten się reklamuje. Z punktu widzenia terminala Pi pozostaje serwerem, bo to na nim działa powłoka.

## Protokół

Pełna specyfikacja (UUID-y, flow control, cykl życia sesji): [`../docs/PROTOCOL.md`](../docs/PROTOCOL.md).

Co z niej wynika dla serwera:
- Serwer jest klientem GATT: subskrybuje **TX** (komendy z Flippera) i **Flow control**, pisze output do **RX**.
- Wysyła najwyżej tyle bajtów, ile wynosi okno, i przy oknie 0 czeka na notify. Okno trzeba dobić dokładnie do zera, bo dopiero wtedy Flipper je przywraca.
- Łączy się tylko z MAC z konfiguracji i nigdy nie pisze do charakterystyki RPC status.

UUID-y w kodzie trzymaj w jednym module (`src/protocol.rs`) z komentarzem odsyłającym do `docs/PROTOCOL.md`.

## Stos

| Zadanie | Crate |
|---|---|
| Async runtime | `tokio` (features: `full`) |
| BLE (BlueZ przez D-Bus) | `bluer` (features: `bluetoothd`) |
| Pseudoterminal | `pty-process` (feature `async`) albo `nix` (`forkpty`) |
| Konfiguracja | `serde` + `toml` |
| CLI | `clap` |
| Błędy / logi | `anyhow`, `tracing` + `tracing-subscriber` (logi trafiają do journald) |

Alternatywa dla `bluer`: `btleplug`, jeśli chcesz testować serwer również na macOS/Windows.

## Architektura

```
pi-server/
├── Cargo.toml   # [package] name = "piterm-server"
├── src/
│   ├── main.rs        # CLI, konfiguracja, pętla połącz → sesja → reconnect
│   ├── config.rs      # /etc/piterm/config.toml
│   ├── protocol.rs    # UUID-y i stałe z docs/PROTOCOL.md
│   ├── ble.rs         # połączenie, wyszukanie charakterystyk, parowanie
│   ├── flow.rs        # okno wysyłania (Arc<Mutex<u32>> + Notify)
│   ├── shell.rs       # uruchomienie powłoki w pty
│   └── session.rs     # dwa zadania: pty → RX, TX → pty
└── dist/
    ├── piterm.service # unit systemd
    └── config.toml    # przykładowa konfiguracja
```

Konfiguracja (`/etc/piterm/config.toml`):
```toml
flipper_addr = "AA:BB:CC:DD:EE:FF"
shell = "/bin/bash"
shell_args = ["-l"]
term = "dumb"
reconnect_delay_secs = 5
```

## Szkic logiki

```rust
const SVC:  Uuid = uuid!("8fe5b3d5-2e7f-4a98-2a48-7acc60fe0000");
const TX:   Uuid = uuid!("19ed82ae-ed21-4c9d-4145-228e61fe0000"); // Flipper -> Pi
const RX:   Uuid = uuid!("19ed82ae-ed21-4c9d-4145-228e62fe0000"); // Pi -> Flipper
const FLOW: Uuid = uuid!("19ed82ae-ed21-4c9d-4145-228e63fe0000");

async fn session(adapter: &Adapter, addr: Address, cfg: &Config) -> anyhow::Result<()> {
    let dev = adapter.device(addr)?;
    if !dev.is_connected().await? {
        dev.connect().await?;
    }
    let (tx, rx, flow) = find_chars(&dev).await?;   // iteracja po dev.services() i characteristics()

    let window = Arc::new(Mutex::new(u32::from_be_bytes(flow.read().await?[..4].try_into()?)));
    let window_changed = Arc::new(Notify::new());

    let mut shell = spawn_shell(cfg)?;               // pty + /bin/bash -l, TERM=dumb
    let mut tx_stream = tx.notify().await?;          // indications z TX
    let mut flow_stream = flow.notify().await?;

    loop {
        tokio::select! {
            Some(cmd) = tx_stream.next() => shell.write_all(&cmd).await?,
            Some(v) = flow_stream.next() => {
                *window.lock().await = u32::from_be_bytes(v[..4].try_into()?);
                window_changed.notify_one();
            }
            n = shell.read(&mut buf) => {
                let n = n?;
                if n == 0 { break; }                 // powłoka zakończona
                send_windowed(&rx, &buf[..n], &window, &window_changed, mtu).await?;
            }
        }
    }
    Ok(())
}
```

Uwagi do implementacji:
- `send_windowed` nie może blokować pętli `select!` podczas czekania na okno, bo notify z `fe63` przychodzi właśnie w tej pętli. Najprościej wydzielić wysyłanie do osobnego zadania z kanałem `mpsc` od strony pty.
- MTU: `bluer` udostępnia je przy `write_io()` / `notify_io()`. Przy zwykłym `write()` bezpiecznie zacząć od 20 bajtów i podnieść po sprawdzeniu.
- Zerwanie połączenia: wyjdź z sesji, zamknij powłokę (albo zostaw ją i podłącz ponownie, jak `tmux`), odczekaj `reconnect_delay_secs` i próbuj dalej.
- Przy wyjściu powłoki (`exit`, `Ctrl+D`) wyślij Flipperowi komunikat i uruchom nową.

## Parowanie

Dwie opcje:
1. **Ręcznie, raz**: `bluetoothctl` → `agent KeyboardOnly`, `default-agent`, `scan le on`, `pair <MAC>` (wpisz PIN z Flippera), `trust <MAC>`.
2. **Wbudowane w serwer**: podkomenda `piterm-server pair`, która rejestruje agenta `bluer` (`request_passkey` pyta o PIN na stdin), paruje i zapisuje MAC do konfiguracji. Wtedy na Pi nie trzeba w ogóle używać `bluetoothctl`.

## Budowanie

Z laptopa (cross-kompilacja):
```bash
cargo install cross
cross build --release --target aarch64-unknown-linux-gnu   # 64-bit Raspberry Pi OS
```
`bluer` linkuje `libdbus-1`, więc obraz `cross` potrzebuje `libdbus-1-dev:arm64` (własny `Cross.toml` z `pre-build`). Alternatywnie zbuduj bezpośrednio na Pi: `cargo build --release` (wolniej, ale bez konfiguracji).

Dla 32-bitowego systemu target to `armv7-unknown-linux-gnueabihf`, a dla Pi Zero (v1) `arm-unknown-linux-gnueabihf`.

## Instalacja na Pi

```bash
sudo install -m 755 piterm-server /usr/local/bin/
sudo mkdir -p /etc/piterm && sudo cp config.toml /etc/piterm/
sudo cp piterm.service /etc/systemd/system/
sudo systemctl daemon-reload && sudo systemctl enable --now piterm
```

`piterm.service`:
```ini
[Unit]
Description=PiTerm BLE terminal server
After=bluetooth.service
Requires=bluetooth.service

[Service]
ExecStart=/usr/local/bin/piterm-server --config /etc/piterm/config.toml
User=pi
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Użytkownik usługi musi mieć dostęp do BlueZ przez D-Bus. Na Raspberry Pi OS zwykle wystarcza grupa `bluetooth` (sprawdzić polityki w `/etc/dbus-1/system.d/bluetooth.conf`).

### Wgranie z Flippera

Binarka ma kilka MB, więc **nie da się jej sensownie wpisać przez Bad USB**. Wersja praktyczna:
1. Aplikacja **Mass Storage** na Flipperze wystawia obraz z karty SD jako pendrive z `piterm-server`, `config.toml`, `piterm.service` i `install.sh`.
2. Krótki skrypt **Bad USB** w otwartym terminalu montuje pendrive i uruchamia `install.sh`.

Pamiętaj o układzie klawiatury (Pi często ma `gb`, a Bad USB domyślnie `us`).

## Bezpieczeństwo

- Kto sparuje się z Flipperem, ten ma powłokę na Pi. Minimum to PIN przy parowaniu i łączenie wyłącznie z MAC z konfiguracji.
- Serwer nie działa jako root. Lepiej dedykowany użytkownik albo uruchamianie `login`, który pyta o hasło.
- Brak zapisu do charakterystyki RPC status.

## Kolejność prac

1. `piterm-server scan`: lista urządzeń `PiTerm`, wypisanie usług i charakterystyk.
2. Połączenie + subskrypcja TX: wypisywanie na stdout tego, co wpisujesz na Flipperze.
3. Wysyłanie do RX z flow control (test: `echo` dłuższego tekstu).
4. Podłączenie pty z powłoką.
5. Reconnect, podkomenda `pair`, unit systemd, konfiguracja.

## Do zweryfikowania

- API `bluer` w aktualnej wersji (nazwy metod `notify`, `write_io`, sposób odczytu MTU).
- Czy BlueZ na Pi prawidłowo obsługuje indications z TX przez `notify()` (powinien, BlueZ traktuje je jednakowo).
- Uprawnienia D-Bus dla nie-roota na Twojej wersji Raspberry Pi OS.
