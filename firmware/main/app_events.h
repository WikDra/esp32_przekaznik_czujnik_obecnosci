/*
 * Historia ostatnich zdarzeń (obecność + zmiany stanu lampy).
 *
 * Po to, żeby dało się rano sprawdzić, czy w nocy ktoś wszedł do pomieszczenia, czy
 * czujnikowi się „przywidziało”. Bufor jest cykliczny i trzymany w RAM - restart
 * urządzenia czyści historię (zapis każdego zdarzenia do NVS zużywałby pamięć
 * nieulotną bez sensownego powodu).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    APP_EVENT_PRESENCE_START = 0, /* obecność uznana (po filtrach)        */
    APP_EVENT_PRESENCE_END = 1,   /* obecność utracona                    */
    APP_EVENT_LIGHT_ON = 2,
    APP_EVENT_LIGHT_OFF = 3,
} app_event_type_t;

typedef struct {
    uint8_t type;         /* app_event_type_t                              */
    uint16_t distance_cm; /* odległość celu przy zdarzeniu obecności        */
    uint32_t uptime_s;    /* czas od startu urządzenia                     */
    time_t wall;          /* czas zegarowy albo 0, gdy brak synchronizacji  */
    uint16_t duration_s;  /* dla PRESENCE_END: jak długo trwała obecność    */
    char src[12];         /* źródło zmiany stanu lampy                     */
} app_event_t;

void app_events_init(void);

/* Dopisuje zdarzenie obecności (src może być NULL). */
void app_events_add_presence(bool present, uint16_t distance_cm, uint16_t duration_s);

/* Dopisuje zdarzenie zmiany stanu lampy. */
void app_events_add_light(bool on, const char *src);

/* Kopiuje historię od najnowszego zdarzenia. Zwraca liczbę skopiowanych wpisów. */
size_t app_events_get(app_event_t *out, size_t max);

/* Ile zdarzeń obecności zarejestrowano od startu urządzenia. */
uint32_t app_events_presence_count(void);

const char *app_event_type_name(uint8_t type);

#ifdef __cplusplus
}
#endif
