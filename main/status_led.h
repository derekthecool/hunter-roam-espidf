// Status LED driver — three modes driven by device state.
//
//   WIFI_DISCONNECTED : double-blink every ~1.15s (1000 off / 50 on / 50 off / 50 on)
//   WIFI_CONNECTED    : solid on
//   COMMAND_ACTIVE    : rapid blink 80 ms on / 80 ms off
//
// A command pulse is a one-shot override: while active, the LED rapid-blinks
// regardless of the underlying WiFi state. When the pulse timer expires,
// the LED returns to whatever the WiFi-state base mode is. Starting a new
// pulse (or setting the base mode) cancels any in-flight pulse timer.

#pragma once

#include "esp_err.h"
#include <stdint.h>

typedef enum {
    STATUS_LED_MODE_WIFI_DISCONNECTED,
    STATUS_LED_MODE_WIFI_CONNECTED,
    STATUS_LED_MODE_COMMAND_ACTIVE,  // only set internally by command_pulse
} status_led_mode_t;

// Initialise the LED driver on `gpio_num`. Spawns a dedicated blink task.
esp_err_t status_led_init(int gpio_num);

// Set the "base" mode (driven by WiFi state). If a command pulse is currently
// active, the base mode is remembered but the LED keeps rapid-blinking until
// the pulse timer expires.
void status_led_set_mode(status_led_mode_t mode);

// Override the LED into COMMAND_ACTIVE (rapid blink) for `duration_ms`.
// Cancels any in-flight pulse timer first — last-pulse-wins.
void status_led_command_pulse(uint32_t duration_ms);

// Cancel any active command pulse and return to the base mode immediately.
void status_led_cancel_pulse(void);
