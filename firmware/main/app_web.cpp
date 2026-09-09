/*
 * Local HTTP panel (REST + single page UI).
 *
 * SECURITY NOTE: this server switches mains voltage. HTTP Basic authentication is
 * enabled by default (CONFIG_APP_WEB_AUTH) but the traffic itself is plain HTTP, so
 * it only protects against casual access inside a trusted LAN. Do not expose it to
 * the internet.
 */
#include "app_priv.h"
#include "app_events.h"
#include "app_sun.h"
#include "app_wifi.h"
#include "ld2420.h"

#include <esp_app_desc.h>
#include <esp_err.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <sdkconfig.h>
#include <string.h>
#include <time.h>

#include <cJSON.h>
#include <mbedtls/base64.h>

static const char *TAG = "web";
static httpd_handle_t s_server = NULL;

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

/* ------------------------------------------------------------ helpers */

static esp_err_t send_json(httpd_req_t *req, cJSON *root, int status_code)
{
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    if (status_code == 400) {
        httpd_resp_set_status(req, "400 Bad Request");
    } else if (status_code == 500) {
        httpd_resp_set_status(req, "500 Internal Server Error");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, int code, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", false);
    cJSON_AddStringToObject(root, "error", msg);
    return send_json(req, root, code);
}

#if CONFIG_APP_WEB_AUTH
/* Returns true when the request carries valid credentials, otherwise answers 401. */
static bool check_auth(httpd_req_t *req)
{
    static char expected[160];
    static uint32_t expected_generation = UINT32_MAX;

    /* Poświadczenia mogą się zmienić w trakcie pracy (POST /api/password), dlatego
     * cache jest wiązany z licznikiem zmian z app_settings. */
    if (expected[0] == '\0' || expected_generation != app_settings_web_generation()) {
        char creds[96];
        int n = snprintf(creds, sizeof(creds), "%s:%s", app_settings_web_user(),
                         app_settings_web_pass());
        size_t olen = 0;
        unsigned char b64[128];
        if (mbedtls_base64_encode(b64, sizeof(b64), &olen, (const unsigned char *)creds, n) != 0) {
            ESP_LOGE(TAG, "failed to build credentials");
            return false;
        }
        snprintf(expected, sizeof(expected), "Basic %.*s", (int)olen, (const char *)b64);
        expected_generation = app_settings_web_generation();
    }

    char provided[160];
    if (httpd_req_get_hdr_value_str(req, "Authorization", provided, sizeof(provided)) == ESP_OK) {
        if (strcmp(provided, expected) == 0) {
            return true;
        }
    }

    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"swiatlo\"");
    httpd_resp_send(req, "Unauthorized", HTTPD_RESP_USE_STRLEN);
    return false;
}
#else
static bool check_auth(httpd_req_t *req)
{
    (void)req;
    return true;
}
#endif

/* Reads the whole request body (max 2 kB) and parses it as JSON. */
static cJSON *read_json_body(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > 2048) {
        return NULL;
    }
    char *buf = (char *)malloc(req->content_len + 1);
    if (!buf) {
        return NULL;
    }
    int received = 0;
    while (received < (int)req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            free(buf);
            return NULL;
        }
        received += r;
    }
    buf[received] = '\0';
    cJSON *json = cJSON_Parse(buf);
    free(buf);
    return json;
}

static bool json_get_bool(const cJSON *root, const char *key, bool *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(item)) {
        *out = cJSON_IsTrue(item);
        return true;
    }
    if (cJSON_IsNumber(item)) {
        *out = item->valuedouble != 0;
        return true;
    }
    return false;
}

static bool json_get_uint(const cJSON *root, const char *key, uint32_t max, uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsNumber(item) || item->valuedouble < 0 || item->valuedouble > max) {
        return false;
    }
    *out = (uint32_t)item->valuedouble;
    return true;
}

static void get_ip_str(char *out, size_t len)
{
    snprintf(out, len, "0.0.0.0");
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (!netif) {
        netif = esp_netif_next_unsafe(NULL);
    }
    esp_netif_ip_info_t ip;
    if (netif && esp_netif_get_ip_info(netif, &ip) == ESP_OK) {
        snprintf(out, len, IPSTR, IP2STR(&ip.ip));
    }
}

/* ------------------------------------------------------------ handlers */

