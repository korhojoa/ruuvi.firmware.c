#ifndef APP_LOG_VLONGMEM_CODEC_H
#define APP_LOG_VLONGMEM_CODEC_H
/**
 * @file app_log_vlongmem_codec.h
 * @brief Delta encoding of environmental samples for the vlongmem variant.
 *
 * These functions have no hardware dependencies. Thus the unit tests of the
 * codec operate on the host. The byte layout is in docs/vlongmem/FORMAT.md.
 *
 * Each sample has three fields in a fixed order: temperature in 0.01 deg C,
 * humidity in 0.01 %RH and pressure in Pa minus 50000. A field is one byte
 * with a signed delta from the same field of the previous sample, or the
 * escape byte 0x80 and then the absolute value as 16-bit little-endian. The
 * encoder never writes 0xFF at the start of a field. Thus 0xFF shows the end
 * of the data.
 *
 * @copyright Ruuvi Innovations Ltd, license BSD-3-Clause.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VLMC_FIELDS            (3U)
#define VLMC_FIELD_TEMPERATURE (0U)
#define VLMC_FIELD_HUMIDITY    (1U)
#define VLMC_FIELD_PRESSURE    (2U)

#define VLMC_ESCAPE            (0x80U) //!< Delta byte value: "the absolute value follows".
#define VLMC_END               (0xFFU) //!< Byte that is never valid at the start of a field.
#define VLMC_DELTA_MIN         (-127)
#define VLMC_DELTA_MAX         (127)
#define VLMC_MAX_SAMPLE_BYTES  (3U * VLMC_FIELDS) //!< All fields escaped.

#define VLMC_PRESSURE_OFFSET_PA (50000)  //!< Pressure in flash = Pa - this value.
#define VLMC_TEMPERATURE_MISSING (INT16_MIN) //!< Absolute sentinel, signed field.
#define VLMC_UNSIGNED_MISSING    (0xFFFF)    //!< Absolute sentinel, unsigned fields.

/** @brief One sample. The values are in the units of the flash format, refer to the file header. */
typedef struct
{
    int32_t value[VLMC_FIELDS];
    bool valid[VLMC_FIELDS];
} vlmc_sample_t;

/** @brief Reference of each field. Set all to zero at the start of each block. */
typedef struct
{
    int32_t ref[VLMC_FIELDS];
} vlmc_state_t;

/**
 * @brief Encode one sample.
 *
 * @param[in,out] p_state Reference. The function updates the valid fields.
 * @param[in] p_sample Sample to encode. Values out of the range are limited
 *                     to the range.
 * @param[out] out Buffer of VLMC_MAX_SAMPLE_BYTES or more.
 * @return Number of bytes written, 3 to 9.
 */
size_t vlmc_encode (vlmc_state_t * const p_state, const vlmc_sample_t * const p_sample,
                    uint8_t * const out);

/**
 * @brief Decode one sample.
 *
 * @param[in,out] p_state Reference. The function updates the valid fields.
 * @param[in] in Bytes to decode.
 * @param[in] avail Number of bytes available at in.
 * @param[out] p_sample Decoded sample.
 * @return Number of bytes used, or 0 if the data stops here (0xFF at the
 *         start of a field, or less bytes than the sample has). When the
 *         function returns 0, p_state does not change.
 */
size_t vlmc_decode (vlmc_state_t * const p_state, const uint8_t * const in,
                    const size_t avail, vlmc_sample_t * const p_sample);

/**
 * @brief Change engineering units to a sample.
 *
 * @param[in] temperature_c Degrees Celsius, not used if !temperature_valid.
 * @param[in] humidity_rh Percent relative humidity.
 * @param[in] pressure_pa Pascals.
 */
void vlmc_sample_from_units (vlmc_sample_t * const p_sample,
                             const float temperature_c, const bool temperature_valid,
                             const float humidity_rh, const bool humidity_valid,
                             const float pressure_pa, const bool pressure_valid);

/** @brief Change a sample to engineering units. The invalid fields do not change. */
void vlmc_sample_to_units (const vlmc_sample_t * const p_sample,
                           float * const p_temperature_c,
                           float * const p_humidity_rh,
                           float * const p_pressure_pa);

#endif
