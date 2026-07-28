// Optional OTA firmware update from CONFIG_HUNTER_OTA_URL.
#pragma once

#include "esp_err.h"

// If CONFIG_HUNTER_OTA_URL is non-empty, fetch and apply the firmware image.
// On success, the device reboots. On failure, returns an error and keeps
// running the current firmware.
esp_err_t ota_check_and_apply(void);
