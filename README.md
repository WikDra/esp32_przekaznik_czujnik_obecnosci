# Sterownik żarówki: ESP32-C3 SuperMini + przekaźnik + LD2420

Sterownik oświetlenia 230 V z czujnikiem obecności mmWave, sterowany **jednocześnie**
przez **Matter** (Apple Home / Google Home / Home Assistant / SmartThings) i przez
**lokalny panel HTTP** (regulacja progów czujnika, czasu podtrzymania, tryb automatyki).

- Automatyka: obecność zapala żarówkę, po utracie obecności gaśnie z opóźnieniem `hold_s`.
- Panel HTTP działa od pierwszego bootu (dane Wi-Fi z `firmware/sdkconfig.local`),
  nie tylko po sparowaniu Matter.
- Awaryjne wymuszenie ON: **dwa szybkie odcięcia zasilania** (bez sieci i bez apki).
- Przycisk BOOT: krótkie wciśnięcie = przełącz światło, ≥5 s = factory reset Matter.

Notatki techniczne, stan weryfikacji i plan rozwoju: [`AGENTS.md`](AGENTS.md).

---

## 1. Elementy

| Element | Uwagi |
|---|---|
| ESP32-C3 SuperMini (TENSTAR ROBOT) | 4 MB flash, USB-C = natywny USB (USB-Serial/JTAG) |
| Przekaźnik 1-kanałowy 5 V (HW-307 / JQC3F-05VDC-C) | `IN / GND / VCC`, wyzwalanie **stanem niskim**, styki 10 A 250 VAC |
| HLK-LD2420 v2.1 (radar 24 GHz) | pady `3V3 / GND / OT1 / RX / OT2`, logika 3,3 V, UART 115200 8N1 |
| Konwerter poziomów 4CH (BSS138) | `LV / HV / GND` + kanały `LV1..LV4` / `HV1..HV4` |
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

- Cewka przekaźnika: `VCC` → **5 V**, `GND` → masa. Pobiera ~70–80 mA, **nie zasilać z 3,3 V**.
- Poziomy: GPIO10 nisko = przekaźnik załączony (żarówka świeci), wysoko = rozwarty.

**Dlaczego konwerter jest tu potrzebny:** moduły wyzwalane stanem niskim mają zwykle
tranzystor PNP z emiterem na +5 V. Stan wysoki 3,3 V daje U_BE ≈ 1,7 V — tranzystor
dalej przewodzi i przekaźnik **nie wyłączy się**. Konwerter (open-drain + pull-up do 5 V)
podaje na `IN` pełne 5 V. Bonus: podczas bootu/resetu pin ESP jest wejściem, a pull-up
konwertera trzyma `IN` wysoko, więc przekaźnik jest rozwarty i żarówka zgaszona.

Jeśli zmierzysz, że twój egzemplarz modułu poprawnie wyłącza się przy 3,3 V, konwerter
można pominąć — ale to trzeba **zmierzyć**, nie założyć.

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
| POST | `/api/light` | `{"on":true}` / `{"toggle":true}` | sterowanie żarówką |
| POST | `/api/config` | `{"auto_mode":true,"hold_s":60,"max_cm":400,"min_cm":0,"restore_state":false}` | ustawienia aplikacji (NVS) |
| POST | `/api/sensor` | `{"min_gate":1,"max_gate":6,"timeout_s":30}` | zakres i timeout modułu |
| POST | `/api/sensor` | `{"gate":3,"move":250,"still":200}` | progi jednej bramki |
| POST | `/api/sensor` | `{"mode":"energy"\|"simple"}` | tryb wyjścia modułu |
| POST | `/api/sensor` | `{"action":"refresh"\|"restart"\|"factory_reset"}` | operacje na module |
| POST | `/api/reboot` | `{}` | restart ESP32 |

```bash
curl -u admin:swiatlo http://192.168.8.120/api/status
curl -u admin:swiatlo -H "Content-Type: application/json" \
     -d '{"toggle":true}' http://192.168.8.120/api/light
```

### 4.3 Matter

Kody parowania są drukowane w logu na starcie (dopóki urządzenie nie jest sparowane):

```
Setup QRCode:        MT:Y.K9042C00KA0648G00
Manual pairing code: 34970112332
```

Sparuj telefonem (Apple Home / Google Home) albo Home Assistantem w tej samej sieci.
Urządzenie zgłasza dwa endpointy: **1 = On/Off Light** (przekaźnik), **2 = Occupancy**
(obecność z LD2420). Zmiana z apki i z panelu jest natychmiast widoczna po obu stronach.

Firmware używa **testowych certyfikatów DAC** — do użytku domowego OK, do certyfikacji
Matter potrzebne własne certyfikaty w partycji `fctry`.

### 4.4 Awaryjne wymuszenie ON

Dwa szybkie odcięcia zasilania (domyślnie 2 cykle w oknie 10 s) zapalają żarówkę
z pominięciem automatyki — dopóki nie wyłączysz jej jawnie z Mattera, panelu lub
przycisku. Stan widoczny jako `force_on` w `/api/status`. Zwykły reset płytki liczy się
tak samo jak odcięcie prądu. Opcje: `CONFIG_APP_POWER_CYCLE_*`.

### 4.5 Kalibracja czujnika

1. `max_gate` ustawić tak, by ściana/korytarz nie łapały (bramka ≈ 0,7 m).
2. Dla bramek dających fałszywe wyzwolenia podnieść progi `ruch`/`spoczynek`
   (energie bieżące widać w tabeli w panelu).
3. Dodatkowy filtr aplikacyjny: `max_cm` / `min_cm` w `/api/config`.
4. Powrót do wartości fabrycznych modułu: `{"action":"factory_reset"}`.

---

## 5. Struktura repo

```
firmware/            projekt ESP-IDF + esp-matter (main/: Matter, przekaźnik, LD2420, panel HTTP)
scripts/             build.sh (WSL), build-win.bat, flash-win.bat, monitor*.py
zdjecia_referencyjne/ zdjęcia użytego sprzętu
AGENTS.md            notatki techniczne, stan weryfikacji, plan prac
```
