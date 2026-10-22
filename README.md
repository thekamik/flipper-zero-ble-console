# PiTerm

Terminal powłoki Raspberry Pi (albo dowolnego Linuksa z BlueZ) na ekranie Flipper Zero, przez Bluetooth LE. Bez Wi-Fi, bez kabla i bez TCP/IP.

Status: działający PoC, przetestowany z Flipper Zero (oficjalny firmware 1.5.1) i serwerem na Linuksie (Debian, BlueZ). Interfejs aplikacji i serwera jest po angielsku.

## Struktura repozytorium

```
├── README.md              # ten plik
├── docs/
│   └── PROTOCOL.md        # wspólna specyfikacja BLE, jedyne źródło prawdy
├── flipper-app/           # klient: aplikacja .fap na Flipper Zero (C, ufbt)
│   ├── CONCEPT.md
│   ├── application.fam    # manifest aplikacji
│   ├── transport/         # warstwa transportu (BLE)
│   ├── views/             # terminal i klawiatura
│   ├── scenes/            # ekrany
│   └── tests/             # testy logiki na hoście
├── pi-server/             # serwer: demon na Linuksie (Rust, bluer + tokio)
│   ├── CONCEPT.md
│   ├── src/
│   └── dist/              # config.toml, piterm.service, install.sh
└── .venv/                 # lokalne środowisko Pythona z ufbt (poza gitem)
```

| Projekt | Język | Rola BLE | Narzędzia |
|---|---|---|---|
| `flipper-app/` | C | peryferium, serwer GATT | `ufbt` |
| `pi-server/` | Rust | centrala, klient GATT | `cargo`, `cross` |

Oba projekty budują się niezależnie i komunikują się wyłącznie przez protokół opisany w [`docs/PROTOCOL.md`](docs/PROTOCOL.md).

## Jak to działa

```
 Flipper Zero (flipper-app)                         Raspberry Pi (pi-server)
┌─────────────────────────┐                        ┌──────────────────────────┐
│ klawiatura ─► TX (fe61) │ ──── indication ─────► │ ─► pty ─► /bin/bash -l    │
│ ekran ◄────── RX (fe62) │ ◄──── write ────────── │ ◄─ pty ◄─┘                │
│ bufor ─► FLOW (fe63)    │ ──── notify ─────────► │ okno wysyłania           │
└─────────────────────────┘                        └──────────────────────────┘
```

Flipper reklamuje się jako `PiTerm <nazwa>`, a Pi łączy się z nim (tylko z MAC z konfiguracji), paruje z PIN-em i uruchamia powłokę.

## Instrukcja w skrócie

