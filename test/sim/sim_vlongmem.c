/**
 * Host simulation of the vlongmem log module.
 *
 * The real app_log_vlongmem.c operates against a fake nRF52 flash in RAM,
 * with the rules of the real flash: erase sets 0xFF, a write can only clear
 * bits, a word can be written two times between erases, writes are
 * word-aligned. The simulation drives the module through some years of
 * heartbeats with a slow tag clock, phone connections, reboots, power cuts
 * and gaps. After each connection it reads the full log back and compares
 * each sample and its time with the truth. Build and operate with
 * scripts/sim-vlongmem.sh, with AddressSanitizer and UndefinedBehaviorSanitizer.
 */
#include "app_log.h"
#include "app_log_vlongmem_codec.h"
#include "app_log_vlongmem_fake.h"
#include "ruuvi_driver_error.h"
#include "ruuvi_driver_sensor.h"
#include "ruuvi_interface_flash.h"
#include "ruuvi_interface_log.h"
#include "ruuvi_interface_rtc.h"
#include "ruuvi_interface_yield.h"
#include "ruuvi_task_flash.h"
#include "nrf_fstorage_sd.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define REGION_START (APP_VLONGMEM_REGION_START)
#define REGION_END   (APP_VLONGMEM_REGION_END)
#define REGION_SIZE  (REGION_END - REGION_START)
#define PAGE_SIZE    (4096U)
#define INTERVAL_MS  ((uint64_t) APP_LOG_INTERVAL_S * 1000ULL)
#define HEARTBEAT_MS (2570ULL)
#define EPOCH_BASE_S (1735689600ULL) // 2025-01-01
#define DAY_MS       (86400000ULL)

#define FAIL(...) do { fprintf (stderr, "FAIL: " __VA_ARGS__); fprintf (stderr, "\n"); exit (1); } while (0)

/* ---------------------------------------------------------------- fake flash */

static uint8_t * g_flash;                    // Mapped at REGION_START.
static uint8_t g_wcount[REGION_SIZE / 4U];   // Writes for each word since the erase.
static int64_t g_cut_after_words = -1;       // A power cut stops the next write here.
static bool g_cut_hit;
static uint64_t g_writes;
static uint64_t g_erases;

nrf_fstorage_api_t nrf_fstorage_sd;

static void flash_map (void)
{
    void * p = mmap ( (void *) (uintptr_t) REGION_START, REGION_SIZE, PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);

    if ( (MAP_FAILED == p) || ( (uintptr_t) p != REGION_START))
    {
        FAIL ("cannot map the fake flash at 0x%x", (unsigned) REGION_START);
    }

    g_flash = p;
    memset (g_flash, 0xFF, REGION_SIZE);
}

ret_code_t nrf_fstorage_init (nrf_fstorage_t * p_fs, nrf_fstorage_api_t * p_api, void * p_param)
{
    (void) p_api;
    (void) p_param;

    if ( (p_fs->start_addr != REGION_START) || (p_fs->end_addr != REGION_END))
    {
        FAIL ("fstorage bounds");
    }

    return NRF_SUCCESS;
}

ret_code_t nrf_fstorage_write (nrf_fstorage_t const * p_fs, uint32_t dest, void const * p_src,
                               uint32_t len, void * p_param)
{
    (void) p_param;

    if ( (dest < p_fs->start_addr) || ( (dest + len) > p_fs->end_addr)) { FAIL ("write out of region 0x%x+%u", dest, len); }

    if ( (0U != (dest % 4U)) || (0U != (len % 4U)) || (0U == len)) { FAIL ("write alignment 0x%x+%u", dest, len); }

    if (0U != ( ( (uintptr_t) p_src) % 4U)) { FAIL ("write source alignment"); }

    const uint8_t * src = p_src;

    for (uint32_t w = 0; w < (len / 4U); w++)
    {
        if ( (g_cut_after_words >= 0) && ( (int64_t) w >= g_cut_after_words))
        {
            g_cut_hit = true;
            g_cut_after_words = -1;
            break;
        }

        const uint32_t word = (dest - REGION_START) / 4U + w;
        g_wcount[word]++;

        if (g_wcount[word] > 2U) { FAIL ("word 0x%x written %u times", dest + (w * 4U), g_wcount[word]); }

        for (uint32_t b = 0; b < 4U; b++)
        {
            const uint8_t old = g_flash[dest - REGION_START + (w * 4U) + b];
            const uint8_t new = src[ (w * 4U) + b];

            if ( (old & new) != new) { FAIL ("write sets a bit to 1 at 0x%x", dest + (w * 4U) + b); }

            g_flash[dest - REGION_START + (w * 4U) + b] = new;
        }
    }

    g_writes++;
    nrf_fstorage_evt_t evt = { NRF_FSTORAGE_EVT_WRITE_RESULT, NRF_SUCCESS, dest, p_src, len, NULL };
    p_fs->evt_handler (&evt);
    return NRF_SUCCESS;
}

