# Sterownik żarówki: ESP32-C3 SuperMini + przekaźnik + LD2420

Sterownik oświetlenia 230 V z czujnikiem obecności mmWave, sterowany **jednocześnie**
przez **Matter** (Apple Home / Google Home / Home Assistant / SmartThings) i przez
**lokalny panel HTTP** (regulacja progów czujnika, czasu podtrzymania, tryb automatyki).

- Automatyka: obecność zapala żarówkę, po utracie obecności gaśnie z opóźnieniem `hold_s`.
- Tryb nocny bez czujnika zmierzchu: czas z internetu (SNTP) + wyliczony zachód słońca,
  z offsetami — automatyka zapala tylko po zmroku (§4.6).
- Panel HTTP działa od pierwszego bootu (dane Wi-Fi z `firmware/sdkconfig.local`),
  nie tylko po sparowaniu Matter.
- Awaryjne wymuszenie ON: **dwa szybkie odcięcia zasilania** (bez sieci i bez apki).
- Awaryjna konfiguracja Wi-Fi: brak połączenia przez minutę → urządzenie rozgłasza własną
  sieć i wystawia panel pod `192.168.4.1` (§4.5). Sieć można też zmienić z panelu.
- Przycisk BOOT: krótkie wciśnięcie = przełącz światło, ≥5 s = factory reset Matter.

Notatki techniczne, stan weryfikacji i plan rozwoju: [`AGENTS.md`](AGENTS.md).

---

## 1. Elementy

| Element | Uwagi |
|---|---|
| ESP32-C3 SuperMini (TENSTAR ROBOT) | 4 MB flash, USB-C = natywny USB (USB-Serial/JTAG) |
| Przekaźnik 1-kanałowy 5 V (HW-307 / JQC3F-05VDC-C) | `IN / GND / VCC`, wyzwalanie **stanem niskim**, styki 10 A 250 VAC |
| HLK-LD2420 v2.1 (radar 24 GHz) | pady `3V3 / GND / OT1 / RX / OT2`, logika 3,3 V, UART 115200 8N1 |
| Konwerter poziomów 4CH (BSS138) | `LV / HV / GND` + kanały `LV1..LV4` / `HV1..HV4` — opcjonalny, patrz §2.3a |
| Zasilacz 5 V | zasila płytkę i cewkę przekaźnika |

## 2. Podłączenie

### 2.1 Rozkład pinów SuperMini

```
            ┌───────────── USB-C ─────────────┐
      5V ───┤ 5V                            5 ├─── GPIO5   → LD2420 RX
     GND ───┤ G                             6 ├─── GPIO6   (wolny / opcjonalnie OT2)
     3V3 ───┤ 3.3                           7 ├─── GPIO7   (wolny)
   GPIO4 ───┤ 4       ESP32-C3 SuperMini    8 ├─── GPIO8   dioda on-board (aktywna niskim)
   GPIO3 ───┤ 3                             9 ├─── GPIO9   przycisk BOOT (aktywny niskim)
   GPIO2 ───┤ 2                            10 ├─── GPIO10  → przekaźnik IN (przez konwerter)
   GPIO1 ───┤ 1                            20 ├─── GPIO20  (UART0 RX)
   GPIO0 ───┤ 0                            21 ├─── GPIO21  (UART0 TX)
            └─────────────────────────────────┘
```

GPIO2 / GPIO8 / GPIO9 to piny strapping — nie używać do przekaźnika.

### 2.2 Czujnik LD2420

| LD2420 | ESP32-C3 | Uwagi |
|---|---|---|
| `3V3` | `3.3` | ~60 mA, LDO płytki wystarcza |
| `GND` | `G` | masa wspólna |
| `OT1` | `GPIO4` | `OT1` to **TX modułu** → RX ESP (UART1 RX) |
| `RX` | `GPIO5` | TX ESP → RX modułu; **konieczne** do zapisu progów |
| `OT2` | – | opcjonalne cyfrowe wyjście obecności (`CONFIG_APP_LD2420_OT2_GPIO=6`) |

