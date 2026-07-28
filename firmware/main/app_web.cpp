/*
 * Local HTTP panel (REST + single page UI).
 *
 * SECURITY NOTE: this server switches mains voltage. HTTP Basic authentication is
 * enabled by default (CONFIG_APP_WEB_AUTH) but the traffic itself is plain HTTP, so
 * it only protects against casual access inside a trusted LAN. Do not expose it to
 * the internet.
 */
#include "app_priv.h"
#include "ld2420.h"

#include <esp_err.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <sdkconfig.h>
#include <string.h>

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
    static char expected[128];
    if (expected[0] == '\0') {
        char creds[96];
        int n = snprintf(creds, sizeof(creds), "%s:%s", CONFIG_APP_WEB_USER, CONFIG_APP_WEB_PASS);
        size_t olen = 0;
        unsigned char b64[128];
        if (mbedtls_base64_encode(b64, sizeof(b64), &olen, (const unsigned char *)creds, n) != 0) {
            ESP_LOGE(TAG, "failed to build credentials");
            return false;
        }
        snprintf(expected, sizeof(expected), "Basic %.*s", (int)olen, (const char *)b64);
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

    cJSON *config = cJSON_AddObjectToObject(root, "config");
    cJSON_AddBoolToObject(config, "auto_mode", cfg->auto_mode);
    cJSON_AddNumberToObject(config, "hold_s", cfg->hold_s);
    cJSON_AddNumberToObject(config, "max_cm", cfg->max_cm);
    cJSON_AddNumberToObject(config, "min_cm", cfg->min_cm);
    cJSON_AddBoolToObject(config, "restore_state", cfg->restore_state);

    cJSON *sensor = cJSON_AddObjectToObject(root, "sensor");
    cJSON_AddBoolToObject(sensor, "link_ok", st.link_ok);
    cJSON_AddStringToObject(sensor, "fw", st.fw);
    cJSON_AddStringToObject(sensor, "mode", st.mode == LD2420_MODE_ENERGY ? "energy" : "simple");
    cJSON_AddBoolToObject(sensor, "config_valid", st.config_valid);
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
    cJSON_AddNumberToObject(sys, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(sys, "uptime_s", (double)(esp_timer_get_time() / 1000000));

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
    bool b;
    uint32_t u;
    if (json_get_bool(body, "auto_mode", &b)) {
        cfg->auto_mode = b;
    }
    if (json_get_bool(body, "restore_state", &b)) {
        cfg->restore_state = b;
    }
    if (json_get_uint(body, "hold_s", 3600, &u)) {
        cfg->hold_s = (uint16_t)u;
    }
    if (json_get_uint(body, "max_cm", 1000, &u)) {
        cfg->max_cm = (uint16_t)u;
    }
    if (json_get_uint(body, "min_cm", 1000, &u)) {
        cfg->min_cm = (uint16_t)u;
    }
    cJSON_Delete(body);

    esp_err_t err = app_settings_save();
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", err == ESP_OK);
    cJSON_AddBoolToObject(root, "auto_mode", cfg->auto_mode);
    cJSON_AddNumberToObject(root, "hold_s", cfg->hold_s);
    cJSON_AddNumberToObject(root, "max_cm", cfg->max_cm);
    cJSON_AddNumberToObject(root, "min_cm", cfg->min_cm);
    cJSON_AddBoolToObject(root, "restore_state", cfg->restore_state);
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
    if (err == ESP_OK && json_get_uint(body, "gate", 15, &gate)) {
        ld2420_get_state(&st);
        move_thresh = st.move_thresh[gate];
        still_thresh = st.still_thresh[gate];
        json_get_uint(body, "move", 65535, &move_thresh);
        json_get_uint(body, "still", 65535, &still_thresh);
        err = ld2420_set_gate_threshold((uint8_t)gate, move_thresh, still_thresh);
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
    if (err == ESP_OK && cJSON_IsString(action) && action->valuestring) {
        if (strcmp(action->valuestring, "factory_reset") == 0) {
            err = ld2420_factory_reset();
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
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "ok", true);
    cJSON_AddNumberToObject(root, "min_gate", st.min_gate);
    cJSON_AddNumberToObject(root, "max_gate", st.max_gate);
    cJSON_AddNumberToObject(root, "timeout_s", st.timeout_s);
    return send_json(req, root, 200);
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
    config.max_uri_handlers = 8;
    /* RAM on the ESP32-C3 is tight next to Matter + BLE, so keep the socket pool small. */
    config.max_open_sockets = 4;
    config.stack_size = 6144;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler, .user_ctx = NULL},
        {.uri = "/api/light", .method = HTTP_POST, .handler = light_post_handler, .user_ctx = NULL},
        {.uri = "/api/config", .method = HTTP_POST, .handler = config_post_handler, .user_ctx = NULL},
        {.uri = "/api/sensor", .method = HTTP_POST, .handler = sensor_post_handler, .user_ctx = NULL},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_post_handler, .user_ctx = NULL},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(s_server, &uris[i]);
    }

    get_ip_str(ip, sizeof(ip));
#if CONFIG_APP_WEB_AUTH
    ESP_LOGI(TAG, "panel: http://%s:%d/ (basic auth, user '%s')", ip, CONFIG_APP_WEB_PORT, CONFIG_APP_WEB_USER);
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
