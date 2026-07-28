#include "mqtt.h"

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "hunter_cmd.h"
#include "mqtt_client.h"
#include "ota.h"
#include "wifi.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "cJSON.h"

static const char *TAG = "mqtt";

#define DEFAULT_RUN_MINUTES  15

static esp_mqtt_client_handle_t s_client = NULL;
static char s_topic_prefix[64];   // "hunter/<hostname>"
static char s_unique_prefix[64];  // "<hostname>_<mac_suffix>"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void build_prefixes(void)
{
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    // Use hostname + last 4 hex of MAC so multiple devices on one network
    // don't collide on discovery unique_ids.
    snprintf(s_topic_prefix, sizeof(s_topic_prefix),
             "hunter/%s", CONFIG_HUNTER_HOSTNAME);
    snprintf(s_unique_prefix, sizeof(s_unique_prefix),
             "%s_%02x%02x", CONFIG_HUNTER_HOSTNAME, mac[4], mac[5]);
}

static void publish_ha_discovery(void)
{
    char topic[128];

    for (uint8_t z = 1; z <= CONFIG_HUNTER_ZONE_COUNT; z++) {
        snprintf(topic, sizeof(topic),
                 "homeassistant/switch/%s_zone_%u/config", s_unique_prefix, z);

        // cmd_t pl_on embeds the duration. pl_off just says stop.
        char cmd_payload_on[40];
        snprintf(cmd_payload_on, sizeof(cmd_payload_on),
                 "{\"action\":\"start\",\"time\":%d}", DEFAULT_RUN_MINUTES);

        char name[40];
        snprintf(name, sizeof(name), "%s Zone %u", CONFIG_HUNTER_HOSTNAME, z);

        cJSON *root = cJSON_CreateObject();
        cJSON_AddStringToObject(root, "name", name);

        char uniq[80];
        snprintf(uniq, sizeof(uniq), "%s_zone_%u", s_unique_prefix, z);
        cJSON_AddStringToObject(root, "uniq_id", uniq);

        char cmd_t[80];
        snprintf(cmd_t, sizeof(cmd_t), "%s/zone/%u/set", s_topic_prefix, z);
        cJSON_AddStringToObject(root, "cmd_t", cmd_t);

        char stat_t[80];
        snprintf(stat_t, sizeof(stat_t), "%s/zone/%u/state", s_topic_prefix, z);
        cJSON_AddStringToObject(root, "stat_t", stat_t);

        cJSON_AddStringToObject(root, "pl_on",  cmd_payload_on);
        cJSON_AddStringToObject(root, "pl_off", "{\"action\":\"stop\"}");
        cJSON_AddBoolToObject  (root, "pl_opt", true);
        cJSON_AddStringToObject(root, "ic", "mdi:sprinkler-variant");

        char *json = cJSON_PrintUnformatted(root);

        esp_mqtt_client_publish(s_client, topic, json, 0, 1, 1);

        free(json);
        cJSON_Delete(root);
    }

    ESP_LOGI(TAG, "HA discovery published for %d zones", CONFIG_HUNTER_ZONE_COUNT);
}

void mqtt_publish_zone_state(uint8_t zone, bool state)
{
    if (s_client == NULL) return;
    char topic[96];
    char payload[48];
    snprintf(topic, sizeof(topic), "%s/zone/%u/state",
             s_topic_prefix, zone);
    snprintf(payload, sizeof(payload), "%s",
             state ? "{\"action\":\"start\"}" : "{\"action\":\"stop\"}");
    esp_mqtt_client_publish(s_client, topic, payload, 0, 0, 0);
}

void *mqtt_get_client(void)
{
    return s_client;
}

// ---------------------------------------------------------------------------
// Incoming message parsing
// ---------------------------------------------------------------------------