Jeśli czujnik ma starszy firmware (< 1.5.4), pracuje na 256000 baud — trzeba zmienić
`baud_rate` w `firmware/main/ld2420.cpp` (`ld2420_init`).

### 2.3 Przekaźnik przez konwerter poziomów

```
  ESP32-C3                 Konwerter 4CH (BSS138)              Przekaźnik 5 V
  ─────────                ──────────────────────              ──────────────
  GPIO10  ────────────────► LV1                 HV1 ──────────► IN
  3.3     ────────────────► LV                  HV  ◄────────── VCC ◄─── +5 V
  G       ────────────────► GND (obie strony)   GND ──────────► GND ◄─── GND 5 V
```

- Cewka przekaźnika: `VCC` → **5 V bezpośrednio** (nie przez kanał konwertera — to
  sygnałowy MOSFET, nie udźwignie cewki). Pobiera ~70–80 mA, **nie zasilać z 3,3 V**.
- Szynę `HV` konwertera też podłącz do **5 V**. Bez tego stan wysoki na `IN` powstaje
  tylko przez złącze baza-emiter tranzystora w module (który zaczyna przewodzić już przy
  ~4,4 V) — działa, ale bez zapasu i zależnie od egzemplarza modułu. Pull-up 10 kΩ do
  `HV` wymusza pełne 5 V.
- `GND`: oba piny masy konwertera to na tych płytkach ta sama masa, więc wystarczy jedno
  połączenie do masy wspólnej.
- Poziomy: GPIO10 nisko = przekaźnik załączony (żarówka świeci), wysoko = rozwarty.

**Dlaczego konwerter jest tu potrzebny:** moduły wyzwalane stanem niskim mają zwykle
tranzystor PNP z emiterem na +5 V. Stan wysoki 3,3 V daje U_BE ≈ 1,7 V — tranzystor
dalej przewodzi i przekaźnik **nie wyłączy się**. Konwerter (open-drain + pull-up do 5 V)
podaje na `IN` pełne 5 V. Bonus: podczas bootu/resetu pin ESP jest wejściem, a pull-up
konwertera trzyma `IN` wysoko, więc przekaźnik jest rozwarty i żarówka zgaszona.

Jeśli zmierzysz, że twój egzemplarz modułu poprawnie wyłącza się przy 3,3 V, konwerter
można pominąć — ale to trzeba **zmierzyć**, nie założyć. Warianty bez konwertera: §2.3a.

### 2.3a Warianty bez konwertera poziomów

> **Wynik testu na tym egzemplarzu (2026-07-29):** wariant A **nie działa** — przy
> `GPIO10` podłączonym wprost do `IN` przekaźnik załączał się i zostawał załączony
> (3,3 V nie zatyka tranzystora PNP). Docelowe podłączenie to konwerter z §2.3.
> Wariant A zostaje tu opisany tylko dla innych modułów przekaźnika.

**Wariant A — GPIO wprost na `IN` (najprostszy, wymaga testu)**

```
  GPIO10 ──────────────► IN
  5V     ──────────────► VCC
  G      ──────────────► GND
```

Ustawienia firmware: domyślne (`CONFIG_APP_RELAY_OPEN_DRAIN=n`). Test po wgraniu:

1. `POST /api/light {"on":true}` → przekaźnik klika, dioda na module świeci,
   napięcie na `IN` ≈ 0 V.
2. `POST /api/light {"on":false}` → przekaźnik **musi** odpaść (słyszalny klik,
   dioda gaśnie), napięcie na `IN` ≈ 3,3 V.
3. Powtórz kilka razy i sprawdź stan po resecie płytki.

Jeśli w punkcie 2 przekaźnik **nie** odpada (zostaje załączony, dioda świeci) — to
właśnie ten przypadek z tranzystorem PNP: 3,3 V nie wystarcza, by go zatkać. Wtedy
wariant B albo konwerter z §2.3.

