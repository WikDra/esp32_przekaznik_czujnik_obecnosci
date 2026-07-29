# Sterownik żarówki: ESP32-C3 SuperMini + przekaźnik + LD2420 (Matter + panel HTTP)

Notatki projektu i plan pracy dla kolejnego agenta. Stan na 2026-07-28.

Repo: `https://github.com/WikDra/esp32_przekaznik_czujnik_obecnosci` (prywatne).
Instrukcja montażu i użytkowania dla człowieka: [`README.md`](README.md).

## 1. Cel

Sterownik żarówki 230 V:

- przekaźnik 1-kanałowy 5 V (styk 10 A / 250 VAC) załącza żarówkę,
- czujnik obecności mmWave **HLK-LD2420** wykrywa człowieka (także nieruchomego),
- **Matter over Wi-Fi** — sterowanie z Apple Home / Google Home / Home Assistant / SmartThings,
- **panel HTTP** — jednocześnie z Matterem; służy do regulacji progów czujnika, czasu
  podtrzymania, trybu automatyki i ręcznego załączania. Działa **od pierwszego bootu**
  (Wi-Fi z `firmware/sdkconfig.local`), nie tylko po sparowaniu Matter,
- **wymuszenie ON przez podwójne odcięcie prądu** — awaryjne załączenie bez sieci i apki.

Sterowanie z Mattera i z panelu działa równolegle: każda zmiana stanu przechodzi przez
`app_light_set()`, które ustawia przekaźnik i dosyła stan do atrybutu Matter OnOff.

## 2. Stan projektu

Zrobione i **zweryfikowane na sprzęcie** (ESP32-C3 na COM5, sieć `192.168.8.120`):

| Element | Status |
|---|---|
| Szkielet projektu ESP-IDF + esp-matter, target esp32c3 | build OK, 1,62 MB / 1,875 MB partycji (17 % wolne) |
| Firmware wgrany na fizyczną płytkę (COM5) | boot OK, brak crashy |
| Matter: node + endpoint 1 (On/Off Light) + endpoint 2 (Occupancy) | `endpoints: light=1 occupancy=2`, dynamic endpoint 0/1/2 added |
| Matter: serwer, mDNS commissionable, CHIPoBLE advertising | `Advertise commission parameter ... discriminator=3840 cm=1`, `Configuring CHIPoBLE advertising` |
| Kody parowania drukowane na starcie | QR `MT:Y.K9042C00KA0648G00`, kod ręczny `34970112332` |
| Wi-Fi przed commissioningiem (`CONFIG_DEFAULT_WIFI_SSID/PASSWORD`) | `WIFI_EVENT_STA_CONNECTED`, `sta ip: 192.168.8.120` |
| Panel HTTP + REST | `GET /` 200 (9,3 kB), `GET /api/status` 200, bez auth → 401 |
| Sterowanie z panelu | `POST /api/light {"on":true}` → `{"ok":true,"on":true}`, `{"toggle":true}` → `on:false` |
| Zapis ustawień do NVS | `POST /api/config {"hold_s":90,"max_cm":350}` → odczyt zwraca te wartości |
| Wymuszenie ON po 2 szybkich odcięciach zasilania | `W light: 2 power cycles detected - forcing the lamp ON`, `lamp ON (source: power_cycle)` |
| Sterownik LD2420 — brak czujnika nie blokuje bootu | `W ld2420: sensor not responding on UART1 (rx=4 tx=5), retrying in 30 s` |
| **LD2420 po UART** (test właściciela, 2026-07-29, ESP z powerbanka) | działa: `link_ok=true`, `fw=v1.6.1`, `mode=energy`, odczytane `min_gate=0 max_gate=12 timeout=30s` i wszystkie 16 par progów |
| **Automatyka end-to-end: radar → filtr → przekaźnik** | działa: przy `max_cm=50`, `hold_s=1` wejście w promień < 50 cm przełącza przekaźnik, wyjście gasi |
| Przekaźnik przez konwerter poziomów (test właściciela, 2026-07-29, ESP z powerbanka) | działa: `NO`–`COM` = 0 Ω przy ON, rozwarte przy OFF |
| Przekaźnik `GPIO10` **wprost** na `IN` (wariant A) | **nie działa** — przekaźnik załącza się i zostaje załączony (3,3 V nie zatyka PNP) |
| Skrypty build (WSL) / flash + monitor (Windows) | działają, patrz §5 |