static esp_err_t root_get_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }

    ld2420_state_t st;
    ld2420_get_state(&st);
    app_settings_t *cfg = app_settings();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "on", app_light_get());
    cJSON_AddStringToObject(root, "source", app_light_last_src());
    cJSON_AddBoolToObject(root, "presence", app_light_presence());
    cJSON_AddNumberToObject(root, "distance_cm", app_light_distance_cm());
    cJSON_AddNumberToObject(root, "auto_off_in", app_light_auto_off_in());
    cJSON_AddBoolToObject(root, "force_on", app_light_force_on());
    cJSON_AddBoolToObject(root, "auto_pending", app_light_auto_pending());

    cJSON *config = cJSON_AddObjectToObject(root, "config");
    cJSON_AddBoolToObject(config, "auto_mode", cfg->auto_mode);
    cJSON_AddNumberToObject(config, "hold_s", cfg->hold_s);
    cJSON_AddNumberToObject(config, "max_cm", cfg->max_cm);
    cJSON_AddNumberToObject(config, "min_cm", cfg->min_cm);
    cJSON_AddNumberToObject(config, "hyst_cm", cfg->hyst_cm);
    cJSON_AddNumberToObject(config, "blank_ms", cfg->blank_ms);
    cJSON_AddNumberToObject(config, "on_delay_ms", cfg->on_delay_ms);
    cJSON_AddNumberToObject(config, "presence_src", cfg->presence_src);
    cJSON_AddStringToObject(config, "presence_src_name",
                            cfg->presence_src == PRESENCE_SRC_DISTANCE ? "distance"
                            : cfg->presence_src == PRESENCE_SRC_FLAG   ? "flag"
                                                                      : "and");
    cJSON_AddBoolToObject(config, "restore_state", cfg->restore_state);
    cJSON_AddBoolToObject(config, "night_only", cfg->night_only);
    cJSON_AddNumberToObject(config, "sunset_off_min", cfg->sunset_off_min);
    cJSON_AddNumberToObject(config, "sunrise_off_min", cfg->sunrise_off_min);
    cJSON_AddNumberToObject(config, "lat", (double)cfg->lat_udeg / 1000000.0);
    cJSON_AddNumberToObject(config, "lon", (double)cfg->lon_udeg / 1000000.0);
    cJSON_AddStringToObject(config, "tz", cfg->tz);
    cJSON_AddStringToObject(config, "ntp_server", cfg->ntp_server);

    app_sun_state_t sun;
    app_sun_get(&sun);
    cJSON *time_obj = cJSON_AddObjectToObject(root, "time");
    cJSON_AddBoolToObject(time_obj, "synced", sun.time_valid);
    cJSON_AddBoolToObject(time_obj, "is_night", sun.is_night);
    cJSON_AddBoolToObject(time_obj, "polar_day", sun.polar_day);
    cJSON_AddBoolToObject(time_obj, "polar_night", sun.polar_night);
    if (sun.time_valid) {
        char buf[32];
        struct tm tm_local;
        localtime_r(&sun.now, &tm_local);
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_local);
        cJSON_AddStringToObject(time_obj, "local", buf);
        if (!sun.polar_day && !sun.polar_night) {
            localtime_r(&sun.sunrise, &tm_local);
            strftime(buf, sizeof(buf), "%H:%M", &tm_local);
            cJSON_AddStringToObject(time_obj, "sunrise", buf);
            localtime_r(&sun.sunset, &tm_local);
            strftime(buf, sizeof(buf), "%H:%M", &tm_local);
            cJSON_AddStringToObject(time_obj, "sunset", buf);
            localtime_r(&sun.night_to, &tm_local);
            strftime(buf, sizeof(buf), "%H:%M", &tm_local);
            cJSON_AddStringToObject(time_obj, "night_to", buf);
            localtime_r(&sun.night_from, &tm_local);
            strftime(buf, sizeof(buf), "%H:%M", &tm_local);
            cJSON_AddStringToObject(time_obj, "night_from", buf);
        }
    }

    cJSON *sensor = cJSON_AddObjectToObject(root, "sensor");
    cJSON_AddBoolToObject(sensor, "link_ok", st.link_ok);
    cJSON_AddStringToObject(sensor, "fw", st.fw);
    cJSON_AddStringToObject(sensor, "mode", st.mode == LD2420_MODE_ENERGY ? "energy" : "simple");
    cJSON_AddBoolToObject(sensor, "config_valid", st.config_valid);
    cJSON_AddNumberToObject(sensor, "restored_writes", st.restored_writes);
    cJSON_AddBoolToObject(sensor, "config_remembered", app_settings_sensor()->valid);
    cJSON_AddNumberToObject(sensor, "min_gate", st.min_gate);
    cJSON_AddNumberToObject(sensor, "max_gate", st.max_gate);
    cJSON_AddNumberToObject(sensor, "timeout_s", st.timeout_s);
    cJSON_AddNumberToObject(sensor, "raw_presence", st.presence);
    cJSON_AddNumberToObject(sensor, "raw_distance_cm", st.distance_cm);

    cJSON *gates = cJSON_AddArrayToObject(sensor, "gates");
    for (int i = 0; i < LD2420_GATES; i++) {
        cJSON *gate = cJSON_CreateObject();
        cJSON_AddNumberToObject(gate, "gate", i);
        cJSON_AddNumberToObject(gate, "energy", st.gate_energy[i]);
        cJSON_AddNumberToObject(gate, "move", st.move_thresh[i]);
        cJSON_AddNumberToObject(gate, "still", st.still_thresh[i]);
        cJSON_AddItemToArray(gates, gate);
    }

    char ip[16];
    get_ip_str(ip, sizeof(ip));
    cJSON *sys = cJSON_AddObjectToObject(root, "system");
    cJSON_AddStringToObject(sys, "ip", ip);
    cJSON_AddBoolToObject(sys, "commissioned", app_matter_commissioned);
    cJSON_AddBoolToObject(sys, "matter_enabled", app_settings_matter_enabled());
    cJSON_AddNumberToObject(sys, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(sys, "uptime_s", (double)(esp_timer_get_time() / 1000000));

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        cJSON_AddStringToObject(sys, "partition", running->label);
    }
    const esp_app_desc_t *app = esp_app_get_description();
    if (app) {
        cJSON_AddStringToObject(sys, "app_version", app->version);
        cJSON_AddStringToObject(sys, "app_built", app->date);
        cJSON_AddStringToObject(sys, "app_time", app->time);
    }
    /* Sam login (nigdy hasło) - panel podpowiada go w formularzu zmiany hasła. */
    cJSON_AddStringToObject(sys, "web_user", app_settings_web_user());

    char sta_ssid[33];
    app_wifi_current_ssid(sta_ssid, sizeof(sta_ssid));
    cJSON *wifi = cJSON_AddObjectToObject(root, "wifi");
    cJSON_AddStringToObject(wifi, "mode", app_wifi_is_prov_mode() ? "setup_ap" : "station");
    cJSON_AddStringToObject(wifi, "ssid", sta_ssid);
    cJSON_AddStringToObject(wifi, "ap_ssid", app_wifi_ap_ssid());
    cJSON_AddBoolToObject(wifi, "connected", app_net_connected);

    return send_json(req, root, 200);
}