**Wariant B — GPIO jako open drain + rezystor 10 kΩ do +5 V (1 element)**

```
                     +5V
                      │
                     [10k]
                      │
  GPIO10 ─────────────┴──► IN          (GPIO tylko zwiera do masy)
  5V     ──────────────► VCC
  G      ──────────────► GND
```

Ustawienia firmware: `CONFIG_APP_RELAY_OPEN_DRAIN=y` (w `menuconfig` → *Swiatlo LD2420*
→ *Pin map* → *Drive the relay pin as open drain*, albo linijka w `sdkconfig.local`).
Wtedy GPIO nigdy nie wystawia 3,3 V — stan wysoki robi rezystor, podając na `IN` pełne
5 V, dokładnie jak konwerter. To elektrycznie równoważne rozwiązanie, tylko tańsze.
W logu po starcie zobaczysz `relay on GPIO10 (active low, open drain)`.

**Czego nie robić:** nie zasilać cewki z pinu `3.3` (potrzebuje ~70–80 mA i 5 V) i nie
podawać 5 V na GPIO ESP32-C3 — piny nie są 5 V tolerant, `IN` przy 3,3 V logiki nigdy
nie powinien być podciągany do ESP.

Zalety wariantu z konwertera z §2.3: dodatkowa separacja szyn i pewność, że przy
resecie/bootowaniu `IN` jest trzymany wysoko (żarówka zgaszona). Wariant B daje to samo
zachowanie przy bootcie, bo pull-up jest zewnętrzny. Wariant A polega na tym, że w stanie
wysokim pin sam wystawia 3,3 V — po resecie pin jest wejściem, więc `IN` „wisi” i stan
przekaźnika jest nieokreślony do momentu inicjalizacji GPIO (zwykle < 300 ms).

### 2.4 Strona 230 V

```
  L (faza) ──► COM przekaźnika
  NO przekaźnika ──► oprawa/żarówka
  N (zero) ──────────────────────► oprawa/żarówka
  PE ─────────────────────────────► oprawa (jeśli metalowa)
```

⚠️ **Bezpieczeństwo**

- Wszystkie prace przy stronie sieciowej przy **odłączonym** zasilaniu.
- Przez styk przekaźnika prowadzić **fazę**, nie zero.
- Ten moduł przekaźnika **nie ma optoizolacji** — masa logiki jest galwanicznie połączona
  z ESP, więc po podłączeniu sieci traktować cały układ jako potencjalnie pod napięciem.
- Zachować odstępy/izolację między częścią 230 V a elektroniką; docelowo obudowa.
- Nie dotykać padów przekaźnika przy załączonym zasilaniu.
- Brak pewności co do instalacji → zlecić elektrykowi.

### 2.5 Kolejność uruchamiania

1. Podłączyć tylko przekaźnik (bez 230 V) i sprawdzić klikanie z panelu / przycisku BOOT;
   zmierzyć napięcie na `IN` (≈0 V = ON, ≈5 V = OFF).
2. Sprawdzić, że po resecie i po zaniku zasilania przekaźnik jest **rozwarty**.
3. Podłączyć LD2420, sprawdzić w logu `ld2420: sensor ready` i dane w panelu.
4. Dopiero na końcu podłączać oprawę 230 V.

---

## 3. Budowanie i wgrywanie

Wymagane: WSL z ESP-IDF **v5.4.2** i esp-matter **1.4.2** (esp-matter nie buduje się na
Windows), oraz ESP-IDF na Windows tylko do `esptool` i monitora portu COM.

```bash
# build (WSL)
./scripts/build.sh esp32c3
```

```bat
REM build z Windows (wywołuje to samo w WSL)
scripts\build-win.bat esp32c3

REM wgranie (esptool z ESP-IDF na Windows)
scripts\flash-win.bat COM5 esp32c3

REM log
python scripts\monitor.py COM5 20
python scripts\reset_monitor.py COM5 20   REM reset płytki + log od bootu
```

