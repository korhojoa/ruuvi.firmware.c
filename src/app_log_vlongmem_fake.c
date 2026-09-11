/**
 * @file app_log_vlongmem_fake.c
 * @brief Synthetic environmental data for tests of the vlongmem variant.
 *
 * @copyright Ruuvi Innovations Ltd, license BSD-3-Clause.
 */
#include "app_log_vlongmem_fake.h"

#define SLOTS_PER_YEAR (365U * VLMF_SLOTS_PER_DAY)

/** @brief Triangle wave, period p, amplitude +/- a, at position x. */
static int32_t triangle (const uint32_t x, const uint32_t p, const int32_t a)
{
    const uint32_t q = x % p;
    const int32_t half = (int32_t) (p / 2U);
    const int32_t v = (q < (uint32_t) half) ? (int32_t) q : (int32_t) (p - q);
    return ( (v * 2 * a) / half) - a;
}

/** @brief Deterministic noise in -range..+range for a slot and a channel. */
static int32_t noise (const uint32_t slot, const uint32_t channel, const int32_t range)
{
    uint32_t x = (slot * 2654435761U) ^ (channel * 40503U) ^ 0x5BD1E995U;
    x ^= x >> 13;
    x *= 0x5BD1E995U;
    x ^= x >> 15;
    return (int32_t) (x % (uint32_t) ( (2 * range) + 1)) - range;
}

void vlmf_sample (const uint32_t slot, vlmc_sample_t * const p_sample)
{
    // Temperature: winter start, yearly +/-12 deg C around 8 deg C, daily +/-3 deg C.
    int32_t temp = 800 + triangle (slot, SLOTS_PER_YEAR, 1200)
                   + triangle (slot + (VLMF_SLOTS_PER_DAY / 4U), VLMF_SLOTS_PER_DAY, 300)
                   + noise (slot, 1, 8);
    // Humidity: opposite to the temperature, 30 to 90 %RH.
    int32_t humi = 6000 - ( (temp - 800) * 2) + noise (slot, 2, 60);
    // Pressure: slow waves in a band of 4000 Pa, 1 Pa units minus 50000.
    int32_t pres = 51300 + triangle (slot, 977U, 1200) + triangle (slot + 100U, 3011U, 800)
                   + triangle (slot, 288U, 300) + noise (slot, 3, 15);

    // Transport event on days 200 to 203: a warm truck, a storm and a tunnel.
    const uint32_t event_start = 200U * VLMF_SLOTS_PER_DAY;
    const uint32_t event_end = 203U * VLMF_SLOTS_PER_DAY;

    if ( (slot >= event_start) && (slot < event_end))
    {
        const uint32_t t = slot - event_start;
        temp += 1500 + triangle (t, 36U, 900);
        humi = 3000 + triangle (t, 20U, 2500);
        pres -= 2000 + triangle (t, 12U, 1500);
    }

    // A storm on day 300: pressure falls 3000 Pa in 6 hours and comes back.
    const uint32_t storm_start = 300U * VLMF_SLOTS_PER_DAY;

    if ( (slot >= storm_start) && (slot < (storm_start + 72U)))
    {
        pres -= 3000 - triangle (slot - storm_start, 72U, 3000) - 3000;
    }

    if (humi < 500) { humi = 500; }

    if (humi > 9900) { humi = 9900; }

    p_sample->value[VLMC_FIELD_TEMPERATURE] = temp;
    p_sample->value[VLMC_FIELD_HUMIDITY] = humi;
    p_sample->value[VLMC_FIELD_PRESSURE] = pres;
    // Some failed readings: the humidity at each 977th slot, all fields at each 5003rd.
    p_sample->valid[VLMC_FIELD_TEMPERATURE] = (0U != (slot % 5003U));
    p_sample->valid[VLMC_FIELD_HUMIDITY] = (0U != (slot % 977U)) && (0U != (slot % 5003U));
    p_sample->valid[VLMC_FIELD_PRESSURE] = (0U != (slot % 5003U));
}
