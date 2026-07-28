#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "hunter_cmd.h"
#include "hunter_rmt.h"
#include "mqtt.h"
#include "nvs_flash.h"
#include "ota.h"
#include "wifi.h"

static const char *TAG = "main";

// FreeRTOS task adapters — esp_err_t-returning functions wrapped in
// void-returning task bodies so xTaskCreate stops complaining about
// function-type casts.
static void mqtt_start_task(void *arg)
{
    mqtt_start((EventGroupHandle_t)arg);
    vTaskDelete(NULL);
}

static void ota_boot_task(void *arg)
{
    (void)arg;
    ota_check_and_apply();
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "hunter-roam-espidf booting — %d zones",
             CONFIG_HUNTER_ZONE_COUNT);

    // NVS — required by WiFi
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }

    esp_netif_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // 1. RMT driver (independent of WiFi/MQTT)
    ESP_ERROR_CHECK(hunter_rmt_init(CONFIG_HUNTER_RMT_GPIO));

    // 2. Command queue + safety timers + dispatch task
    ESP_ERROR_CHECK(hunter_cmd_init());

    // 3. WiFi → blocks until IP obtained (via event group)
    EventGroupHandle_t wifi_eg = NULL;
    ESP_ERROR_CHECK(wifi_init(&wifi_eg));

    // 4. MQTT — start it on a separate task; it waits on wifi_eg internally
    xTaskCreate(mqtt_start_task, "mqtt_start", 6144, wifi_eg, 5, NULL);

    // 5. Optional OTA check on boot
#if CONFIG_HUNTER_OTA_CHECK_ON_BOOT
    xTaskCreate(ota_boot_task, "ota_boot", 8192, NULL, 4, NULL);
#endif

    ESP_LOGI(TAG, "boot sequence complete");
}