### Dane Wi-Fi i hasło panelu

Skopiuj `firmware/sdkconfig.local.example` na `firmware/sdkconfig.local` (plik jest
w `.gitignore`, nie trafia do repo) i wpisz swoje dane:

```
CONFIG_DEFAULT_WIFI_SSID="MojeWiFi"
CONFIG_DEFAULT_WIFI_PASSWORD="tajnehaslo"
CONFIG_APP_WEB_USER="admin"
CONFIG_APP_WEB_PASS="wlasne-haslo"
```

`scripts/build.sh` dokleja ten plik na koniec `SDKCONFIG_DEFAULTS`. Piny, progi domyślne
i pozostałe opcje: `menuconfig` (komenda w [`AGENTS.md`](AGENTS.md), §4).

---

## 4. Użytkowanie

### 4.1 Panel HTTP

`http://<ip-urządzenia>/` — adres pojawia się w logu (`sta ip: ...`).
Domyślny login: **`admin`**, hasło: **`swiatlo`** (zmienić przez `CONFIG_APP_WEB_PASS`).
To zwykły HTTP — tylko do zaufanej sieci LAN, nie wystawiać na internet.

W panelu: ręczne włączanie/wyłączanie, stan obecności i odległości, tryb automatyki,
czas podtrzymania, okno odległości (`min_cm`/`max_cm`), zakres bramek i progi
`ruch`/`spoczynek` dla każdej z 16 bramek czujnika (1 bramka ≈ 0,7 m).

### 4.2 REST API

| Metoda | Ścieżka | Body | Opis |
|---|---|---|---|
| GET | `/api/status` | – | pełny stan (światło, obecność, czujnik, IP, heap) |
| GET | `/api/events` | – | historia ostatnich wykryć i przełączeń (§4.12) |
| POST | `/api/light` | `{"on":true}` / `{"toggle":true}` | sterowanie żarówką |
| POST | `/api/config` | `{"auto_mode":true,"hold_s":60,"max_cm":400,"min_cm":0,"hyst_cm":30,"presence_src":"distance","restore_state":false}` | ustawienia aplikacji (NVS) |
| POST | `/api/config` | `{"night_only":true,"sunset_off_min":-30,"sunrise_off_min":30,"lat":52.2297,"lon":21.0122,"tz":"CET-1CEST,M3.5.0,M10.5.0/3","ntp_server":"pool.ntp.org"}` | tryb nocny i czas (§4.6) |
| POST | `/api/sensor` | `{"min_gate":1,"max_gate":6,"timeout_s":30}` | zakres i timeout modułu |
| POST | `/api/sensor` | `{"gate":3,"move":250,"still":200}` | progi jednej bramki |
| POST | `/api/sensor` | `{"mode":"energy"\|"simple"}` | tryb wyjścia modułu |
| POST | `/api/sensor` | `{"action":"refresh"\|"restart"\|"factory_reset"}` | operacje na module |
| POST | `/api/wifi` | `{"ssid":"...","password":"..."}` | zapis sieci Wi-Fi + restart (§4.5) |
| POST | `/api/wifi` | `{"setup_mode":true}` | restart do trybu serwisowego z własnym AP |
| GET | `/api/wifi/scan` | – | lista widocznych sieci (tylko w trybie serwisowym) |
| POST | `/api/password` | `{"current":"...","password":"...","user":"admin"}` | zmiana hasła panelu (§4.10) |
| GET | `/api/settings` | – | kopia ustawień w JSON (bez sekretów) |
| POST | `/api/settings` | zawartość kopii | odtworzenie ustawień i kalibracji |
| POST | `/api/ota` | plik `.bin` (`application/octet-stream`) | aktualizacja firmware (§4.8) |
| POST | `/api/reboot` | `{}` | restart ESP32 |

```bash
curl -u admin:swiatlo http://192.168.8.120/api/status
curl -u admin:swiatlo -H "Content-Type: application/json" \
     -d '{"toggle":true}' http://192.168.8.120/api/light
```