ret_code_t nrf_fstorage_erase (nrf_fstorage_t const * p_fs, uint32_t page_addr, uint32_t len,
                               void * p_param)
{
    (void) p_param;

    if ( (page_addr < p_fs->start_addr) || ( (page_addr + (len * PAGE_SIZE)) > p_fs->end_addr)) { FAIL ("erase out of region"); }

    if (0U != (page_addr % PAGE_SIZE)) { FAIL ("erase alignment"); }

    memset (&g_flash[page_addr - REGION_START], 0xFF, len * PAGE_SIZE);
    memset (&g_wcount[ (page_addr - REGION_START) / 4U], 0, len * PAGE_SIZE / 4U);
    g_erases++;
    nrf_fstorage_evt_t evt = { NRF_FSTORAGE_EVT_ERASE_RESULT, NRF_SUCCESS, page_addr, NULL, len, NULL };
    p_fs->evt_handler (&evt);
    return NRF_SUCCESS;
}

bool nrf_fstorage_is_busy (nrf_fstorage_t const * p_fs)
{
    (void) p_fs;
    return false;
}

/* ---------------------------------------------------------- FDS records stub */

typedef struct
{
    uint16_t file;
    uint16_t record;
    size_t len;
    uint8_t data[1024];
} fds_rec_t;

static fds_rec_t g_records[8];
static size_t g_record_count;

rd_status_t rt_flash_store (const uint16_t file_id, const uint16_t record_id,
                            const void * const message, const size_t message_length)
{
    if (message_length > sizeof (g_records[0].data)) { FAIL ("record too large"); }

    for (size_t ii = 0; ii < g_record_count; ii++)
    {
        if ( (g_records[ii].file == file_id) && (g_records[ii].record == record_id))
        {
            memcpy (g_records[ii].data, message, message_length);
            g_records[ii].len = message_length;
            return RD_SUCCESS;
        }
    }

    if (g_record_count >= 8U) { FAIL ("too many records"); }

    g_records[g_record_count].file = file_id;
    g_records[g_record_count].record = record_id;
    g_records[g_record_count].len = message_length;
    memcpy (g_records[g_record_count].data, message, message_length);
    g_record_count++;
    return RD_SUCCESS;
}

rd_status_t rt_flash_load (const uint16_t file_id, const uint16_t record_id,
                           void * const message, const size_t message_length)
{
    for (size_t ii = 0; ii < g_record_count; ii++)
    {
        if ( (g_records[ii].file == file_id) && (g_records[ii].record == record_id))
        {
            if (g_records[ii].len > message_length) { return RD_ERROR_DATA_SIZE; }

            memcpy (message, g_records[ii].data, g_records[ii].len);
            return RD_SUCCESS;
        }
    }

    return RD_ERROR_NOT_FOUND;
}

bool rt_flash_busy (void)
{
    return false;
}

void ri_flash_purge (void)
{
    memset (g_flash, 0xFF, REGION_SIZE);
    memset (g_wcount, 0, sizeof (g_wcount));
    g_record_count = 0;
}

/* ------------------------------------------------------------- other stubs */

static bool g_verbose;

void ri_log (const ri_log_severity_t severity, const char * const message)
{
    (void) severity;

    if (g_verbose) { fputs (message, stdout); }
}

rd_status_t ri_yield (void)
{
    return RD_SUCCESS;
}

/* ----------------------------------------------------------- simulated time */

