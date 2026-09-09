#include "app_wifi.h"
#include "app_priv.h"
#include "app_sun.h"

#include <stdio.h>
#include <string.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <nvs.h>
#include <sdkconfig.h>

static const char *TAG = "app_wifi";
static const char *NVS_NS = "swiatlo";
static const char *NVS_KEY_PROV = "prov";

static bool s_prov_mode;
static char s_ap_ssid[33];
static esp_timer_handle_t s_watchdog;

/* ------------------------------------------------------------ flaga trybu serwisowego */

bool app_wifi_prov_requested(void)
{
    nvs_handle_t h;
    uint8_t value = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, NVS_KEY_PROV, &value) != ESP_OK) {
            value = 0;
        }
        nvs_close(h);
    }
    return value != 0;
}

esp_err_t app_wifi_set_prov_flag(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_u8(h, NVS_KEY_PROV, enabled ? 1 : 0);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

bool app_wifi_is_prov_mode(void) { return s_prov_mode; }
const char *app_wifi_ap_ssid(void) { return s_ap_ssid; }

/* ------------------------------------------------------------ restart */

static void reboot_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "restarting");
    esp_restart();
}

void app_wifi_schedule_reboot(uint32_t delay_ms)
{
    esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {
        .callback = reboot_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "reboot",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, (uint64_t)delay_ms * 1000);
    }
}

/* ------------------------------------------------------------ dozór połączenia */

static void watchdog_cb(void *arg)
{
    (void)arg;
    /* Jeśli Wi-Fi podnosiliśmy sami (Matter wyłączony w panelu), to najpierw wracamy
     * na sprawdzoną ścieżkę z Matterem - awaria jest wtedy najprawdopodobniej w naszym
     * kodzie stacji, a nie w danych logowania. Dopiero gdy i to nie da adresu, następny
     * dozór wprowadzi urządzenie w tryb serwisowy z SoftAP. */
    if (!app_settings_matter_enabled()) {
        ESP_LOGW(TAG, "no IP address within %d s and Matter is disabled - re-enabling Matter and rebooting",
                 CONFIG_APP_WIFI_FALLBACK_S);
        app_settings_set_matter_enabled(true);
        esp_restart();
        return;
    }

    ESP_LOGW(TAG, "no IP address within %d s - rebooting into Wi-Fi setup mode (SoftAP)",
             CONFIG_APP_WIFI_FALLBACK_S);
    app_wifi_set_prov_flag(true);
    esp_restart();
}

void app_wifi_watchdog_start(void)
{
#if CONFIG_APP_WIFI_FALLBACK_S > 0
    if (s_watchdog) {
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = watchdog_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_wd",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_watchdog) == ESP_OK) {
        esp_timer_start_once(s_watchdog, (uint64_t)CONFIG_APP_WIFI_FALLBACK_S * 1000000ULL);
        ESP_LOGI(TAG, "connection watchdog armed (%d s)", CONFIG_APP_WIFI_FALLBACK_S);
    }
#endif
}

void app_wifi_notify_got_ip(void)
{
    if (s_watchdog) {
        esp_timer_stop(s_watchdog);
        ESP_LOGI(TAG, "connection watchdog disarmed");
    }
}

/* ------------------------------------------------------------ dane stacji */

esp_err_t app_wifi_set_credentials(const char *ssid, const char *password)
{
    if (!ssid || ssid[0] == '\0' || strlen(ssid) > 32) {
        return ESP_ERR_INVALID_ARG;
    }
    if (password && strlen(password) > 63) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    if (password) {
        strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    }
    cfg.sta.threshold.authmode = (password && password[0]) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;

    /* Sterownik Wi-Fi ma domyślnie WIFI_STORAGE_FLASH, więc zapis jest trwały i po
     * restarcie chip zobaczy stację jako skonfigurowaną (ESP32Utils::IsStationProvisioned). */
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_config failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGW(TAG, "new Wi-Fi credentials stored (ssid '%s')", ssid);
    return ESP_OK;
}

void app_wifi_current_ssid(char *out, size_t len)
{
    if (!out || len == 0) {
        return;
    }
    out[0] = '\0';
    wifi_config_t cfg;
    if (esp_wifi_get_config(WIFI_IF_STA, &cfg) == ESP_OK) {
        strlcpy(out, (const char *)cfg.sta.ssid, len);
    }
}

esp_err_t app_wifi_scan(app_wifi_ap_info_t *out, size_t max, size_t *count)
{
    if (!out || !count || max == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;
    if (!s_prov_mode) {
        /* W trybie normalnym stanem stacji zarządza Matter - skan mógłby zerwać
         * połączenie, więc nie ruszamy. */
        return ESP_ERR_INVALID_STATE;
    }

    wifi_scan_config_t scan = {};
    scan.show_hidden = false;
    esp_err_t err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) {
        return err;
    }

    uint16_t found = 0;
    esp_wifi_scan_get_ap_num(&found);
    if (found == 0) {
        return ESP_OK;
    }
    if (found > APP_WIFI_SCAN_MAX) {
        found = APP_WIFI_SCAN_MAX;
    }

    wifi_ap_record_t records[APP_WIFI_SCAN_MAX];
    err = esp_wifi_scan_get_ap_records(&found, records);
    if (err != ESP_OK) {
        return err;
    }

    size_t n = 0;
    for (uint16_t i = 0; i < found && n < max; i++) {
        if (records[i].ssid[0] == '\0') {
            continue;
        }
        strlcpy(out[n].ssid, (const char *)records[i].ssid, sizeof(out[n].ssid));
        out[n].rssi = records[i].rssi;
        out[n].open_network = records[i].authmode == WIFI_AUTH_OPEN;
        n++;
    }
    *count = n;
    return ESP_OK;
}

