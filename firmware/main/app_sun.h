/*
 * Czas z sieci (SNTP) + wyliczanie wschodu/zachodu słońca.
 *
 * Zastępuje czujnik zmierzchu: zamiast mierzyć światło, liczymy pozycję słońca dla
 * zadanych współrzędnych i lokalnej daty. Używany algorytm to uproszczone równanie
 * wschodu słońca (dokładność ~1-2 min, w zupełności wystarczająca dla oświetlenia).
 *
 * Fail-safe: dopóki czas nie jest zsynchronizowany, app_sun_is_night() zwraca true,
 * żeby automatyka obecności działała normalnie (lepiej zapalić w dzień niż nie zapalić
 * w nocy).
 */
#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool time_valid;   /* czy SNTP zsynchronizował zegar            */
    time_t now;
    time_t sunrise;    /* wschód dla lokalnej daty (bez offsetu)    */
    time_t sunset;     /* zachód dla lokalnej daty (bez offsetu)    */
    time_t night_from; /* zachód + offset                           */
    time_t night_to;   /* wschód + offset                           */
    bool is_night;
    bool polar_day;    /* słońce nie zachodzi (skrajne szerokości)  */
    bool polar_night;  /* słońce nie wschodzi                       */
} app_sun_state_t;

/* Ustawia strefę czasową z konfiguracji. Wołane raz na starcie, przed app_sun_start(). */
esp_err_t app_sun_init(void);

/* Startuje (lub restartuje) klienta SNTP - wołane po uzyskaniu adresu IP. */
void app_sun_start(void);

/* Przelicza strefę czasową po zmianie ustawień (POSIX TZ). */
void app_sun_apply_timezone(void);

/* Czy teraz jest "noc" wg zachodu/wschodu i offsetów. Przy braku synchronizacji: true. */
bool app_sun_is_night(void);

void app_sun_get(app_sun_state_t *out);

#ifdef __cplusplus
}
#endif
