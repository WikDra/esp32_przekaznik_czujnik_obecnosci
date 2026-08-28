/*
 * Awaryjna konfiguracja Wi-Fi.
 *
 * W oprawie nie ma dostępu do USB, więc urządzenie musi samo wyjść z sytuacji
 * "zmieniło się hasło do Wi-Fi". Mechanizm:
 *
 *  1. Tryb normalny: po starcie liczymy czas do uzyskania adresu IP. Jeśli w ciągu
 *     CONFIG_APP_WIFI_FALLBACK_S sekund go nie ma, ustawiamy flagę w NVS i restartujemy.
 *  2. Tryb serwisowy (flaga ustawiona): Matter **nie** startuje, za to podnosimy własny
 *     SoftAP (Swiatlo-XXXX) i ten sam panel HTTP pod http://192.168.4.1/ . Flaga jest
 *     czyszczona od razu przy wejściu, żeby zanik zasilania nie zostawił urządzenia
 *     w trybie AP na stałe.
 *  3. Po zapisaniu nowych danych z panelu (albo po CONFIG_APP_PROV_TIMEOUT_MIN minutach)
 *     urządzenie restartuje się do trybu normalnego.
 *
 * Stos Matter przy wyłączonym CHIP_DEVICE_CONFIG_ENABLE_WIFI_AP wymusza WIFI_MODE_STA,
 * dlatego AP i Matter nigdy nie działają jednocześnie.
 */
#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APP_WIFI_SCAN_MAX 12

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool open_network;
} app_wifi_ap_info_t;

/* Czy w NVS jest ustawiona flaga trybu serwisowego (czytane raz, na starcie). */
bool app_wifi_prov_requested(void);

/* Ustawia/czyści flagę trybu serwisowego. */
esp_err_t app_wifi_set_prov_flag(bool enabled);

/* Tryb serwisowy: inicjalizuje Wi-Fi w trybie AP+STA i startuje SoftAP.
 * Wołane zamiast esp_matter::start(). */
esp_err_t app_wifi_prov_start(void);

/* Czy pracujemy teraz w trybie serwisowym. */
bool app_wifi_is_prov_mode(void);

/* SSID rozgłaszanego AP (pusty łańcuch w trybie normalnym). */
const char *app_wifi_ap_ssid(void);

/* Tryb normalny: uruchamia dozór połączenia (restart do trybu serwisowego). */
void app_wifi_watchdog_start(void);

/* Anuluje dozór - wołane po uzyskaniu adresu IP. */
void app_wifi_notify_got_ip(void);

/* Zapisuje dane stacji Wi-Fi (trwale, przez esp_wifi_set_config). */
esp_err_t app_wifi_set_credentials(const char *ssid, const char *password);

/* SSID, z którym urządzenie próbuje się łączyć (z konfiguracji sterownika Wi-Fi). */
void app_wifi_current_ssid(char *out, size_t len);

/* Skan sieci (działa w trybie serwisowym; w trybie normalnym zwraca ESP_ERR_INVALID_STATE,
 * bo skanowanie kolidowałoby ze stanem stacji zarządzanym przez Matter). */
esp_err_t app_wifi_scan(app_wifi_ap_info_t *out, size_t max, size_t *count);

/* Restart urządzenia po `delay_ms` (używane po zapisaniu danych Wi-Fi). */
void app_wifi_schedule_reboot(uint32_t delay_ms);

#ifdef __cplusplus
}
#endif
