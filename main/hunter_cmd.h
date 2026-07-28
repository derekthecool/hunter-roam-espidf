// Command queue + safety timers + dispatch task.
//
// Producers (mqtt.c, ota.c) call hunter_cmd_send_*() with the requested action.
// The hunter_cmd task consumes the queue and invokes the hunter_rmt driver.

#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    HUNTER_CMD_ZONE_START,
    HUNTER_CMD_ZONE_STOP,
    HUNTER_CMD_PROGRAM,
    HUNTER_CMD_STOP_ALL,
    HUNTER_CMD_REBOOT,
    HUNTER_CMD_OTA,
} hunter_cmd_type_t;

typedef struct {
    hunter_cmd_type_t type;
    uint8_t  zone;       // valid for ZONE_START / ZONE_STOP
    uint8_t  minutes;    // valid for ZONE_START
    uint8_t  program;    // valid for PROGRAM
} hunter_cmd_t;

// Initialise the queue, safety timers, and the dispatch task.
// Must be called once at boot after hunter_rmt_init().
esp_err_t hunter_cmd_init(void);

// Producer API — all push to the queue and return immediately.
void hunter_cmd_send_zone_start(uint8_t zone, uint8_t minutes);
void hunter_cmd_send_zone_stop(uint8_t zone);
void hunter_cmd_send_program(uint8_t program);
void hunter_cmd_send_stop_all(void);
void hunter_cmd_send_reboot(void);
void hunter_cmd_send_ota(void);
