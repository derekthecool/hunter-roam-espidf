// hunter_rmt — ESP-IDF driver for Hunter Sprinkler "Roam" / SmartPort REM protocol.
//
// Two layers:
//   1. Pure logic (no IDF deps) — bitfield encoder, frame builders, RMT symbol
//      translator. Always compiled. Unit-tested on the linux host target.
//   2. Hardware wrapper (target only) — RMT channel setup, GPIO reset impulse,
//      public API that composes the above. Stubs provided on linux for the
//      public-API error-path tests.
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include "hunter_rmt.h"
#include <string.h>

#if !CONFIG_IDF_TARGET_LINUX
#include "driver/gpio.h"
#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static const char *TAG = "hunter_rmt";

// ---------------------------------------------------------------------------
// Pure logic — host-testable
// ---------------------------------------------------------------------------

void hunter_bitfield_encode(uint8_t *blob, uint8_t pos, uint8_t val, uint8_t len)
{
    while (len > 0) {
        uint8_t byte_idx = pos / 8;
        uint8_t bit_mask = 0x80 >> (pos % 8);
        if (val & 0x1) {
            blob[byte_idx] |= bit_mask;
        } else {
            blob[byte_idx] &= ~bit_mask;
        }
        len--;
        val >>= 1;
        pos++;
    }
}

void hunter_build_zone_frame(uint8_t zone, uint8_t minutes, uint8_t out[15])
{
    // Base frame per seb821 — fixed bits + redundancy bits.
    static const uint8_t base[15] = {
        0xff, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x04,
        0x00, 0x00, 0x01, 0x00, 0x01, 0xb8, 0x3f
    };
    memcpy(out, base, 15);

    // Bits 9:10 are 0x1 for zones > 12, else 0x2.
    hunter_bitfield_encode(out, 9, (zone > 12) ? 0x1 : 0x2, 2);

    // Zone offset redundantly encoded at three offsets (0x17, 0x23, 0x2f).
    hunter_bitfield_encode(out, 23, zone + 0x17, 7);
    hunter_bitfield_encode(out, 36, zone + 0x17, 7);
    hunter_bitfield_encode(out, 49, zone + 0x23, 7);
    hunter_bitfield_encode(out, 62, zone + 0x23, 7);
    hunter_bitfield_encode(out, 75, zone + 0x2f, 7);
    hunter_bitfield_encode(out, 88, zone + 0x2f, 7);

    // Time encoded 6 places, low nibble then high nibble alternating.
    hunter_bitfield_encode(out, 31, minutes, 4);
    hunter_bitfield_encode(out, 44, minutes >> 4, 4);
    hunter_bitfield_encode(out, 57, minutes, 4);
    hunter_bitfield_encode(out, 70, minutes >> 4, 4);
    hunter_bitfield_encode(out, 83, minutes, 4);
    hunter_bitfield_encode(out, 96, minutes >> 4, 4);

    // Zone - 1 at bits 109:112.
    hunter_bitfield_encode(out, 109, zone - 1, 4);
}

void hunter_build_program_frame(uint8_t program, uint8_t out[7])
{
    static const uint8_t base[7] = {0xff, 0x40, 0x03, 0x96, 0x09, 0xbd, 0x7f};
    memcpy(out, base, 7);
    hunter_bitfield_encode(out, 31, program - 1, 2);
}

static rmt_symbol_word_t bit_symbol(int is_one)
{
    // One Hunter bit → one RMT symbol (two phases).
    return is_one
        ? (rmt_symbol_word_t){ .duration0 = HUNTER_BIT1_HIGH_US, .level0 = 1,
                                .duration1 = HUNTER_BIT1_LOW_US,  .level1 = 0 }
        : (rmt_symbol_word_t){ .duration0 = HUNTER_BIT0_HIGH_US, .level0 = 1,
                                .duration1 = HUNTER_BIT0_LOW_US,  .level1 = 0 };
}