static esp_err_t light_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_error(req, 400, "invalid json body");
    }

    bool on = false;
    bool toggle = false;
    if (json_get_bool(body, "toggle", &toggle) && toggle) {
        on = !app_light_get();
    } else if (!json_get_bool(body, "on", &on)) {
        cJSON_Delete(body);
        return send_error(req, 400, "expected {\"on\":true|false} or {\"toggle\":true}");
    }
    cJSON_Delete(body);

    app_light_set(on, LIGHT_SRC_WEB);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "on", app_light_get());
    return send_json(req, root, 200);
}

/* Stosuje pola ustawień aplikacji z obiektu JSON. Zwraca liczbę rozpoznanych pól,
 * albo -1 przy nieprawidłowej wartości. Używane przez /api/config i import z /api/settings,
 * żeby obie ścieżki nie rozjechały się w walidacji. */
static int apply_app_config(const cJSON *src, bool *tz_changed)
{
    app_settings_t *cfg = app_settings();
    int applied = 0;
    bool b;
    uint32_t u;

    if (tz_changed) {
        *tz_changed = false;
    }

    if (json_get_bool(src, "auto_mode", &b)) {
        cfg->auto_mode = b;
        applied++;
    }
    if (json_get_bool(src, "restore_state", &b)) {
        cfg->restore_state = b;
        applied++;
    }
    if (json_get_bool(src, "night_only", &b)) {
        cfg->night_only = b;
        applied++;
    }

    const cJSON *item = cJSON_GetObjectItemCaseSensitive(src, "sunset_off_min");
    if (cJSON_IsNumber(item) && item->valuedouble >= -180 && item->valuedouble <= 180) {
        cfg->sunset_off_min = (int16_t)item->valuedouble;
        applied++;
    }
    item = cJSON_GetObjectItemCaseSensitive(src, "sunrise_off_min");
    if (cJSON_IsNumber(item) && item->valuedouble >= -180 && item->valuedouble <= 180) {
        cfg->sunrise_off_min = (int16_t)item->valuedouble;
        applied++;
    }
    item = cJSON_GetObjectItemCaseSensitive(src, "lat");
    if (cJSON_IsNumber(item) && item->valuedouble >= -90.0 && item->valuedouble <= 90.0) {
        cfg->lat_udeg = (int32_t)(item->valuedouble * 1000000.0);
        applied++;
    }
    item = cJSON_GetObjectItemCaseSensitive(src, "lon");
    if (cJSON_IsNumber(item) && item->valuedouble >= -180.0 && item->valuedouble <= 180.0) {
        cfg->lon_udeg = (int32_t)(item->valuedouble * 1000000.0);
        applied++;
    }
    item = cJSON_GetObjectItemCaseSensitive(src, "tz");
    if (cJSON_IsString(item) && item->valuestring && strlen(item->valuestring) < sizeof(cfg->tz)) {
        strlcpy(cfg->tz, item->valuestring, sizeof(cfg->tz));
        applied++;
        if (tz_changed) {
            *tz_changed = true;
        }
    }
    item = cJSON_GetObjectItemCaseSensitive(src, "ntp_server");
    if (cJSON_IsString(item) && item->valuestring &&
        strlen(item->valuestring) < sizeof(cfg->ntp_server)) {
        /* Zmiana serwera SNTP wymaga restartu ESP - klient startuje raz, przy pierwszym IP. */
        strlcpy(cfg->ntp_server, item->valuestring, sizeof(cfg->ntp_server));
        applied++;
    }
    if (json_get_uint(src, "hold_s", 3600, &u)) {
        cfg->hold_s = (uint16_t)u;
        applied++;
    }
    if (json_get_uint(src, "max_cm", 1000, &u)) {
        cfg->max_cm = (uint16_t)u;
        applied++;
    }
    if (json_get_uint(src, "min_cm", 1000, &u)) {
        cfg->min_cm = (uint16_t)u;
        applied++;
    }
    if (json_get_uint(src, "hyst_cm", 300, &u)) {
        cfg->hyst_cm = (uint16_t)u;
        applied++;
    }
    if (json_get_uint(src, "blank_ms", 10000, &u)) {
        cfg->blank_ms = (uint16_t)u;
        applied++;
    }
    if (json_get_uint(src, "on_delay_ms", 5000, &u)) {
        cfg->on_delay_ms = (uint16_t)u;
        applied++;
    }

    const cJSON *psrc = cJSON_GetObjectItemCaseSensitive(src, "presence_src");
    if (cJSON_IsString(psrc) && psrc->valuestring) {
        if (strcmp(psrc->valuestring, "distance") == 0) {
            cfg->presence_src = PRESENCE_SRC_DISTANCE;
        } else if (strcmp(psrc->valuestring, "flag") == 0) {
            cfg->presence_src = PRESENCE_SRC_FLAG;
        } else if (strcmp(psrc->valuestring, "and") == 0) {
            cfg->presence_src = PRESENCE_SRC_AND;
        } else {
            return -1;
        }
        applied++;
    } else if (json_get_uint(src, "presence_src", PRESENCE_SRC_FLAG, &u)) {
        cfg->presence_src = (uint8_t)u;
        applied++;
    }
    return applied;
}

