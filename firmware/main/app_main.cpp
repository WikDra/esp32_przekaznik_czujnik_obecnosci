/*
 * Swiatlo LD2420 - Matter (over Wi-Fi) lamp controller with a local HTTP panel.
 *
 * Endpoint 1: On/Off Light   -> 5 V relay (low level trigger) -> mains lamp
 * Endpoint 2: Occupancy      -> HLK-LD2420 24 GHz presence radar
 *
 * Commissioning: BLE (default test setup code 20202021 / discriminator 3840 unless
 * changed in menuconfig). After commissioning the HTTP panel is reachable over the
 * Wi-Fi network provisioned by the Matter controller.
 */
#include "app_priv.h"
#include "ld2420.h"

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <sdkconfig.h>

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <esp_matter.h>
#include <esp_matter_ota.h>
#include <setup_payload/OnboardingCodesUtil.h>

static const char *TAG = "app_main";

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

volatile bool app_matter_commissioned = false;
volatile bool app_net_connected = false;

static void open_commissioning_window_if_necessary()
{
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() != 0) {
        return;
    }
    chip::CommissioningWindowManager &mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    if (mgr.IsCommissioningWindowOpen()) {
        return;
    }
    CHIP_ERROR err = mgr.OpenBasicCommissioningWindow(chip::System::Clock::Seconds16(300),
                                                      chip::CommissioningWindowAdvertisement::kDnssdOnly);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to open commissioning window: %" CHIP_ERROR_FORMAT, err.Format());
    }
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "IP address assigned - starting HTTP panel");
        app_net_connected = true;
        app_web_start();
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
        app_matter_commissioned = true;
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGW(TAG, "Commissioning failed, fail safe timer expired");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved:
        ESP_LOGI(TAG, "Fabric removed");
        app_matter_commissioned = chip::Server::GetInstance().GetFabricTable().FabricCount() != 0;
        open_commissioning_window_if_necessary();
        break;

    case chip::DeviceLayer::DeviceEventType::kBLEDeinitialized:
        ESP_LOGI(TAG, "BLE deinitialized and memory reclaimed");
        break;

    default:
        break;
    }
}

static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identify: type=%u effect=%u variant=%u", type, effect_id, effect_variant);
    return ESP_OK;
}

/* Matter -> relay. Runs on the Matter thread. */
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    if (type == PRE_UPDATE && endpoint_id == app_light_endpoint_id && cluster_id == OnOff::Id &&
        attribute_id == OnOff::Attributes::OnOff::Id) {
        /* Skip updates that only mirror the current state - those are the reports we push
         * ourselves from app_light_set() (panel, button, automation, boot defaults).
         * Without this the reported source would always end up as "matter". */
        if (val->val.b != app_light_get()) {
            app_light_set(val->val.b, LIGHT_SRC_MATTER);
        }
    }
    return ESP_OK;
}

extern "C" void app_main()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    esp_log_level_set("esp_matter_attribute", ESP_LOG_WARN);

    ESP_ERROR_CHECK(app_settings_init());
    ESP_ERROR_CHECK(app_light_init());

    /* --- Matter data model --- */
    /* app_light_init() may already have switched the lamp on (power-cycle override),
     * and the restore-state option is applied here, so the OnOff attribute starts
     * with the real relay state. */
    bool initial_on = app_light_get();
    if (!initial_on && app_settings()->restore_state && app_settings()->last_on) {
        app_light_set(true, LIGHT_SRC_BOOT);
        initial_on = true;
    }

    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ESP_ERROR_CHECK(node ? ESP_OK : ESP_FAIL);

    on_off_light::config_t light_config;
    light_config.on_off.on_off = initial_on;
    endpoint_t *light_ep = on_off_light::create(node, &light_config, ENDPOINT_FLAG_NONE, NULL);
    ESP_ERROR_CHECK(light_ep ? ESP_OK : ESP_FAIL);
    app_light_endpoint_id = endpoint::get_id(light_ep);

    occupancy_sensor::config_t occ_config;
    /* LD2420 is an mmWave radar; Matter has no "radar" sensor type enum, but it does
     * have a radar feature flag (at least one feature must be enabled). */
    occ_config.occupancy_sensing.occupancy_sensor_type =
        chip::to_underlying(OccupancySensing::OccupancySensorTypeEnum::kPir);
    occ_config.occupancy_sensing.occupancy_sensor_type_bitmap =
        chip::to_underlying(OccupancySensing::OccupancySensorTypeBitmap::kPir);
    occ_config.occupancy_sensing.feature_flags = cluster::occupancy_sensing::feature::radar::get_id();
    endpoint_t *occ_ep = occupancy_sensor::create(node, &occ_config, ENDPOINT_FLAG_NONE, NULL);
    ESP_ERROR_CHECK(occ_ep ? ESP_OK : ESP_FAIL);
    app_occupancy_endpoint_id = endpoint::get_id(occ_ep);

    ESP_LOGI(TAG, "endpoints: light=%u occupancy=%u", app_light_endpoint_id, app_occupancy_endpoint_id);

    /* --- start Matter --- */
    err = esp_matter::start(app_event_cb);
    ESP_ERROR_CHECK(err);

    app_matter_commissioned = chip::Server::GetInstance().GetFabricTable().FabricCount() != 0;

    if (!app_matter_commissioned) {
        /* Prints the QR code URL and the 11-digit manual pairing code. */
        PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
    }

    /* --- sensor --- */
    err = ld2420_init(app_light_on_presence);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LD2420 init failed: %s (device keeps running without presence input)",
                 esp_err_to_name(err));
    }

    /* --- restore lamp state --- */
    ESP_LOGI(TAG, "ready (commissioned=%d, lamp=%s)", app_matter_commissioned ? 1 : 0,
             app_light_get() ? "on" : "off");
}
