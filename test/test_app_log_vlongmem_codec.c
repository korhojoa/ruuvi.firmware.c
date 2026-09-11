/**
 * Unit tests of the vlongmem delta codec. The tests operate with ceedling,
 * and with gcc through scripts/test-vlongmem-codec.sh (no mocks).
 */
#include "unity.h"
#include "app_log_vlongmem_codec.h"

#include <string.h>

static vlmc_state_t enc;
static vlmc_state_t dec;

void setUp (void)
{
    memset (&enc, 0, sizeof (enc));
    memset (&dec, 0, sizeof (dec));
}

void tearDown (void)
{
}

static vlmc_sample_t make (int32_t t, int32_t h, int32_t p)
{
    vlmc_sample_t s = { .value = { t, h, p }, .valid = { true, true, true } };
    return s;
}

static void roundtrip (const vlmc_sample_t * const in, size_t expected_len)
{
    uint8_t buf[VLMC_MAX_SAMPLE_BYTES];
    vlmc_sample_t out = {0};
    size_t len = vlmc_encode (&enc, in, buf);
    TEST_ASSERT_EQUAL_size_t (expected_len, len);
    TEST_ASSERT_EQUAL_size_t (len, vlmc_decode (&dec, buf, len, &out));

    for (size_t f = 0; f < VLMC_FIELDS; f++)
    {
        TEST_ASSERT_EQUAL (in->valid[f], out.valid[f]);

        if (in->valid[f])
        {
            TEST_ASSERT_EQUAL_INT32 (in->value[f], out.value[f]);
        }
    }
}

void test_first_sample_near_zero_is_delta (void)
{
    vlmc_sample_t s = make (50, 100, 127);
    roundtrip (&s, 3);
}

void test_first_sample_far_from_zero_escapes (void)
{
    vlmc_sample_t s = make (2137, 4550, 101325 - VLMC_PRESSURE_OFFSET_PA);
    roundtrip (&s, 9);
}

void test_small_deltas_are_one_byte (void)
{
    vlmc_sample_t a = make (2137, 4550, 51325);
    vlmc_sample_t b = make (2137 + 127, 4550 - 127, 51325 + 3);
    roundtrip (&a, 9);
    roundtrip (&b, 3);
}

void test_delta_minus_one_escapes (void)
{
    vlmc_sample_t a = make (2137, 4550, 51325);
    vlmc_sample_t b = make (2136, 4549, 51324);
    uint8_t buf[VLMC_MAX_SAMPLE_BYTES];
    roundtrip (&a, 9);
    size_t len = vlmc_encode (&enc, &b, buf);
    TEST_ASSERT_EQUAL_size_t (9, len);

    for (size_t i = 0; i < len; i++)
    {
        TEST_ASSERT_NOT_EQUAL (VLMC_END, buf[i]);
    }
}

void test_delta_minus_128_escapes (void)
{
    vlmc_sample_t a = make (2137, 4550, 51325);
    vlmc_sample_t b = make (2137 - 128, 4550 - 128, 51325 - 128);
    roundtrip (&a, 9);
    roundtrip (&b, 9);
}

void test_large_step_escapes_and_resets_reference (void)
{
    vlmc_sample_t a = make (2137, 4550, 51325);
    vlmc_sample_t b = make (-1000, 9900, 50900);
    vlmc_sample_t c = make (-999, 9901, 50901);
    roundtrip (&a, 9);
    roundtrip (&b, 9);
    roundtrip (&c, 3);
}

void test_missing_field_is_reported_invalid_and_keeps_reference (void)
{
    vlmc_sample_t a = make (2137, 4550, 51325);
    vlmc_sample_t b = make (0, 4551, 51326);
    vlmc_sample_t c = make (2138, 4552, 51327);
    b.valid[VLMC_FIELD_TEMPERATURE] = false;
    roundtrip (&a, 9);
    roundtrip (&b, 5);
    roundtrip (&c, 3); // The temperature delta is from a, not from the missing sample.
}

void test_negative_temperature_absolute (void)
{
    vlmc_sample_t a = make (-2550, 0, 0);
    roundtrip (&a, 5);
}

void test_pressure_clamps_below_offset (void)
{
    vlmc_sample_t a = make (0, 0, -5);
    uint8_t buf[VLMC_MAX_SAMPLE_BYTES];
    vlmc_sample_t out = {0};
    size_t len = vlmc_encode (&enc, &a, buf);
    vlmc_decode (&dec, buf, len, &out);
    TEST_ASSERT_EQUAL_INT32 (0, out.value[VLMC_FIELD_PRESSURE]);
}