/* Application-side settings (automation window, hold time, ...). */
static esp_err_t config_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_error(req, 400, "invalid json body");
    }

    app_settings_t *cfg = app_settings();
    bool tz_changed = false;
    const int applied = apply_app_config(body, &tz_changed);
    cJSON_Delete(body);

    if (applied < 0) {
        return send_error(req, 400, "presence_src must be \"and\", \"distance\" or \"flag\"");
    }

    if (tz_changed) {
        app_sun_apply_timezone();
    }

    esp_err_t err = app_settings_save();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddBoolToObject(root, "auto_mode", cfg->auto_mode);
    cJSON_AddNumberToObject(root, "hold_s", cfg->hold_s);
    cJSON_AddNumberToObject(root, "max_cm", cfg->max_cm);
    cJSON_AddNumberToObject(root, "min_cm", cfg->min_cm);
    cJSON_AddNumberToObject(root, "hyst_cm", cfg->hyst_cm);
    cJSON_AddNumberToObject(root, "blank_ms", cfg->blank_ms);
    cJSON_AddNumberToObject(root, "on_delay_ms", cfg->on_delay_ms);
    cJSON_AddNumberToObject(root, "presence_src", cfg->presence_src);
    cJSON_AddBoolToObject(root, "restore_state", cfg->restore_state);
    cJSON_AddBoolToObject(root, "night_only", cfg->night_only);
    cJSON_AddNumberToObject(root, "sunset_off_min", cfg->sunset_off_min);
    cJSON_AddNumberToObject(root, "sunrise_off_min", cfg->sunrise_off_min);
    cJSON_AddNumberToObject(root, "lat", (double)cfg->lat_udeg / 1000000.0);
    cJSON_AddNumberToObject(root, "lon", (double)cfg->lon_udeg / 1000000.0);
    cJSON_AddStringToObject(root, "tz", cfg->tz);
    cJSON_AddStringToObject(root, "ntp_server", cfg->ntp_server);
    return send_json(req, root, err == ESP_OK ? 200 : 500);
}

/* LD2420 module settings: detection gates, module hold time, output mode. */
static esp_err_t sensor_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_error(req, 400, "invalid json body");
    }

    ld2420_state_t st;
    ld2420_get_state(&st);

    esp_err_t err = ESP_OK;
    uint32_t min_gate = st.min_gate, max_gate = st.max_gate, timeout_s = st.timeout_s;
    bool ranges_touched = false;
    ranges_touched |= json_get_uint(body, "min_gate", 15, &min_gate);
    ranges_touched |= json_get_uint(body, "max_gate", 15, &max_gate);
    ranges_touched |= json_get_uint(body, "timeout_s", 65535, &timeout_s);
    if (ranges_touched) {
        err = ld2420_set_ranges((uint16_t)min_gate, (uint16_t)max_gate, (uint16_t)timeout_s);
    }

    uint32_t gate, move_thresh, still_thresh;
    bool wrote_gate = false;
    if (err == ESP_OK && json_get_uint(body, "gate", 15, &gate)) {
        ld2420_get_state(&st);
        move_thresh = st.move_thresh[gate];
        still_thresh = st.still_thresh[gate];
        json_get_uint(body, "move", 65535, &move_thresh);
        json_get_uint(body, "still", 65535, &still_thresh);
        err = ld2420_set_gate_threshold((uint8_t)gate, move_thresh, still_thresh);
        wrote_gate = (err == ESP_OK);
    }

    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(body, "mode");
    if (err == ESP_OK && cJSON_IsString(mode) && mode->valuestring) {
        if (strcmp(mode->valuestring, "energy") == 0) {
            err = ld2420_set_mode(LD2420_MODE_ENERGY);
        } else if (strcmp(mode->valuestring, "simple") == 0) {
            err = ld2420_set_mode(LD2420_MODE_SIMPLE);
        } else {
            err = ESP_ERR_INVALID_ARG;
        }
    }

    const cJSON *action = cJSON_GetObjectItemCaseSensitive(body, "action");
    bool forget_stored = false;
    if (err == ESP_OK && cJSON_IsString(action) && action->valuestring) {
        if (strcmp(action->valuestring, "factory_reset") == 0) {
            err = ld2420_factory_reset();
            forget_stored = true; /* nie odtwarzaj starych wartości po resecie modułu */
        } else if (strcmp(action->valuestring, "restart") == 0) {
            err = ld2420_restart();
        } else if (strcmp(action->valuestring, "refresh") == 0) {
            err = ld2420_refresh_config();
        } else {
            err = ESP_ERR_INVALID_ARG;
        }
    }
    cJSON_Delete(body);

    if (err != ESP_OK) {
        return send_error(req, err == ESP_ERR_INVALID_ARG ? 400 : 500, esp_err_to_name(err));
    }

    ld2420_get_state(&st);

    /* Zapamiętujemy pełny stan konfiguracji modułu, żeby odtworzyć go po zaniku
     * zasilania - LD2420 nie utrwala niezawodnie wszystkich parametrów. */
    if (forget_stored) {
        app_settings_sensor_forget();
    } else if (st.config_valid && (ranges_touched || wrote_gate)) {
        app_sensor_cfg_t *saved = app_settings_sensor();
        saved->valid = true;
        saved->min_gate = st.min_gate;
        saved->max_gate = st.max_gate;
        saved->timeout_s = st.timeout_s;
        memcpy(saved->move_thresh, st.move_thresh, sizeof(saved->move_thresh));
        memcpy(saved->still_thresh, st.still_thresh, sizeof(saved->still_thresh));
        app_settings_sensor_save();
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "min_gate", st.min_gate);
    cJSON_AddNumberToObject(root, "max_gate", st.max_gate);
    cJSON_AddNumberToObject(root, "timeout_s", st.timeout_s);
    return send_json(req, root, 200);
}

