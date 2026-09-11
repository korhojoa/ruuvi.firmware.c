/**
 * Unit tests of the vlongmem time anchors. The tests operate with ceedling,
 * and with gcc through scripts/test-vlongmem-codec.sh (no mocks).
 */
#include "unity.h"
#include "app_log_vlongmem_time.h"

#include <string.h>

#define EPOCH0 (1700000000U)
#define DAY    (86400U)

static vlmt_anchors_t list;

void setUp (void)
{
    memset (&list, 0, sizeof (list));
}

void tearDown (void)
{
}

static bool add (uint32_t boot_id, uint32_t uptime_s, uint32_t epoch_s)
{
    return vlmt_anchor_add (&list, boot_id, uptime_s, epoch_s, 0);
}

void test_no_anchor_uses_header_offset (void)
{
    uint32_t e = 0;
    TEST_ASSERT_TRUE (vlmt_epoch_get (&list, 1, 500, EPOCH0, &e));
    TEST_ASSERT_EQUAL_UINT32 (EPOCH0 + 500, e);
}

void test_no_anchor_no_offset_has_no_date (void)
{
    uint32_t e = 0;
    TEST_ASSERT_FALSE (vlmt_epoch_get (&list, 1, 500, VLMT_NO_OFFSET, &e));
}

void test_one_anchor_is_an_offset (void)
{
    uint32_t e = 0;
    TEST_ASSERT_TRUE (add (1, 100, EPOCH0));
    TEST_ASSERT_TRUE (vlmt_epoch_get (&list, 1, 100 + DAY, VLMT_NO_OFFSET, &e));
    TEST_ASSERT_EQUAL_UINT32 (EPOCH0 + DAY, e);
    // Before the anchor as well.
    TEST_ASSERT_TRUE (vlmt_epoch_get (&list, 1, 40, VLMT_NO_OFFSET, &e));
    TEST_ASSERT_EQUAL_UINT32 (EPOCH0 - 60, e);
}

void test_anchor_of_other_session_is_not_used (void)
{
    uint32_t e = 0;
    TEST_ASSERT_TRUE (add (1, 100, EPOCH0));
    TEST_ASSERT_FALSE (vlmt_epoch_get (&list, 2, 100, VLMT_NO_OFFSET, &e));
    TEST_ASSERT_TRUE (vlmt_epoch_get (&list, 2, 100, EPOCH0 + 7, &e));
    TEST_ASSERT_EQUAL_UINT32 (EPOCH0 + 107, e);
}

void test_two_anchors_correct_a_slow_clock (void)
{
    // The tag counts 50 ppm slow: 365 days of uptime are 365 days + 1577 s of real time.
    const uint32_t year = 365U * DAY;
    const uint32_t real_year = year + 1577U;
    uint32_t e = 0;
    TEST_ASSERT_TRUE (add (1, 0, EPOCH0));
    TEST_ASSERT_TRUE (add (1, year, EPOCH0 + real_year));
    // Halfway: half of the drift is corrected.
    TEST_ASSERT_TRUE (vlmt_epoch_get (&list, 1, year / 2U, VLMT_NO_OFFSET, &e));
    TEST_ASSERT_UINT32_WITHIN (1, EPOCH0 + (real_year / 2U), e);
    // Extrapolation after the last anchor keeps the rate.
    TEST_ASSERT_TRUE (vlmt_epoch_get (&list, 1, 2U * year, VLMT_NO_OFFSET, &e));
    TEST_ASSERT_UINT32_WITHIN (2, EPOCH0 + (2U * real_year), e);
}

void test_extrapolation_far_from_a_short_pair_uses_the_offset (void)
{
    uint32_t e = 0;
    const uint32_t year = 365U * DAY;
    // Two anchors 2 h apart, the phone clock 40 s ahead at the second: a
    // rate error of 0.55 %, which is 48 h in one year of extrapolation.
    TEST_ASSERT_TRUE (add (1, year, EPOCH0 + year));
    TEST_ASSERT_TRUE (add (1, year + 7200U, EPOCH0 + year + 7240U));
    TEST_ASSERT_EQUAL_UINT32 (2, list.count);
    // One year before: the offset of the first anchor, not the rate.
    TEST_ASSERT_TRUE (vlmt_epoch_get (&list, 1, 0, VLMT_NO_OFFSET, &e));
    TEST_ASSERT_EQUAL_UINT32 (EPOCH0, e);
    // In the limit (20 times the span): the rate.
    TEST_ASSERT_TRUE (vlmt_epoch_get (&list, 1, year - 36000U, VLMT_NO_OFFSET, &e));
    TEST_ASSERT_EQUAL_UINT32 (EPOCH0 + year - 36000U - 200U, e);
    // After the pair, far: the offset of the second anchor.
    TEST_ASSERT_TRUE (vlmt_epoch_get (&list, 1, 2U * year, VLMT_NO_OFFSET, &e));
    TEST_ASSERT_EQUAL_UINT32 (EPOCH0 + (2U * year) + 40U, e);
}