void test_decode_stops_at_end_marker (void)
{
    const uint8_t buf[] = { VLMC_END, 0x01, 0x01 };
    vlmc_sample_t out = {0};
    TEST_ASSERT_EQUAL_size_t (0, vlmc_decode (&dec, buf, sizeof (buf), &out));
}

void test_decode_stops_on_truncated_sample (void)
{
    const uint8_t buf[] = { 0x01, 0x01 };
    vlmc_sample_t out = {0};
    vlmc_state_t before = dec;
    TEST_ASSERT_EQUAL_size_t (0, vlmc_decode (&dec, buf, sizeof (buf), &out));
    TEST_ASSERT_EQUAL_MEMORY (&before, &dec, sizeof (dec));
}

void test_decode_stops_on_truncated_escape (void)
{
    const uint8_t buf[] = { VLMC_ESCAPE, 0x34, VLMC_END, 0x01 };
    vlmc_sample_t out = {0};
    TEST_ASSERT_EQUAL_size_t (0, vlmc_decode (&dec, buf, 2, &out));
}

void test_decode_end_marker_mid_sample_is_truncation (void)
{
    const uint8_t buf[] = { 0x01, VLMC_END, 0x01 };
    vlmc_sample_t out = {0};
    TEST_ASSERT_EQUAL_size_t (0, vlmc_decode (&dec, buf, sizeof (buf), &out));
}

void test_units_roundtrip (void)
{
    vlmc_sample_t s = {0};
    float t = 0, h = 0, p = 0;
    vlmc_sample_from_units (&s, 21.374F, true, 45.505F, true, 101325.4F, true);
    TEST_ASSERT_EQUAL_INT32 (2137, s.value[VLMC_FIELD_TEMPERATURE]);
    TEST_ASSERT_EQUAL_INT32 (4551, s.value[VLMC_FIELD_HUMIDITY]);
    TEST_ASSERT_EQUAL_INT32 (51325, s.value[VLMC_FIELD_PRESSURE]);
    vlmc_sample_to_units (&s, &t, &h, &p);
    // Integer comparisons, because ceedling builds Unity with UNITY_EXCLUDE_FLOAT.
    TEST_ASSERT_EQUAL_INT32 (2137, (int32_t) (t * 100.0F + 0.5F));
    TEST_ASSERT_EQUAL_INT32 (4551, (int32_t) (h * 100.0F + 0.5F));
    TEST_ASSERT_EQUAL_INT32 (101325, (int32_t) (p + 0.5F));
}

void test_units_invalid_inputs (void)
{
    vlmc_sample_t s = {0};
    vlmc_sample_from_units (&s, 0.0F / 0.0F, true, 50.0F, false, 100000.0F, true);
    TEST_ASSERT_FALSE (s.valid[VLMC_FIELD_TEMPERATURE]);
    TEST_ASSERT_FALSE (s.valid[VLMC_FIELD_HUMIDITY]);
    TEST_ASSERT_TRUE (s.valid[VLMC_FIELD_PRESSURE]);
}

void test_long_random_walk_roundtrips (void)
{
    uint32_t seed = 12345;
    vlmc_sample_t cur = make (2000, 5000, 51000);
    uint8_t buf[VLMC_MAX_SAMPLE_BYTES];

    for (int i = 0; i < 20000; i++)
    {
        for (size_t f = 0; f < VLMC_FIELDS; f++)
        {
            seed = seed * 1103515245U + 12345U;
            int32_t step = (int32_t) ( (seed >> 16) % 600U) - 300;

            // Some large steps, some missing readings.
            if (0 == (seed % 97U)) { step *= 40; }

            cur.valid[f] = (0 != (seed % 53U));
            cur.value[f] += step;

            // Keep the value in the range of each field.
            if (VLMC_FIELD_TEMPERATURE == f)
            {
                if (cur.value[f] > INT16_MAX) { cur.value[f] = INT16_MAX; }

                if (cur.value[f] < INT16_MIN + 1) { cur.value[f] = INT16_MIN + 1; }
            }
            else
            {
                if (cur.value[f] < 0) { cur.value[f] = 0; }

                if (cur.value[f] > 0xFFFE) { cur.value[f] = 0xFFFE; }
            }
        }

        vlmc_sample_t out = {0};
        size_t len = vlmc_encode (&enc, &cur, buf);
        TEST_ASSERT_EQUAL_size_t (len, vlmc_decode (&dec, buf, len, &out));

        for (size_t f = 0; f < VLMC_FIELDS; f++)
        {
            TEST_ASSERT_EQUAL (cur.valid[f], out.valid[f]);

            if (cur.valid[f]) { TEST_ASSERT_EQUAL_INT32 (cur.value[f], out.value[f]); }
        }
    }
}