/* ------------------------------------------------------------ stacja bez Mattera */

static esp_timer_handle_t s_reconnect_timer;

static void reconnect_cb(void *arg)
{
    (void)arg;
    esp_wifi_connect();
}

static void sta_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        app_net_connected = false;
        /* Ponawiamy z opóźnieniem, żeby przy wyłączonym routerze nie kręcić pętli. */
        if (s_reconnect_timer) {
            esp_timer_stop(s_reconnect_timer);
            esp_timer_start_once(s_reconnect_timer, 2 * 1000 * 1000);
        }
        return;
    }
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "sta ip: " IPSTR, IP2STR(&event->ip_info.ip));
        app_net_connected = true;
        app_wifi_notify_got_ip();
        app_web_start();
        app_sun_start();
    }
}

esp_err_t app_wifi_sta_start(void)
{
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        return err;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                       sta_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                       sta_event_handler, NULL, NULL));

    const esp_timer_create_args_t args = {
        .callback = reconnect_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "wifi_retry",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&args, &s_reconnect_timer);

    /* Dane logowania siedzą w NVS sterownika Wi-Fi (WIFI_STORAGE_FLASH), tam gdzie
     * zapisał je Matter albo panel. Gdy ich nie ma, sięgamy po wartości z kompilacji. */
    wifi_config_t sta;
    memset(&sta, 0, sizeof(sta));
    if (esp_wifi_get_config(WIFI_IF_STA, &sta) != ESP_OK || sta.sta.ssid[0] == '\0') {
#if defined(CONFIG_DEFAULT_WIFI_SSID)
        if (strlen(CONFIG_DEFAULT_WIFI_SSID) > 0) {
            strlcpy((char *)sta.sta.ssid, CONFIG_DEFAULT_WIFI_SSID, sizeof(sta.sta.ssid));
            strlcpy((char *)sta.sta.password, CONFIG_DEFAULT_WIFI_PASSWORD, sizeof(sta.sta.password));
            esp_wifi_set_config(WIFI_IF_STA, &sta);
            ESP_LOGI(TAG, "using Wi-Fi credentials compiled into the firmware");
        } else
#endif
        {
            ESP_LOGW(TAG, "no Wi-Fi credentials stored - the connection watchdog will start setup mode");
        }
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "station start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "Matter disabled - running as plain Wi-Fi station with the HTTP panel only");    return ESP_OK;
}

/* ------------------------------------------------------------ tryb serwisowy */

#if CONFIG_APP_PROV_TIMEOUT_MIN > 0
static void prov_timeout_cb(void *arg)
{
    (void)arg;
    ESP_LOGW(TAG, "setup mode timeout (%d min) - rebooting to retry the configured network",
             CONFIG_APP_PROV_TIMEOUT_MIN);
    esp_restart();
}
#endif

esp_err_t app_wifi_prov_start(void)
{
    s_prov_mode = true;

    /* Flagę czyścimy od razu: gdyby zabrakło prądu w trybie serwisowym, urządzenie
     * wróci do normalnej pracy, a dozór połączenia i tak wprowadzi je tu ponownie. */
    app_wifi_set_prov_flag(false);

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK) {
        return err;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta(); /* potrzebne do skanowania sieci */

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "%s-%02X%02X", CONFIG_APP_PROV_AP_PREFIX, mac[4], mac[5]);

    wifi_config_t ap_cfg;
    memset(&ap_cfg, 0, sizeof(ap_cfg));
    strlcpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid));
    ap_cfg.ap.ssid_len = strlen(s_ap_ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 2;
    ap_cfg.ap.beacon_interval = 100;
    const char *ap_pass = CONFIG_APP_PROV_AP_PASSWORD;
    if (strlen(ap_pass) >= 8) {
        strlcpy((char *)ap_cfg.ap.password, ap_pass, sizeof(ap_cfg.ap.password));
        ap_cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_cfg.ap.authmode = WIFI_AUTH_OPEN; /* hasło krótsze niż 8 znaków = sieć otwarta */
    }

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SoftAP start failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGW(TAG, "Wi-Fi setup mode: SSID '%s' (%s), panel at http://192.168.4.1/", s_ap_ssid,
             ap_cfg.ap.authmode == WIFI_AUTH_OPEN ? "open" : "WPA2");

#if CONFIG_APP_PROV_TIMEOUT_MIN > 0
    esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {
        .callback = prov_timeout_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "prov_to",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, (uint64_t)CONFIG_APP_PROV_TIMEOUT_MIN * 60ULL * 1000000ULL);
    }
#endif
    return ESP_OK;
}
