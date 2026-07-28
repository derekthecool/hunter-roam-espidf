// MQTT client + Home Assistant discovery + topic routing.
//
// Waits for WIFI_CONNECTED_BIT before starting. On connect, subscribes to
// the per-zone command topics and publishes HA discovery documents.

#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdbool.h>
#include <stdint.h>

// Start the MQTT client (waits for WiFi first, then connects).
esp_err_t mqtt_start(EventGroupHandle_t wifi_event_group);

// Returns the MQTT client handle, or NULL if not connected.
// Safe to call from any task.
void *mqtt_get_client(void);

// Publish a zone state update — called by hunter_cmd when a start/stop
// completes successfully. state=true publishes "on" payload, false publishes "off".
void mqtt_publish_zone_state(uint8_t zone, bool state);