1. Zbuduj i wgraj aplikację na Flippera ([Aplikacja na Flippera](#aplikacja-na-flippera)).
2. Zbuduj serwer i zainstaluj go na Raspberry Pi albo innym Linuksie ([Serwer](#serwer-raspberry-pi-lub-inny-linux)).
3. Uruchom PiTerm na Flipperze i sparuj z nim serwer ([Parowanie](#parowanie)).
4. Włącz usługę `piterm` ([Uruchamianie](#uruchamianie)).

## Aplikacja na Flippera

### Wymagania

- Python 3 z modułem `venv`. Na Debianie/Ubuntu: `sudo apt install python3-venv`. Jeśli mimo to `python3 -m venv` zgłasza brak `ensurepip`, zainstaluj pakiet dla swojej wersji Pythona, np. `sudo apt install python3.13-venv` (wersję pokaże `python3 --version`).
- Dostęp do portu USB Flippera (`/dev/ttyACM0`, grupa `dialout`):
  ```bash
  sudo usermod -aG dialout $USER     # potem wyloguj się i zaloguj ponownie
  ```
  Jednorazowo, bez wylogowania (do odłączenia Flippera): `sudo setfacl -m u:$USER:rw /dev/ttyACM0`.

Kompilatora ARM ani innych bibliotek nie instalujesz ręcznie. `ufbt` sam pobiera SDK Flippera i toolchain `arm-none-eabi-gcc` do `~/.ufbt` przy pierwszym `ufbt update`.

### Środowisko Pythona z ufbt

`ufbt` instalujemy w wirtualnym środowisku w katalogu repo (`.venv/`, ignorowane przez git).

```bash
cd flipper-zero-ble-console          # katalog główny repo

rm -rf .venv                         # tylko jeśli odtwarzasz środowisko
python3 -m venv .venv

source .venv/bin/activate            # bash/zsh
source .venv/bin/activate.fish       # fish (zwykłe `activate` w fish nie działa)

pip install --upgrade pip
pip install ufbt

which ufbt                           # powinno wskazać .../.venv/bin/ufbt
ufbt --version
```

Wyjście ze środowiska: `deactivate`. Bez aktywacji można też wołać `ufbt` wprost: `../.venv/bin/ufbt` z katalogu `flipper-app`.

### Budowanie

```bash
cd flipper-app
ufbt update      # pobiera SDK zgodne z firmware'em Flippera
ufbt             # build -> flipper-app/dist/piterm.fap
```

`application.fam` to manifest aplikacji (nazwa, plik startowy, źródła, ikona), a nie binarka. Binarka to `dist/piterm.fap`. Jest dynamicznie linkowana z firmware'em, więc musi być zbudowana na SDK tej samej wersji, którą masz na Flipperze.

Testowane na oficjalnym firmware 1.5.1 (API 88.2). Inny firmware:
```bash
ufbt update --channel=release                 # oficjalny, najnowsze wydanie (domyślne)
ufbt update --channel=dev                     # oficjalny, gałąź deweloperska
ufbt update --index-url=<URL indeksu SDK>     # custom firmware (Momentum, Unleashed): URL z jego dokumentacji
```
Wersję firmware'u Flippera pokazuje `Settings > About` (albo qFlipper).

### Wgrywanie i uruchamianie

Flipper podłączony po USB, qFlipper zamknięty (blokuje port):

```bash
ufbt launch      # build, wgranie na kartę SD i uruchomienie
```

Ręcznie: skopiuj `dist/piterm.fap` na kartę SD do `apps/Bluetooth/` (qFlipper albo czytnik kart). Na Flipperze aplikacja jest w `Apps > Bluetooth > PiTerm`.

Po starcie aplikacja przełącza Bluetooth Flippera na własny profil i reklamuje się jako `PiTerm <nazwa Flippera>`. Po wyjściu z aplikacji przywraca profil domyślny, więc aplikacja mobilna Flippera znowu działa.

Logi aplikacji: `ufbt cli`, potem `log debug`.

## Serwer (Raspberry Pi lub inny Linux)

Serwer nie ma nic specyficznego dla Raspberry Pi: działa na każdym Linuksie z BlueZ i adapterem Bluetooth LE (laptop, mini PC, inne płytki ARM). Inny adapter niż domyślny ustawiasz w konfiguracji: `adapter = "hci1"`.

### Wymagania

- Rust (`rustup`, https://rustup.rs).
- Nagłówki D-Bus (`bluer` linkuje `libdbus-1`): `sudo apt install libdbus-1-dev pkg-config`.
- Na maszynie docelowej: BlueZ (`bluetoothd`), w Raspberry Pi OS jest domyślnie.

### Budowanie

Na tej samej architekturze, np. bezpośrednio na Pi (wolniej, ale bez konfiguracji):
```bash
cd pi-server
cargo build --release            # -> target/release/piterm-server
```

Cross-kompilacja z laptopa (wymaga Dockera albo Podmana; `Cross.toml` doinstalowuje `libdbus-1-dev` dla ARM):
```bash
cargo install cross
cross build --release --target aarch64-unknown-linux-gnu      # Raspberry Pi OS 64-bit
cross build --release --target armv7-unknown-linux-gnueabihf  # Raspberry Pi OS 32-bit
cross build --release --target arm-unknown-linux-gnueabihf    # Pi Zero (v1)
# -> target/<target>/release/piterm-server
```

### Instalacja

Skopiuj binarkę i pliki z `pi-server/dist/` do jednego katalogu na maszynie docelowej i uruchom instalator:

```bash
scp target/aarch64-unknown-linux-gnu/release/piterm-server dist/* pi@raspberrypi.local:~/piterm/
ssh pi@raspberrypi.local
cd ~/piterm
sudo ./install.sh pi     # użytkownik, którego powłoka będzie dostępna z Flippera (nie root)
```

`install.sh`:
- kopiuje binarkę do `/usr/local/bin/piterm-server`,
- tworzy `/etc/piterm/config.toml` (jeśli go nie ma; `flipper_addr` jest w nim zakomentowane),
- instaluje unit `/etc/systemd/system/piterm.service` z podanym użytkownikiem,
- dodaje użytkownika do grupy `bluetooth` (dostęp do BlueZ przez D-Bus).

Konfiguracja (`/etc/piterm/config.toml`, przykład w `pi-server/dist/config.toml`):

| Klucz | Domyślnie | Znaczenie |
|---|---|---|
| `flipper_addr` | brak | MAC Flippera w profilu PiTerm; serwer łączy się tylko z nim. Brak = jedyne sparowane urządzenie `PiTerm …` |
| `adapter` | domyślny | adapter BlueZ, np. `"hci1"` |
| `shell`, `shell_args` | `/bin/bash`, `["-l"]` | uruchamiana powłoka |
| `term` | `dumb` | wartość `TERM` |
| `term_cols`, `term_rows` | `21`, `7` | rozmiar terminala zgłaszany powłoce (ekran Flippera) |
| `reconnect_delay_secs` | `5` | przerwa między próbami połączenia |

## Parowanie

Połączenie jest szyfrowane i wymaga jednorazowego sparowania z PIN-em wyświetlanym na Flipperze. Parujesz zwykłymi narzędziami Bluetooth systemu, a serwer potem sam znajduje sparowanego Flippera.

1. Na Flipperze uruchom **PiTerm** (stan „Waiting to pair”). Flipper widoczny jest jako `PiTerm <nazwa>` z innym MAC niż zwykły Flipper (np. `Flipper Re0pl0p` ma `…:F0:36:CE`, a `PiTerm Re0pl0p` ma `…:F2:36:CE`).
2. Sparuj w `bluetoothctl`:
   ```
   bluetoothctl
   scan on                       # poczekaj, aż pojawi się "PiTerm <nazwa>"
   scan off
   pair 80:E1:27:F2:36:CE        # MAC urządzenia PiTerm, nie zwykłego Flippera
                                 # Flipper pokaże 6-cyfrowy PIN, wpisz go po "Enter passkey"
   trust 80:E1:27:F2:36:CE
   disconnect 80:E1:27:F2:36:CE
   exit
   ```
   Jeśli `pair` nie pyta o PIN, najpierw zarejestruj agenta: `agent KeyboardOnly`, potem `default-agent`.
   Można też sparować w ustawieniach Bluetooth środowiska graficznego (GNOME/KDE): wybierz `PiTerm <nazwa>` i wpisz PIN z Flippera.
3. Uruchom serwer ([Uruchamianie](#uruchamianie)). Bez `flipper_addr` w konfiguracji serwer użyje jedynego sparowanego urządzenia o nazwie `PiTerm …`. Gdy sparowanych PiTermów jest kilka, ustaw `flipper_addr`.

Alternatywnie parowanie z poziomu serwera, które od razu zapisuje adres w konfiguracji:
```bash
piterm-server scan                                   # MAC urządzenia PiTerm
sudo piterm-server pair 80:E1:27:F2:36:CE --save --config /etc/piterm/config.toml
```

Usunięcie parowania (np. przy zmianie Flippera albo błędach szyfrowania), po obu stronach:
- Flipper: PiTerm, a w ustawieniach „Forget pairing”.
- Serwer: `bluetoothctl remove 80:E1:27:F2:36:CE`.

Klucze parowania PiTerm Flipper trzyma osobno (`apps_data/piterm/.bt_term.keys`), więc nie wpływa to na parowanie z telefonem.

### Stany na Flipperze

Stan widać w pozycji „Status” w ustawieniach i na pasku u góry ekranu terminala (pasek znika, gdy trwa sesja).

| Stan | Znaczenie |
|---|---|
| Waiting to pair | Flipper się reklamuje, nie było jeszcze sesji z serwerem |
| Paired | Flipper się reklamuje, wcześniej była sesja; czeka na połączenie serwera |
| Connected | jest połączenie BLE, serwer jeszcze nic nie wysłał (w tym czasie trwa parowanie z PIN-em) |
| Session active | serwer przysłał dane (prompt powłoki); Flipper sam przechodzi na ekran terminala, pasek stanu znika |
| Off | Bluetooth nie działa (np. nie udało się uruchomić profilu) |

„Paired” Flipper zapamiętuje po pierwszej udanej sesji (firmware nie udostępnia aplikacjom informacji o bondingu). „Forget pairing” czyści też ten znacznik. Jeśli usuniesz parowanie tylko po stronie serwera, użyj też „Forget pairing” na Flipperze.

## Uruchamianie

Jako usługa systemd (zalecane):
```bash
sudo systemctl enable --now piterm
journalctl -u piterm -f                 # logi
```

Ręcznie, np. do testów na laptopie:
```bash
cd pi-server
cargo run -- --config dist/config.toml scan
cargo run -- --config dist/config.toml run
RUST_LOG=debug cargo run -- --config dist/config.toml run    # szczegółowe logi (okno flow control, MTU)
```

Podkomendy `piterm-server`:

| Komenda | Działanie |
|---|---|
| `run` (domyślna) | demon: łączy się z Flipperem, uruchamia powłokę, po rozłączeniu ponawia |
| `scan [--secs N]` | wypisuje Flippery z uruchomionym PiTerm |
| `inspect [MAC]` | łączy się i wypisuje usługi i charakterystyki |
| `pair [MAC] [--save]` | paruje (PIN z Flippera) i oznacza urządzenie jako zaufane |

Wszystkie przyjmują `--config <plik>` (domyślnie `/etc/piterm/config.toml`; brak pliku = ustawienia domyślne). Bez podanego MAC `run` i `inspect` używają `flipper_addr`, a gdy go nie ma, jedynego sparowanego urządzenia `PiTerm …`. Demon czeka, dopóki takie urządzenie się nie pojawi.

Przebieg sesji: uruchamiasz PiTerm na Flipperze, serwer łączy się w ciągu kilku sekund, a Flipper sam przechodzi na ekran outputu z promptem powłoki. `exit` albo `^D` kończy powłokę; serwer wypisuje `[piterm] new shell` i uruchamia nową. Wyjście z aplikacji na Flipperze kończy sesję, a serwer czeka na kolejne połączenie.

Przykładowe logi serwera:
```
INFO piterm_server: PiTerm: adapter hci0
INFO piterm_server::ble: using paired device 80:E1:27:F2:36:CE (PiTerm Re0pl0p)
INFO piterm_server::session: connected to 80:E1:27:F2:36:CE
INFO piterm_server::session: started shell /bin/bash
INFO piterm_server::session: Flipper disconnected
```

### Sterowanie na Flipperze

| Ekran | Przycisk | Akcja |
|---|---|---|
| Ustawienia | „Status” | stan połączenia (patrz [Stany na Flipperze](#stany-na-flipperze)) |
| | OK na „Terminal >” | output powłoki (po rozpoczęciu sesji przełącza się sam) |
| | „Line ending” | `\n` albo `\r\n` po komendzie |
| | „Local echo” (`Off`/`On`) | wypisywanie wysłanych bajtów lokalnie (zwykle zbędne, powłoka robi echo) |
| | OK na „Forget pairing” | usuwa klucze parowania PiTerm |
| Output | góra / dół (przytrzymanie: szybko) | przewijanie |
| | OK | klawiatura |
| | długie OK | Ctrl+C |
| | prawo | Enter (pusta linia) |
| | lewo | historia komend |
| | Back | ustawienia |
| Klawiatura | strzałki + OK | wpisywanie; długie OK na literze = wielka litera |
| | `aA` / `#@` | wielkie litery (jednorazowo) / symbole |
| | `del` (długie OK: czyści linię) | backspace |
| | `ent`, `tab`, `^C`, `^D` | wysyła linię / linię + Tab / Ctrl+C / Ctrl+D |
| | Back | powrót bez wysyłania (linia zostaje) |
| History | OK | wstawia komendę do klawiatury z kursorem na `ent` |

## Testy

```bash
# logika bufora terminala Flippera (filtr ANSI, zawijanie) na hoście
cc -Iflipper-app flipper-app/tests/term_buffer_test.c flipper-app/term_buffer.c -o /tmp/term_buffer_test && /tmp/term_buffer_test

# serwer
cd pi-server && cargo test && cargo clippy --all-targets -- -D warnings
```

Test BLE Flippera bez serwera: nRF Connect na telefonie. Zapis do RX (`...62fe0000`) pojawia się na ekranie outputu, a komenda z klawiatury przychodzi jako indication na TX (`...61fe0000`).

## Rozwiązywanie problemów

| Objaw | Przyczyna i rozwiązanie |
|---|---|
| `source .venv/bin/activate` w fish zwraca błąd | w fish użyj `source .venv/bin/activate.fish` |
| `python3 -m venv`: „ensurepip is not available” | `sudo apt install python3-venv` (albo `python3.X-venv` dla swojej wersji), potem `rm -rf .venv` i od nowa |
| `ufbt launch`: brak dostępu do `/dev/ttyACM0` | grupa `dialout` (patrz [Wymagania](#wymagania)), zamknij qFlipper |
| aplikacja na Flipperze: „API mismatch” / nie startuje | `.fap` zbudowany na innym SDK niż firmware: `ufbt update` dla właściwego firmware'u i przebuduj |
| serwer: „nie jest sparowany” / „brak sparowanego urządzenia PiTerm” | wykonaj [Parowanie](#parowanie); paruj urządzenie `PiTerm …`, nie zwykłego `Flipper …` |
| serwer: „nie znaleziono … w ciągu 10s” | PiTerm nie działa na Flipperze albo Flipper jest poza zasięgiem; serwer sam ponowi próbę |
| serwer: błąd D-Bus „not authorized” | użytkownik usługi musi być w grupie `bluetooth` (sprawdź `/etc/dbus-1/system.d/bluetooth.conf`) |
| parowanie się nie udaje albo zrywa szyfrowanie | usuń parowanie po obu stronach (patrz wyżej) i sparuj ponownie |

## Zasady pracy w repozytorium

- Zmiana protokołu zaczyna się od `docs/PROTOCOL.md`, a potem obejmuje oba projekty w tym samym PR.
- Stałe protokołu są w jednym miejscu w każdym projekcie: `flipper-app/protocol.h` i `pi-server/src/protocol.rs`. Oba pliki mają komentarz odsyłający do `docs/PROTOCOL.md`.
- Commity i PR-y oznaczaj prefiksem projektu: `flipper:`, `server:`, `protocol:`.

## CI

GitHub Actions (`.github/workflows/ci.yml`) z dwoma jobami uruchamianymi filtrem ścieżek (`paths:`):
- `flipper-app/**`: test hosta `term_buffer` i build `.fap` przez akcję `flipperdevices/flipperzero-ufbt-action`.
- `pi-server/**`: `cargo fmt --check`, `cargo clippy`, `cargo test`, `cargo build --release --target aarch64-unknown-linux-gnu` (przez `cross`).
- Zmiana w `docs/PROTOCOL.md` uruchamia oba joby.

## Dalszy rozwój

Zrobione: skan i parowanie, własny profil BLE na Flipperze, pełna pętla z pty i flow control, ekrany Flippera ze stanami połączenia, reconnect, unit systemd.

Otwarte tematy:
- Transport UART (`flipper-app/transport/transport_uart.c`) do pracy nad UI bez BLE; interfejs `transport.h` jest gotowy.
- Obsługa kilku Flipperów przez jeden serwer (lista MAC, osobna sesja i powłoka na urządzenie).
- Większe pakiety TX z Flippera (obecnie 20 B, bo aplikacja nie zna wynegocjowanego MTU).
- Zachowanie powłoki po rozłączeniu (jak `tmux`) zamiast uruchamiania nowej.
- Handshake z wersją protokołu, jeśli protokół zacznie się zmieniać (patrz `docs/PROTOCOL.md`).

Pierwotne założenia projektowe: `CONCEPT.md` w każdym projekcie.