void test_three_anchors_use_the_nearest_segment (void)
{
    uint32_t e = 0;
    // Rate 1.0 in the first segment, rate 1.005 in the second (a test value).
    TEST_ASSERT_TRUE (add (1, 0, EPOCH0));
    TEST_ASSERT_TRUE (add (1, 10000, EPOCH0 + 10000));
    TEST_ASSERT_TRUE (add (1, 20000, EPOCH0 + 10000 + 10050));
    TEST_ASSERT_TRUE (vlmt_epoch_get (&list, 1, 5000, VLMT_NO_OFFSET, &e));
    TEST_ASSERT_EQUAL_UINT32 (EPOCH0 + 5000, e);
    TEST_ASSERT_TRUE (vlmt_epoch_get (&list, 1, 15000, VLMT_NO_OFFSET, &e));
    TEST_ASSERT_EQUAL_UINT32 (EPOCH0 + 10000 + 5025, e);
    // Before the first anchor: the first segment.
    TEST_ASSERT_TRUE (vlmt_epoch_get (&list, 1, 0, VLMT_NO_OFFSET, &e));
    TEST_ASSERT_EQUAL_UINT32 (EPOCH0, e);
}

void test_close_anchor_replaces_the_last (void)
{
    TEST_ASSERT_TRUE (add (1, 100, EPOCH0));
    TEST_ASSERT_TRUE (add (1, 100 + DAY, EPOCH0 + DAY));
    TEST_ASSERT_TRUE (add (1, 100 + DAY + 60, EPOCH0 + DAY + 61));
    TEST_ASSERT_EQUAL_UINT32 (2, list.count);
    TEST_ASSERT_EQUAL_UINT32 (100 + DAY + 60, list.anchors[1].uptime_s);
    TEST_ASSERT_EQUAL_UINT32 (EPOCH0 + DAY + 61, list.anchors[1].epoch_s);
}

void test_anchor_not_after_the_last_is_rejected (void)
{
    TEST_ASSERT_TRUE (add (1, 100, EPOCH0));
    TEST_ASSERT_FALSE (add (1, 100, EPOCH0 + 5));
    TEST_ASSERT_FALSE (add (1, 50, EPOCH0));
    TEST_ASSERT_EQUAL_UINT32 (1, list.count);
}

void test_wrong_phone_clock_is_rejected (void)
{
    TEST_ASSERT_TRUE (add (1, 0, EPOCH0));
    // One day of uptime, but the phone says one day and one hour: 4 % error.
    TEST_ASSERT_FALSE (add (1, DAY, EPOCH0 + DAY + 3600));
    TEST_ASSERT_EQUAL_UINT32 (1, list.count);
    // 50 ppm is accepted.
    TEST_ASSERT_TRUE (add (1, DAY, EPOCH0 + DAY + 4));
    TEST_ASSERT_EQUAL_UINT32 (2, list.count);
}

void test_unset_phone_clock_is_rejected (void)
{
    TEST_ASSERT_FALSE (add (1, 0, 1000));
    TEST_ASSERT_EQUAL_UINT32 (0, list.count);
}

void test_session_limit_replaces_the_last (void)
{
    for (uint32_t ii = 0; ii < 6; ii++)
    {
        TEST_ASSERT_TRUE (add (1, ii * DAY, EPOCH0 + (ii * DAY)));
    }

    TEST_ASSERT_EQUAL_UINT32 (VLMT_ANCHORS_PER_SESSION, list.count);
    TEST_ASSERT_EQUAL_UINT32 (0, list.anchors[0].uptime_s);
    TEST_ASSERT_EQUAL_UINT32 (5 * DAY, list.anchors[VLMT_ANCHORS_PER_SESSION - 1].uptime_s);
}

/** @brief Fill the list with sessions 1..N of two anchors each. */
static void fill_two_per_session (void)
{
    const uint32_t sessions = VLMT_MAX_ANCHORS / 2U;

    for (uint32_t s = 1; s <= sessions; s++)
    {
        TEST_ASSERT_TRUE (add (s, 0, EPOCH0 + (s * 10U * DAY)));
        TEST_ASSERT_TRUE (add (s, DAY, EPOCH0 + (s * 10U * DAY) + DAY));
    }

    TEST_ASSERT_EQUAL_UINT32 (VLMT_MAX_ANCHORS, list.count);
}

