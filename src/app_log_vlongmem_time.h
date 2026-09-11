#ifndef APP_LOG_VLONGMEM_TIME_H
#define APP_LOG_VLONGMEM_TIME_H
/**
 * @file app_log_vlongmem_time.h
 * @brief Time anchors and clock drift correction for the vlongmem variant.
 *
 * The tag has no clock, only an uptime counter from a 32.768 kHz crystal.
 * The crystal has a drift of some tens of ppm, and more when the tag is
 * cold. Each phone connection gives the current time. This module keeps
 * these (uptime, epoch) pairs as anchors for each boot session. The read
 * function calculates the epoch time of a sample by linear interpolation
 * between the anchors of its session. Thus two connections, one at the start
 * and one at the end, correct the drift of the full period between them.
 *
 * These functions have no hardware dependencies. Thus the unit tests operate
 * on the host.
 *
 * @copyright Ruuvi Innovations Ltd, license BSD-3-Clause.
 */
#include <stdbool.h>
#include <stdint.h>

#define VLMT_MAX_ANCHORS          (32U)   //!< Total anchors in the persistent record.
#define VLMT_ANCHORS_PER_SESSION  (4U)    //!< Maximum anchors of one boot session.
#define VLMT_MIN_ANCHOR_SPACING_S (3600U) //!< A new anchor closer than this replaces the last one.
#define VLMT_MAX_RATE_ERROR_PPM   (10000) //!< 1 %, more than this is a wrong phone clock.
#define VLMT_MIN_EPOCH_S          (1577836800U) //!< 2020-01-01, a phone clock before this is not set.
/**
 * @brief Maximum extrapolation as a multiple of the span of the anchor pair.
 *
 * The rate of a pair has an error of some seconds divided by the span. An
 * extrapolation of more than this multiple uses the offset of the nearest
 * anchor, not the rate. With 20, an error of 3 s in a pair gives a maximum
 * error of 60 s at the limit.
 */
#define VLMT_MAX_EXTRAPOLATION    (20)
#define VLMT_NO_OFFSET            (0xFFFFFFFFU) //!< No header offset available.

/** @brief One anchor: the phone time at a known tag uptime. */
typedef struct
{
    uint32_t boot_id;  //!< Boot session.
    uint32_t uptime_s; //!< Tag uptime, seconds.
    uint32_t epoch_s;  //!< Phone time, seconds from 1970.
} vlmt_anchor_t;

/** @brief Anchor list. The layout is the persistent record. */
typedef struct
{
    uint32_t count;
    vlmt_anchor_t anchors[VLMT_MAX_ANCHORS];
} vlmt_anchors_t;

/**
 * @brief Add an anchor for a boot session.
 *
 * The anchors of one session are kept in the sequence of uptime. A new
 * anchor closer than VLMT_MIN_ANCHOR_SPACING_S to the last anchor of the
 * session replaces that anchor. An anchor that shows a clock rate error of
 * more than VLMT_MAX_RATE_ERROR_PPM against the first anchor of the session
 * is not added. When the session has VLMT_ANCHORS_PER_SESSION anchors, the
 * new anchor replaces the last one. When the list is full, the anchors of
 * the sessions before oldest_boot_id (sessions with no data in the ring)
 * are removed first, then the anchors of the oldest other session.
 *
 * @param[in] oldest_boot_id The oldest boot session with data in the ring,
 *                           or 0 when not known.
 * @return true if the list changed and must be written to flash.
 */
bool vlmt_anchor_add (vlmt_anchors_t * const p_list, const uint32_t boot_id,
                      const uint32_t uptime_s, const uint32_t epoch_s,
                      const uint32_t oldest_boot_id);

/**
 * @brief Calculate the epoch time of a tag uptime in a boot session.
 *
 * With two or more anchors for the session, the function interpolates
 * between the two nearest anchors, or extrapolates with the rate of the
 * nearest pair. The extrapolation with the rate is limited to
 * VLMT_MAX_EXTRAPOLATION times the span of the pair, after that the offset
 * of the nearest anchor is used. With one anchor, the function uses its
 * offset. With no anchor, the function uses header_offset_s.
 *
 * @param[in] header_offset_s Offset from the block header, or VLMT_NO_OFFSET.
 * @param[out] p_epoch_s Result.
 * @return true if a time was calculated, false if the session has no date.
 */
bool vlmt_epoch_get (const vlmt_anchors_t * const p_list, const uint32_t boot_id,
                     const uint32_t uptime_s, const uint32_t header_offset_s,
                     uint32_t * const p_epoch_s);

#endif
