// hunter_rmt — ESP-IDF driver for Hunter Sprinkler "Roam" / SmartPort REM protocol.
//
// Drives the REM pin on Hunter controllers (X-Core, X2, Pro-C, ICC, ACC) using
// the ESP32 RMT peripheral for cycle-accurate µs bit-bang transmission.
//
// Protocol reverse-engineered by Scott Shumate (2015), Sebastien (seb821),
// and Dave Fleck. Bitfield layout lifted from seb821/OpenSprinkler-Firmware-Hunter.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "sdkconfig.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// On real targets, rmt_symbol_word_t comes from the IDF RMT driver header.
// On the linux host test target, we provide a structurally-compatible
// typedef with the same field names — tests access fields by name, never
// mixing the two layouts.
#if CONFIG_IDF_TARGET_LINUX
typedef struct {
    uint32_t duration0;
    uint32_t level0;
    uint32_t duration1;
    uint32_t level1;
} rmt_symbol_word_t;
#else
#include "driver/rmt_tx.h"
#endif

typedef enum {
    HUNTER_OK = 0,
    HUNTER_ERR_INVALID_ZONE,      // zone outside [1, CONFIG_HUNTER_ZONE_COUNT]
    HUNTER_ERR_INVALID_TIME,      // minutes outside [0, 240]
    HUNTER_ERR_INVALID_PROGRAM,   // program outside [1, 4]
    HUNTER_ERR_NOT_INIT,          // hunter_rmt_init() not called yet
    HUNTER_ERR_RMT_FAIL,          // RMT channel setup or transmit failed
} hunter_err_t;

// ---------------------------------------------------------------------------
// Public API — target only (stubbed on linux host for unit tests)
// ---------------------------------------------------------------------------

// Initialise the RMT TX channel on `gpio_num`. Call once at boot.
hunter_err_t hunter_rmt_init(int gpio_num);

// Start watering `zone` (1..48) for `minutes` (0..240).
// minutes == 0 stops the zone.
hunter_err_t hunter_rmt_start_zone(uint8_t zone, uint8_t minutes);

// Convenience: hunter_rmt_start_zone(zone, 0).
hunter_err_t hunter_rmt_stop_zone(uint8_t zone);

// Run one of the four programs stored on the Hunter controller itself.
hunter_err_t hunter_rmt_run_program(uint8_t program_num);

// ---------------------------------------------------------------------------
// Internal / testable API — no hardware deps, host-testable
// ---------------------------------------------------------------------------

// Hunter protocol timing constants (microseconds).
// Reset impulse is in milliseconds (handled outside RMT).
#define HUNTER_RESET_HIGH_MS    325
#define HUNTER_RESET_LOW_MS      65
#define HUNTER_START_HIGH_US    900
#define HUNTER_START_LOW_US     208
#define HUNTER_BIT0_HIGH_US     208
#define HUNTER_BIT0_LOW_US     1875
#define HUNTER_BIT1_HIGH_US    1875
#define HUNTER_BIT1_LOW_US      208

// Encode `val`'s low `len` bits into `blob` starting at bit position `pos`.
// LSB of val goes to the lowest-numbered bit position. Within each byte,
// bit position 0 = MSB. Direct port of HunterBitfield from seb821/hunter.cpp.
void hunter_bitfield_encode(uint8_t *blob, uint8_t pos, uint8_t val, uint8_t len);

// Build the 15-byte zone-start frame for the Hunter protocol.
// `zone` in [1, 48], `minutes` in [0, 240].
void hunter_build_zone_frame(uint8_t zone, uint8_t minutes, uint8_t out[15]);

// Build the 7-byte program-run frame for the Hunter protocol.
// `program` in [1, 4].
void hunter_build_program_frame(uint8_t program, uint8_t out[7]);

// Translate a frame buffer into a sequence of RMT symbols:
//   [start pulse, frame bits MSB-first, optional extra "1" bit, stop bit]
// Returns the number of symbols written. Caller must provide `out` of size
// at least `frame_len * 8 + 2` (or `+ 3` if extrabit).
size_t hunter_frame_to_symbols(const uint8_t *frame, size_t frame_len,
                                int extrabit,
                                rmt_symbol_word_t *out, size_t out_size);

#ifdef __cplusplus
}
#endif
