#include "hunter_cmd.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/timers.h"
#include "hunter_rmt.h"
#include "mqtt.h"
#include "ota.h"
#include <string.h>

static const char *TAG = "hunter_cmd";

#define CMD_QUEUE_LEN  16
#define WDT_TIMEOUT_S  30
// Cap on safety timer — defends against a runaway time=240 cmd if HA forgets
// the zone exists. The discovery payload's default is 15 min; HA-side
// `time` parameter can override up to the actual command limit (240 min).

static QueueHandle_t  s_queue         = NULL;
static TimerHandle_t  s_safety_timers[CONFIG_HUNTER_ZONE_COUNT];

static void safety_timer_cb(TimerHandle_t timer)
{
    int zone = (int)(intptr_t)pvTimerGetTimerID(timer);
    ESP_LOGW(TAG, "safety timer expired for zone %d — auto-stopping", zone);
    hunter_cmd_send_zone_stop((uint8_t)zone);
}

static void arm_safety_timer(uint8_t zone, uint8_t minutes)
{
    if (zone < 1 || zone > CONFIG_HUNTER_ZONE_COUNT) return;
    TimerHandle_t t = s_safety_timers[zone - 1];
    if (t == NULL) return;

    // Replace any prior timeout.
    xTimerStop(t, 0);

    TickType_t period = pdMS_TO_TICKS((uint32_t)minutes * 60 * 1000);
    if (period == 0) period = pdMS_TO_TICKS(60 * 1000);  // min 1 min fallback
    xTimerChangePeriod(t, period, 0);
    xTimerStart(t, 0);
}

static void disarm_safety_timer(uint8_t zone)
{
    if (zone < 1 || zone > CONFIG_HUNTER_ZONE_COUNT) return;
    TimerHandle_t t = s_safety_timers[zone - 1];
    if (t != NULL) xTimerStop(t, 0);
}

static void dispatch_zone_start(uint8_t zone, uint8_t minutes)
{
    hunter_err_t err = hunter_rmt_start_zone(zone, minutes);
    if (err == HUNTER_OK) {
        ESP_LOGI(TAG, "zone %u → ON (%u min)", zone, minutes);
        mqtt_publish_zone_state(zone, true);
        arm_safety_timer(zone, minutes);
    } else {
        ESP_LOGE(TAG, "start_zone(%u, %u) failed: %d", zone, minutes, err);
    }
}

static void dispatch_zone_stop(uint8_t zone)
{
    hunter_err_t err = hunter_rmt_stop_zone(zone);
    if (err == HUNTER_OK) {
        ESP_LOGI(TAG, "zone %u → OFF", zone);
        mqtt_publish_zone_state(zone, false);
        disarm_safety_timer(zone);
    } else {
        ESP_LOGE(TAG, "stop_zone(%u) failed: %d", zone, err);
    }
}

static void dispatch_program(uint8_t program)
{
    hunter_err_t err = hunter_rmt_run_program(program);
    ESP_LOGI(TAG, "program %u → %s", program,
             err == HUNTER_OK ? "triggered" : "FAIL");
}

static void dispatch_stop_all(void)
{
    for (uint8_t z = 1; z <= CONFIG_HUNTER_ZONE_COUNT; z++) {
        dispatch_zone_stop(z);
        vTaskDelay(pdMS_TO_TICKS(50));  // avoid back-to-back RMT bursts
    }
}

static void cmd_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);

    hunter_cmd_t cmd;
    while (1) {
        // Poll with a finite timeout so we can reset the watchdog periodically
        // even when no commands arrive — otherwise the WDT fires every 30s
        // when the queue sits idle (the original portMAX_DELAY bug).
        if (xQueueReceive(s_queue, &cmd, pdMS_TO_TICKS(1000)) == pdTRUE) {
            switch (cmd.type) {
            case HUNTER_CMD_ZONE_START: dispatch_zone_start(cmd.zone, cmd.minutes); break;
            case HUNTER_CMD_ZONE_STOP:  dispatch_zone_stop(cmd.zone);               break;
            case HUNTER_CMD_PROGRAM:    dispatch_program(cmd.program);              break;
            case HUNTER_CMD_STOP_ALL:   dispatch_stop_all();                        break;
            case HUNTER_CMD_REBOOT:
                ESP_LOGW(TAG, "reboot requested — restarting in 1s");
                vTaskDelay(pdMS_TO_TICKS(1000));
                esp_restart();
                break;
            case HUNTER_CMD_OTA:
                ota_check_and_apply();
                break;
            }
        }
        esp_task_wdt_reset();
    }
}

esp_err_t hunter_cmd_init(void)
{
    s_queue = xQueueCreate(CMD_QUEUE_LEN, sizeof(hunter_cmd_t));
    if (s_queue == NULL) {
        ESP_LOGE(TAG, "xQueueCreate failed");
        return ESP_ERR_NO_MEM;
    }

    // Per-zone one-shot safety timers. Timer ID is the zone number.
    for (int z = 1; z <= CONFIG_HUNTER_ZONE_COUNT; z++) {
        char name[16];
        snprintf(name, sizeof(name), "safety_z%d", z);
        s_safety_timers[z - 1] = xTimerCreate(name, portMAX_DELAY, pdFALSE,
                                              (void *)(intptr_t)z,
                                              safety_timer_cb);
        if (s_safety_timers[z - 1] == NULL) {
            ESP_LOGE(TAG, "xTimerCreate failed for zone %d", z);
            return ESP_ERR_NO_MEM;
        }
    }

    BaseType_t ok = xTaskCreate(cmd_task, "hunter_cmd", 4096, NULL,
                                5, NULL);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate failed");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "cmd queue + %d safety timers + task up",
             CONFIG_HUNTER_ZONE_COUNT);
    return ESP_OK;
}

// ---- producer API ----

static void push(const hunter_cmd_t *cmd)
{
    if (s_queue == NULL) return;
    if (xQueueSend(s_queue, cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cmd queue full — dropping");
    }
}

void hunter_cmd_send_zone_start(uint8_t zone, uint8_t minutes)
{
    hunter_cmd_t c = { .type = HUNTER_CMD_ZONE_START,
                       .zone = zone, .minutes = minutes };
    push(&c);
}

void hunter_cmd_send_zone_stop(uint8_t zone)
{
    hunter_cmd_t c = { .type = HUNTER_CMD_ZONE_STOP, .zone = zone };
    push(&c);
}

void hunter_cmd_send_program(uint8_t program)
{
    hunter_cmd_t c = { .type = HUNTER_CMD_PROGRAM, .program = program };
    push(&c);
}

void hunter_cmd_send_stop_all(void)
{
    hunter_cmd_t c = { .type = HUNTER_CMD_STOP_ALL };
    push(&c);
}

void hunter_cmd_send_reboot(void)
{
    hunter_cmd_t c = { .type = HUNTER_CMD_REBOOT };
    push(&c);
}

void hunter_cmd_send_ota(void)
{
    hunter_cmd_t c = { .type = HUNTER_CMD_OTA };
    push(&c);
}
