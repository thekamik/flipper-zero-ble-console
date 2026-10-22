# PiTerm: klient na Flipper Zero (C)

> Dokument koncepcyjny sprzed implementacji. Aktualny stan, budowanie i obsługa: [`README.md`](../README.md).

Folder `flipper-app/` w repozytorium PiTerm. Druga część: [`../pi-server/CONCEPT.md`](../pi-server/CONCEPT.md) (serwer na Raspberry Pi w Rust). Wspólny protokół: [`../docs/PROTOCOL.md`](../docs/PROTOCOL.md).

## Cel

Aplikacja `.fap` na Flipper Zero, która daje interaktywny terminal powłoki działającej na Raspberry Pi. Połączenie idzie po Bluetooth LE: Flipper jest urządzeniem peryferyjnym (serwerem GATT), a Pi łączy się z nim jako centrala.

Flipper nie robi tu SSH ani TCP/IP. Przesyła surowy strumień bajtów terminala kanałem szeregowym zbudowanym na GATT. Bezpieczeństwo zapewnia parowanie BLE z PIN-em.

## Zakres tej części

- Własny profil BLE z usługą szeregową (bez RPC aplikacji mobilnej).
- 4 ekrany: połączenie, klawiatura + linia komendy, przewijany output, historia komend.
- Warstwa transportu oddzielona od UI, żeby UI dało się rozwijać najpierw po UART (kabel), a potem przełączyć na BLE.

Poza zakresem: wszystko po stronie Pi (patrz część 2).

## Protokół

Pełna specyfikacja (UUID-y, flow control, cykl życia sesji): [`../docs/PROTOCOL.md`](../docs/PROTOCOL.md).

Co z niej wynika dla Flippera:
- Flipper jest serwerem GATT: wysyła wpisane bajty przez **TX** (indication), odbiera output przez **RX**.
- Po opróżnieniu bufora odbiorczego woła `ble_svc_serial_notify_buffer_is_empty()`, żeby przywrócić okno flow control.
- Reklamuje się jako `PiTerm <nazwa>` z przesuniętym MAC i własnym plikiem kluczy parowania.

## Ustalenia z kodu firmware'u

### Nie używać `ble_profile_serial` wprost

Serwis `bt` (`applications/services/bt/bt_service/bt.c`) przy każdym połączeniu sprawdza, czy aktywny profil to `ble_profile_serial`. Jeśli tak, otwiera sesję RPC i podmienia callback usługi szeregowej na własny, więc nasze dane trafiłyby do RPC.

Rozwiązanie: **własny profil** (`FuriHalBleProfileTemplate`) w aplikacji, który w `start()` woła eksportowane `ble_svc_serial_start()`. Inny wskaźnik szablonu oznacza, że serwis `bt` nie otworzy RPC. Wzór: `lib/ble_profile/extra_profiles/hid_profile.c`.

Eksportowane API (publiczne w `api_symbols.csv`):
```c
BleServiceSerial* ble_svc_serial_start(void);
void ble_svc_serial_stop(BleServiceSerial* service);
void ble_svc_serial_set_callbacks(BleServiceSerial* service, uint16_t buff_size,
                                  SerialServiceEventCallback callback, void* context);
void ble_svc_serial_notify_buffer_is_empty(BleServiceSerial* service);
bool ble_svc_serial_update_tx(BleServiceSerial* service, uint8_t* data, uint16_t data_len);
```
Limity: `BLE_SVC_SERIAL_DATA_LEN_MAX = 486` (na jedno `update_tx`), `BLE_SVC_SERIAL_CHAR_VALUE_LEN_MAX = 243`.

### Szkic profilu (`term_profile.c`)