### 4.3 Matter

Kody parowania są drukowane w logu na starcie (dopóki urządzenie nie jest sparowane):

```
Setup QRCode:        MT:SAGA442C00KA0648G00
Manual pairing code: 34970112332
```

Urządzenie zgłasza się jako VID `0xFFF1` (test VID) i PID `0x8010`. **Google Home wymaga
huba Matter** (Nest Hub / głośnik Nest / Google TV) oraz IPv6 na routerze, a dla
testowego VID także zarejestrowania integracji w Google Home Developer Console
(device type *Light*, VID `0xFFF1`, PID `0x8010`). Apple Home też potrzebuje huba
(HomePod / Apple TV). **Home Assistant** z dodatkiem *Matter Server* paruje bez
dodatkowego sprzętu — to najprostsza droga.

Zmiana `CONFIG_DEVICE_PRODUCT_ID` zmienia QR (kod ręczny zostaje ten sam, bo nie koduje
VID/PID).
Urządzenie zgłasza dwa endpointy: **1 = On/Off Light** (przekaźnik), **2 = Occupancy**
(obecność z LD2420). Zmiana z apki i z panelu jest natychmiast widoczna po obu stronach.

Firmware używa **testowych certyfikatów DAC** — do użytku domowego OK, do certyfikacji
Matter potrzebne własne certyfikaty w partycji `fctry`.

### 4.4 Awaryjne wymuszenie ON

Dwa szybkie odcięcia zasilania (domyślnie 2 cykle w oknie 10 s) zapalają żarówkę
z pominięciem automatyki — dopóki nie wyłączysz jej jawnie z Mattera, panelu lub
przycisku. Stan widoczny jako `force_on` w `/api/status`. Zwykły reset płytki liczy się
tak samo jak odcięcie prądu. Trzy odcięcia cofają firmware (§4.8), cztery resetują hasło
panelu (§4.10). Opcje: `CONFIG_APP_POWER_CYCLE_*`.

### 4.5 Zmiana Wi-Fi i awaryjny tryb serwisowy

Sterownik siedzi w oprawie, gdzie nie ma dostępu do USB, więc musi sam wyjść z sytuacji
„zmieniło się hasło do Wi-Fi”:

1. Po starcie liczy czas do uzyskania adresu IP. Jeśli w ciągu **60 s** go nie dostanie
   (`CONFIG_APP_WIFI_FALLBACK_S`), zapisuje flagę w NVS i restartuje się.
2. Wstaje w **trybie serwisowym**: Matter nie startuje, za to urządzenie rozgłasza własną
   sieć **`Swiatlo-XXXX`** (`XXXX` = dwa ostatnie bajty MAC), hasło `swiatlo123`
   (`CONFIG_APP_PROV_AP_PASSWORD`). Panel jest wtedy pod **http://192.168.4.1/**.
3. W panelu w sekcji *Wi-Fi* wpisujesz nową sieć (albo wybierasz ze skanu) i zapisujesz —
   urządzenie zapisuje dane i restartuje się do normalnej pracy.
4. Jeśli w ciągu **10 minut** (`CONFIG_APP_PROV_TIMEOUT_MIN`) nic nie wpiszesz, urządzenie
   restartuje się i ponawia próbę z zapisaną siecią — to na wypadek, gdy router tylko się
   restartował. Flaga trybu serwisowego jest czyszczona już przy wejściu, więc zanik
   zasilania nigdy nie zostawi urządzenia w trybie AP na stałe.

Automatyka obecności i przekaźnik działają normalnie także w trybie serwisowym
(bez czasu z sieci sterownik traktuje porę jako noc — patrz fail-safe w §4.6).

Sieć można też zmienić bez czekania na awarię: w panelu, w sekcji *Wi-Fi*, albo przez API.
Przycisk *Uruchom AP serwisowy* wchodzi w tryb serwisowy na żądanie (przydatne, gdy
wymieniasz router i chcesz przygotować urządzenie zawczasu).