static void handle_zone_msg(const char *topic, const char *data, int data_len)
{
    uint8_t zone;
    if (sscanf(topic, "hunter/%*[^/]/zone/%hhu", &zone) != 1) {
        ESP_LOGW(TAG, "couldn't parse zone from topic: %s", topic);
        return;
    }

    // Parse JSON. Allow either bare {"action":"start"} or with "time".
    char buf[128];
    int n = data_len < (int)sizeof(buf) - 1 ? data_len : (int)sizeof(buf) - 1;
    memcpy(buf, data, n);
    buf[n] = '\0';

    cJSON *root = cJSON_ParseWithLength(buf, n);
    if (root == NULL) {
        ESP_LOGW(TAG, "bad JSON on zone %u: %s", zone, buf);
        return;
    }

    const cJSON *action = cJSON_GetObjectItem(root, "action");
    if (action && cJSON_IsString(action)) {
        if (strcmp(action->valuestring, "start") == 0) {
            uint8_t minutes = DEFAULT_RUN_MINUTES;
            const cJSON *t = cJSON_GetObjectItem(root, "time");
            if (t && cJSON_IsNumber(t)) {
                minutes = (uint8_t)t->valuedouble;
            }
            hunter_cmd_send_zone_start(zone, minutes);
        } else if (strcmp(action->valuestring, "stop") == 0) {
            hunter_cmd_send_zone_stop(zone);
        }
    }
    cJSON_Delete(root);
}

static void handle_program_msg(const char *topic, const char *data, int data_len)
{
    (void)data; (void)data_len;

    uint8_t prog;
    if (sscanf(topic, "hunter/%*[^/]/program/%hhu", &prog) != 1) return;
    hunter_cmd_send_program(prog);
}

static void handle_cmd_msg(const char *data, int data_len)
{
    char buf[64];
    int n = data_len < (int)sizeof(buf) - 1 ? data_len : (int)sizeof(buf) - 1;
    memcpy(buf, data, n);
    buf[n] = '\0';

    cJSON *root = cJSON_ParseWithLength(buf, n);
    if (root == NULL) return;
    const cJSON *a = cJSON_GetObjectItem(root, "action");
    if (a && cJSON_IsString(a)) {
        if      (strcmp(a->valuestring, "stop_all") == 0) hunter_cmd_send_stop_all();
        else if (strcmp(a->valuestring, "reboot")   == 0) hunter_cmd_send_reboot();
        else if (strcmp(a->valuestring, "ota")      == 0) hunter_cmd_send_ota();
    }
    cJSON_Delete(root);
}

// ---------------------------------------------------------------------------
// MQTT event handler
// ---------------------------------------------------------------------------

static void mqtt_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)base;
    esp_mqtt_event_handle_t event = data;

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "connected to broker");

        char sub_zone[80];
        snprintf(sub_zone, sizeof(sub_zone), "%s/zone/+/set", s_topic_prefix);
        esp_mqtt_client_subscribe(s_client, sub_zone, 0);

        char sub_prog[80];
        snprintf(sub_prog, sizeof(sub_prog), "%s/program/+/set", s_topic_prefix);
        esp_mqtt_client_subscribe(s_client, sub_prog, 0);

        char sub_cmd[80];
        snprintf(sub_cmd, sizeof(sub_cmd), "%s/cmd", s_topic_prefix);
        esp_mqtt_client_subscribe(s_client, sub_cmd, 0);

        publish_ha_discovery();
        break;
    }
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "disconnected");
        break;
    case MQTT_EVENT_DATA: {
        // Null-terminate topic for sscanf.
        char topic[128];
        int tn = event->topic_len < (int)sizeof(topic) - 1
                 ? event->topic_len : (int)sizeof(topic) - 1;
        memcpy(topic, event->topic, tn);
        topic[tn] = '\0';

        if (strstr(topic, "/zone/")) {
            handle_zone_msg(topic, event->data, event->data_len);
        } else if (strstr(topic, "/program/")) {
            handle_program_msg(topic, event->data, event->data_len);
        } else if (strstr(topic, "/cmd")) {
            handle_cmd_msg(event->data, event->data_len);
        }
        break;
    }
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
        break;
    default:
        break;
    }
}

// ---------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------

esp_err_t mqtt_start(EventGroupHandle_t wifi_event_group)
{
    // Wait for WiFi before starting MQTT.
    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
        portMAX_DELAY);
    (void)bits;

    build_prefixes();

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = CONFIG_HUNTER_MQTT_BROKER_URI,
        .credentials.username = CONFIG_HUNTER_MQTT_USER,
        .credentials.authentication.password = CONFIG_HUNTER_MQTT_PASSWORD,
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (s_client == NULL) {
        ESP_LOGE(TAG, "esp_mqtt_client_init failed");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID,
                                   mqtt_event_handler, NULL);
    esp_mqtt_client_start(s_client);

    ESP_LOGI(TAG, "MQTT client started — broker %s, prefix %s",
             CONFIG_HUNTER_MQTT_BROKER_URI, s_topic_prefix);
    return ESP_OK;
}