**Niezweryfikowane** (wymaga podłączonego sprzętu / kontrolera Matter):

- commissioning Matter i synchronizacja OnOff apka ↔ panel (odłożone, patrz niżej),
- praca z oprawą 230 V i długi test stabilności (24 h),
- zachowanie przekaźnika po zaniku zasilania i po resecie z podłączoną oprawą,
- **do wgrania przy najbliższym podłączeniu USB:** poprawka atrybucji źródła zmiany
  (`source` w `/api/status` pokazywało `matter` także dla zmian z panelu/automatyki,
  bo nasz własny raport atrybutu wracał przez `app_attribute_update_cb`).

### Kalibracja — stan na 2026-07-29

Właściciel wyregulował progi w module (bramki 2–4 podniesione: `move` 3000/2000/500
wobec fabrycznych 400/250/250) i testował z `max_cm=50`, `hold_s=1` — celowo ostre
nastawy do sprawdzenia reakcji. Do pracy docelowej: `max_cm=0` (filtr aplikacyjny
wyłączony, zasięgiem rządzi `max_gate`) albo 400–500, `hold_s` 30–120 s.
Warto jeszcze sprawdzić `/api/status` przy **pustym** pomieszczeniu — jeśli
`sensor.raw_presence` zostaje `1`, obniżyć `max_gate` albo podnieść progi bramek,
które pokazują wysoką energię (bramka 2 pokazywała 9908 przy progu 3000).
### Matter — stan na 2026-07-28 (wstrzymane przez właściciela)

Parowanie odłożone; właściciel chce najpierw uruchomić żarówkę + czujnik + panel.
Ustalenia, żeby nie badać tego dwa razy:

- `chip-tool` z WSL **nie zadziała**: host build w
  `~/esp/esp-matter-1.4.2/.../out/host/chip-tool` jest bez IPv4
  (`Unsupported address: 192.168.8.120`), a WSL2 siedzi za NAT-em (brak mDNS/IPv6 do LAN).
- **Google Home wymaga huba Matter również dla urządzeń po Wi-Fi** (dokumentacja Google,
  „What you need”: *A Matter-enabled hub for Google Home*; border router Thread jest
  dopisany dodatkowo tylko dla Thread). Trzeba też **IPv6 na routerze**.
- Devkit ESP32-H2 z `D:\wysypisko\esp32c6h2\matter_h2\sensors` używał **tego samego**
  `VID 0xFFF1 / PID 0x8000`, więc Google Home dopasowywał nową płytkę do starej
  integracji (opisanej jako Thread) i szukał sieci Thread. Dlatego ten projekt ma teraz
  **własny PID `0x8010`** (`CONFIG_DEVICE_PRODUCT_ID` w `firmware/sdkconfig.local`).
  W Google Home Developer Console integrację trzeba zarejestrować jako
  VID `0xFFF1` + PID `0x8010`, device type *Light*.
- Aktualne kody: QR `MT:SAGA442C00KA0648G00`, kod ręczny `34970112332`, discriminator 3840.
  Po zmianie PID **QR się zmienia**, kod ręczny nie (nie koduje VID/PID).
- Najprostsza droga do przetestowania Mattera w tym domu: **Home Assistant** + dodatek
  *Matter Server* (właściciel ma HA, nie ma potwierdzonego huba Google) — nie wymaga
  osobnego sprzętu i paruje po IP w tej samej sieci.

**Uwaga o RAM:** przy aktywnym BLE (przed sparowaniem) `free_heap` ≈ 24 kB. To ciasno.
Po commissioningu BLE jest zwalniane (`CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING=y`,
zdarzenie `kBLEDeinitialized`) — zmierzyć heap po sparowaniu i jeśli dalej będzie
< 30 kB, ograniczać: bufory CHIP, `max_open_sockets` (już 4), logi.

### Ważne sprostowanie
Płytka podłączona do **COM5 to ESP32-C3**, nie C6:

```
Detecting chip type... ESP32-C3
Chip is ESP32-C3 (QFN32) (revision v0.4)
Features: WiFi, BLE, Embedded Flash 4MB (XMC)
USB mode: USB-Serial/JTAG   MAC: a4:cb:8f:20:d0:48
```

