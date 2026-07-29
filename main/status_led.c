#include "status_led.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"

static const char *TAG = "status_led";

// A blink "step" — LED level + how long to hold it (ms).
typedef struct {
    uint8_t  level;
    uint16_t ms;
} blink_step_t;

// WiFi-disconnected: 1000 off, 50 on, 50 off, 50 on, then loop.
// 4 steps, total cycle 1150 ms.
static const blink_step_t DISCONNECT_STEPS[] = {
    { .level = 0, .ms = 1000 },
    { .level = 1, .ms =   50 },
    { .level = 0, .ms =   50 },
    { .level = 1, .ms =   50 },
};
#define DISCONNECT_STEP_COUNT (sizeof(DISCONNECT_STEPS) / sizeof(DISCONNECT_STEPS[0]))

// Command-active: 80 on, 80 off.
static const blink_step_t COMMAND_STEPS[] = {
    { .level = 1, .ms = 80 },
    { .level = 0, .ms = 80 },
};
#define COMMAND_STEP_COUNT (sizeof(COMMAND_STEPS) / sizeof(COMMAND_STEPS[0]))

static int                  s_gpio        = -1;
static volatile status_led_mode_t s_base_mode    = STATUS_LED_MODE_WIFI_DISCONNECTED;
static volatile bool        s_pulse_active = false;
static TimerHandle_t        s_pulse_timer  = NULL;

static status_led_mode_t effective_mode(void)
{
    return s_pulse_active ? STATUS_LED_MODE_COMMAND_ACTIVE : s_base_mode;
}

static void set_led(int on)
{
    if (s_gpio >= 0) {
        gpio_set_level((gpio_num_t)s_gpio, on ? 1 : 0);
    }
}

static void pulse_timer_cb(TimerHandle_t timer)
{
    (void)timer;
    s_pulse_active = false;
    ESP_LOGI(TAG, "command pulse ended — returning to base mode %d",
             s_base_mode);
}

static void led_task(void *arg)
{
    (void)arg;
    size_t phase = 0;
    status_led_mode_t last_mode = (status_led_mode_t)-1;

    while (1) {
        status_led_mode_t m = effective_mode();
        if (m != last_mode) {
            phase = 0;
            last_mode = m;
        }

        const blink_step_t *steps;
        size_t step_count;
        switch (m) {
        case STATUS_LED_MODE_WIFI_CONNECTED:
            set_led(1);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        case STATUS_LED_MODE_WIFI_DISCONNECTED:
            steps = DISCONNECT_STEPS;
            step_count = DISCONNECT_STEP_COUNT;
            break;
        case STATUS_LED_MODE_COMMAND_ACTIVE:
        default:
            steps = COMMAND_STEPS;
            step_count = COMMAND_STEP_COUNT;
            break;
        }

        set_led(steps[phase].level);
        vTaskDelay(pdMS_TO_TICKS(steps[phase].ms));
        phase = (phase + 1) % step_count;
    }
}

esp_err_t status_led_init(int gpio_num)
{
    s_gpio = gpio_num;

    gpio_config_t io = {
        .pin_bit_mask  = 1ULL << gpio_num,
        .mode          = GPIO_MODE_OUTPUT,
        .pull_up_en    = GPIO_PULLUP_DISABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config(%d) failed: %s", gpio_num, esp_err_to_name(ret));
        return ret;
    }

    s_pulse_timer = xTimerCreate("led_pulse", portMAX_DELAY, pdFALSE,
                                  NULL, pulse_timer_cb);
    if (s_pulse_timer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    BaseType_t ok = xTaskCreate(led_task, "status_led", 2048, NULL, 4, NULL);
    if (ok != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "status LED initialised on GPIO %d", gpio_num);
    return ESP_OK;
}

void status_led_set_mode(status_led_mode_t mode)
{
    // Only update the base mode. An in-flight command pulse continues until
    // its timer expires — this preserves "blink for the duration specified"
    // even if WiFi drops mid-pulse.
    s_base_mode = mode;
}

void status_led_command_pulse(uint32_t duration_ms)
{
    // Last-pulse-wins: stop any existing timer first.
    xTimerStop(s_pulse_timer, 0);
    s_pulse_active = true;
    xTimerChangePeriod(s_pulse_timer, pdMS_TO_TICKS(duration_ms), 0);
    xTimerStart(s_pulse_timer, 0);
}

void status_led_cancel_pulse(void)
{
    if (s_pulse_active) {
        xTimerStop(s_pulse_timer, 0);
        s_pulse_active = false;
    }
}