```c
#include <furi_ble/profile_interface.h>
#include <services/serial_service.h>
#include <furi_hal_version.h>

typedef struct {
    FuriHalBleProfileBase base;       // musi być pierwszym polem
    BleServiceSerial* serial_svc;
} BleProfileTerm;

static const FuriHalBleProfileTemplate term_profile_template; // forward

static FuriHalBleProfileBase* term_profile_start(FuriHalBleProfileParams params) {
    UNUSED(params);
    BleProfileTerm* p = malloc(sizeof(BleProfileTerm));
    p->base.config = &term_profile_template;
    p->serial_svc = ble_svc_serial_start();
    return &p->base;
}

static void term_profile_stop(FuriHalBleProfileBase* base) {
    furi_check(base && base->config == &term_profile_template);
    BleProfileTerm* p = (BleProfileTerm*)base;
    ble_svc_serial_stop(p->serial_svc);
    // zwalnianie pamięci profilu: sprawdzić, czy robi to core (porównać z hid_profile.c)
}

static void term_profile_get_config(GapConfig* cfg, FuriHalBleProfileParams params) {
    UNUSED(params);
    memset(cfg, 0, sizeof(GapConfig));
    cfg->adv_service.UUID_Type = UUID_TYPE_16;
    cfg->adv_service.Service_UUID_16 = 0x3080;
    cfg->appearance_char = 0x8600;
    cfg->bonding_mode = true;
    cfg->pairing_method = GapPairingPinCodeShow;   // PIN na ekranie Flippera
    cfg->conn_param.conn_int_min = 0x06;           // 7.5 ms
    cfg->conn_param.conn_int_max = 0x24;           // 45 ms
    memcpy(cfg->mac_address, furi_hal_version_get_ble_mac(), sizeof(cfg->mac_address));
    cfg->mac_address[2] += 2;  // inny MAC niż domyślny (+0) i HID (+1)
    snprintf(cfg->adv_name, sizeof(cfg->adv_name), "%cPiTerm %s",
             furi_hal_version_get_ble_local_device_name_ptr()[0],
             furi_hal_version_get_name_ptr());
}

static const FuriHalBleProfileTemplate term_profile_template = {
    .start = term_profile_start,
    .stop = term_profile_stop,
    .get_gap_config = term_profile_get_config,
};
```

### Przełączanie profilu (wzór: `applications/main/bad_usb/helpers/bad_usb_hid.c`)

Start:
```c
Bt* bt = furi_record_open(RECORD_BT);
bt_disconnect(bt);
furi_delay_ms(200);                                   // 2. rdzeń zapisuje NVM
bt_keys_storage_set_storage_path(bt, APP_DATA_PATH(".bt_term.keys")); // osobne klucze parowania
FuriHalBleProfileBase* profile = bt_profile_start(bt, &term_profile_template, NULL);
furi_check(profile);
ble_svc_serial_set_callbacks(((BleProfileTerm*)profile)->serial_svc,
                             RX_BUF_SIZE, on_serial_event, app);
furi_hal_bt_start_advertising();
bt_set_status_changed_callback(bt, on_bt_status, app);
```

Wyjście z aplikacji (koniecznie, inaczej Flipper zostanie w naszym profilu):
```c
bt_set_status_changed_callback(bt, NULL, NULL);
bt_disconnect(bt);
furi_delay_ms(200);
bt_keys_storage_set_default_path(bt);
furi_check(bt_profile_restore_default(bt));
furi_record_close(RECORD_BT);
```

### Odbiór i wysyłanie

- Callback `on_serial_event` działa w **wątku BLE i trzyma mutex usługi**. W `DataReceived` tylko kopiujemy dane do `FuriStreamBuffer` i zwracamy wolne miejsce (`uint16_t`).
- `ble_svc_serial_notify_buffer_is_empty()` wołamy **z wątku aplikacji** po opróżnieniu bufora. Wywołanie z callbacka spowoduje deadlock (mutex nie jest rekurencyjny).
- `ble_svc_serial_update_tx()` wysyła indication. Potwierdzenie przychodzi jako `SerialServiceEventTypeDataSent`. Następną paczkę wysyłamy po nim (flaga/semafor).
- `SerialServiceEventTypesBleResetRequest` ignorujemy albo logujemy.

## Architektura aplikacji

```
flipper-app/
├── application.fam
├── piterm.c / piterm.h        # alokacja App, ViewDispatcher, SceneManager, pętla
├── transport/
│   ├── transport.h            # interfejs: open/close/send/set_rx_callback/status
│   ├── transport_ble.c        # profil BLE + usługa szeregowa (powyżej)
│   └── transport_uart.c       # opcjonalnie: UART na GPIO 13/14 do testów UI
├── protocol.h                 # UUID-y i stałe z docs/PROTOCOL.md
├── term_profile.c / .h        # własny profil BLE
├── term_buffer.c / .h         # bufor kołowy outputu + filtr ANSI
├── history.c / .h             # historia komend, zapis na SD
├── views/
│   └── terminal_view.c        # własny View do przewijanego outputu
└── scenes/
    ├── scene_connection.c
    ├── scene_input.c
    ├── scene_output.c
    └── scene_history.c
```

