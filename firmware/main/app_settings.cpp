/* Persistent application settings (NVS namespace "swiatlo"). */
#include "app_priv.h"

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <sdkconfig.h>
#include <string.h>
#include <sys/param.h>

static const char *TAG = "settings";
static const char *NVS_NS = "swiatlo";

static app_settings_t s_settings;
static bool s_matter_enabled = true;

static void sensor_cfg_load(void);
static void web_credentials_load(void);

static void load_defaults(void)
{
#ifdef CONFIG_APP_AUTO_MODE_DEFAULT
    s_settings.auto_mode = true;
#else
    s_settings.auto_mode = false;
#endif
    s_settings.hold_s = CONFIG_APP_HOLD_SECONDS_DEFAULT;
    s_settings.max_cm = CONFIG_APP_MAX_DISTANCE_CM_DEFAULT;
    s_settings.min_cm = CONFIG_APP_MIN_DISTANCE_CM_DEFAULT;
    s_settings.hyst_cm = CONFIG_APP_HYSTERESIS_CM_DEFAULT;
    s_settings.blank_ms = CONFIG_APP_RELAY_BLANK_MS_DEFAULT;
    s_settings.on_delay_ms = CONFIG_APP_PRESENCE_ON_MS_DEFAULT;
    s_settings.presence_src = CONFIG_APP_PRESENCE_SRC_DEFAULT;
#ifdef CONFIG_APP_RESTORE_STATE_DEFAULT
    s_settings.restore_state = true;
#else
    s_settings.restore_state = false;
#endif
    s_settings.last_on = false;

#ifdef CONFIG_APP_NIGHT_ONLY_DEFAULT
    s_settings.night_only = true;
#else
    s_settings.night_only = false;
#endif
    s_settings.sunset_off_min = CONFIG_APP_SUNSET_OFFSET_MIN_DEFAULT;
    s_settings.sunrise_off_min = CONFIG_APP_SUNRISE_OFFSET_MIN_DEFAULT;
    s_settings.lat_udeg = CONFIG_APP_LATITUDE_UDEG_DEFAULT;
    s_settings.lon_udeg = CONFIG_APP_LONGITUDE_UDEG_DEFAULT;
    strlcpy(s_settings.tz, CONFIG_APP_TZ_DEFAULT, sizeof(s_settings.tz));
    strlcpy(s_settings.ntp_server, CONFIG_APP_NTP_SERVER_DEFAULT, sizeof(s_settings.ntp_server));
}

esp_err_t app_settings_init(void)
{
    load_defaults();

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "no stored settings, using defaults");
        return ESP_OK;
    }

    uint8_t u8;
    uint16_t u16;
    if (nvs_get_u8(h, "auto", &u8) == ESP_OK) {
        s_settings.auto_mode = u8 != 0;
    }
    if (nvs_get_u16(h, "hold", &u16) == ESP_OK) {
        s_settings.hold_s = u16;
    }
    if (nvs_get_u16(h, "max_cm", &u16) == ESP_OK) {
        s_settings.max_cm = u16;
    }
    if (nvs_get_u16(h, "min_cm", &u16) == ESP_OK) {
        s_settings.min_cm = u16;
    }
    if (nvs_get_u16(h, "hyst_cm", &u16) == ESP_OK) {
        s_settings.hyst_cm = u16;
    }
    if (nvs_get_u16(h, "blank_ms", &u16) == ESP_OK) {
        s_settings.blank_ms = u16;
    }
    if (nvs_get_u16(h, "on_ms", &u16) == ESP_OK) {
        s_settings.on_delay_ms = u16;
    }
    if (nvs_get_u8(h, "psrc", &u8) == ESP_OK && u8 <= PRESENCE_SRC_FLAG) {
        s_settings.presence_src = u8;
    }
    if (nvs_get_u8(h, "restore", &u8) == ESP_OK) {
        s_settings.restore_state = u8 != 0;
    }
    if (nvs_get_u8(h, "last_on", &u8) == ESP_OK) {
        s_settings.last_on = u8 != 0;
    }

    if (nvs_get_u8(h, "night", &u8) == ESP_OK) {
        s_settings.night_only = u8 != 0;
    }
    int16_t i16;
    if (nvs_get_i16(h, "set_off", &i16) == ESP_OK) {
        s_settings.sunset_off_min = i16;
    }
    if (nvs_get_i16(h, "rise_off", &i16) == ESP_OK) {
        s_settings.sunrise_off_min = i16;
    }
    int32_t i32;
    if (nvs_get_i32(h, "lat", &i32) == ESP_OK) {
        s_settings.lat_udeg = i32;
    }
    if (nvs_get_i32(h, "lon", &i32) == ESP_OK) {
        s_settings.lon_udeg = i32;
    }
    size_t len = sizeof(s_settings.tz);
    nvs_get_str(h, "tz", s_settings.tz, &len);
    len = sizeof(s_settings.ntp_server);
    nvs_get_str(h, "ntp", s_settings.ntp_server, &len);
    if (nvs_get_u8(h, "matter_en", &u8) == ESP_OK) {
        s_matter_enabled = u8 != 0;
    }
    nvs_close(h);

    ESP_LOGI(TAG, "loaded: auto=%d hold=%us range=%u..%ucm hyst=%ucm psrc=%u restore=%d last_on=%d",
             s_settings.auto_mode, s_settings.hold_s, s_settings.min_cm, s_settings.max_cm,
             s_settings.hyst_cm, s_settings.presence_src, s_settings.restore_state, s_settings.last_on);
    sensor_cfg_load();
    web_credentials_load();
    return ESP_OK;
}

