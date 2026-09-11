/**
 * @file app_log_vlongmem_codec.c
 * @brief Delta encoding of environmental samples for the vlongmem variant.
 *
 * @copyright Ruuvi Innovations Ltd, license BSD-3-Clause.
 */
#include "app_log_vlongmem_codec.h"

#include <math.h>

/** @brief Limits of the absolute value of each field, in the units of the flash format. */
static const int32_t m_abs_min[VLMC_FIELDS] = { INT16_MIN + 1, 0, 0 };
static const int32_t m_abs_max[VLMC_FIELDS] = { INT16_MAX, 0xFFFE, 0xFFFE };

/** @brief Absolute sentinel of each field: "the reading is missing". */
static const int32_t m_missing[VLMC_FIELDS] =
{
    VLMC_TEMPERATURE_MISSING, VLMC_UNSIGNED_MISSING, VLMC_UNSIGNED_MISSING
};

static int32_t clamp (const int32_t value, const int32_t lo, const int32_t hi)
{
    return (value < lo) ? lo : ((value > hi) ? hi : value);
}

static int32_t abs_from_bytes (const size_t field, const uint8_t lo, const uint8_t hi)
{
    const uint16_t raw = (uint16_t) (lo | (hi << 8U));
    return (VLMC_FIELD_TEMPERATURE == field) ? (int32_t) (int16_t) raw : (int32_t) raw;
}

size_t vlmc_encode (vlmc_state_t * const p_state, const vlmc_sample_t * const p_sample,
                    uint8_t * const out)
{
    size_t len = 0;

    for (size_t f = 0; f < VLMC_FIELDS; f++)
    {
        int32_t absolute = m_missing[f];

        if (p_sample->valid[f])
        {
            absolute = clamp (p_sample->value[f], m_abs_min[f], m_abs_max[f]);
            const int32_t delta = absolute - p_state->ref[f];

            if ( (delta >= VLMC_DELTA_MIN) && (delta <= VLMC_DELTA_MAX))
            {
                // -128 is the escape and -1 is 0xFF. Both go through the escape.
                if (delta != -1)
                {
                    out[len++] = (uint8_t) (int8_t) delta;
                    p_state->ref[f] = absolute;
                    continue;
                }
            }

            p_state->ref[f] = absolute;
        }

        out[len++] = VLMC_ESCAPE;
        out[len++] = (uint8_t) (absolute & 0xFFU);
        out[len++] = (uint8_t) ( (absolute >> 8U) & 0xFFU);
    }

    return len;
}

size_t vlmc_decode (vlmc_state_t * const p_state, const uint8_t * const in,
                    const size_t avail, vlmc_sample_t * const p_sample)
{
    size_t pos = 0;
    vlmc_state_t next = *p_state;

    for (size_t f = 0; f < VLMC_FIELDS; f++)
    {
        if (pos >= avail) { return 0; }

        const uint8_t b = in[pos];

        if (VLMC_END == b) { return 0; }

        if (VLMC_ESCAPE == b)
        {
            if ( (pos + 3U) > avail) { return 0; }

            const int32_t absolute = abs_from_bytes (f, in[pos + 1U], in[pos + 2U]);
            pos += 3U;

            if (m_missing[f] == absolute)
            {
                p_sample->valid[f] = false;
                p_sample->value[f] = 0;
            }
            else
            {
                p_sample->valid[f] = true;
                p_sample->value[f] = absolute;
                next.ref[f] = absolute;
            }
        }
        else
        {
            next.ref[f] += (int8_t) b;
            p_sample->valid[f] = true;
            p_sample->value[f] = next.ref[f];
            pos++;
        }
    }

    *p_state = next;
    return pos;
}

static int32_t round_to_int (const float value)
{
    return (int32_t) roundf (value);
}

void vlmc_sample_from_units (vlmc_sample_t * const p_sample,
                             const float temperature_c, const bool temperature_valid,
                             const float humidity_rh, const bool humidity_valid,
                             const float pressure_pa, const bool pressure_valid)
{
    p_sample->valid[VLMC_FIELD_TEMPERATURE] = temperature_valid && !isnan (temperature_c);
    p_sample->valid[VLMC_FIELD_HUMIDITY] = humidity_valid && !isnan (humidity_rh);
    p_sample->valid[VLMC_FIELD_PRESSURE] = pressure_valid && !isnan (pressure_pa);
    p_sample->value[VLMC_FIELD_TEMPERATURE] =
        p_sample->valid[VLMC_FIELD_TEMPERATURE] ? round_to_int (temperature_c * 100.0F) : 0;
    p_sample->value[VLMC_FIELD_HUMIDITY] =
        p_sample->valid[VLMC_FIELD_HUMIDITY] ? round_to_int (humidity_rh * 100.0F) : 0;
    p_sample->value[VLMC_FIELD_PRESSURE] =
        p_sample->valid[VLMC_FIELD_PRESSURE] ?
        (round_to_int (pressure_pa) - VLMC_PRESSURE_OFFSET_PA) : 0;
}

void vlmc_sample_to_units (const vlmc_sample_t * const p_sample,
                           float * const p_temperature_c,
                           float * const p_humidity_rh,
                           float * const p_pressure_pa)
{
    if (p_sample->valid[VLMC_FIELD_TEMPERATURE])
    {
        *p_temperature_c = ( (float) p_sample->value[VLMC_FIELD_TEMPERATURE]) / 100.0F;
    }

    if (p_sample->valid[VLMC_FIELD_HUMIDITY])
    {
        *p_humidity_rh = ( (float) p_sample->value[VLMC_FIELD_HUMIDITY]) / 100.0F;
    }

    if (p_sample->valid[VLMC_FIELD_PRESSURE])
    {
        *p_pressure_pa = (float) (p_sample->value[VLMC_FIELD_PRESSURE] + VLMC_PRESSURE_OFFSET_PA);
    }
}
