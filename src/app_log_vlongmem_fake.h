#ifndef APP_LOG_VLONGMEM_FAKE_H
#define APP_LOG_VLONGMEM_FAKE_H
/**
 * @file app_log_vlongmem_fake.h
 * @brief Synthetic environmental data for tests of the vlongmem variant.
 *
 * The generator gives the same sample for the same slot index on each
 * platform. It uses only integer arithmetic. The data has a yearly cycle, a
 * daily cycle, noise, a transport event with fast changes, some missing
 * readings, and a pressure random walk with storms.
 *
 * @copyright Ruuvi Innovations Ltd, license BSD-3-Clause.
 */
#include "app_log_vlongmem_codec.h"

#include <stdint.h>

/** @brief Slots for one day at 10 min. */
#define VLMF_SLOTS_PER_DAY (144U)

/**
 * @brief Get the synthetic sample of a slot.
 *
 * @param[in] slot Slot index, 0 is the first sample. Slot 0 is the start of
 *                 a year in winter.
 * @param[out] p_sample Sample in the units of the codec.
 */
void vlmf_sample (const uint32_t slot, vlmc_sample_t * const p_sample);

#endif