Czyli to docelowy SuperMini i testy robimy bezpośrednio na nim. Plik
`sdkconfig.defaults.esp32c6` pozostawiony na wypadek pracy na C6 (wtedy Matter też po Wi-Fi).

## 3. Sprzęt (potwierdzony ze zdjęć w `zdjecia_referencyjne/`)

- **ESP32-C3 SuperMini** (TENSTAR ROBOT), 4 MB flash, USB-C = natywny USB (USB-Serial/JTAG).
  Wyprowadzenia: lewa strona `5V, G, 3.3, 4, 3, 2, 1, 0`; prawa `5, 6, 7, 8, 9, 10, 20, 21`.
  GPIO8 = dioda on-board (aktywna stanem niskim), GPIO9 = przycisk BOOT (aktywny niski),
  GPIO2/8/9 to piny strapping — nie używać ich do przekaźnika.
- **Przekaźnik** HW-307 / JQC3F-05VDC-C, 1 kanał, `IN / GND / VCC`, cewka 5 V,
  wyzwalanie **stanem niskim**, styki 10 A 250 VAC.
- **HLK-LD2420 v2.1**, pady `3V3 | GND | OT1 | RX | OT2`:
  `OT1` = UART TX modułu (do RX ESP), `RX` = UART RX modułu (do TX ESP),
  `OT2` = cyfrowe wyjście obecności. Logika 3,3 V, 115200 8N1 (FW ≥ 1.5.4; starsze 256000).
- **Konwerter poziomów 4CH** (BSS138, dwukierunkowy, `LV/HV` + `GND`).

### Plan połączeń (zgodny z domyślnym Kconfig)

| Sygnał | ESP32-C3 SuperMini | Uwagi |
|---|---|---|
| LD2420 `3V3` | `3.3` | ~60 mA, LDO płytki wystarcza |
| LD2420 `GND` | `G` | masa wspólna |
| LD2420 `OT1` (TX) | `GPIO4` (UART1 RX) | |
| LD2420 `RX` | `GPIO5` (UART1 TX) | konieczne do zapisu progów |
| LD2420 `OT2` | niepodłączone (opcjonalnie `GPIO6`) | wtedy `CONFIG_APP_LD2420_OT2_GPIO=6` |
| Przekaźnik `VCC` | `5V` | cewka ~70–80 mA, tylko z 5 V (VBUS), **nie z 3,3 V** |
| Przekaźnik `GND` | `G` | |
| Przekaźnik `IN` | `GPIO10` **przez konwerter** (`LV1`←GPIO10, `HV1`→IN) | patrz niżej |
| Konwerter `LV` | `3.3` | |
| Konwerter `HV` | `5V` | |
| Konwerter `GND` | `G` | oba GND konwertera |
| Dioda statusu | `GPIO8` (on-board) | aktywna niskim |
| Przycisk | `GPIO9` (BOOT) | krótkie = toggle światła, ≥5 s = factory reset Matter |

**Dlaczego konwerter poziomów przy przekaźniku:** moduły wyzwalane stanem niskim mają
zwykle tranzystor PNP z emiterem na +5 V. Stan wysoki 3,3 V daje wtedy U_BE ≈ 1,7 V,
tranzystor dalej przewodzi i przekaźnik **nie wyłączy się**. Konwerter (open-drain +
pull-up do 5 V) podaje na `IN` pełne 5 V w stanie wysokim i zwiera do masy w niskim.
Bonus: podczas resetu/bootu ESP pin jest wejściem, a pull-up konwertera trzyma `IN`
w stanie wysokim, czyli przekaźnik jest rozwarty (żarówka zgaszona).

Jeśli po testach okaże się, że dany egzemplarz modułu poprawnie wyłącza się przy 3,3 V,
konwerter można pominąć — ale to trzeba **zmierzyć**, nie założyć.

**Warianty bez konwertera** (opisane w [`README.md`](README.md) §2.3a):

- A: `GPIO10` wprost na `IN`, firmware bez zmian (`CONFIG_APP_RELAY_OPEN_DRAIN=n`) —
  działa tylko wtedy, gdy moduł faktycznie odpada przy 3,3 V,