void test_full_list_removes_the_oldest_session (void)
{
    fill_two_per_session();
    const uint32_t new_id = (VLMT_MAX_ANCHORS / 2U) + 1U;
    // A new session removes all anchors of session 1.
    TEST_ASSERT_TRUE (add (new_id, 0, EPOCH0 + (new_id * 10U * DAY)));
    TEST_ASSERT_EQUAL_UINT32 (VLMT_MAX_ANCHORS - 1, list.count);
    TEST_ASSERT_EQUAL_UINT32 (2, list.anchors[0].boot_id);
    uint32_t e = 0;
    TEST_ASSERT_FALSE (vlmt_epoch_get (&list, 1, 0, VLMT_NO_OFFSET, &e));
    TEST_ASSERT_TRUE (vlmt_epoch_get (&list, new_id, 0, VLMT_NO_OFFSET, &e));
    // A second anchor of the new session fits without a removal.
    TEST_ASSERT_TRUE (add (new_id, DAY, EPOCH0 + (new_id * 10U * DAY) + DAY));
    TEST_ASSERT_EQUAL_UINT32 (VLMT_MAX_ANCHORS, list.count);
    TEST_ASSERT_EQUAL_UINT32 (new_id, list.anchors[VLMT_MAX_ANCHORS - 1].boot_id);
    TEST_ASSERT_EQUAL_UINT32 (new_id, list.anchors[VLMT_MAX_ANCHORS - 2].boot_id);
    // A third anchor of the new session removes session 2, not the new session.
    TEST_ASSERT_TRUE (add (new_id, 2U * DAY, EPOCH0 + (new_id * 10U * DAY) + (2U * DAY)));
    TEST_ASSERT_EQUAL_UINT32 (VLMT_MAX_ANCHORS - 1, list.count);
    TEST_ASSERT_EQUAL_UINT32 (3, list.anchors[0].boot_id);
    TEST_ASSERT_EQUAL_UINT32 (new_id, list.anchors[list.count - 1].boot_id);
    TEST_ASSERT_EQUAL_UINT32 (2U * DAY, list.anchors[list.count - 1].uptime_s);
}

void test_full_list_removes_sessions_with_no_data_first (void)
{
    fill_two_per_session();
    const uint32_t new_id = (VLMT_MAX_ANCHORS / 2U) + 1U;
    // The ring holds data from session 5 on: sessions 1 to 4 go first.
    TEST_ASSERT_TRUE (vlmt_anchor_add (&list, new_id, 0, EPOCH0 + (new_id * 10U * DAY), 5));
    TEST_ASSERT_EQUAL_UINT32 (VLMT_MAX_ANCHORS - 8U + 1U, list.count);
    TEST_ASSERT_EQUAL_UINT32 (5, list.anchors[0].boot_id);
    uint32_t e = 0;
    TEST_ASSERT_FALSE (vlmt_epoch_get (&list, 4, 0, VLMT_NO_OFFSET, &e));
    TEST_ASSERT_TRUE (vlmt_epoch_get (&list, 5, 0, VLMT_NO_OFFSET, &e));
}

void test_session_anchors_stay_together (void)
{
    TEST_ASSERT_TRUE (add (1, 0, EPOCH0));
    TEST_ASSERT_TRUE (add (2, 0, EPOCH0 + (10U * DAY)));
    // Session 1 cannot get a later anchor after a reboot in reality, but the list must stay sorted.
    TEST_ASSERT_TRUE (add (1, DAY, EPOCH0 + DAY));
    TEST_ASSERT_EQUAL_UINT32 (3, list.count);
    TEST_ASSERT_EQUAL_UINT32 (1, list.anchors[0].boot_id);
    TEST_ASSERT_EQUAL_UINT32 (1, list.anchors[1].boot_id);
    TEST_ASSERT_EQUAL_UINT32 (2, list.anchors[2].boot_id);
}

void test_corrupt_count_is_ignored (void)
{
    uint32_t e = 0;
    list.count = 0xFFFFFFFFU;
    TEST_ASSERT_FALSE (vlmt_epoch_get (&list, 1, 0, VLMT_NO_OFFSET, &e));
    TEST_ASSERT_TRUE (add (1, 0, EPOCH0));
    TEST_ASSERT_EQUAL_UINT32 (1, list.count);
}
