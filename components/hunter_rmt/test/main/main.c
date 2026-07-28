// Unit tests for hunter_rmt — pure-logic functions plus the public-API
// validation paths (stubbed on linux so they run without real RMT hardware).
//
// Build & run:
//   cd components/hunter_rmt/test
//   cmake -B build && cmake --build build
//   ./build/hunter_rmt_test

#include "hunter_rmt.h"
#include "unity.h"
#include <string.h>

// ---------------------------------------------------------------------------
// Golden references — computed by mirroring seb821/hunter.cpp:
//   HunterBitfield + HunterStart + HunterProgram
// Keep in sync with the algorithm in hunter_rmt.c.
// ---------------------------------------------------------------------------

static const uint8_t ZONE_1_MIN_0[]    = {0xff, 0x20, 0x00, 0x30, 0x11, 0x80, 0x12, 0x04, 0x90, 0x01, 0x81, 0x0c, 0x01, 0xb8, 0x3f};
static const uint8_t ZONE_5_MIN_15[]   = {0xff, 0x20, 0x00, 0x71, 0xf3, 0x80, 0x0a, 0x7c, 0x50, 0x05, 0x9f, 0x2c, 0x01, 0xb9, 0x3f};
static const uint8_t ZONE_13_MIN_240[] = {0xff, 0x40, 0x00, 0x48, 0x12, 0x4f, 0x06, 0x04, 0x33, 0xc7, 0x81, 0x3c, 0xf1, 0xb9, 0xbf};
static const uint8_t ZONE_48_MIN_1[]   = {0xff, 0x40, 0x01, 0xc5, 0x1e, 0x20, 0x65, 0x47, 0x28, 0x1f, 0x51, 0xfa, 0x01, 0xbf, 0xbf};

static const uint8_t PROG_1[] = {0xff, 0x40, 0x03, 0x96, 0x09, 0xbd, 0x7f};
static const uint8_t PROG_4[] = {0xff, 0x40, 0x03, 0x97, 0x89, 0xbd, 0x7f};

// ---------------------------------------------------------------------------
// hunter_bitfield_encode
// ---------------------------------------------------------------------------

static void test_bitfield_sets_lsb_at_position_0(void)
{
    // Encode 0x01 (only LSB set) at pos=0, len=8 → byte 0 should equal 0x80
    // because bit position 0 maps to MSB of byte 0.
    uint8_t blob[2] = {0};
    hunter_bitfield_encode(blob, 0, 0x01, 8);
    TEST_ASSERT_EQUAL_UINT8(0x80, blob[0]);
    TEST_ASSERT_EQUAL_UINT8(0x00, blob[1]);
}

static void test_bitfield_writes_across_bytes(void)
{
    // Encode 0x7F (0b01111111) at pos=5, len=7.
    // Trace through HunterBitfield:
    //   iter1: pos=5, val&1=1, set byte0 bit 2  → byte0 = 0x04
    //   iter2: pos=6, val&1=1, set byte0 bit 1  → byte0 = 0x06
    //   iter3: pos=7, val&1=1, set byte0 bit 0  → byte0 = 0x07
    //   iter4: pos=8, val&1=1, set byte1 bit 7  → byte1 = 0x80
    //   iter5: pos=9, val&1=1, set byte1 bit 6  → byte1 = 0xC0
    //   iter6: pos=10,val&1=1, set byte1 bit 5  → byte1 = 0xE0
    //   iter7: pos=11,val&1=1, set byte1 bit 4  → byte1 = 0xF0
    uint8_t blob[2] = {0};
    hunter_bitfield_encode(blob, 5, 0x7F, 7);
    TEST_ASSERT_EQUAL_UINT8(0x07, blob[0]);
    TEST_ASSERT_EQUAL_UINT8(0xF0, blob[1]);
}

static void test_bitfield_clears_existing_bits(void)
{
    // Start with all bits set; encode 0x00 at pos=0, len=4 → top nibble cleared.
    uint8_t blob[1] = {0xFF};
    hunter_bitfield_encode(blob, 0, 0x00, 4);
    TEST_ASSERT_EQUAL_UINT8(0x0F, blob[0]);
}

// ---------------------------------------------------------------------------
// hunter_build_zone_frame
// ---------------------------------------------------------------------------