- B: `GPIO10` jako **open drain** + rezystor 10 kΩ do `+5 V`,
  `CONFIG_APP_RELAY_OPEN_DRAIN=y` — elektrycznie równoważne konwerterowi (pin tylko
  zwiera do masy, stan wysoki daje rezystor). W logu: `relay on GPIO10 (active low, open drain)`.

### ⚠️ Bezpieczeństwo 230 V
- Wszystkie prace przy stronie sieciowej przy **odłączonym** zasilaniu.
- Przez styk przekaźnika prowadzić przewód **fazowy** (L → COM, NO → żarówka), N bezpośrednio.
- Zachować izolację / odstępy między częścią 230 V a elektroniką 3,3/5 V; nie dotykać
  padów przekaźnika przy załączonym zasilaniu; docelowo zamknąć w obudowie.
- Ten moduł przekaźnika nie ma optoizolacji — masa logiki jest galwanicznie połączona
  z ESP, więc traktować cały układ jako potencjalnie pod napięciem po podłączeniu sieci.
- Jeśli nie ma pewności co do instalacji — zlecić podłączenie elektrykowi.

## 4. Środowisko

| Element | Ścieżka / wersja | Rola |
|---|---|---|
| WSL | `Ubuntu-24.04`, user `wikdra` | **jedyne** miejsce budowania (esp-matter nie działa na Windows) |
| ESP-IDF (WSL) | `~/esp/v5.4.2/esp-idf` | esp-matter 1.4.2 celuje w IDF v5.4.x |
| esp-matter (WSL) | `~/esp/esp-matter-1.4.2` | `export.sh` jest już wołany z `~/.bashrc` (ale **nie** ustawia `IDF_PATH`) |
| ESP-IDF (Windows) | `C:\Users\1thew\esp\v5.5.1\esp-idf` | tylko `esptool` + monitor przez COM |
| Płytka | `COM5` (USB-Serial/JTAG) | flash z Windows, bez usbipd |
| Projekt referencyjny Matter+LD2420 | `D:\wysypisko\esp32c6h2\matter_h2\sensors` | **tylko do czytania** |
| Projekt referencyjny skryptów | `D:\wysypisko\openlara_esp32` | **tylko do czytania** |

### Pułapki (już rozwiązane — nie powtarzać)

1. **Nie dodawać** `${ESP_MATTER_PATH}/examples/common` do `EXTRA_COMPONENT_DIRS` —
   ciągnie `app_reset`, który wymaga komponentu `button`:
   `Failed to resolve component 'button' required by component 'app_reset'`.
   Przycisk i factory reset są zaimplementowane własnym kodem w `app_light.cpp`.
2. **Kconfig `bool` z `default n` jest niezdefiniowany** w C — `CONFIG_X ? a : b` się nie
   kompiluje. Używać `#ifdef CONFIG_X`.
3. **`-Werror=narrowing`** w C++: makra typu `(0x0010 + gate)` trzeba rzutować na `uint16_t`.
4. `CONFIG_ENABLE_WIFI_AP` nie istnieje w esp-matter 1.4.2 (tylko ostrzeżenie, usunięte).
5. Konsola SuperMini idzie przez natywny USB → `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`
   (jest w `sdkconfig.defaults.esp32c3`). Bez tego log nie pojawi się na COM5.
6. ESP-IDF `export.bat`/`export.sh` odmawia pracy, gdy ustawione `MSYSTEM` (Git Bash) —
   skrypty `.bat` czyszczą tę zmienną.
7. Nieszkodliwe komunikaty w logu esp-matter 1.4.2: `E chip[SVR]: Could not find endpoint
   index for endpoint 0` i `E data_model: Cluster cannot be NULL.` (wewnętrzny guard przy
   przechodzeniu listy klastrów) — pojawiają się też w oryginalnych przykładach.

### Sekrety i lokalna konfiguracja