size_t hunter_frame_to_symbols(const uint8_t *frame, size_t frame_len,
                                int extrabit,
                                rmt_symbol_word_t *out, size_t out_size)
{
    size_t needed = 1 /*start*/ + (frame_len * 8) /*bits*/
                    + (extrabit ? 1 : 0) /*extra*/ + 1 /*stop*/;
    if (out_size < needed) {
        return 0;  // caller bug: buffer too small
    }

    size_t n = 0;

    // Start pulse: 900 µs HIGH, 208 µs LOW.
    out[n++] = (rmt_symbol_word_t){
        .duration0 = HUNTER_START_HIGH_US, .level0 = 1,
        .duration1 = HUNTER_START_LOW_US,  .level1 = 0,
    };

    // Bits, MSB-first within each byte.
    for (size_t i = 0; i < frame_len; i++) {
        uint8_t byte = frame[i];
        for (int bit = 7; bit >= 0; bit--) {
            out[n++] = bit_symbol((byte >> bit) & 1);
        }
    }

    if (extrabit) {
        out[n++] = bit_symbol(1);
    }

    // Stop pulse: a "0" bit.
    out[n++] = bit_symbol(0);

    return n;
}

// ---------------------------------------------------------------------------
// Hardware wrapper — target only
// ---------------------------------------------------------------------------

#if !CONFIG_IDF_TARGET_LINUX

#define RMT_RES_HZ  1000000   // 1 µs per tick

static rmt_channel_handle_t s_chan       = NULL;
static rmt_encoder_handle_t s_encoder    = NULL;
static int                   s_init_done  = 0;

// Fill symbols with a continuous level for `total_us` microseconds.
// Each RMT symbol can hold up to ~65 ms of continuous level, so long
// durations (like the 325 ms reset HIGH) are split across multiple
// symbols — all with the same level.
static size_t emit_continuous(rmt_symbol_word_t *sym, int level, uint32_t total_us)
{
    size_t count = 0;
    while (total_us > 0) {
        uint32_t d0 = total_us > 32767 ? 32767 : total_us;
        total_us -= d0;
        uint32_t d1 = total_us > 32767 ? 32767 : total_us;
        total_us -= d1;
        sym[count].duration0 = d0;
        sym[count].level0    = level;
        sym[count].duration1 = d1;
        sym[count].level1    = level;
        count++;
    }
    return count;
}

// Build the complete RMT symbol stream for one Hunter frame and transmit it.
// Everything — including the 325 ms reset impulse — goes through the RMT
// peripheral. Never use gpio_set_level() on the RMT-owned pin; doing so
// disconnects the RMT's IO_MUX routing and the subsequent bit-bang goes
// nowhere.
static hunter_err_t hunter_transmit(const uint8_t *frame, size_t frame_len,
                                     int extrabit)
{
    // Worst case: 5 (reset high) + 1 (reset low) + 1 (start)
    //           + 120 (15 bytes × 8 bits) + 1 (extra) + 1 (stop) = 129
    rmt_symbol_word_t sym[140];
    size_t n = 0;

    // Reset impulse — entirely via RMT, no GPIO direct drive.
    n += emit_continuous(&sym[n], 1, (uint32_t)HUNTER_RESET_HIGH_MS * 1000);
    n += emit_continuous(&sym[n], 0, (uint32_t)HUNTER_RESET_LOW_MS  * 1000);

    // Start pulse: 900 µs HIGH, 208 µs LOW.
    sym[n].duration0 = HUNTER_START_HIGH_US; sym[n].level0 = 1;
    sym[n].duration1 = HUNTER_START_LOW_US;  sym[n].level1 = 0;
    n++;

    // Data bits, MSB-first.
    for (size_t i = 0; i < frame_len; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            sym[n++] = bit_symbol((frame[i] >> bit) & 1);
        }
    }

    if (extrabit) sym[n++] = bit_symbol(1);
    sym[n++] = bit_symbol(0);  // stop

    rmt_transmit_config_t cfg = { .loop_count = 0 };
    esp_err_t ret = rmt_transmit(s_chan, s_encoder,
                                  sym, n * sizeof(sym[0]), &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rmt_transmit failed: %s", esp_err_to_name(ret));
        return HUNTER_ERR_RMT_FAIL;
    }
    ret = rmt_tx_wait_all_done(s_chan, portMAX_DELAY);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rmt_tx_wait_all_done failed: %s", esp_err_to_name(ret));
        return HUNTER_ERR_RMT_FAIL;
    }
    return HUNTER_OK;
}

