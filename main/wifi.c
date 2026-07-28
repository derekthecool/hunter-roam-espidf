#include "wifi.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "wifi";

static EventGroupHandle_t s_eg = NULL;

static void event_handler(void *arg, esp_event_base_t base,
                          int32_t id, void *data)
{
    (void)arg;

    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGW(TAG, "disconnected — retrying");
            xEventGroupClearBits(s_eg, WIFI_CONNECTED_BIT);
            esp_wifi_connect();
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&e->ip_info.ip));
        xEventGroupSetBits(s_eg, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_init(EventGroupHandle_t *out_event_group)
{
    s_eg = xEventGroupCreate();
    if (s_eg == NULL) {
        return ESP_ERR_NO_MEM;
    }

    // Initialise TCP/IP adapter + default STA netif
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &event_handler, NULL, NULL);

    wifi_config_t wifi_cfg = {0};
    strncpy((char *)wifi_cfg.sta.ssid, CONFIG_HUNTER_WIFI_SSID,
            sizeof(wifi_cfg.sta.ssid) - 1);
    strncpy((char *)wifi_cfg.sta.password, CONFIG_HUNTER_WIFI_PASSWORD,
            sizeof(wifi_cfg.sta.password) - 1);

    ret = esp_wifi_set_mode(WIFI_MODE_STA);
    if (ret != ESP_OK) return ret;
    ret = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    if (ret != ESP_OK) return ret;
    ret = esp_wifi_start();
    if (ret != ESP_OK) return ret;

    ESP_LOGI(TAG, "WiFi STA init complete — joining \"%s\"",
             CONFIG_HUNTER_WIFI_SSID);

    if (out_event_group) *out_event_group = s_eg;
    return ESP_OK;
}