/* --- Wi-Fi: zmiana danych logowania i skan sieci (tryb serwisowy) --- */

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_error(req, 400, "invalid json body");
    }

    /* {"setup_mode":true} - wejście w tryb serwisowy na żądanie (np. przed zmianą routera) */
    bool setup_mode = false;
    if (json_get_bool(body, "setup_mode", &setup_mode) && setup_mode) {
        cJSON_Delete(body);
        if (app_wifi_set_prov_flag(true) != ESP_OK) {
            return send_error(req, 500, "cannot set setup flag");
        }
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddStringToObject(root, "info", "rebooting into Wi-Fi setup mode (SoftAP)");
        send_json(req, root, 200);
        app_wifi_schedule_reboot(1000);
        return ESP_OK;
    }

    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(body, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(body, "password");
    if (!cJSON_IsString(ssid) || !ssid->valuestring || ssid->valuestring[0] == '\0') {
        cJSON_Delete(body);
        return send_error(req, 400, "expected {\"ssid\":\"...\",\"password\":\"...\"}");
    }

    esp_err_t err = app_wifi_set_credentials(ssid->valuestring,
                                             cJSON_IsString(pass) ? pass->valuestring : NULL);
    cJSON_Delete(body);
    if (err != ESP_OK) {
        return send_error(req, err == ESP_ERR_INVALID_ARG ? 400 : 500, esp_err_to_name(err));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "info", "credentials stored, rebooting to connect");
    send_json(req, root, 200);
    app_wifi_schedule_reboot(1500);
    return ESP_OK;
}

static esp_err_t wifi_scan_get_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    app_wifi_ap_info_t list[APP_WIFI_SCAN_MAX];
    size_t count = 0;
    esp_err_t err = app_wifi_scan(list, APP_WIFI_SCAN_MAX, &count);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_error(req, 400, "scan only available in Wi-Fi setup mode");
    }
    if (err != ESP_OK) {
        return send_error(req, 500, esp_err_to_name(err));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON *arr = cJSON_AddArrayToObject(root, "networks");
    for (size_t i = 0; i < count; i++) {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "ssid", list[i].ssid);
        cJSON_AddNumberToObject(item, "rssi", list[i].rssi);
        cJSON_AddBoolToObject(item, "open", list[i].open_network);
        cJSON_AddItemToArray(arr, item);
    }
    return send_json(req, root, 200);
}

/* --- OTA: wgranie firmware przez panel (bez USB) ---
 *
 * Urządzenie siedzi w oprawie, więc aktualizacja po Wi-Fi jest jedyną drogą.
 * Ciało żądania to surowy plik .bin (nie multipart). Obraz trafia do wolnej partycji
 * OTA, a poprzedni zostaje nienaruszony - jeśli nowy okaże się zły, trzy szybkie
 * odcięcia zasilania przywracają poprzedni (patrz app_light.cpp).
 *
 * UWAGA: to zwykły HTTP z Basic Auth. Każdy, kto zna hasło panelu i ma dostęp do tej
 * sieci, może wgrać dowolny firmware - trzymać wyłącznie w zaufanej sieci LAN.
 */
static esp_err_t ota_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    if (req->content_len < 64 * 1024 || req->content_len > 4 * 1024 * 1024) {
        return send_error(req, 400, "unexpected image size");
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        return send_error(req, 500, "no OTA partition available");
    }
    ESP_LOGW(TAG, "OTA started: %u bytes -> partition '%s'", (unsigned)req->content_len,
             target->label);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(target, req->content_len, &handle);
    if (err != ESP_OK) {
        return send_error(req, 500, esp_err_to_name(err));
    }

    char buf[1024];
    int received = 0;
    while (received < (int)req->content_len) {
        int r = httpd_req_recv(req, buf, sizeof(buf));
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (r <= 0) {
            esp_ota_abort(handle);
            ESP_LOGE(TAG, "OTA aborted: connection lost after %d bytes", received);
            return send_error(req, 400, "connection lost");
        }
        err = esp_ota_write(handle, buf, r);
        if (err != ESP_OK) {
            esp_ota_abort(handle);
            ESP_LOGE(TAG, "OTA write failed: %s", esp_err_to_name(err));
            return send_error(req, 500, esp_err_to_name(err));
        }
        received += r;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA image invalid: %s", esp_err_to_name(err));
        return send_error(req, 400, esp_err_to_name(err));
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        return send_error(req, 500, esp_err_to_name(err));
    }

    ESP_LOGW(TAG, "OTA finished (%d bytes), booting from '%s'", received, target->label);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "written", received);
    cJSON_AddStringToObject(root, "partition", target->label);
    cJSON_AddStringToObject(root, "info", "rebooting into the new firmware");
    send_json(req, root, 200);
    app_wifi_schedule_reboot(1500);
    return ESP_OK;
}