`application.fam`:
```python
App(
    appid="piterm",
    name="PiTerm",
    apptype=FlipperAppType.EXTERNAL,
    entry_point="piterm_app",
    stack_size=4 * 1024,
    fap_category="Bluetooth",
    fap_version="0.1",
    fap_description="Terminal to Raspberry Pi over Bluetooth LE",
)
```

Interfejs transportu (`transport.h`) pozwala zacząć od UART (3 przewody, na Pi tylko włączona konsola szeregowa), dopracować UI, a potem podmienić na BLE bez zmian w ekranach:
```c
typedef void (*TransportRxCallback)(const uint8_t* data, size_t len, void* ctx);
typedef enum { TransportStatusIdle, TransportStatusWaiting, TransportStatusConnected } TransportStatus;

typedef struct {
    bool (*open)(void* inst);
    void (*close)(void* inst);
    bool (*send)(void* inst, const uint8_t* data, size_t len);
    void (*set_rx_callback)(void* inst, TransportRxCallback cb, void* ctx);
    TransportStatus (*status)(void* inst);
} Transport;
```

Wątki:
- **GUI**: `ViewDispatcher`.
- **Worker**: czyta `FuriStreamBuffer` z danymi z transportu, filtruje ANSI, dopisuje do `term_buffer`, woła `notify_buffer_is_empty()` (BLE) i wysyła custom event do odświeżenia widoku.

## Ekrany

1. **Połączenie** (`VariableItemList` + `Widget`)
   - Status: Czeka na Pi / Połączono / PIN parowania.
   - Opcje: transport (BLE/UART), koniec linii (`\n` / `\r\n`), echo lokalne on/off, „Zapomnij sparowane” (`bt_forget_bonded_devices`).
2. **Klawiatura + linia komendy** (`TextInput`)
   - Klawiatura ekranowa z jedną linią podglądu wpisywanego tekstu.
   - OK wysyła komendę, dopisuje ją do historii i przechodzi na ekran 3.
   - Skróty: `Ctrl+C`, `Ctrl+D`, `Tab` (np. z menu pod przytrzymaniem przycisku).
   - Do sprawdzenia: wbudowany `TextInput` ma ograniczony zestaw znaków (`|`, `>`, `$`, `-`, `/`…). Jeśli brakuje, potrzebny będzie własny widok klawiatury z warstwą symboli.
3. **Output** (własny `View`)
   - Bufor kołowy ok. 4 KB, przewijanie góra/dół, przytrzymanie przycisku przewija szybciej.
   - Ekran 128×64: ok. 21–32 znaków × 6–10 linii zależnie od fontu (`FontSecondary`, `FontKeyboard` albo własny 4×6).
   - Filtr ANSI: usuwanie sekwencji od `ESC` do litery kończącej. Serwer i tak ustawia `TERM=dumb`.
4. **Historia** (`Submenu`)
   - Ostatnie 20 komend w `APP_DATA_PATH("history.txt")` (`Storage`).
   - Wybór: wyślij od razu albo wstaw do ekranu 2 do edycji.

## Budowanie i testy

- `pip install ufbt`, w katalogu aplikacji `ufbt update` (SDK zgodne z firmware'em na Flipperze; dla custom firmware pobrać jego SDK), `ufbt` (build), `ufbt launch` (wgranie i uruchomienie po USB).
- Logi: `ufbt cli` → `log debug`.
- Test BLE bez Pi: nRF Connect na telefonie. Zapis do RX powinien pojawić się na ekranie 3, a komenda z ekranu 2 jako indication na TX.
- Test UI bez BLE: transport UART + konsola szeregowa na Pi.

## Kolejność prac

1. Szkielet `.fap`: profil BLE, advertising, parowanie, logowanie odebranych bajtów. Test z nRF Connect.
2. `transport.h` + `transport_ble.c` z flow control.
3. Ekran 3 (output) i ekran 2 (`TextInput`).
4. Ekran 4 (historia) i ekran 1 (status, ustawienia).
5. Filtr ANSI, skróty klawiszowe, ewentualnie transport UART.

## Do zweryfikowania

- Zarządzanie pamięcią profilu w `stop` (porównać z `hid_profile.c`).
- Jak wyświetla się PIN parowania przy niestandardowym profilu (zdarzenie `GapEventTypePinCodeShow` obsługuje serwis `bt`).
- Zgodność API z firmware'em na Twoim Flipperze (oficjalny vs. Momentum/Unleashed). API BLE zmieniało się w 2024 r. (0.98+).
- Zestaw znaków w `TextInput` w aktualnym firmware.