static uint64_t g_real_ms;       // Simulated real time from EPOCH_BASE_S.
static uint64_t g_boot_real_ms;  // Real time of the current tag boot.
static int64_t g_drift_ppm = -50; // The tag clock is slow.

uint64_t ri_rtc_millis (void)
{
    const uint64_t e = g_real_ms - g_boot_real_ms;
    return (uint64_t) ( (int64_t) e + ( ( (int64_t) e * g_drift_ppm) / 1000000LL));
}

static uint32_t real_epoch_s (void)
{
    return (uint32_t) (EPOCH_BASE_S + (g_real_ms / 1000ULL));
}

/* -------------------------------------------------------------------- truth */

typedef struct
{
    uint32_t session;
    uint32_t uptime_s;
    uint64_t real_ms;    // Real time of the slot.
    vlmc_sample_t s;
    bool may_be_absent;
    bool seen;
} truth_t;

static truth_t * g_truth;
static size_t g_truth_count;
static size_t g_truth_cap;

typedef struct
{
    uint64_t boot_real_ms;
} session_t;

static session_t g_sessions[64];
static uint32_t g_session;  // Current session, 1..

static void truth_add (const uint32_t uptime_s, const uint64_t slot_real_ms,
                       const vlmc_sample_t * const s)
{
    if (g_truth_count >= g_truth_cap)
    {
        g_truth_cap = g_truth_cap ? (g_truth_cap * 2U) : 4096U;
        g_truth = realloc (g_truth, g_truth_cap * sizeof (truth_t));

        if (NULL == g_truth) { FAIL ("realloc"); }
    }

    truth_t * t = &g_truth[g_truth_count++];
    memset (t, 0, sizeof (*t));
    t->session = g_session;
    t->uptime_s = uptime_s;
    t->real_ms = slot_real_ms;
    t->s = *s;
}

// Grid mirror, the same rules as app_log_process.
static bool g_grid_started;
static uint64_t g_grid_next_ms;

static void grid_reset (void)
{
    g_grid_started = false;
}

/** @brief Real time of a tag uptime in the current session, inverse of ri_rtc_millis. */
static uint64_t real_of_uptime_ms (const uint64_t uptime_ms)
{
    // e + e * ppm / 1e6 = uptime  =>  e = uptime * 1e6 / (1e6 + ppm)
    const uint64_t e = (uptime_ms * 1000000ULL) / (uint64_t) (1000000LL + g_drift_ppm);
    return g_boot_real_ms + e;
}

static void heartbeat (void)
{
    const uint64_t ts = ri_rtc_millis();
    const uint32_t real_slot = (uint32_t) (g_real_ms / INTERVAL_MS);
    vlmc_sample_t s;
    vlmf_sample (real_slot, &s);
    // Feed the module.
    rd_sensor_data_t data = {0};
    float values[3] = {0};
    data.fields.datas.temperature_c = 1;
    data.fields.datas.humidity_rh = 1;
    data.fields.datas.pressure_pa = 1;
    data.data = values;
    data.timestamp_ms = ts;
    float t = 0, h = 0, p = 0;
    vlmc_sample_to_units (&s, &t, &h, &p);

    if (s.valid[VLMC_FIELD_TEMPERATURE]) { rd_sensor_data_set (&data, RD_SENSOR_TEMP_FIELD, t); }

    if (s.valid[VLMC_FIELD_HUMIDITY]) { rd_sensor_data_set (&data, RD_SENSOR_HUMI_FIELD, h); }

    if (s.valid[VLMC_FIELD_PRESSURE]) { rd_sensor_data_set (&data, RD_SENSOR_PRES_FIELD, p); }

    const rd_status_t err = app_log_process (&data);

    if (RD_SUCCESS != err) { FAIL ("app_log_process 0x%x at %" PRIu64, err, g_real_ms); }

    // Mirror the grid.
    if (!g_grid_started)
    {
        g_grid_next_ms = ts;
        g_grid_started = true;
    }

    if (ts < g_grid_next_ms) { return; }

    uint64_t missed = (ts - g_grid_next_ms) / INTERVAL_MS;

    if (missed > APP_VLONGMEM_MAX_MISSING)
    {
        g_grid_next_ms = ts;
        missed = 0;
    }

    const vlmc_sample_t missing = { .valid = { false, false, false } };

    for (uint64_t ii = 0; ii < missed; ii++)
    {
        truth_add ( (uint32_t) (g_grid_next_ms / 1000ULL), real_of_uptime_ms (g_grid_next_ms), &missing);
        g_grid_next_ms += INTERVAL_MS;
    }

    truth_add ( (uint32_t) (g_grid_next_ms / 1000ULL), real_of_uptime_ms (g_grid_next_ms), &s);
    g_grid_next_ms += INTERVAL_MS;
}