hunter_err_t hunter_rmt_init(int gpio_num)
{
    if (s_init_done) {
        return HUNTER_OK;
    }

    rmt_tx_channel_config_t tx_cfg = {
        .gpio_num           = gpio_num,
        .clk_src            = RMT_CLK_SRC_DEFAULT,
        .resolution_hz      = RMT_RES_HZ,
        .mem_block_symbols  = 64,
        .trans_queue_depth  = 4,
    };

    esp_err_t ret = rmt_new_tx_channel(&tx_cfg, &s_chan);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_tx_channel failed: %s", esp_err_to_name(ret));
        return HUNTER_ERR_RMT_FAIL;
    }

    rmt_copy_encoder_config_t enc_cfg;  // empty struct
    ret = rmt_new_copy_encoder(&enc_cfg, &s_encoder);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "rmt_new_copy_encoder failed: %s", esp_err_to_name(ret));
        return HUNTER_ERR_RMT_FAIL;
    }

    rmt_enable(s_chan);

    s_init_done = 1;
    ESP_LOGI(TAG, "RMT TX channel initialised on GPIO %d @ %d Hz",
             gpio_num, RMT_RES_HZ);
    return HUNTER_OK;
}

hunter_err_t hunter_rmt_start_zone(uint8_t zone, uint8_t minutes)
{
    if (!s_init_done) return HUNTER_ERR_NOT_INIT;
    if (zone < 1 || zone > 48) return HUNTER_ERR_INVALID_ZONE;
    if (minutes > 240) return HUNTER_ERR_INVALID_TIME;

    uint8_t frame[15];
    hunter_build_zone_frame(zone, minutes, frame);

    hunter_err_t err = hunter_transmit(frame, 15, /*extrabit=*/1);
    if (err == HUNTER_OK) {
        ESP_LOGI(TAG, "zone %u started for %u min", zone, minutes);
    }
    return err;
}

hunter_err_t hunter_rmt_stop_zone(uint8_t zone)
{
    return hunter_rmt_start_zone(zone, 0);
}

hunter_err_t hunter_rmt_run_program(uint8_t program_num)
{
    if (!s_init_done) return HUNTER_ERR_NOT_INIT;
    if (program_num < 1 || program_num > 4) return HUNTER_ERR_INVALID_PROGRAM;

    uint8_t frame[7];
    hunter_build_program_frame(program_num, frame);

    hunter_err_t err = hunter_transmit(frame, 7, /*extrabit=*/0);
    if (err == HUNTER_OK) {
        ESP_LOGI(TAG, "program %u triggered", program_num);
    }
    return err;
}

#else  // CONFIG_IDF_TARGET_LINUX — host stubs for the public-API tests

// The linux stubs replicate the validation logic of the target code so the
// error-path tests (invalid zone / time / program / not-init) pass on host.
static int s_init_done_stub = 0;

hunter_err_t hunter_rmt_init(int gpio_num)
{
    (void)gpio_num;
    s_init_done_stub = 1;
    return HUNTER_OK;
}

hunter_err_t hunter_rmt_start_zone(uint8_t zone, uint8_t minutes)
{
    if (!s_init_done_stub) return HUNTER_ERR_NOT_INIT;
    if (zone < 1 || zone > 48) return HUNTER_ERR_INVALID_ZONE;
    if (minutes > 240) return HUNTER_ERR_INVALID_TIME;
    return HUNTER_OK;
}

hunter_err_t hunter_rmt_stop_zone(uint8_t zone)
{
    return hunter_rmt_start_zone(zone, 0);
}

hunter_err_t hunter_rmt_run_program(uint8_t program_num)
{
    if (!s_init_done_stub) return HUNTER_ERR_NOT_INIT;
    if (program_num < 1 || program_num > 4) return HUNTER_ERR_INVALID_PROGRAM;
    return HUNTER_OK;
}

#endif  // CONFIG_IDF_TARGET_LINUX
