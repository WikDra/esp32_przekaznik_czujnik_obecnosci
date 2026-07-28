/* Persistent application settings (NVS namespace "swiatlo"). */
#include "app_priv.h"

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <sdkconfig.h>
#include <string.h>

static const char *TAG = "settings";
static const char *NVS_NS = "swiatlo";

static app_settings_t s_settings;

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
#ifdef CONFIG_APP_RESTORE_STATE_DEFAULT
    s_settings.restore_state = true;
#else
    s_settings.restore_state = false;
#endif
    s_settings.last_on = false;
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
    if (nvs_get_u8(h, "restore", &u8) == ESP_OK) {
        s_settings.restore_state = u8 != 0;
    }
    if (nvs_get_u8(h, "last_on", &u8) == ESP_OK) {
        s_settings.last_on = u8 != 0;
    }
    nvs_close(h);

    ESP_LOGI(TAG, "loaded: auto=%d hold=%us range=%u..%ucm restore=%d last_on=%d",
             s_settings.auto_mode, s_settings.hold_s, s_settings.min_cm, s_settings.max_cm,
             s_settings.restore_state, s_settings.last_on);
    return ESP_OK;
}

app_settings_t *app_settings(void) { return &s_settings; }

uint8_t app_settings_get_power_cycles(void)
{
    nvs_handle_t h;
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
    nvs_set_u8(h, "restore", s_settings.restore_state ? 1 : 0);
    nvs_set_u8(h, "last_on", s_settings.last_on ? 1 : 0);
    err = nvs_commit(h);
    nvs_close(h);
    return err;
}