`firmware/sdkconfig.local` (w `.gitignore`) jest doklejany na koniec `SDKCONFIG_DEFAULTS`
przez `scripts/build.sh`. Trzyma dane Wi-Fi, dzięki którym ESP łączy się z siecią
**przed** commissioningiem Matter, więc panel HTTP jest dostępny od pierwszego bootu
(`CONFIG_DEFAULT_WIFI_SSID` / `CONFIG_DEFAULT_WIFI_PASSWORD` — mechanizm chipa: używane,
gdy w NVS nie ma jeszcze konfiguracji stacji; po sparowaniu Matter zapisuje własne dane).
Wzór: `firmware/sdkconfig.local.example`. Plik `haslo-wifi.md` też jest ignorowany.
Do repo **nie commitować** haseł ani `sdkconfig.*` generowanych przez build.

### Komendy

```bash
# build (WSL)
wsl -e bash -lc "cd /mnt/d/wysypisko/esp32_przekaznik_czujnik_obecnosci && ./scripts/build.sh esp32c3"
```

```bat
REM to samo z Windows
scripts\build-win.bat esp32c3

REM flash (Windows, esptool z IDF 5.5.1, binarki z build.esp32c3/)
scripts\flash-win.bat COM5 esp32c3

REM log
python scripts\monitor.py COM5 20
python scripts\reset_monitor.py COM5 20   REM reset płytki + log od bootu
```

`menuconfig` (piny, progi domyślne, hasło panelu):

```bash
wsl -e bash -lc "cd /mnt/d/wysypisko/esp32_przekaznik_czujnik_obecnosci/firmware && \
  source ~/esp/v5.4.2/esp-idf/export.sh && export ESP_MATTER_PATH=~/esp/esp-matter-1.4.2 && \
  source \$ESP_MATTER_PATH/export.sh && \
  idf.py -B build.esp32c3 -D SDKCONFIG=sdkconfig.esp32c3 menuconfig"
```

## 5. Struktura repo

```
firmware/
  CMakeLists.txt                  # projekt + ścieżki esp-matter
  partitions.csv                  # 4 MB, 2x OTA 1,875 MB, fctry, esp_secure_cert
  sdkconfig.defaults              # Matter over Wi-Fi, BLE tylko do commissioningu
  sdkconfig.defaults.esp32c3      # target + konsola po natywnym USB
  sdkconfig.defaults.esp32c6      # zapas, gdyby pracować na C6
  main/
    Kconfig.projbuild             # piny, domyślne progi, konfiguracja panelu HTTP
    app_priv.h                    # wspólne API modułów
    app_main.cpp                  # Matter: node, endpointy, callbacki, start
    app_light.cpp                 # przekaźnik, LED, przycisk, automatyka obecności
    app_settings.cpp              # ustawienia w NVS (namespace "swiatlo")
    ld2420.cpp/.h                 # sterownik czujnika (UART, tryb energy/simple, progi)
    app_web.cpp                   # serwer HTTP + REST + Basic Auth
    www/index.html                # panel (wbudowany w firmware przez EMBED_FILES)
scripts/                          # build.sh, build-win.bat, flash-win.bat, monitor*.py
zdjecia_referencyjne/             # zdjęcia sprzętu
```

### Architektura sterowania

```
Matter OnOff (EP1) ─┐
panel HTTP  ────────┼─→ app_light_set(on, źródło) ─→ GPIO przekaźnika
przycisk BOOT ──────┤        │
automatyka ─────────┘        └─→ (jeśli źródło ≠ matter) update atrybutu OnOff
LD2420 ─→ app_light_on_presence() ─→ filtr min/max cm ─→ Occupancy (EP2) + automatyka
```

Automatyka: obecność (zbocze narastające) zapala światło; utrata obecności uruchamia
odliczanie `hold_s` i po nim gasi. Ręczne wyłączenie w obecności człowieka **nie jest**
natychmiast nadpisywane (zapali się dopiero przy kolejnym zboczu obecności).

### Wymuszenie ON przez odcięcie zasilania

`CONFIG_APP_POWER_CYCLE_FORCE_ON` (domyślnie włączone, `APP_POWER_CYCLE_COUNT=2`,
`APP_POWER_CYCLE_WINDOW_S=10`):

- każdy start zwiększa licznik w NVS (`swiatlo/pc_cnt`); po 10 s pracy licznik się zeruje,
- drugi start w ciągu 10 s ⇒ żarówka zapala się od razu (`source: power_cycle`) i
  **automatyka jest pomijana** (żadnego auto-off), aż do jawnego OFF z Mattera, panelu
  lub przycisku,