app_settings_t *app_settings(void) { return &s_settings; }

/* ------------------------------------------------------------ konfiguracja czujnika */

static app_sensor_cfg_t s_sensor_cfg;

app_sensor_cfg_t *app_settings_sensor(void) { return &s_sensor_cfg; }

static void sensor_cfg_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t valid = 0;
    uint16_t u16;
    if (nvs_get_u8(h, "sn_valid", &valid) == ESP_OK && valid) {
        if (nvs_get_u16(h, "sn_min", &u16) == ESP_OK) {
            s_sensor_cfg.min_gate = u16;
        }
        if (nvs_get_u16(h, "sn_max", &u16) == ESP_OK) {
            s_sensor_cfg.max_gate = u16;
        }
        if (nvs_get_u16(h, "sn_to", &u16) == ESP_OK) {
            s_sensor_cfg.timeout_s = u16;
        }
        size_t len = sizeof(s_sensor_cfg.move_thresh);
        if (nvs_get_blob(h, "sn_move", s_sensor_cfg.move_thresh, &len) == ESP_OK &&
            len == sizeof(s_sensor_cfg.move_thresh)) {
            len = sizeof(s_sensor_cfg.still_thresh);
            if (nvs_get_blob(h, "sn_still", s_sensor_cfg.still_thresh, &len) == ESP_OK &&
                len == sizeof(s_sensor_cfg.still_thresh)) {
                s_sensor_cfg.valid = true;
            }
        }
    }
    nvs_close(h);

    if (s_sensor_cfg.valid) {
        ESP_LOGI(TAG, "sensor config remembered: gates %u..%u timeout=%us (odtworzę po starcie modułu)",
                 s_sensor_cfg.min_gate, s_sensor_cfg.max_gate, s_sensor_cfg.timeout_s);
    }
}

esp_err_t app_settings_sensor_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_u8(h, "sn_valid", s_sensor_cfg.valid ? 1 : 0);
    nvs_set_u16(h, "sn_min", s_sensor_cfg.min_gate);
    nvs_set_u16(h, "sn_max", s_sensor_cfg.max_gate);
    nvs_set_u16(h, "sn_to", s_sensor_cfg.timeout_s);
    nvs_set_blob(h, "sn_move", s_sensor_cfg.move_thresh, sizeof(s_sensor_cfg.move_thresh));
    nvs_set_blob(h, "sn_still", s_sensor_cfg.still_thresh, sizeof(s_sensor_cfg.still_thresh));
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t app_settings_sensor_forget(void)
{
    memset(&s_sensor_cfg, 0, sizeof(s_sensor_cfg));
    return app_settings_sensor_save();
}

/* ------------------------------------------------------------ Matter on/off */

bool app_settings_matter_enabled(void) { return s_matter_enabled; }

esp_err_t app_settings_set_matter_enabled(bool enabled)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_u8(h, "matter_en", enabled ? 1 : 0);
    err = nvs_commit(h);
    nvs_close(h);
    if (err == ESP_OK) {
        s_matter_enabled = enabled;
        ESP_LOGW(TAG, "Matter %s (effective after restart)", enabled ? "enabled" : "disabled");
    }
    return err;
}

