// WiFi station-mode init for hunter-roam-espidf.
//
// Connects using CONFIG_HUNTER_WIFI_SSID / CONFIG_HUNTER_WIFI_PASSWORD.
// Signals the WIFI_CONNECTED_BIT of an EventGroup once an IP is obtained,
// so dependent subsystems (MQTT, OTA) can wait for connectivity.

#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define WIFI_CONNECTED_BIT (1 << 0)

// Initialise WiFi (does not block). Returns the event group whose
// WIFI_CONNECTED_BIT gets set when an IP is obtained and cleared on disconnect.
esp_err_t wifi_init(EventGroupHandle_t *out_event_group);
