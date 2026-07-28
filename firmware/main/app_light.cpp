/*
 * Lamp control: relay output, status LED, BOOT button and the presence automation.
 * Every control path (Matter, HTTP panel, button, automation) funnels through
 * app_light_set() so that all of them stay in sync.
 */
#include "app_priv.h"
#include "ld2420.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include <app/server/Server.h>
#include <esp_matter.h>

static const char *TAG = "light";

using namespace esp_matter;
using namespace chip::app::Clusters;

uint16_t app_light_endpoint_id = 0;
uint16_t app_occupancy_endpoint_id = 0;

#define RELAY_GPIO ((gpio_num_t)CONFIG_APP_RELAY_GPIO)
#define LED_GPIO ((gpio_num_t)CONFIG_APP_LED_GPIO)
#define BUTTON_GPIO ((gpio_num_t)CONFIG_APP_BUTTON_GPIO)

static SemaphoreHandle_t s_lock;
static bool s_on;
static light_src_t s_last_src = LIGHT_SRC_BOOT;
static bool s_presence;
static uint16_t s_distance_cm;
static int64_t s_auto_off_at_us; /* 0 = no pending auto-off */
static bool s_force_on;          /* power-cycle override active */

static const char *src_name(light_src_t src)
{
    switch (src) {
    case LIGHT_SRC_MATTER: return "matter";
    case LIGHT_SRC_WEB: return "web";
    case LIGHT_SRC_AUTO: return "auto";
    case LIGHT_SRC_BUTTON: return "button";
    case LIGHT_SRC_POWER_CYCLE: return "power_cycle";
    default: return "boot";
    }
}

static inline void relay_write(bool on)
{
#if CONFIG_APP_RELAY_ACTIVE_LOW
    gpio_set_level(RELAY_GPIO, on ? 0 : 1);
#else
    gpio_set_level(RELAY_GPIO, on ? 1 : 0);
#endif
}

static inline void led_write(bool on)
{
#if CONFIG_APP_LED_GPIO >= 0
#if CONFIG_APP_LED_ACTIVE_LOW
    gpio_set_level(LED_GPIO, on ? 0 : 1);
#else
    gpio_set_level(LED_GPIO, on ? 1 : 0);
#endif
#else
    (void)on;
#endif
}

/* Pushes the current state into the Matter OnOff attribute (Matter thread). */
static void matter_report_on_off(bool on)
{
    uint16_t endpoint_id = app_light_endpoint_id;
    if (endpoint_id == 0) {
        return;
    }
    chip::DeviceLayer::SystemLayer().ScheduleLambda([endpoint_id, on]() {
        esp_matter_attr_val_t val = esp_matter_bool(on);
        attribute::update(endpoint_id, OnOff::Id, OnOff::Attributes::OnOff::Id, &val);
    });
}

static void matter_report_occupancy(bool occupied)
{
    uint16_t endpoint_id = app_occupancy_endpoint_id;
    if (endpoint_id == 0) {
        return;
    }
    chip::DeviceLayer::SystemLayer().ScheduleLambda([endpoint_id, occupied]() {
        esp_matter_attr_val_t val = esp_matter_uint8(occupied ? 1 : 0);
        attribute::update(endpoint_id, OccupancySensing::Id, OccupancySensing::Attributes::Occupancy::Id, &val);
    });
}