- stan widoczny w `/api/status` jako `force_on`,
- zwykły reset płytki liczy się tak samo jak odcięcie prądu (dlatego flash + reset w
  krótkim czasie potrafi wywołać ten tryb — tak to zostało zweryfikowane).

Sekwencja startowa ustawia atrybut Matter OnOff na **rzeczywisty** stan przekaźnika
(uwzględnia `force_on` i opcję `restore_state`), więc apka nie pokazuje fałszywego OFF.

### REST API (`Content-Type: application/json`, opcjonalny Basic Auth)

| Metoda | Ścieżka | Body | Opis |
|---|---|---|---|
| GET | `/` | – | panel HTML |
| GET | `/api/status` | – | pełny stan: światło, obecność, dystans, ustawienia, czujnik (bramki, progi), IP, heap |
| POST | `/api/light` | `{"on":true}` / `{"toggle":true}` | sterowanie żarówką |
| POST | `/api/config` | `{"auto_mode":true,"hold_s":60,"max_cm":400,"min_cm":0,"restore_state":false}` | ustawienia aplikacji (NVS) |
| POST | `/api/sensor` | `{"min_gate":1,"max_gate":6,"timeout_s":30}` | zakres i timeout modułu |
| POST | `/api/sensor` | `{"gate":3,"move":250,"still":200}` | progi jednej bramki (1 bramka ≈ 0,7 m) |
| POST | `/api/sensor` | `{"mode":"energy"\|"simple"}` | tryb wyjścia modułu |
| POST | `/api/sensor` | `{"action":"refresh"\|"restart"\|"factory_reset"}` | operacje na module |
| POST | `/api/reboot` | `{}` | restart ESP32 |

Panel HTTP startuje po zdarzeniu `kInterfaceIpAddressChanged` (pierwsze IP, także IPv6 —
dlatego w logu może najpierw pojawić się `http://0.0.0.0:80/`, a po IPv4 właściwy adres).
Dzięki `sdkconfig.local` dzieje się to już przed commissioningiem Matter. Domyślnie
chroniony Basic Auth (`admin` / `swiatlo` — **zmienić** w `menuconfig`,
`CONFIG_APP_WEB_PASS`). To zwykły HTTP, więc tylko do zaufanej sieci LAN;
nie wystawiać na internet.

## 6. Plan dla następnego agenta

Kolejność wg priorytetu właściciela: **żarówka → czujnik → panel**, Matter na końcu
(świadomie odłożony, patrz §2).

### Krok 1 — przekaźnik (bez 230 V!)
1. Podłączyć jednym z trzech sposobów:
   - z konwerterem (§3): `IN`→`HV1`, `LV1`→`GPIO10`, `LV`→`3.3`, `HV`→`5V`, oba `GND` do masy,
   - wariant A: `GPIO10` wprost na `IN` (firmware bez zmian),
   - wariant B: `GPIO10` + rezystor 10 kΩ do `+5 V` i `CONFIG_APP_RELAY_OPEN_DRAIN=y`.
   Zawsze: przekaźnik `VCC`→`5V`, `GND`→`G`.
2. Flash, potem `POST /api/light` lub krótkie wciśnięcie BOOT — sprawdzić kliknięcie
   przekaźnika i diodę na module; zmierzyć napięcie na `IN` (blisko 0 V = ON, ~5 V = OFF).
3. **Kluczowy test wariantu A:** czy przekaźnik faktycznie odpada przy stanie wysokim
   3,3 V. Jeśli nie — przejść na wariant B albo konwerter.
4. Sprawdzić stan po resecie i po zaniku zasilania: przekaźnik musi zostać rozwarty.
5. Dopiero po tym podłączać oprawę 230 V (§3, ostrzeżenia).

### Krok 2 — LD2420
1. Podłączyć `3V3/GND/OT1→GPIO4/RX→GPIO5`.
2. W logu powinno pojawić się `ld2420: firmware v1.x.x`, `config: min_gate=… max_gate=…
   timeout=…s`, `sensor ready (mode=energy)`. Jeśli zamiast tego leci
   `sensor not responding` — sprawdzić: zamienione TX/RX, baud (starsze FW: 256000 —
   zmienić w `ld2420_init`), masę.