static void mark_tail_may_be_absent (const size_t n)
{
    for (size_t ii = 0; (ii < n) && (ii < g_truth_count); ii++)
    {
        g_truth[g_truth_count - 1U - ii].may_be_absent = true;
    }
}

static void boot (void)
{
    g_session++;

    if (g_session >= 64U) { FAIL ("too many sessions"); }

    g_boot_real_ms = g_real_ms;
    g_sessions[g_session].boot_real_ms = g_real_ms;
    grid_reset();
    const rd_status_t err = app_log_init();

    if (RD_SUCCESS != err) { FAIL ("app_log_init 0x%x", err); }
}

/* ------------------------------------------------------------------- reading */

static uint64_t g_reads;
static uint64_t g_samples_read;

/** @brief Read the full log from start_s and compare with the truth. */
static void sync_and_verify (const uint32_t start_s, const uint64_t retention_ms)
{
    app_log_time_set (real_epoch_s());
    rd_sensor_data_t sample = {0};
    float values[3] = {0};
    sample.fields.datas.temperature_c = 1;
    sample.fields.datas.humidity_rh = 1;
    sample.fields.datas.pressure_pa = 1;
    sample.data = values;
    app_log_read_state_t rs =
    {
        .oldest_element_ms = (uint64_t) start_s * 1000ULL,
        .element_idx = 0,
        .page_idx = 0
    };

    for (size_t ii = 0; ii < g_truth_count; ii++) { g_truth[ii].seen = false; }

    uint64_t last_epoch_ms = 0;
    size_t count = 0;
    size_t cursor = 0; // Truth entries come in time order, the read as well.

    while (true)
    {
        sample.valid.bitfield = 0;
        const rd_status_t err = app_log_read (&sample, &rs);

        if (RD_ERROR_NOT_FOUND == err) { break; }

        if (RD_SUCCESS != err) { FAIL ("app_log_read 0x%x", err); }

        count++;
        const uint64_t epoch_ms = sample.timestamp_ms;

        if (epoch_ms < ( (uint64_t) start_s * 1000ULL)) { FAIL ("sample before start"); }

        if (epoch_ms < last_epoch_ms) { FAIL ("time goes backwards at %" PRIu64, epoch_ms); }

        last_epoch_ms = epoch_ms;
        // Find the truth entry: the same real time in 3 s.
        const uint64_t real_ms = epoch_ms - (EPOCH_BASE_S * 1000ULL);
        // The nearest unseen entry in the window. Around a reboot, the last
        // slot of the old session and the first slot of the new session can
        // be some seconds apart.
        truth_t * t = NULL;
        int64_t best = 3000;
        bool window_started = false;

        for (size_t ii = cursor; ii < g_truth_count; ii++)
        {
            const int64_t d = (int64_t) g_truth[ii].real_ms - (int64_t) real_ms;

            if (d >= 3000) { break; }

            if (d <= -3000) { continue; }

            if (!window_started) { cursor = ii; window_started = true; }

            const int64_t ad = (d < 0) ? -d : d;

            if ( (ad < best) && !g_truth[ii].seen) { best = ad; t = &g_truth[ii]; }
        }

        if (NULL == t) { FAIL ("no truth for epoch %" PRIu64 " (read %" PRIu64 ")", (uint64_t) (epoch_ms / 1000ULL), g_reads); }

        if (t->seen) { FAIL ("sample returned twice at %" PRIu64, epoch_ms); }

        t->seen = true;
        // Compare the values.
        vlmc_sample_t got;
        vlmc_sample_from_units (&got,
                                rd_sensor_data_parse (&sample, RD_SENSOR_TEMP_FIELD), true,
                                rd_sensor_data_parse (&sample, RD_SENSOR_HUMI_FIELD), true,
                                rd_sensor_data_parse (&sample, RD_SENSOR_PRES_FIELD), true);

        for (size_t f = 0; f < VLMC_FIELDS; f++)
        {
            if (got.valid[f] != t->s.valid[f])
            {
                FAIL ("validity of field %zu at %" PRIu64 ": got %d want %d", f, epoch_ms, got.valid[f], t->s.valid[f]);
            }

            if (got.valid[f] && (got.value[f] != t->s.value[f]))
            {
                FAIL ("field %zu at %" PRIu64 ": got %d want %d", f, epoch_ms, got.value[f], t->s.value[f]);
            }
        }
    }

    // Every entry in the retention window must be present.
    size_t must = 0;

    for (size_t ii = 0; ii < g_truth_count; ii++)
    {
        const truth_t * t = &g_truth[ii];

        if ( (t->real_ms + retention_ms) < g_real_ms) { continue; }

        if ( ( (t->real_ms / 1000ULL) + EPOCH_BASE_S) < start_s) { continue; }

        if (t->may_be_absent) { continue; }

        // The last sample of a block from an earlier session is not used
        // when its pressure is missing (the power cut rule). Not required.
        if (!t->s.valid[VLMC_FIELD_PRESSURE]) { continue; }

        must++;

        if (!t->seen)
        {
            FAIL ("truth entry missing: session %u uptime %u real day %.2f (read %" PRIu64 ")",
                  t->session, t->uptime_s, (double) t->real_ms / (double) DAY_MS, g_reads);
        }
    }

    g_reads++;
    g_samples_read += count;

    if (g_verbose)
    {
        printf ("read %" PRIu64 ": day %.1f, %zu samples, %zu required\n",
                g_reads, (double) g_real_ms / (double) DAY_MS, count, must);
    }
}

