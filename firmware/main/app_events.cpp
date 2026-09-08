#include "app_events.h"

#include <string.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sdkconfig.h>

static const char *TAG = "events";

#define EVENT_LOG_SIZE CONFIG_APP_EVENT_LOG_SIZE

static app_event_t s_log[EVENT_LOG_SIZE];
static size_t s_count;     /* ile wpisów jest wypełnionych (<= EVENT_LOG_SIZE) */
static size_t s_next;      /* gdzie trafi kolejny wpis                        */
static uint32_t s_presence_events;
static SemaphoreHandle_t s_lock;

void app_events_init(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
}

static void add_entry(const app_event_t *ev)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_log[s_next] = *ev;
    s_next = (s_next + 1) % EVENT_LOG_SIZE;
    if (s_count < EVENT_LOG_SIZE) {
        s_count++;
    }
    xSemaphoreGive(s_lock);
}

static void fill_time(app_event_t *ev)
{
    ev->uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    const time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    /* Rok < 2024 oznacza, że SNTP jeszcze nie ustawił zegara - wtedy zostaje uptime. */
    ev->wall = (tm_utc.tm_year + 1900 >= 2024) ? now : 0;
}

void app_events_add_presence(bool present, uint16_t distance_cm, uint16_t duration_s)
{
    app_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = present ? APP_EVENT_PRESENCE_START : APP_EVENT_PRESENCE_END;
    ev.distance_cm = distance_cm;
    ev.duration_s = duration_s;
    fill_time(&ev);
    if (present) {
        s_presence_events++;
    }
    add_entry(&ev);
    ESP_LOGI(TAG, "%s (dist=%u cm%s)", present ? "presence detected" : "presence lost",
             distance_cm, present ? "" : ", end of period");
}

void app_events_add_light(bool on, const char *src)
{
    app_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = on ? APP_EVENT_LIGHT_ON : APP_EVENT_LIGHT_OFF;
    if (src) {
        strlcpy(ev.src, src, sizeof(ev.src));
    }
    fill_time(&ev);
    add_entry(&ev);
}

size_t app_events_get(app_event_t *out, size_t max)
{
    if (!out || max == 0 || !s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n = (s_count < max) ? s_count : max;
    for (size_t i = 0; i < n; i++) {
        /* od najnowszego: s_next-1, s_next-2, ... (modulo) */
        const size_t idx = (s_next + EVENT_LOG_SIZE - 1 - i) % EVENT_LOG_SIZE;
        out[i] = s_log[idx];
    }
    xSemaphoreGive(s_lock);
    return n;
}

uint32_t app_events_presence_count(void) { return s_presence_events; }

const char *app_event_type_name(uint8_t type)
{
    switch (type) {
    case APP_EVENT_PRESENCE_START: return "presence";
    case APP_EVENT_PRESENCE_END: return "presence_end";
    case APP_EVENT_LIGHT_ON: return "light_on";
    case APP_EVENT_LIGHT_OFF: return "light_off";
    default: return "unknown";
    }
}
