#include "ota.h"

#include "esp_https_ota.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ota";

esp_err_t ota_check_and_apply(void)
{
    if (strlen(CONFIG_HUNTER_OTA_URL) == 0) {
        ESP_LOGI(TAG, "OTA disabled — CONFIG_HUNTER_OTA_URL is empty");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "checking %s", CONFIG_HUNTER_OTA_URL);

    esp_http_client_config_t http_cfg = {
        .url = CONFIG_HUNTER_OTA_URL,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = { .http_config = &http_cfg };

    esp_err_t ret = esp_https_ota(&ota_cfg);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "OTA success — rebooting in 1s");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s — continuing on current firmware",
                 esp_err_to_name(ret));
    }
    return ret;
}
