/**
 * @file app_log_vlongmem_time.c
 * @brief Time anchors and clock drift correction for the vlongmem variant.
 *
 * @copyright Ruuvi Innovations Ltd, license BSD-3-Clause.
 */
#include "app_log_vlongmem_time.h"

#include <string.h>

/** @brief Find the first and last index of a session. Returns false if the session has no anchors. */
static bool session_range (const vlmt_anchors_t * const p_list, const uint32_t boot_id,
                           uint32_t * const p_first, uint32_t * const p_last)
{
    bool found = false;

    for (uint32_t ii = 0; ii < p_list->count; ii++)
    {
        if (p_list->anchors[ii].boot_id == boot_id)
        {
            if (!found) { *p_first = ii; }

            *p_last = ii;
            found = true;
        }
    }

    return found;
}

static void remove_anchor (vlmt_anchors_t * const p_list, const uint32_t idx)
{
    for (uint32_t ii = idx; (ii + 1U) < p_list->count; ii++)
    {
        p_list->anchors[ii] = p_list->anchors[ii + 1U];
    }

    p_list->count--;
}

/** @brief Remove all anchors of the oldest session that is not boot_id. Returns false if there is none. */
static bool remove_oldest_other_session (vlmt_anchors_t * const p_list, const uint32_t boot_id)
{
    uint32_t victim = 0;

    while ( (victim < p_list->count) && (p_list->anchors[victim].boot_id == boot_id))
    {
        victim++;
    }

    if (victim >= p_list->count) { return false; }

    const uint32_t victim_id = p_list->anchors[victim].boot_id;
    uint32_t ii = 0;

    while (ii < p_list->count)
    {
        if (p_list->anchors[ii].boot_id == victim_id) { remove_anchor (p_list, ii); }
        else { ii++; }
    }

    return true;
}

/** @brief Rate error in ppm between two anchors, or false if the pair is not usable. */
static bool rate_error_ppm (const vlmt_anchor_t * const p_a, const vlmt_anchor_t * const p_b,
                            int64_t * const p_ppm)
{
    const int64_t du = (int64_t) p_b->uptime_s - (int64_t) p_a->uptime_s;
    const int64_t de = (int64_t) p_b->epoch_s - (int64_t) p_a->epoch_s;

    if (du <= 0) { return false; }

    *p_ppm = ( (de - du) * 1000000LL) / du;
    return true;
}

bool vlmt_anchor_add (vlmt_anchors_t * const p_list, const uint32_t boot_id,
                      const uint32_t uptime_s, const uint32_t epoch_s)
{
    if (epoch_s < VLMT_MIN_EPOCH_S) { return false; }

    if (p_list->count > VLMT_MAX_ANCHORS) { p_list->count = 0; } // Corrupt record.

    const vlmt_anchor_t anchor = { boot_id, uptime_s, epoch_s };
    uint32_t first = 0;
    uint32_t last = 0;

    if (session_range (p_list, boot_id, &first, &last))
    {
        const vlmt_anchor_t * const p_last = &p_list->anchors[last];

        if (uptime_s <= p_last->uptime_s) { return false; }

        if ( (uptime_s - p_last->uptime_s) < VLMT_MIN_ANCHOR_SPACING_S)
        {
            p_list->anchors[last] = anchor;
            return true;
        }

        int64_t ppm = 0;

        if (rate_error_ppm (&p_list->anchors[first], &anchor, &ppm)
                && ( (ppm > VLMT_MAX_RATE_ERROR_PPM) || (ppm < -VLMT_MAX_RATE_ERROR_PPM)))
        {
            return false; // The phone clock does not agree with the session.
        }

        if ( (last + 1U - first) >= VLMT_ANCHORS_PER_SESSION)
        {
            p_list->anchors[last] = anchor;
            return true;
        }

        // Keep the anchors of the session together: put the new one after the last.
        if (p_list->count >= VLMT_MAX_ANCHORS)
        {
            if (!remove_oldest_other_session (p_list, boot_id)) { return false; }

            session_range (p_list, boot_id, &first, &last);
        }

        for (uint32_t ii = p_list->count; ii > (last + 1U); ii--)
        {
            p_list->anchors[ii] = p_list->anchors[ii - 1U];
        }

        p_list->anchors[last + 1U] = anchor;
        p_list->count++;
        return true;
    }

    // First anchor of this session.
    if ( (p_list->count >= VLMT_MAX_ANCHORS) && !remove_oldest_other_session (p_list, boot_id))
    {
        return false;
    }

    p_list->anchors[p_list->count] = anchor;
    p_list->count++;
    return true;
}

bool vlmt_epoch_get (const vlmt_anchors_t * const p_list, const uint32_t boot_id,
                     const uint32_t uptime_s, const uint32_t header_offset_s,
                     uint32_t * const p_epoch_s)
{
    uint32_t first = 0;
    uint32_t last = 0;

    if ( (p_list->count > VLMT_MAX_ANCHORS) || !session_range (p_list, boot_id, &first, &last))
    {
        if (VLMT_NO_OFFSET == header_offset_s) { return false; }

        *p_epoch_s = header_offset_s + uptime_s;
        return true;
    }

    if (first == last)
    {
        const vlmt_anchor_t * const p_a = &p_list->anchors[first];
        *p_epoch_s = (uint32_t) ( (int64_t) p_a->epoch_s
                                  + ( (int64_t) uptime_s - (int64_t) p_a->uptime_s));
        return true;
    }

    // Select the pair: the segment that contains uptime_s, or the nearest end segment.
    uint32_t k = first;

    while ( ( (k + 1U) < last) && (p_list->anchors[k + 1U].uptime_s <= uptime_s))
    {
        k++;
    }

    const vlmt_anchor_t * const p_a = &p_list->anchors[k];
    const vlmt_anchor_t * const p_b = &p_list->anchors[k + 1U];
    const int64_t du = (int64_t) p_b->uptime_s - (int64_t) p_a->uptime_s;
    const int64_t de = (int64_t) p_b->epoch_s - (int64_t) p_a->epoch_s;
    const int64_t d = (int64_t) uptime_s - (int64_t) p_a->uptime_s;
    int64_t epoch = (int64_t) p_a->epoch_s + d;

    if (du > 0)
    {
        epoch = (int64_t) p_a->epoch_s + ( (d * de) / du);
    }

    *p_epoch_s = (uint32_t) epoch;
    return true;
}