/* ------------------------------------------------------------ hasło panelu */

static char s_web_user[24];
static char s_web_pass[64];
static uint32_t s_web_generation;

static void web_credentials_load(void)
{
    strlcpy(s_web_user, CONFIG_APP_WEB_USER, sizeof(s_web_user));
    strlcpy(s_web_pass, CONFIG_APP_WEB_PASS, sizeof(s_web_pass));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_web_user);
    nvs_get_str(h, "web_user", s_web_user, &len);
    len = sizeof(s_web_pass);
    if (nvs_get_str(h, "web_pass", s_web_pass, &len) == ESP_OK) {
        ESP_LOGI(TAG, "panel credentials loaded from NVS (user '%s')", s_web_user);
    }
    nvs_close(h);
}

const char *app_settings_web_user(void) { return s_web_user; }
const char *app_settings_web_pass(void) { return s_web_pass; }
uint32_t app_settings_web_generation(void) { return s_web_generation; }

esp_err_t app_settings_set_web_credentials(const char *user, const char *pass)
{
    if (!user || !pass) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(user) == 0 || strlen(user) >= sizeof(s_web_user)) {
        return ESP_ERR_INVALID_ARG;
    }
    /* Panel przełącza 230 V, więc nie pozwalamy na hasło krótsze niż 4 znaki. */
    if (strlen(pass) < 4 || strlen(pass) >= sizeof(s_web_pass)) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_str(h, "web_user", user);
    nvs_set_str(h, "web_pass", pass);
    err = nvs_commit(h);
    nvs_close(h);
    if (err != ESP_OK) {
        return err;
    }

    strlcpy(s_web_user, user, sizeof(s_web_user));
    strlcpy(s_web_pass, pass, sizeof(s_web_pass));
    s_web_generation++;
    ESP_LOGW(TAG, "panel credentials changed (user '%s')", s_web_user);
    return ESP_OK;
}

esp_err_t app_settings_reset_web_credentials(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_erase_key(h, "web_user");
    nvs_erase_key(h, "web_pass");
    err = nvs_commit(h);
    nvs_close(h);

    strlcpy(s_web_user, CONFIG_APP_WEB_USER, sizeof(s_web_user));
    strlcpy(s_web_pass, CONFIG_APP_WEB_PASS, sizeof(s_web_pass));
    s_web_generation++;
    ESP_LOGW(TAG, "panel credentials reset to firmware defaults (user '%s')", s_web_user);
    return err;
}

uint8_t app_settings_get_power_cycles(void)
{    nvs_handle_t h;
    uint8_t value = 0;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "pc_cnt", &value) != ESP_OK) {
            value = 0;
        }
        nvs_close(h);
    }
    return value;
}

esp_err_t app_settings_set_power_cycles(uint8_t value)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    nvs_set_u8(h, "pc_cnt", value);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}

esp_err_t app_settings_save(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    nvs_set_u8(h, "auto", s_settings.auto_mode ? 1 : 0);
    nvs_set_u16(h, "hold", s_settings.hold_s);
    nvs_set_u16(h, "max_cm", s_settings.max_cm);
    nvs_set_u16(h, "min_cm", s_settings.min_cm);
    nvs_set_u16(h, "hyst_cm", s_settings.hyst_cm);
    nvs_set_u16(h, "blank_ms", s_settings.blank_ms);
    nvs_set_u16(h, "on_ms", s_settings.on_delay_ms);
    nvs_set_u8(h, "psrc", s_settings.presence_src);
    nvs_set_u8(h, "restore", s_settings.restore_state ? 1 : 0);
    nvs_set_u8(h, "last_on", s_settings.last_on ? 1 : 0);
    nvs_set_u8(h, "night", s_settings.night_only ? 1 : 0);
    nvs_set_i16(h, "set_off", s_settings.sunset_off_min);
    nvs_set_i16(h, "rise_off", s_settings.sunrise_off_min);
    nvs_set_i32(h, "lat", s_settings.lat_udeg);
    nvs_set_i32(h, "lon", s_settings.lon_udeg);
    nvs_set_str(h, "tz", s_settings.tz);
    nvs_set_str(h, "ntp", s_settings.ntp_server);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}