3. Zweryfikować w `/api/status`: `sensor.link_ok`, `raw_distance_cm`, tablicę `gates`
   (energie rosną, gdy ktoś jest w polu widzenia).
4. Kalibracja: `max_gate` ustawić tak, by ściana/korytarz nie łapały (1 bramka ≈ 0,7 m),
   progi `move`/`still` podnieść dla bramek dających fałszywe wyzwolenia. Wartości
   fabryczne są w `ld2420.cpp` (`FACTORY_MOVE_THRESH` / `FACTORY_STILL_THRESH`).
   Dodatkowy filtr aplikacyjny: `max_cm` / `min_cm` w `/api/config`.

### Krok 3 — Matter (odłożony, robić po żarówce i czujniku)
1. Kody z logu: QR `MT:SAGA442C00KA0648G00`, kod ręczny `34970112332`,
   discriminator 3840, VID `0xFFF1`, PID `0x8010` (testowe certyfikaty DAC — do użytku
   domowego OK, do certyfikacji potrzebne własne w partycji `fctry`).
2. Zalecana droga: **Home Assistant** + dodatek *Matter Server* (paruje po IP, bez huba).
   Google Home wymaga huba Matter i IPv6 na routerze; `chip-tool` z WSL nie działa (§2).
3. Sprawdzić: włączanie/wyłączanie z apki → klik przekaźnika; zmiana z panelu HTTP →
   aktualizacja stanu w apce; atrybut Occupancy na EP2 reaguje na obecność.
4. Zmierzyć `free_heap` po sparowaniu (BLE zwolnione) — patrz uwaga o RAM w §2.

### Krok 4 — dokończenie funkcji
- [ ] Zmienić domyślne hasło panelu (`CONFIG_APP_WEB_PASS`).
- [ ] Rozważyć mDNS dla panelu (`swiatlo.local`) — uwaga: Matter używa własnego mDNS
      (minimal mDNS), więc trzeba sprawdzić, czy `mdns_hostname_set()` nie psuje
      rozgłaszania `_matterc._udp` / `_matter._tcp`.
- [ ] Test zaniku Wi-Fi / restartu routera (czy panel wraca po rekonekcie — obecnie
      serwer startuje raz, przy pierwszym IP; `app_web_start()` jest idempotentne).
- [ ] Opcjonalnie: harmonogram / blokada nocna, licznik godzin pracy, log zdarzeń.
- [ ] Opcjonalnie: OTA (partycje już przygotowane, `CONFIG_ENABLE_OTA_REQUESTOR=y`).
- [ ] Opcjonalnie: SoftAP + portal, gdyby dane Wi-Fi miały być wpisywane bez rekompilacji.
- [ ] Opcjonalnie: watchdog na zawieszony czujnik (obecnie po 10 s bez ramek
      `link_ok=false`, obecność wymuszana na `false`).

### Krok 5 — testy końcowe
- 24 h pracy: brak restartów (`uptime_s` w `/api/status`), stabilny `free_heap`.
- Brak fałszywych zapaleń przy pustym pomieszczeniu; brak gaśnięcia przy nieruchomej osobie.
- Test factory reset (BOOT ≥5 s) i ponownego parowania.

## 7. Decyzje właściciela (2026-07-28)

1. Panel HTTP ma działać **także przed** commissioningiem Matter → zrealizowane przez
   `firmware/sdkconfig.local` z danymi Wi-Fi (`CONFIG_DEFAULT_WIFI_SSID/PASSWORD`).
   Opcjonalne rozwinięcie w przyszłości: SoftAP z portalem konfiguracyjnym, gdyby
   urządzenie miało trafiać do innych sieci bez rekompilacji.
2. Dane Wi-Fi dostarczone (`haslo-wifi.md`, plik ignorowany przez gita).
3. **Bez** fizycznego włącznika ściennego — sterownik siedzi przy żarówce. Zamiast tego
   awaryjne wymuszenie ON przez 2 szybkie odcięcia prądu (zaimplementowane).
4. Zasilanie: zasilacz 5 V robi właściciel — nie ruszać tej części.

### Do rozstrzygnięcia później

- Czy zostawić domyślne hasło panelu, czy wygenerować własne (na razie `admin`/`swiatlo`).
- Czy dodać `swiatlo.local` (mDNS) obok mDNS Mattera.