> **Zweryfikowane 2026-08-28 (poza zasięgiem Wi-Fi, telefon):** dozór 60 s zadziałał —
> wyniesienie urządzenia poza zasięg wywołało restart do AP `Swiatlo-XXXX`; panel na
> `http://192.168.4.1/` odpowiedział, `GET /api/wifi/scan` zwrócił sieci posortowane po
> RSSI, zmiana `POST /api/wifi {"ssid":...}` z panelu w AP połączyła z nową siecią
> i wróciła do stacji. Ten sam kod służy wejściu na żądanie `{"setup_mode":true}`.

```bash
# zmiana sieci (urządzenie zrestartuje się i połączy)
curl -u admin:swiatlo -H "Content-Type: application/json" \
     -d '{"ssid":"NowaSiec","password":"nowehaslo"}' http://192.168.8.127/api/wifi

# wejście w tryb serwisowy na żądanie
curl -u admin:swiatlo -H "Content-Type: application/json" \
     -d '{"setup_mode":true}' http://192.168.8.127/api/wifi

# lista widocznych sieci (tylko w trybie serwisowym)
curl -u admin:swiatlo http://192.168.4.1/api/wifi/scan
```

Skanowanie działa tylko w trybie serwisowym — w normalnej pracy stanem stacji zarządza
Matter i skan mógłby zerwać połączenie.

### 4.6 Tryb nocny (zamiast czujnika zmierzchu)

Nie ma czujnika zmierzchu — zamiast mierzyć światło, sterownik pobiera czas z internetu
(SNTP) i liczy wschód/zachód słońca dla podanych współrzędnych. Włączenie
`night_only` sprawia, że **automatyka obecności zapala tylko po zmroku**; sterowanie
ręczne (Matter, panel, przycisk) działa zawsze i o każdej porze.

| Ustawienie | Znaczenie | Domyślnie |
|---|---|---|
| `night_only` | automatyka tylko po zmroku | `false` |
| `sunset_off_min` | offset zachodu w minutach (− = wcześniej) | `-30` |
| `sunrise_off_min` | offset wschodu w minutach (+ = później) | `+30` |
| `lat` / `lon` | współrzędne w stopniach (do obliczeń słońca) | `52.2297` / `21.0122` |
| `tz` | strefa czasowa w formacie POSIX TZ | `CET-1CEST,M3.5.0,M10.5.0/3` |
| `ntp_server` | serwer czasu | `pool.ntp.org` |

Okno nocy to `zachód + sunset_off_min` … `wschód + sunrise_off_min`. Przykład dla
Warszawy 30 lipca: wschód 04:53, zachód 20:31, offsety −30/+30 → automatyka działa
od **20:01** do **05:23**.

Aktualny czas, obliczone godziny i to, czy sterownik uznaje teraz noc, widać w panelu
oraz w `/api/status` w sekcji `time`:

```json
"time": { "synced": true, "local": "2026-07-30 11:12:44", "sunrise": "04:53",
          "sunset": "20:31", "night_from": "20:01", "night_to": "05:23", "is_night": false }
```

Uwagi:

- Strefa czasowa w formacie POSIX ma **odwrócone znaki** (`CET-1` oznacza UTC+1);
  reguły zmiany czasu (`M3.5.0,M10.5.0/3`) są potrzebne, żeby lato/zima liczyło się samo.
- Zmiana `tz`, `lat`, `lon` i offsetów działa od razu; zmiana `ntp_server` po restarcie
  ESP (klient SNTP startuje raz, przy pierwszym adresie IP).
- **Fail-safe:** dopóki zegar nie jest zsynchronizowany, firmware traktuje porę jako noc,
  więc automatyka działa normalnie (lepiej zapalić w dzień niż nie zapalić w nocy).
- Tryb nocny blokuje tylko **zapalanie** przez automatykę. Jeśli świt zastanie
  zapaloną żarówkę, zgaśnie normalnie po utracie obecności i `hold_s`.