/* --- zmiana hasła panelu --- */

static esp_err_t password_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_error(req, 400, "invalid json body");
    }

    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(body, "current");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(body, "password");
    const cJSON *user = cJSON_GetObjectItemCaseSensitive(body, "user");

    /* Dodatkowe potwierdzenie aktualnym hasłem: przeglądarka trzyma Basic Auth
     * w pamięci, więc bez tego wystarczyłaby otwarta karta, żeby zmienić hasło. */
    if (!cJSON_IsString(cur) || strcmp(cur->valuestring, app_settings_web_pass()) != 0) {
        cJSON_Delete(body);
        return send_error(req, 403, "current password does not match");
    }
    if (!cJSON_IsString(pass)) {
        cJSON_Delete(body);
        return send_error(req, 400, "expected {\"current\":\"...\",\"password\":\"...\"}");
    }

    const char *new_user = cJSON_IsString(user) && user->valuestring[0]
                               ? user->valuestring
                               : app_settings_web_user();
    esp_err_t err = app_settings_set_web_credentials(new_user, pass->valuestring);
    cJSON_Delete(body);
    if (err == ESP_ERR_INVALID_ARG) {
        return send_error(req, 400, "user must be non-empty and password at least 4 characters");
    }
    if (err != ESP_OK) {
        return send_error(req, 500, esp_err_to_name(err));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "user", app_settings_web_user());
    cJSON_AddStringToObject(root, "info", "password changed - reload the panel and log in again");
    return send_json(req, root, 200);
}

/* --- kopia zapasowa ustawień (eksport/import JSON) ---
 *
 * Świadomie NIE eksportujemy sekretów: hasła panelu ani danych Wi-Fi. Kopia ma służyć
 * do odtworzenia kalibracji i zachowania automatyki, a nie do klonowania dostępu.
 */
static void settings_to_json(cJSON *root)
{
    app_settings_t *cfg = app_settings();
    cJSON_AddNumberToObject(root, "version", 1);

    cJSON *automation = cJSON_AddObjectToObject(root, "config");
    cJSON_AddBoolToObject(automation, "auto_mode", cfg->auto_mode);
    cJSON_AddNumberToObject(automation, "hold_s", cfg->hold_s);
    cJSON_AddNumberToObject(automation, "max_cm", cfg->max_cm);
    cJSON_AddNumberToObject(automation, "min_cm", cfg->min_cm);
    cJSON_AddNumberToObject(automation, "hyst_cm", cfg->hyst_cm);
    cJSON_AddNumberToObject(automation, "blank_ms", cfg->blank_ms);
    cJSON_AddNumberToObject(automation, "on_delay_ms", cfg->on_delay_ms);
    cJSON_AddStringToObject(automation, "presence_src",
                            cfg->presence_src == PRESENCE_SRC_DISTANCE ? "distance"
                            : cfg->presence_src == PRESENCE_SRC_FLAG   ? "flag"
                                                                      : "and");
    cJSON_AddBoolToObject(automation, "restore_state", cfg->restore_state);
    cJSON_AddBoolToObject(automation, "night_only", cfg->night_only);
    cJSON_AddNumberToObject(automation, "sunset_off_min", cfg->sunset_off_min);
    cJSON_AddNumberToObject(automation, "sunrise_off_min", cfg->sunrise_off_min);
    cJSON_AddNumberToObject(automation, "lat", (double)cfg->lat_udeg / 1000000.0);
    cJSON_AddNumberToObject(automation, "lon", (double)cfg->lon_udeg / 1000000.0);
    cJSON_AddStringToObject(automation, "tz", cfg->tz);
    cJSON_AddStringToObject(automation, "ntp_server", cfg->ntp_server);

    ld2420_state_t st;
    ld2420_get_state(&st);
    cJSON *sensor = cJSON_AddObjectToObject(root, "sensor");
    cJSON_AddNumberToObject(sensor, "min_gate", st.min_gate);
    cJSON_AddNumberToObject(sensor, "max_gate", st.max_gate);
    cJSON_AddNumberToObject(sensor, "timeout_s", st.timeout_s);
    cJSON *move = cJSON_AddArrayToObject(sensor, "move");
    cJSON *still = cJSON_AddArrayToObject(sensor, "still");
    for (int i = 0; i < LD2420_GATES; i++) {
        cJSON_AddItemToArray(move, cJSON_CreateNumber(st.move_thresh[i]));
        cJSON_AddItemToArray(still, cJSON_CreateNumber(st.still_thresh[i]));
    }
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    settings_to_json(root);
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"swiatlo-config.json\"");
    return send_json(req, root, 200);
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_error(req, 400, "invalid json body");
    }

    int applied_app = 0;
    int applied_sensor = 0;
    esp_err_t err = ESP_OK;

    /* --- ustawienia aplikacji --- */
    const cJSON *cfg_json = cJSON_GetObjectItemCaseSensitive(body, "config");
    if (cJSON_IsObject(cfg_json)) {
        bool tz_changed = false;
        applied_app = apply_app_config(cfg_json, &tz_changed);
        if (applied_app > 0) {
            if (tz_changed) {
                app_sun_apply_timezone();
            }
            err = app_settings_save();
        }
    }

    /* --- konfiguracja czujnika --- */
    const cJSON *sensor_json = cJSON_GetObjectItemCaseSensitive(body, "sensor");
    if (err == ESP_OK && cJSON_IsObject(sensor_json)) {
        ld2420_state_t st;
        ld2420_get_state(&st);
        uint32_t min_gate = st.min_gate, max_gate = st.max_gate, timeout_s = st.timeout_s;
        bool ranges = false;
        ranges |= json_get_uint(sensor_json, "min_gate", 15, &min_gate);
        ranges |= json_get_uint(sensor_json, "max_gate", 15, &max_gate);
        ranges |= json_get_uint(sensor_json, "timeout_s", 65535, &timeout_s);
        /* Nie zapisujemy do modułu, jeśli nic się nie zmienia - jego pamięć nieulotna
         * ma ograniczoną liczbę cykli, a import bywa powtarzany. */
        if (ranges && (min_gate != st.min_gate || max_gate != st.max_gate ||
                       timeout_s != st.timeout_s)) {
            err = ld2420_set_ranges((uint16_t)min_gate, (uint16_t)max_gate, (uint16_t)timeout_s);
            if (err == ESP_OK) {
                applied_sensor++;
            }
        }

        const cJSON *move = cJSON_GetObjectItemCaseSensitive(sensor_json, "move");
        const cJSON *still = cJSON_GetObjectItemCaseSensitive(sensor_json, "still");
        if (err == ESP_OK && cJSON_IsArray(move) && cJSON_IsArray(still) &&
            cJSON_GetArraySize(move) == LD2420_GATES && cJSON_GetArraySize(still) == LD2420_GATES) {
            for (int gate = 0; gate < LD2420_GATES && err == ESP_OK; gate++) {
                const cJSON *m = cJSON_GetArrayItem(move, gate);
                const cJSON *s = cJSON_GetArrayItem(still, gate);
                if (!cJSON_IsNumber(m) || !cJSON_IsNumber(s)) {
                    continue;
                }
                ld2420_get_state(&st);
                if (st.move_thresh[gate] == (uint32_t)m->valuedouble &&
                    st.still_thresh[gate] == (uint32_t)s->valuedouble) {
                    continue; /* bez zmian - nie zużywamy pamięci modułu */
                }
                err = ld2420_set_gate_threshold((uint8_t)gate, (uint32_t)m->valuedouble,
                                                (uint32_t)s->valuedouble);
                if (err == ESP_OK) {
                    applied_sensor++;
                }
            }
        }

        /* Zapamiętaj pełny stan, żeby przetrwał zanik zasilania modułu. */
        if (err == ESP_OK) {
            ld2420_get_state(&st);
            if (st.config_valid) {
                app_sensor_cfg_t *saved = app_settings_sensor();
                saved->valid = true;
                saved->min_gate = st.min_gate;
                saved->max_gate = st.max_gate;
                saved->timeout_s = st.timeout_s;
                memcpy(saved->move_thresh, st.move_thresh, sizeof(saved->move_thresh));
                memcpy(saved->still_thresh, st.still_thresh, sizeof(saved->still_thresh));
                app_settings_sensor_save();
            }
        }
    }
    cJSON_Delete(body);

    if (err != ESP_OK) {
        return send_error(req, 500, esp_err_to_name(err));
    }

    ESP_LOGW(TAG, "settings imported: %d app field(s), %d sensor write(s)", applied_app,
             applied_sensor);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "app_fields", applied_app);
    cJSON_AddNumberToObject(root, "sensor_writes", applied_sensor);
    settings_to_json(root);
    return send_json(req, root, 200);
}

