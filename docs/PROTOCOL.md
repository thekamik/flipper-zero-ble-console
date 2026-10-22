# Protokół PiTerm (BLE)

Jedyne źródło prawdy dla `flipper-app/` i `pi-server/`. Każda zmiana tutaj wymaga zmian w obu projektach.

## Role

- **Flipper Zero**: peryferium BLE, serwer GATT. Reklamuje się i czeka na połączenie.
- **Raspberry Pi**: centrala BLE, klient GATT. Skanuje, łączy się, uruchamia powłokę w pty.

Firmware Flippera nie udostępnia aplikacjom trybu central, więc odwrócenie ról nie jest możliwe.

## Usługa i charakterystyki

Usługa szeregowa z firmware'u Flippera (`targets/f7/ble_glue/services/serial_service.*`), uruchamiana we własnym profilu aplikacji (nie w `ble_profile_serial`, który podpina RPC aplikacji mobilnej).

| Rola | UUID | Właściwości | Kierunek |
|---|---|---|---|
| Serwis | `8fe5b3d5-2e7f-4a98-2a48-7acc60fe0000` | | |
| TX | `19ed82ae-ed21-4c9d-4145-228e61fe0000` | read, **indicate** | Flipper → Pi: bajty wpisane przez użytkownika |
| RX | `19ed82ae-ed21-4c9d-4145-228e62fe0000` | write, write w/o resp | Pi → Flipper: output powłoki |
| Flow control | `19ed82ae-ed21-4c9d-4145-228e63fe0000` | read, notify; `uint32` **big-endian** | wolne miejsce w buforze odbiorczym Flippera |
| RPC status | `19ed82ae-ed21-4c9d-4145-228e64fe0000` | read, write, notify | **nieużywane, Pi nie pisze** (zapis `0` wywołuje reset BLE na Flipperze) |

Limity po stronie Flippera: do 486 B na jedno wysłanie z TX (dzielone na wartości po 243 B), RX ma długość zmienną do 486 B.

## Identyfikacja i bezpieczeństwo

- Nazwa w reklamie: `PiTerm <nazwa Flippera>`.
- MAC: adres BLE Flippera z bajtem `[2]` zwiększonym o 2 (inny niż profil domyślny i HID).
- UUID-y są te same co w oficjalnym RPC Flippera, dlatego serwer łączy się **wyłącznie z adresem MAC z konfiguracji**.
- RX/TX wymagają połączenia uwierzytelnionego: bonding, parowanie metodą PIN wyświetlany na Flipperze.
- Flipper trzyma klucze parowania w osobnym pliku aplikacji (`APP_DATA_PATH(".bt_term.keys")`).

## Dane

- Surowe bajty terminala, bez ramkowania i bez kodowania.
- Komenda kończy się `\n` (opcjonalnie `\r\n`, ustawienie w aplikacji Flippera).
- Znaki sterujące wysyłane wprost: `0x03` Ctrl+C, `0x04` Ctrl+D, `0x09` Tab.
- Serwer uruchamia powłokę z `TERM=dumb`. Flipper i tak filtruje sekwencje ANSI (od `ESC` do litery kończącej).

## Flow control

### Pi → Flipper (RX, output powłoki)

1. Po połączeniu Pi odczytuje `fe63` jako okno (`u32` big-endian) i subskrybuje notify.
2. Pi wysyła kawałki `min(okno, MTU − 3, dane)` i odejmuje wysłane bajty od okna.
3. Przy oknie 0 Pi czeka na notify `fe63`.
4. Flipper po opróżnieniu bufora woła `ble_svc_serial_notify_buffer_is_empty()`. Okno jest przywracane i rozgłaszane **tylko wtedy, gdy spadło dokładnie do 0**, więc Pi musi dobić okno do zera, a nie zatrzymać się na małej wartości.

### Flipper → Pi (TX, komendy)

- Indications: każde wysłanie czeka na potwierdzenie od Pi (`SerialServiceEventTypeDataSent` na Flipperze), dopiero potem idzie następne.
- Flipper nie zna wynegocjowanego MTU, więc wysyła po **20 B** na indication (ATT MTU 23 − 3). Komendy są krótkie, więc zwykle to jeden lub kilka pakietów.

## Cykl życia sesji

1. Flipper: start aplikacji → własny profil BLE → advertising.
2. Pi: skan → połączenie z MAC z konfiguracji → (pierwszy raz) parowanie → odczyt okna → subskrypcje TX i FLOW.
3. Pi uruchamia powłokę w pty, Flipper pokazuje status „Połączono”.
4. Rozłączenie: Pi kończy sesję i ponawia połączenie co kilka sekund. Flipper wraca do advertisingu.
5. Wyjście z powłoki (`exit`, Ctrl+D): Pi uruchamia nową powłokę.
6. Wyjście z aplikacji na Flipperze: rozłączenie, przywrócenie domyślnego profilu BLE i ścieżki kluczy.

## Wersjonowanie

Na razie brak negocjacji wersji. Jeśli protokół zacznie się zmieniać (np. ramkowanie, zmiana rozmiaru terminala, komunikaty statusowe), dodać na początku sesji krótki handshake, np. linię `PITERM/1` wysyłaną przez Pi, i opisać go tutaj.