### 4.8 Aktualizacja firmware przez panel (OTA)

Sterownik zamknięty w oprawie nie ma dostępu do USB, więc panel przyjmuje nowy firmware
po Wi-Fi. W sekcji *Aktualizacja firmware* wybierz plik
`firmware/build.esp32c3/swiatlo_ld2420.bin` i zatwierdź, albo z konsoli:

```bash
curl -u admin:swiatlo --data-binary @firmware/build.esp32c3/swiatlo_ld2420.bin \
     -H "Content-Type: application/octet-stream" http://192.168.8.127/api/ota
```

Obraz trafia do zapasowej partycji OTA, poprzedni zostaje nietknięty; po udanym zapisie
urządzenie restartuje się do nowej wersji. Jeśli nowa wersja okaże się zła, **trzy szybkie
odcięcia zasilania** przywracają poprzedni obraz (`CONFIG_APP_POWER_CYCLE_ROLLBACK_COUNT`).
Uwaga: dwa odcięcia to nadal wymuszenie ON (§4.4), trzecie robi rollback.

⚠️ To zwykły HTTP z Basic Auth — kto zna hasło panelu i jest w tej sieci, może wgrać
dowolny firmware. Trzymać wyłącznie w zaufanej sieci LAN, nie wystawiać na internet.

Ograniczenie: rollback w bootloaderze nie jest włączony (bootloader zmienia się tylko przy
wgraniu po USB), więc zabezpieczeniem jest wyłącznie powyższy licznik odcięć zasilania
realizowany przez aplikację. Obraz jest weryfikowany sumą kontrolną przed przełączeniem
partycji, więc uszkodzony transfer nie zostanie uruchomiony.

### 4.10 Hasło panelu i kopia ustawień

**Zmiana hasła** — sekcja *Hasło panelu*: podajesz aktualne hasło (potwierdzenie, bo
przeglądarka trzyma Basic Auth w pamięci), login i nowe hasło (min. 4 znaki). Nowe
poświadczenia lądują w NVS, więc **przeżywają aktualizację firmware** i nadpisują to,
co jest w `menuconfig`.

```bash
curl -u admin:swiatlo -H "Content-Type: application/json" \
     -d '{"current":"swiatlo","password":"nowe-haslo","user":"admin"}' \
     http://192.168.1.7/api/password
```

Zapomniane hasło: **cztery szybkie odcięcia zasilania** przywracają login i hasło
wkompilowane w firmware (`CONFIG_APP_POWER_CYCLE_PWRESET_COUNT`). Bez tego jedyną drogą
byłoby rozkręcenie oprawy i wgranie firmware po USB. Cała drabinka odcięć:

| Liczba szybkich odcięć | Efekt |
|---|---|
| 2 | wymuszenie ON z pominięciem automatyki (§4.4) |
| 3 | powrót do poprzedniego firmware (§4.8) |
| 4 | reset hasła panelu do wartości z firmware |

**Kopia ustawień** — sekcja *Kopia ustawień (JSON)*: przycisk pobiera plik
`swiatlo-config-RRRR-MM-DD.json`, drugi wgrywa go z powrotem. Kopia zawiera automatykę,
tryb nocny i kalibrację czujnika (progi `move`/`still` wszystkich 16 bramek, zakres bramek,
timeout modułu). Świadomie **nie** zawiera hasła panelu ani danych Wi-Fi — ma odtwarzać
kalibrację, nie klonować dostępu.

```bash
# kopia
curl -u admin:swiatlo http://192.168.1.7/api/settings > swiatlo-config.json

# odtworzenie
curl -u admin:swiatlo -H "Content-Type: application/json" \
     --data-binary @swiatlo-config.json http://192.168.1.7/api/settings
```

Import stosuje tylko te pola, które są w pliku, i pomija zapisy do czujnika, gdy wartości
się nie zmieniają (pamięć nieulotna modułu ma ograniczoną liczbę cykli). W odpowiedzi
dostajesz `app_fields` i `sensor_writes` — ile pól i ile zapisów faktycznie zastosowano.