/* --- historia ostatnich wykryć ---
 *
 * Odpowiedź jest strumieniowana porcjami: przy kilkuset wpisach zbudowanie całego
 * drzewa cJSON zajęłoby dziesiątki kilobajtów, a przy włączonym Matterze tyle nie ma.
 * Parametry: ?limit=N (domyślnie całość), ?offset=N (pomiń N najnowszych).
 */
static esp_err_t events_get_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }

    size_t limit = app_events_capacity();
    size_t offset = 0;
    char query[64];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char value[16];
        if (httpd_query_key_value(query, "limit", value, sizeof(value)) == ESP_OK) {
            const int v = atoi(value);
            if (v > 0) {
                limit = (size_t)v;
            }
        }
        if (httpd_query_key_value(query, "offset", value, sizeof(value)) == ESP_OK) {
            const int v = atoi(value);
            if (v > 0) {
                offset = (size_t)v;
            }
        }
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    char head[160];
    int n = snprintf(head, sizeof(head),
                     "{\"ok\":true,\"presence_events\":%lu,\"stored\":%u,\"capacity\":%u,"
                     "\"uptime_s\":%lu,\"events\":[",
                     (unsigned long)app_events_presence_count(), (unsigned)app_events_count(),
                     (unsigned)app_events_capacity(),
                     (unsigned long)(esp_timer_get_time() / 1000000));
    httpd_resp_send_chunk(req, head, n);

    /* Porcjami po 16 wpisów - stały, mały ślad w pamięci niezależnie od rozmiaru historii. */
    app_event_t batch[16];
    size_t sent = 0;
    bool first = true;
    while (sent < limit) {
        const size_t want = (limit - sent < 16) ? (limit - sent) : 16;
        const size_t got = app_events_get(batch, want, offset + sent);
        if (got == 0) {
            break;
        }
        for (size_t i = 0; i < got; i++) {
            char item[224];
            int len = snprintf(item, sizeof(item), "%s{\"type\":\"%s\",\"uptime_s\":%lu",
                               first ? "" : ",", app_event_type_name(batch[i].type),
                               (unsigned long)batch[i].uptime_s);
            first = false;
            if (batch[i].wall > 0) {
                char buf[24];
                struct tm tm_local;
                localtime_r(&batch[i].wall, &tm_local);
                strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_local);
                len += snprintf(item + len, sizeof(item) - len, ",\"t\":%lld,\"local\":\"%s\"",
                                (long long)batch[i].wall, buf);
            }
            if (batch[i].type == APP_EVENT_PRESENCE_START ||
                batch[i].type == APP_EVENT_PRESENCE_END) {
                len += snprintf(item + len, sizeof(item) - len, ",\"distance_cm\":%u",
                                batch[i].distance_cm);
            }
            if (batch[i].type == APP_EVENT_PRESENCE_END) {
                len += snprintf(item + len, sizeof(item) - len, ",\"duration_s\":%u",
                                batch[i].duration_s);
            }
            if (batch[i].type == APP_EVENT_LIGHT_ON || batch[i].type == APP_EVENT_LIGHT_OFF) {
                len += snprintf(item + len, sizeof(item) - len, ",\"src\":\"%s\"",
                                app_light_src_name((light_src_t)batch[i].src));
            }
            len += snprintf(item + len, sizeof(item) - len, "}");
            httpd_resp_send_chunk(req, item, len);
        }
        sent += got;
        if (got < want) {
            break;
        }
    }

    httpd_resp_send_chunk(req, "]}", 2);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