void app_light_set(bool on, light_src_t src)
{
    bool changed;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    changed = (on != s_on);
    s_on = on;
    s_last_src = src;
    relay_write(on);

    if (src == LIGHT_SRC_POWER_CYCLE && on) {
        s_force_on = true;
    } else if (!on && (src == LIGHT_SRC_MATTER || src == LIGHT_SRC_WEB || src == LIGHT_SRC_BUTTON)) {
        /* An explicit OFF always wins and clears the power-cycle override. */
        s_force_on = false;
    }

    app_settings_t *cfg = app_settings();
    if (on && cfg->auto_mode && !s_presence && cfg->hold_s > 0 && !s_force_on) {
        s_auto_off_at_us = esp_timer_get_time() + (int64_t)cfg->hold_s * 1000000;
    } else if (!on || s_force_on) {
        s_auto_off_at_us = 0;
    }
    xSemaphoreGive(s_lock);

    if (!changed) {
        return;
    }

    ESP_LOGI(TAG, "lamp %s (source: %s)", on ? "ON" : "OFF", src_name(src));

    if (src != LIGHT_SRC_MATTER) {
        matter_report_on_off(on);
    }

    app_settings_t *cfg2 = app_settings();
    if (cfg2->restore_state && cfg2->last_on != on) {
        cfg2->last_on = on;
        app_settings_save();
    }
}

bool app_light_get(void) { return s_on; }

const char *app_light_last_src(void) { return src_name(s_last_src); }

uint32_t app_light_auto_off_in(void)
{
    int64_t at = s_auto_off_at_us;
    if (at == 0) {
        return 0;
    }
    int64_t left = at - esp_timer_get_time();
    return left > 0 ? (uint32_t)(left / 1000000) : 0;
}

/* Presence callback from the LD2420 driver. Applies the distance window and
 * drives the automation on rising edges only, so a manual OFF is not overridden
 * while somebody is still in the room. */
void app_light_on_presence(bool presence, uint16_t distance_cm)
{
    app_settings_t *cfg = app_settings();
    bool accepted = presence;

    if (presence) {
        if (cfg->max_cm > 0 && distance_cm > cfg->max_cm) {
            accepted = false;
        }
        if (distance_cm < cfg->min_cm) {
            accepted = false;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool rising = accepted && !s_presence;
    bool falling = !accepted && s_presence;
    s_presence = accepted;
    s_distance_cm = distance_cm;
    if (s_force_on) {
        s_auto_off_at_us = 0;
    } else if (falling && s_on && cfg->auto_mode) {
        s_auto_off_at_us = cfg->hold_s > 0 ? esp_timer_get_time() + (int64_t)cfg->hold_s * 1000000
                                           : esp_timer_get_time();
    } else if (accepted) {
        s_auto_off_at_us = 0;
    }
    xSemaphoreGive(s_lock);

    matter_report_occupancy(accepted);

    if (rising && cfg->auto_mode && !s_on) {
        app_light_set(true, LIGHT_SRC_AUTO);
    }
}

bool app_light_presence(void) { return s_presence; }
uint16_t app_light_distance_cm(void) { return s_distance_cm; }
bool app_light_force_on(void) { return s_force_on; }

/* ------------------------------------------------------------ power cycle override */

#if CONFIG_APP_POWER_CYCLE_FORCE_ON
static void power_cycle_clear_cb(void *arg)
{
    (void)arg;
    app_settings_set_power_cycles(0);
    ESP_LOGI(TAG, "power cycle counter cleared");
}

/* Counts how many times the device was powered up (or reset) within
 * CONFIG_APP_POWER_CYCLE_WINDOW_S of each other. Reaching the configured count
 * forces the lamp ON with the automation bypassed. Returns true when triggered. */
static bool power_cycle_check(void)
{
    uint8_t count = (uint8_t)(app_settings_get_power_cycles() + 1);
    if (count >= CONFIG_APP_POWER_CYCLE_COUNT) {
        app_settings_set_power_cycles(0);
        ESP_LOGW(TAG, "%d power cycles detected - forcing the lamp ON", count);
        return true;
    }

    app_settings_set_power_cycles(count);
    ESP_LOGI(TAG, "power cycle %d/%d (cut the power again within %d s to force the lamp ON)",
             count, CONFIG_APP_POWER_CYCLE_COUNT, CONFIG_APP_POWER_CYCLE_WINDOW_S);

    esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {
        .callback = power_cycle_clear_cb,
        .arg = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "pc_clear",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &timer) == ESP_OK) {
        esp_timer_start_once(timer, (uint64_t)CONFIG_APP_POWER_CYCLE_WINDOW_S * 1000000ULL);
    }
    return false;
}
#endif

/* ------------------------------------------------------------ button + LED + timer */

static void light_task(void *arg)
{
    (void)arg;
    int64_t press_start_us = 0;
    bool long_press_signalled = false;
    uint32_t tick = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(50));
        tick++;

        /* --- auto off --- */
        int64_t now = esp_timer_get_time();
        bool do_off = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_on && !s_force_on && s_auto_off_at_us != 0 && now >= s_auto_off_at_us &&
            app_settings()->auto_mode) {
            s_auto_off_at_us = 0;
            do_off = true;
        }
        xSemaphoreGive(s_lock);
        if (do_off) {
            app_light_set(false, LIGHT_SRC_AUTO);
        }