### 4.12 Historia wykryć

Sekcja *Historia wykryć* pokazuje ostatnie zdarzenia — po to, żeby rano dało się
sprawdzić, czy ktoś w nocy naprawdę wszedł, czy czujnikowi się „przywidziało”.
Rejestrowane są:

- `presence` — obecność uznana przez automatykę (z odległością celu),
- `presence_end` — koniec obecności (z czasem jej trwania),
- `light_on` / `light_off` — przełączenia lampy wraz ze źródłem (`auto`, `web`, `matter`,
  `button`, `power_cycle`).

```bash
curl -u admin:swiatlo http://192.168.1.7/api/events
```

```json
{"ok":true,"presence_events":1,"uptime_s":300,"events":[
  {"type":"presence_end","local":"2026-09-08 19:18:52","distance_cm":0,"duration_s":6},
  {"type":"presence","local":"2026-09-08 19:18:46","distance_cm":254}]}
```

Bufor ma domyślnie 30 wpisów (`CONFIG_APP_EVENT_LOG_SIZE`, najnowsze pierwsze) i siedzi
w RAM — restart urządzenia czyści historię. Zapisywanie każdego zdarzenia do NVS zużywałoby
pamięć nieulotną bez wyraźnego zysku, a przy zaniku zasilania i tak byłoby niepełne.
Wpisy sprzed synchronizacji zegara nie mają czasu zegarowego, tylko `uptime_s` — panel
pokazuje je wtedy jako „N s temu”.

### 4.13 Kalibracja czujnika

**Skąd bierze się obecność.** LD2420 wystawia w ramce flagę obecności i odległość celu.
Na wielu egzemplarzach ta flaga siedzi na stałe na `1`, bo reaguje też na ściany i meble
(sprawdzone na tym module — patrz `sensor.raw_presence` w `/api/status`). Dlatego
domyślnie obecność wyliczana jest **z odległości**, a flaga jest ignorowana:

| `presence_src` | Znaczenie |
|---|---|
| `distance` (domyślne) | obecność = cel w oknie `min_cm`…`max_cm`; wymaga `max_cm > 0` (przy `0` firmware wraca do flagi) |
| `and` | flaga modułu **oraz** okno odległości |
| `flag` | tylko flaga modułu (dla egzemplarzy, na których działa) |

**Histereza** (`hyst_cm`, domyślnie 30 cm) zapobiega migotaniu na granicy okna: obecność
włącza się przy `max_cm`, a wyłącza dopiero powyżej `max_cm + hyst_cm`. Przykład dla
`max_cm=50`, `hyst_cm=30`: wejście przy ≤ 50 cm, wyjście przy > 80 cm.

**Czas gaszenia** to suma dwóch opóźnień: `timeout` modułu (fabrycznie 120 s, tu ustawione
30 s — widoczne w panelu) oraz `hold_s` naszej automatyki. Przy trybie `distance` liczy się
praktycznie tylko `hold_s`, bo decyduje odległość, a nie flaga z timeoutem modułu.

Kolejność regulacji:

1. `max_gate` ustawić tak, by ściana/korytarz nie łapały (bramka ≈ 0,7 m).
2. Dla bramek dających fałszywe wyzwolenia podnieść progi `ruch`/`spoczynek`
   (energie bieżące widać w tabeli w panelu).
3. Okno `min_cm` / `max_cm` + `hyst_cm` dobrać do miejsca montażu.
4. Powrót do wartości fabrycznych modułu: `{"action":"factory_reset"}`.

---

## 5. Struktura repo

```
firmware/            projekt ESP-IDF + esp-matter (main/: Matter, przekaźnik, LD2420, panel HTTP)
scripts/             build.sh (WSL), build-win.bat, flash-win.bat, monitor*.py
zdjecia_referencyjne/ zdjęcia użytego sprzętu
AGENTS.md            notatki techniczne, stan weryfikacji, plan prac
```