/* --- włączanie/wyłączanie stosu Matter (wymaga restartu) --- */

static esp_err_t matter_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    cJSON *body = read_json_body(req);
    if (!body) {
        return send_error(req, 400, "invalid json body");
    }
    bool enabled = false;
    if (!json_get_bool(body, "enabled", &enabled)) {
        cJSON_Delete(body);
        return send_error(req, 400, "expected {\"enabled\":true|false}");
    }
    cJSON_Delete(body);

    if (enabled == app_settings_matter_enabled()) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", true);
        cJSON_AddBoolToObject(root, "enabled", enabled);
        cJSON_AddStringToObject(root, "info", "no change");
        return send_json(req, root, 200);
    }

    esp_err_t err = app_settings_set_matter_enabled(enabled);
    if (err != ESP_OK) {
        return send_error(req, 500, esp_err_to_name(err));
    }

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddBoolToObject(root, "enabled", enabled);
    /* esp-matter 1.4.2 nie ma API do zatrzymania stosu w locie, więc przełącznik
     * działa przez restart. Ustawienia i dane parowania zostają w NVS. */
    cJSON_AddStringToObject(root, "info", "rebooting to apply");
    send_json(req, root, 200);
    app_wifi_schedule_reboot(1500);
    return ESP_OK;
}

static void reboot_timer_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    if (!check_auth(req)) {
        return ESP_OK;
    }
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddStringToObject(root, "info", "rebooting in 1 s");
    send_json(req, root, 200);

    esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {
        .callback = reboot_timer_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "reboot",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, 1000 * 1000);
    }
    return ESP_OK;
}

/* ------------------------------------------------------------ lifecycle */

esp_err_t app_web_start(void)
{
    char ip[16];
    if (s_server) {
        /* Called again on every IP change - just report the current address. */
        get_ip_str(ip, sizeof(ip));
        ESP_LOGI(TAG, "panel already running: http://%s:%d/", ip, CONFIG_APP_WEB_PORT);
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CONFIG_APP_WEB_PORT;
    config.max_uri_handlers = 14;
    /* RAM on the ESP32-C3 is tight next to Matter + BLE, so keep the socket pool small. */
    config.max_open_sockets = 4;
    config.stack_size = 6144;
    config.lru_purge_enable = true;
    /* Wgranie ~1,7 MB firmware przez Wi-Fi trwa dłużej niż domyślne 5 s. */
    config.recv_wait_timeout = 20;
    config.send_wait_timeout = 20;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler, .user_ctx = NULL},
        {.uri = "/api/events", .method = HTTP_GET, .handler = events_get_handler, .user_ctx = NULL},
        {.uri = "/api/light", .method = HTTP_POST, .handler = light_post_handler, .user_ctx = NULL},
        {.uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler, .user_ctx = NULL},
        {.uri = "/api/sensor", .method = HTTP_POST, .handler = sensor_post_handler, .user_ctx = NULL},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_post_handler, .user_ctx = NULL},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_get_handler, .user_ctx = NULL},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_post_handler, .user_ctx = NULL},
        {.uri = "/api/password", .method = HTTP_POST, .handler = password_post_handler, .user_ctx = NULL},
        {.uri = "/api/matter", .method = HTTP_POST, .handler = matter_post_handler, .user_ctx = NULL},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = settings_get_handler, .user_ctx = NULL},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = settings_post_handler, .user_ctx = NULL},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post_handler, .user_ctx = NULL},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    get_ip_str(ip, sizeof(ip));
#if CONFIG_APP_WEB_AUTH
    ESP_LOGI(TAG, "panel: http://%s:%d/ (basic auth, user '%s')", ip, CONFIG_APP_WEB_PORT,
             app_settings_web_user());
#else
    ESP_LOGW(TAG, "panel: http://%s:%d/ - AUTHENTICATION DISABLED, anyone on the LAN can switch the lamp",
             ip, CONFIG_APP_WEB_PORT);
#endif
    return ESP_OK;
}

void app_web_stop(void)
{
    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }
}
