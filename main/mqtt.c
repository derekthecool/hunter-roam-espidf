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

// Current default watering duration (minutes). Updated via the HA number
// entity "Watering Duration" and used when a start command arrives
// without an explicit "time" field.
static volatile uint8_t s_duration_minutes = CONFIG_HUNTER_DEFAULT_RUN_MINUTES;

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

    // --- Zone switches ---
    for (uint8_t z = 1; z <= CONFIG_HUNTER_ZONE_COUNT; z++) {
        snprintf(topic, sizeof(topic),
                 "homeassistant/switch/%s_zone_%u/config", s_unique_prefix, z);

        // pl_on sends bare start (no time) — firmware uses the duration
        // set via the number entity below (or Kconfig default).
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

        cJSON_AddStringToObject(root, "pl_on",  "{\"action\":\"start\"}");
        cJSON_AddStringToObject(root, "pl_off", "{\"action\":\"stop\"}");
        cJSON_AddBoolToObject  (root, "pl_opt", true);
        cJSON_AddStringToObject(root, "ic", "mdi:sprinkler-variant");

        cJSON *device = cJSON_CreateObject();
        cJSON_AddStringToObject(device, "ids", s_unique_prefix);
        cJSON_AddStringToObject(device, "name", CONFIG_HUNTER_HOSTNAME);
        cJSON_AddStringToObject(device, "mf", "Hunter");
        cJSON_AddStringToObject(device, "mdl", "X2 (via ESP32)");
        cJSON_AddStringToObject(device, "sw", "hunter-roam-espidf v0.1.0");
        cJSON_AddItemToObject(root, "device", device);

        char *json = cJSON_PrintUnformatted(root);
        esp_mqtt_client_publish(s_client, topic, json, 0, 1, 1);
        free(json);
        cJSON_Delete(root);
    }

    // --- Duration number entity (one per device, not per zone) ---
    snprintf(topic, sizeof(topic),
             "homeassistant/number/%s_duration/config", s_unique_prefix);

    cJSON *droot = cJSON_CreateObject();
    char dname[48];
    snprintf(dname, sizeof(dname), "%s Watering Duration", CONFIG_HUNTER_HOSTNAME);
    cJSON_AddStringToObject(droot, "name", dname);

    char duniq[80];
    snprintf(duniq, sizeof(duniq), "%s_duration", s_unique_prefix);
    cJSON_AddStringToObject(droot, "uniq_id", duniq);

    // Explicit object_id so HA creates a predictable entity_id.
    // HA prepends the device name, so just use "watering_duration"
    // → entity becomes number.hunter_x2_watering_duration
    cJSON_AddStringToObject(droot, "object_id", "watering_duration");

    char dcmd_t[96];
    snprintf(dcmd_t, sizeof(dcmd_t), "%s/duration/set", s_topic_prefix);
    cJSON_AddStringToObject(droot, "cmd_t", dcmd_t);

    char dstat_t[96];
    snprintf(dstat_t, sizeof(dstat_t), "%s/duration/state", s_topic_prefix);
    cJSON_AddStringToObject(droot, "stat_t", dstat_t);

    cJSON_AddNumberToObject(droot, "min", 1);
    cJSON_AddNumberToObject(droot, "max", 240);
    cJSON_AddNumberToObject(droot, "step", 1);
    cJSON_AddStringToObject(droot, "ic", "mdi:timer-outline");

    cJSON *ddevice = cJSON_CreateObject();
    cJSON_AddStringToObject(ddevice, "ids", s_unique_prefix);
    cJSON_AddStringToObject(ddevice, "name", CONFIG_HUNTER_HOSTNAME);
    cJSON_AddStringToObject(ddevice, "mf", "Hunter");
    cJSON_AddStringToObject(ddevice, "mdl", "X2 (via ESP32)");
    cJSON_AddStringToObject(ddevice, "sw", "hunter-roam-espidf v0.1.0");
    cJSON_AddItemToObject(droot, "device", ddevice);

    char *djson = cJSON_PrintUnformatted(droot);
    esp_mqtt_client_publish(s_client, topic, djson, 0, 1, 1);
    free(djson);
    cJSON_Delete(droot);

    // Publish current duration state
    char dur_payload[8];
    snprintf(dur_payload, sizeof(dur_payload), "%d", s_duration_minutes);
    char dur_state_topic[96];
    snprintf(dur_state_topic, sizeof(dur_state_topic), "%s/duration/state", s_topic_prefix);
    esp_mqtt_client_publish(s_client, dur_state_topic, dur_payload, 0, 1, 1);

    ESP_LOGI(TAG, "HA discovery published: %d zones + duration entity",
             CONFIG_HUNTER_ZONE_COUNT);
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
            uint8_t minutes = s_duration_minutes;
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

static void handle_duration_msg(const char *data, int data_len)
{
    // Payload is a bare number (HA number entity) — e.g. "10".
    char buf[8];
    int n = data_len < (int)sizeof(buf) - 1 ? data_len : (int)sizeof(buf) - 1;
    memcpy(buf, data, n);
    buf[n] = '\0';

    int val = atoi(buf);
    if (val >= 1 && val <= 240) {
        s_duration_minutes = (uint8_t)val;
        ESP_LOGI(TAG, "duration set to %d minutes", val);
        // Publish state back so HA's number entity stays in sync.
        char state_topic[96];
        snprintf(state_topic, sizeof(state_topic), "%s/duration/state", s_topic_prefix);
        esp_mqtt_client_publish(s_client, state_topic, buf, 0, 1, 1);
    } else {
        ESP_LOGW(TAG, "duration %d out of range [1, 240]", val);
    }
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

        char sub_zone[96];
        snprintf(sub_zone, sizeof(sub_zone), "%s/zone/+/set", s_topic_prefix);
        esp_mqtt_client_subscribe(s_client, sub_zone, 0);

        char sub_prog[96];
        snprintf(sub_prog, sizeof(sub_prog), "%s/program/+/set", s_topic_prefix);
        esp_mqtt_client_subscribe(s_client, sub_prog, 0);

        char sub_cmd[96];
        snprintf(sub_cmd, sizeof(sub_cmd), "%s/cmd", s_topic_prefix);
        esp_mqtt_client_subscribe(s_client, sub_cmd, 0);

        char sub_dur[96];
        snprintf(sub_dur, sizeof(sub_dur), "%s/duration/set", s_topic_prefix);
        esp_mqtt_client_subscribe(s_client, sub_dur, 0);

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
        } else if (strstr(topic, "/duration/")) {
            handle_duration_msg(event->data, event->data_len);
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
