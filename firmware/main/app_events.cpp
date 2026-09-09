#include "app_events.h"

#include <stdlib.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <sdkconfig.h>

static const char *TAG = "events";

static app_event_t *s_log;
static size_t s_capacity;
static size_t s_count; /* ile wpisów jest wypełnionych (<= s_capacity) */
static size_t s_next;  /* gdzie trafi kolejny wpis                     */
static uint32_t s_presence_events;
static SemaphoreHandle_t s_lock;

void app_events_init(void)
{
    if (s_lock) {
        return;
    }
    s_lock = xSemaphoreCreateMutex();

    /* Przy ciasnej pamięci (Matter włączony) wolimy mniejszą historię niż brak startu. */
    size_t wanted = CONFIG_APP_EVENT_LOG_SIZE;
    while (wanted >= 10) {
        s_log = (app_event_t *)calloc(wanted, sizeof(app_event_t));
        if (s_log) {
            s_capacity = wanted;
            break;
        }
        wanted /= 2;
    }
    if (!s_log) {
        ESP_LOGE(TAG, "cannot allocate the event log - history disabled");
        return;
    }
    ESP_LOGI(TAG, "event log: %u entries (%u B), free heap %u B", (unsigned)s_capacity,
             (unsigned)(s_capacity * sizeof(app_event_t)), (unsigned)esp_get_free_heap_size());
}

size_t app_events_capacity(void) { return s_capacity; }

static void add_entry(const app_event_t *ev)
{
    if (!s_lock || !s_log) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_log[s_next] = *ev;
    s_next = (s_next + 1) % s_capacity;
    if (s_count < s_capacity) {
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
    ESP_LOGI(TAG, "%s (dist=%u cm)", present ? "presence detected" : "presence lost", distance_cm);
}

void app_events_add_light(bool on, uint8_t src)
{
    app_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = on ? APP_EVENT_LIGHT_ON : APP_EVENT_LIGHT_OFF;
    ev.src = src;
    fill_time(&ev);
    add_entry(&ev);
}

size_t app_events_get(app_event_t *out, size_t max, size_t skip)
{
    if (!out || max == 0 || !s_lock || !s_log) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t available = (skip < s_count) ? (s_count - skip) : 0;
    size_t n = (available < max) ? available : max;
    for (size_t i = 0; i < n; i++) {
        /* od najnowszego: s_next-1, s_next-2, ... (modulo), z pominięciem `skip` */
        const size_t back = skip + i + 1;
        const size_t idx = (s_next + s_capacity - (back % s_capacity)) % s_capacity;
        out[i] = s_log[idx];
    }
    xSemaphoreGive(s_lock);
    return n;
}

size_t app_events_count(void)
{
    if (!s_lock) {
        return 0;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const size_t n = s_count;
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