static void test_zone_frame_zone_1_min_0(void)
{
    uint8_t out[15];
    hunter_build_zone_frame(1, 0, out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ZONE_1_MIN_0, out, 15);
}

static void test_zone_frame_zone_5_min_15(void)
{
    uint8_t out[15];
    hunter_build_zone_frame(5, 15, out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ZONE_5_MIN_15, out, 15);
}

static void test_zone_frame_zone_13_uses_high_zone_flag(void)
{
    uint8_t out[15];
    hunter_build_zone_frame(13, 240, out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ZONE_13_MIN_240, out, 15);
    TEST_ASSERT_EQUAL_UINT8(0x40, out[1]);  // high-zone flag bit pattern
}

static void test_zone_frame_zone_48_max(void)
{
    uint8_t out[15];
    hunter_build_zone_frame(48, 1, out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ZONE_48_MIN_1, out, 15);
}

// ---------------------------------------------------------------------------
// hunter_build_program_frame
// ---------------------------------------------------------------------------

static void test_program_frame_program_1(void)
{
    uint8_t out[7];
    hunter_build_program_frame(1, out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(PROG_1, out, 7);
}

static void test_program_frame_program_4(void)
{
    uint8_t out[7];
    hunter_build_program_frame(4, out);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(PROG_4, out, 7);
}

// ---------------------------------------------------------------------------
// hunter_frame_to_symbols — symbol count and timing
// ---------------------------------------------------------------------------

static void test_symbol_count_zone_start(void)
{
    uint8_t frame[15] = {0};
    rmt_symbol_word_t sym[130];
    size_t n = hunter_frame_to_symbols(frame, 15, /*extrabit=*/1, sym, 130);
    TEST_ASSERT_EQUAL_size_t(123, n);
}

static void test_symbol_count_program(void)
{
    uint8_t frame[7] = {0};
    rmt_symbol_word_t sym[64];
    size_t n = hunter_frame_to_symbols(frame, 7, /*extrabit=*/0, sym, 64);
    TEST_ASSERT_EQUAL_size_t(58, n);
}

static void test_symbol_bit_zero_timing(void)
{
    uint8_t frame[1] = {0x00};
    rmt_symbol_word_t sym[12];
    size_t n = hunter_frame_to_symbols(frame, 1, /*extrabit=*/0, sym, 12);
    TEST_ASSERT_EQUAL_size_t(10, n);
    TEST_ASSERT_EQUAL_UINT16(HUNTER_BIT0_HIGH_US, sym[1].duration0);
    TEST_ASSERT_EQUAL(1, sym[1].level0);
    TEST_ASSERT_EQUAL_UINT16(HUNTER_BIT0_LOW_US, sym[1].duration1);
    TEST_ASSERT_EQUAL(0, sym[1].level1);
}

static void test_symbol_bit_one_timing(void)
{
    uint8_t frame[1] = {0xFF};
    rmt_symbol_word_t sym[12];
    hunter_frame_to_symbols(frame, 1, /*extrabit=*/0, sym, 12);
    TEST_ASSERT_EQUAL_UINT16(HUNTER_BIT1_HIGH_US, sym[1].duration0);
    TEST_ASSERT_EQUAL(1, sym[1].level0);
    TEST_ASSERT_EQUAL_UINT16(HUNTER_BIT1_LOW_US, sym[1].duration1);
    TEST_ASSERT_EQUAL(0, sym[1].level1);
}

static void test_symbol_start_pulse_timing(void)
{
    uint8_t frame[1] = {0};
    rmt_symbol_word_t sym[12];
    hunter_frame_to_symbols(frame, 1, /*extrabit=*/0, sym, 12);
    TEST_ASSERT_EQUAL_UINT16(HUNTER_START_HIGH_US, sym[0].duration0);
    TEST_ASSERT_EQUAL(1, sym[0].level0);
    TEST_ASSERT_EQUAL_UINT16(HUNTER_START_LOW_US, sym[0].duration1);
    TEST_ASSERT_EQUAL(0, sym[0].level1);
}

static void test_symbol_stop_pulse_is_zero_bit(void)
{
    uint8_t frame[1] = {0xFF};  // all ones, so the only "0" bit is the stop
    rmt_symbol_word_t sym[12];
    size_t n = hunter_frame_to_symbols(frame, 1, /*extrabit=*/0, sym, 12);
    TEST_ASSERT_EQUAL_size_t(10, n);
    TEST_ASSERT_EQUAL_UINT16(HUNTER_BIT0_HIGH_US, sym[n - 1].duration0);
    TEST_ASSERT_EQUAL_UINT16(HUNTER_BIT0_LOW_US,  sym[n - 1].duration1);
}

// ---------------------------------------------------------------------------
// Public API validation — linux stubs
// ---------------------------------------------------------------------------

static void test_api_rejects_calls_before_init(void)
{
    // NB: this test must run BEFORE any test that calls hunter_rmt_init.
    TEST_ASSERT_EQUAL(HUNTER_ERR_NOT_INIT, hunter_rmt_start_zone(1, 5));
    TEST_ASSERT_EQUAL(HUNTER_ERR_NOT_INIT, hunter_rmt_stop_zone(1));
    TEST_ASSERT_EQUAL(HUNTER_ERR_NOT_INIT, hunter_rmt_run_program(1));
}

static void test_api_rejects_invalid_zone_zero(void)
{
    hunter_rmt_init(4);
    TEST_ASSERT_EQUAL(HUNTER_ERR_INVALID_ZONE, hunter_rmt_start_zone(0, 5));
    TEST_ASSERT_EQUAL(HUNTER_ERR_INVALID_ZONE, hunter_rmt_stop_zone(0));
}

static void test_api_rejects_invalid_zone_49(void)
{
    TEST_ASSERT_EQUAL(HUNTER_ERR_INVALID_ZONE, hunter_rmt_start_zone(49, 5));
    TEST_ASSERT_EQUAL(HUNTER_ERR_INVALID_PROGRAM, hunter_rmt_run_program(5));
}

static void test_api_rejects_invalid_time(void)
{
    TEST_ASSERT_EQUAL(HUNTER_ERR_INVALID_TIME, hunter_rmt_start_zone(1, 241));
    TEST_ASSERT_EQUAL(HUNTER_ERR_INVALID_TIME, hunter_rmt_start_zone(1, 255));
}

static void test_api_accepts_boundary_inputs(void)
{
    TEST_ASSERT_EQUAL(HUNTER_OK, hunter_rmt_start_zone(1,   0));
    TEST_ASSERT_EQUAL(HUNTER_OK, hunter_rmt_start_zone(48,  240));
    TEST_ASSERT_EQUAL(HUNTER_OK, hunter_rmt_stop_zone(1));
    TEST_ASSERT_EQUAL(HUNTER_OK, hunter_rmt_run_program(1));
    TEST_ASSERT_EQUAL(HUNTER_OK, hunter_rmt_run_program(4));
}

// ---------------------------------------------------------------------------
// unity setUp / tearDown — required by unity.c even though we don't use them.
// ---------------------------------------------------------------------------

void setUp(void)    {}
void tearDown(void) {}

// ---------------------------------------------------------------------------
// Runner
// ---------------------------------------------------------------------------

int main(void)
{
    UNITY_BEGIN();

    // Order matters for test_api_rejects_calls_before_init — must be first.
    RUN_TEST(test_api_rejects_calls_before_init);
    RUN_TEST(test_api_rejects_invalid_zone_zero);

    RUN_TEST(test_bitfield_sets_lsb_at_position_0);
    RUN_TEST(test_bitfield_writes_across_bytes);
    RUN_TEST(test_bitfield_clears_existing_bits);
    RUN_TEST(test_zone_frame_zone_1_min_0);
    RUN_TEST(test_zone_frame_zone_5_min_15);
    RUN_TEST(test_zone_frame_zone_13_uses_high_zone_flag);
    RUN_TEST(test_zone_frame_zone_48_max);
    RUN_TEST(test_program_frame_program_1);
    RUN_TEST(test_program_frame_program_4);
    RUN_TEST(test_symbol_count_zone_start);
    RUN_TEST(test_symbol_count_program);
    RUN_TEST(test_symbol_bit_zero_timing);
    RUN_TEST(test_symbol_bit_one_timing);
    RUN_TEST(test_symbol_start_pulse_timing);
    RUN_TEST(test_symbol_stop_pulse_is_zero_bit);

    RUN_TEST(test_api_rejects_invalid_zone_49);
    RUN_TEST(test_api_rejects_invalid_time);
    RUN_TEST(test_api_accepts_boundary_inputs);

    return UNITY_END();
}