#if CONFIG_APP_BUTTON_GPIO >= 0
        /* --- button (active low) --- */
        bool pressed = gpio_get_level(BUTTON_GPIO) == 0;
        if (pressed && press_start_us == 0) {
            press_start_us = now;
            long_press_signalled = false;
        } else if (pressed && !long_press_signalled && (now - press_start_us) > 5000000) {
            long_press_signalled = true;
            ESP_LOGW(TAG, "long press detected - factory reset on release");
        } else if (!pressed && press_start_us != 0) {
            int64_t held = now - press_start_us;
            press_start_us = 0;
            if (held > 5000000) {
                ESP_LOGW(TAG, "factory reset requested from button");
                esp_matter::factory_reset();
            } else if (held > 50000) {
                app_light_set(!app_light_get(), LIGHT_SRC_BUTTON);
            }
        }
#endif

#if CONFIG_APP_LED_GPIO >= 0
        /* --- status LED ---
         * not commissioned : 2 Hz blink
         * sensor offline   : short blink every 2 s
         * otherwise        : mirrors the lamp
         */
        ld2420_state_t st;
        if ((tick % 4) == 0) {
            ld2420_get_state(&st);
            if (!app_matter_commissioned) {
                led_write(((tick / 5) % 2) == 0);
            } else if (!st.link_ok) {
                led_write((tick % 40) < 2);
            } else {
                led_write(s_on);
            }
        }
#endif
    }
}

esp_err_t app_light_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }

    /* Set the safe (OFF) level *before* switching the pin to output so the relay
     * cannot click during boot. */
    gpio_reset_pin(RELAY_GPIO);
    relay_write(false);
#if CONFIG_APP_RELAY_OPEN_DRAIN
    /* Open drain: the pin only pulls low, an external 10k pull-up to +5 V provides
     * the high level (wiring without the level shifter). */
    gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT_OD);
#else
    gpio_set_direction(RELAY_GPIO, GPIO_MODE_OUTPUT);
#endif
    relay_write(false);

#if CONFIG_APP_LED_GPIO >= 0
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    led_write(false);
#endif

#if CONFIG_APP_BUTTON_GPIO >= 0
    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << CONFIG_APP_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);
#endif

    if (xTaskCreate(light_task, "light", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

#if CONFIG_APP_POWER_CYCLE_FORCE_ON
    if (power_cycle_check()) {
        app_light_set(true, LIGHT_SRC_POWER_CYCLE);
    }
#endif

#if CONFIG_APP_RELAY_ACTIVE_LOW
    const char *polarity = "active low";
#else
    const char *polarity = "active high";
#endif
#if CONFIG_APP_RELAY_OPEN_DRAIN
    const char *drive = "open drain";
#else
    const char *drive = "push-pull";
#endif
    ESP_LOGI(TAG, "relay on GPIO%d (%s, %s), led GPIO%d, button GPIO%d",
             CONFIG_APP_RELAY_GPIO, polarity, drive, CONFIG_APP_LED_GPIO, CONFIG_APP_BUTTON_GPIO);
    return ESP_OK;
}
