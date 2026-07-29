/*
 * Swiatlo LD2420 - shared declarations.
 *
 * Target hardware: ESP32-C3 SuperMini + 1-channel 5 V relay (low level trigger)
 * + HLK-LD2420 24 GHz presence radar. Control paths: Matter (over Wi-Fi) and a local
 * HTTP panel, both active at the same time.
 */
#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ settings */

/* Where the occupancy decision comes from. The presence flag of some LD2420 units is
 * stuck at 1 (it only reflects "radar sees something", including walls and furniture),
 * so deriving occupancy from the reported distance is usually more reliable. */
typedef enum {
    PRESENCE_SRC_AND = 0,      /* flaga modułu AND okno odległości (domyślne) */
    PRESENCE_SRC_DISTANCE = 1, /* tylko okno odległości, flaga ignorowana     */
    PRESENCE_SRC_FLAG = 2,     /* tylko flaga modułu                          */
} presence_src_t;

typedef struct {
    bool auto_mode;       /* presence automation on/off                        */
    uint16_t hold_s;      /* keep the lamp on for N s after presence is lost   */
    uint16_t max_cm;      /* ignore targets further away than this (0 = off)   */
    uint16_t min_cm;      /* ignore targets closer than this                   */
    uint16_t hyst_cm;     /* hysteresis around the distance window            */
    uint8_t presence_src; /* presence_src_t                                    */
    bool restore_state;   /* restore lamp state after reboot                   */
    bool last_on;         /* last known lamp state (persisted)                 */
} app_settings_t;

esp_err_t app_settings_init(void);
app_settings_t *app_settings(void);
/* Persists the current in-memory settings (call after modifying app_settings()). */
esp_err_t app_settings_save(void);

/* Small counter used by the "cut the power twice to force the lamp ON" feature. */
uint8_t app_settings_get_power_cycles(void);
esp_err_t app_settings_set_power_cycles(uint8_t value);

/* ------------------------------------------------------------------ light */

typedef enum {
    LIGHT_SRC_BOOT = 0,
    LIGHT_SRC_MATTER,
    LIGHT_SRC_WEB,
    LIGHT_SRC_AUTO,
    LIGHT_SRC_BUTTON,
    LIGHT_SRC_POWER_CYCLE,
} light_src_t;

/* Initializes relay GPIO, status LED, button and the presence automation task. */
esp_err_t app_light_init(void);

/* Central entry point for every control path. Drives the relay, mirrors the state
 * into the Matter OnOff attribute (unless the change came from Matter itself) and
 * (re)arms the auto-off timer. Thread-safe. */
void app_light_set(bool on, light_src_t src);
bool app_light_get(void);
const char *app_light_last_src(void);

/* Seconds left before the presence automation switches the lamp off (0 = idle). */
uint32_t app_light_auto_off_in(void);

/* True when the lamp was forced ON by the power-cycle override (automation bypassed
 * until the next explicit OFF from Matter/panel/button). */
bool app_light_force_on(void);

/* Endpoint ids, filled in by app_main before the stack is started. */
extern uint16_t app_light_endpoint_id;
extern uint16_t app_occupancy_endpoint_id;

/* Called by the LD2420 driver whenever presence/distance changes. */
void app_light_on_presence(bool presence, uint16_t distance_cm);

/* Filtered presence state (after applying the min/max distance window). */
bool app_light_presence(void);
uint16_t app_light_distance_cm(void);

/* ------------------------------------------------------------------ status flags */

extern volatile bool app_matter_commissioned;
extern volatile bool app_net_connected;

/* ------------------------------------------------------------------ web */

esp_err_t app_web_start(void);
void app_web_stop(void);

#ifdef __cplusplus
}
#endif