/* ------------------------------------------------------------------ scenario */

static void advance (const uint64_t ms)
{
    const uint64_t end = g_real_ms + ms;

    while ( (g_real_ms < end) && !g_cut_hit)
    {
        g_real_ms += HEARTBEAT_MS;
        heartbeat();
    }
}

int main (int argc, char ** argv)
{
    const uint32_t days = (argc > 1) ? (uint32_t) atoi (argv[1]) : 800U;
    g_verbose = (argc > 2);
    flash_map();
    g_real_ms = 0;
    boot();
    // First connection one hour after the boot.
    advance (3600ULL * 1000ULL);
    sync_and_verify (0, 300ULL * DAY_MS);
    uint32_t day = 0;
    uint32_t next_sync_day = 30;
    uint32_t next_reboot_day = 100;

    while (day < days)
    {
        advance (DAY_MS);

        if (g_cut_hit)
        {
            // Power loss in a flash write: the tag reboots at once. The RAM
            // buffer and the cut write are lost.
            g_cut_hit = false;
            mark_tail_may_be_absent (20);
            boot();
            advance (3600ULL * 1000ULL);
            sync_and_verify (0, 300ULL * DAY_MS);
            continue;
        }

        day++;

        if (day == next_sync_day)
        {
            // A GATT transfer blocks the sampler for 25 min before the sync.
            g_real_ms += 25ULL * 60ULL * 1000ULL;
            sync_and_verify (0, 300ULL * DAY_MS);
            // Also a windowed read, as the apps do.
            sync_and_verify (real_epoch_s() - (10U * 86400U), 300ULL * DAY_MS);
            next_sync_day += 30;
        }

        if (day == next_reboot_day)
        {
            if (0U == ( (day / 100U) % 2U))
            {
                // Clean reboot, the RAM buffer is lost.
                mark_tail_may_be_absent (20);
                boot();
                advance (3600ULL * 1000ULL);
                sync_and_verify (0, 300ULL * DAY_MS);
            }
            else
            {
                // Power cut in the next flash write.
                g_cut_after_words = 3;
            }

            next_reboot_day += 100;
        }
    }

    printf ("OK: %u days, %u sessions, %zu truth samples, %" PRIu64 " reads, %" PRIu64
            " samples read, %" PRIu64 " writes, %" PRIu64 " erases\n",
            days, g_session, g_truth_count, g_reads, g_samples_read, g_writes, g_erases);
    return 0;
}
