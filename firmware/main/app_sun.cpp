#include "app_sun.h"
#include "app_priv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_sntp.h>
#include <sdkconfig.h>

static const char *TAG = "sun";

#define DEG2RAD (M_PI / 180.0)
#define RAD2DEG (180.0 / M_PI)

/* Kąt środka słońca poniżej horyzontu dla wschodu/zachodu (refrakcja + promień tarczy). */
#define SUN_ALTITUDE_DEG (-0.833)

static bool s_sntp_started;
static volatile bool s_time_valid;

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    s_time_valid = true;
    time_t now = time(NULL);
    char buf[32];
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm_local);
    ESP_LOGI(TAG, "time synchronized: %s (%s)", buf, app_settings()->tz);
}

esp_err_t app_sun_init(void)
{
    app_sun_apply_timezone();

    /* Zegar mógł już zostać ustawiony (np. przez Mattera po sparowaniu) - rok > 2023
     * oznacza, że mamy sensowny czas. */
    time_t now = time(NULL);
    struct tm tm_utc;
    gmtime_r(&now, &tm_utc);
    if (tm_utc.tm_year + 1900 >= 2024) {
        s_time_valid = true;
    }
    return ESP_OK;
}

void app_sun_apply_timezone(void)
{
    const char *tz = app_settings()->tz;
    if (tz[0] == '\0') {
        tz = "UTC0";
    }
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "timezone set to '%s'", tz);
}

void app_sun_start(void)
{
    if (s_sntp_started) {
        return;
    }
    const char *server = app_settings()->ntp_server;
    if (server[0] == '\0') {
        server = "pool.ntp.org";
    }

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(server);
    config.start = true;
    config.server_from_dhcp = false;
    config.sync_cb = sntp_sync_cb;
    config.smooth_sync = false;

    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return;
    }
    s_sntp_started = true;
    ESP_LOGI(TAG, "SNTP client started (server %s)", server);
}

/* ------------------------------------------------------------ obliczenia */

static double julian_from_time(time_t t) { return ((double)t / 86400.0) + 2440587.5; }
static time_t time_from_julian(double jd) { return (time_t)llround((jd - 2440587.5) * 86400.0); }

/* Wschód/zachód dla dnia, w którym leży `local_noon` (czas lokalny w połowie dnia).
 * Zwraca false przy dniu/nocy polarnej. */
static bool compute_sun(time_t local_noon, double lat_deg, double lon_deg, time_t *sunrise, time_t *sunset,
                        bool *polar_day, bool *polar_night)
{
    *polar_day = false;
    *polar_night = false;

    const double n = round(julian_from_time(local_noon) - 2451545.0 + 0.0008);
    const double j_star = n - lon_deg / 360.0;

    const double mean_anomaly = fmod(357.5291 + 0.98560028 * j_star, 360.0);
    const double m_rad = mean_anomaly * DEG2RAD;
    const double center = 1.9148 * sin(m_rad) + 0.02 * sin(2 * m_rad) + 0.0003 * sin(3 * m_rad);
    const double ecliptic_lon = fmod(mean_anomaly + center + 180.0 + 102.9372, 360.0);
    const double l_rad = ecliptic_lon * DEG2RAD;

    const double j_transit = 2451545.0 + j_star + 0.0053 * sin(m_rad) - 0.0069 * sin(2 * l_rad);

    const double sin_dec = sin(l_rad) * sin(23.44 * DEG2RAD);
    const double dec = asin(sin_dec);
    const double lat = lat_deg * DEG2RAD;

    const double cos_omega =
        (sin(SUN_ALTITUDE_DEG * DEG2RAD) - sin(lat) * sin_dec) / (cos(lat) * cos(dec));

    if (cos_omega > 1.0) {
        *polar_night = true; /* słońce nie wschodzi */
        return false;
    }
    if (cos_omega < -1.0) {
        *polar_day = true; /* słońce nie zachodzi */
        return false;
    }

    const double omega = acos(cos_omega) * RAD2DEG;
    *sunrise = time_from_julian(j_transit - omega / 360.0);
    *sunset = time_from_julian(j_transit + omega / 360.0);
    return true;
}

void app_sun_get(app_sun_state_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    app_settings_t *cfg = app_settings();
    out->now = time(NULL);
    out->time_valid = s_time_valid;

    if (!s_time_valid) {
        out->is_night = true; /* fail-safe: automatyka ma działać */
        return;
    }

    /* Południe lokalnego dnia, w którym jesteśmy - dzięki temu zaokrąglenie dnia
     * julijskiego jest jednoznaczne, niezależnie od strefy czasowej. */
    struct tm tm_local;
    localtime_r(&out->now, &tm_local);
    tm_local.tm_hour = 12;
    tm_local.tm_min = 0;
    tm_local.tm_sec = 0;
    const time_t local_noon = mktime(&tm_local);

    const double lat = (double)cfg->lat_udeg / 1000000.0;
    const double lon = (double)cfg->lon_udeg / 1000000.0;

    if (!compute_sun(local_noon, lat, lon, &out->sunrise, &out->sunset, &out->polar_day,
                     &out->polar_night)) {
        out->is_night = out->polar_night;
        return;
    }

    out->night_from = out->sunset + (int)cfg->sunset_off_min * 60;
    out->night_to = out->sunrise + (int)cfg->sunrise_off_min * 60;
    out->is_night = (out->now >= out->night_from) || (out->now <= out->night_to);
}

bool app_sun_is_night(void)
{
    app_sun_state_t st;
    app_sun_get(&st);
    return st.is_night;
}
