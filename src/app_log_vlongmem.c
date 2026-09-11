/**
 * @file app_log_vlongmem.c
 * @brief Environmental log for one year in a raw flash ring buffer.
 *
 * This file has the app_log.h API for the vlongmem variant. The samples are
 * delta encoded (app_log_vlongmem_codec.c) into 4 kB blocks, one block for
 * each flash page. The blocks are written through nrf_fstorage. The layout is
 * in docs/vlongmem/FORMAT.md, the design is in docs/vlongmem/README.md.
 *
 * Time: the samples are on a fixed grid of APP_LOG_INTERVAL_S from the start
 * uptime of the block. A slot without a sample is written as a missing
 * marker. Thus the grid does not move. The phone gives the absolute time at
 * each log read. The time is kept as an anchor (app_log_vlongmem_time.c) for
 * the drift correction, and as an epoch offset in the block headers of the
 * session.
 *
 * @copyright Ruuvi Innovations Ltd, license BSD-3-Clause.
 */
#include "app_log.h"

#if APP_VLONGMEM_ENABLED

#include "app_config.h"
#include "app_log_vlongmem_codec.h"
#include "app_log_vlongmem_time.h"
#if APP_VLONGMEM_FAKE_DATA
#include "app_log_vlongmem_fake.h"
#endif
#include "ruuvi_driver_error.h"
#include "ruuvi_driver_sensor.h"
#include "ruuvi_interface_flash.h"
#include "ruuvi_interface_log.h"
#include "ruuvi_interface_rtc.h"
#include "ruuvi_interface_yield.h"
#include "ruuvi_task_flash.h"

#include "nrf_fstorage.h"
#include "nrf_fstorage_sd.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define VLM_PAGE_SIZE      (RB_FLASH_PAGE_SIZE)
#define VLM_PAGES          ((APP_VLONGMEM_REGION_END - APP_VLONGMEM_REGION_START) / VLM_PAGE_SIZE)
#define VLM_MAGIC          (0x4D4C5652U) //!< "RVLM" as little-endian bytes.
#define VLM_FORMAT_VERSION (1U)
#define VLM_HEADER_SIZE    (32U)
#define VLM_DATA_SIZE      (VLM_PAGE_SIZE - VLM_HEADER_SIZE)
#define VLM_UNKNOWN        (0xFFFFFFFFU)
#define VLM_INTERVAL_MS    ((uint64_t) APP_LOG_INTERVAL_S * 1000ULL)
#define VLM_MIN_SAMPLE_BYTES (VLMC_FIELDS)
#define VLM_MAX_SAMPLES_PER_BLOCK (VLM_DATA_SIZE / VLM_MIN_SAMPLE_BYTES)
/** @brief RAM buffer: the samples at the maximum size, and the word padding. */
#define VLM_PENDING_MAX    ((APP_VLONGMEM_FLUSH_SAMPLES * VLMC_MAX_SAMPLE_BYTES) + 4U)

#define ROUND_UP4(x) (((x) + 3U) & ~3U)

/** @brief Uptime bias: the synthetic year of the fake data build is before the boot. */
#if APP_VLONGMEM_FAKE_DATA
#   define VLM_UPTIME_BIAS_MS ((uint64_t) APP_VLONGMEM_FAKE_SAMPLES * VLM_INTERVAL_MS)
#else
#   define VLM_UPTIME_BIAS_MS (0ULL)
#endif

/** @brief Block header, the first 32 bytes of each page. Erased flash = 0xFF. */
typedef struct
{
    uint32_t magic;          //!< VLM_MAGIC.
    uint32_t seq;            //!< Block sequence number, from 1, always increases.
    uint32_t boot_id;        //!< Boot session that wrote the block.
    uint32_t start_uptime_s; //!< Uptime at the first sample, seconds.
    uint32_t epoch_offset_s; //!< Epoch minus uptime, VLM_UNKNOWN until a phone connects.
    uint16_t interval_s;     //!< Period of the grid.
    uint16_t version;        //!< VLM_FORMAT_VERSION.
    uint32_t reserved[2];    //!< 0xFF.
} vlm_header_t;

_Static_assert (sizeof (vlm_header_t) == VLM_HEADER_SIZE, "header size");
_Static_assert (VLM_UNKNOWN == VLMT_NO_OFFSET, "unknown offset value");
_Static_assert (VLM_PAGES >= 2U, "ring needs at least two pages");
_Static_assert (0 == (APP_VLONGMEM_REGION_START % RB_FLASH_PAGE_SIZE), "region alignment");
_Static_assert (0 == (APP_VLONGMEM_REGION_END % RB_FLASH_PAGE_SIZE), "region alignment");

static inline void LOGI (const char * const msg)
{
    ri_log (RI_LOG_LEVEL_INFO, msg);
}

static inline void LOGE (const char * const msg)
{
    ri_log (RI_LOG_LEVEL_ERROR, msg);
}

// Flash state.
static volatile bool m_fs_error;
static void fs_evt_handler (nrf_fstorage_evt_t * p_evt);
// NRF_FSTORAGE_DEF makes a global symbol. FDS has the name m_fs.
NRF_FSTORAGE_DEF (nrf_fstorage_t m_vlongmem_fs) =
{
    .evt_handler = fs_evt_handler,
    .start_addr  = APP_VLONGMEM_REGION_START,
    .end_addr    = APP_VLONGMEM_REGION_END,
};

// Session state.
static bool     m_initialized;
static uint32_t m_boot_id;
static uint32_t m_session_first_seq; //!< The first seq of this boot session.
static uint32_t m_epoch_offset_s = VLM_UNKNOWN;
static uint32_t m_next_seq;          //!< The seq of the next block.
static vlmt_anchors_t m_anchors;     //!< Phone time at tag uptime, in FDS.

// Open block state.
static bool     m_block_open;
static uint32_t m_head_seq;
static uint32_t m_head_page;
static uint16_t m_flash_len;       //!< Data bytes in flash in the open block.
static uint8_t  m_pending[VLM_PENDING_MAX] __attribute__ ( (aligned (4)));
static uint16_t m_pending_len;
static uint16_t m_pending_samples;
static vlmc_state_t m_enc;

// Sample grid.
static bool     m_grid_started;
static uint64_t m_next_sample_ms;

// Word-aligned buffer for small writes.
static uint32_t m_word_buf[VLM_HEADER_SIZE / 4U] __attribute__ ( (aligned (4)));

static void fs_evt_handler (nrf_fstorage_evt_t * p_evt)
{
    if (NRF_SUCCESS != p_evt->result)
    {
        m_fs_error = true;
    }
}

static void fs_wait (void)
{
    while (nrf_fstorage_is_busy (&m_vlongmem_fs))
    {
        ri_yield();
    }
}

static uint32_t page_addr (const uint32_t page)
{
    return APP_VLONGMEM_REGION_START + (page * VLM_PAGE_SIZE);
}

/**
 * @brief Uptime in seconds.
 *
 * The RTC driver reads the counter and the overflow count without a lock.
 * A read at the overflow can be 512 s low. Two reads that agree in 1 s are
 * correct.
 */
static uint32_t uptime_now_s (void)
{
    uint64_t a = ri_rtc_millis();

    for (uint32_t tries = 0; tries < 3U; tries++)
    {
        const uint64_t b = ri_rtc_millis();

        if ( (b >= a) && ( (b - a) < 1000ULL))
        {
            return (uint32_t) ( (b + VLM_UPTIME_BIAS_MS) / 1000ULL);
        }

        a = b;
    }

    return (uint32_t) ( (a + VLM_UPTIME_BIAS_MS) / 1000ULL);
}

static const vlm_header_t * page_header (const uint32_t page)
{
    return (const vlm_header_t *) (uintptr_t) page_addr (page);
}

static const uint8_t * page_data (const uint32_t page)
{
    return (const uint8_t *) (uintptr_t) (page_addr (page) + VLM_HEADER_SIZE);
}

static bool header_valid (const vlm_header_t * const p_h)
{
    return (VLM_MAGIC == p_h->magic)
           && (VLM_FORMAT_VERSION == p_h->version)
           && (0U != p_h->seq) && (VLM_UNKNOWN != p_h->seq);
}

/** @brief Write len bytes (a multiple of 4, from a word-aligned source) and wait. */
static rd_status_t fs_write (const uint32_t addr, const void * const p_src, const uint32_t len)
{
    ret_code_t rc;
    m_fs_error = false;

    do
    {
        rc = nrf_fstorage_write (&m_vlongmem_fs, addr, p_src, len, NULL);

        if (NRF_ERROR_NO_MEM == rc) { ri_yield(); }
    } while (NRF_ERROR_NO_MEM == rc);

    if (NRF_SUCCESS != rc)
    {
        LOGE ("vlongmem: write rejected\r\n");
        return RD_ERROR_INTERNAL;
    }

    fs_wait();
    return m_fs_error ? RD_ERROR_INTERNAL : RD_SUCCESS;
}

static rd_status_t fs_erase_page (const uint32_t page)
{
    ret_code_t rc;
    m_fs_error = false;

    do
    {
        rc = nrf_fstorage_erase (&m_vlongmem_fs, page_addr (page), 1, NULL);

        if (NRF_ERROR_NO_MEM == rc) { ri_yield(); }
    } while (NRF_ERROR_NO_MEM == rc);

    if (NRF_SUCCESS != rc)
    {
        LOGE ("vlongmem: erase rejected\r\n");
        return RD_ERROR_INTERNAL;
    }

    fs_wait();
    return m_fs_error ? RD_ERROR_INTERNAL : RD_SUCCESS;
}

/** @brief Write the samples in RAM, with 0xFF padding to a word. */
static rd_status_t flush (void)
{
    rd_status_t err_code = RD_SUCCESS;

    if ( (0U != m_pending_len) && m_block_open)
    {
        const uint16_t padded = (uint16_t) ROUND_UP4 (m_pending_len);
        memset (&m_pending[m_pending_len], 0xFF, padded - m_pending_len);
        err_code |= fs_write (page_addr (m_head_page) + VLM_HEADER_SIZE + m_flash_len,
                              m_pending, padded);
        m_flash_len += padded;
    }

    m_pending_len = 0;
    m_pending_samples = 0;
    return err_code;
}

static rd_status_t close_block (void)
{
    rd_status_t err_code = flush();
    m_block_open = false;
    return err_code;
}

static rd_status_t open_block (const uint32_t start_uptime_s)
{
    rd_status_t err_code = RD_SUCCESS;
    const uint32_t page = m_next_seq % VLM_PAGES;
    vlm_header_t * const p_h = (vlm_header_t *) m_word_buf;
    err_code |= fs_erase_page (page);
    memset (p_h, 0xFF, sizeof (vlm_header_t));
    p_h->magic = VLM_MAGIC;
    p_h->seq = m_next_seq;
    p_h->boot_id = m_boot_id;
    p_h->start_uptime_s = start_uptime_s;
    p_h->epoch_offset_s = m_epoch_offset_s;
    p_h->interval_s = (uint16_t) APP_LOG_INTERVAL_S;
    p_h->version = VLM_FORMAT_VERSION;
    err_code |= fs_write (page_addr (page), m_word_buf, sizeof (vlm_header_t));
    m_head_seq = m_next_seq;
    m_head_page = page;
    m_next_seq++;
    m_flash_len = 0;
    m_pending_len = 0;
    m_pending_samples = 0;
    memset (&m_enc, 0, sizeof (m_enc));
    m_block_open = true;
    char msg[64];
    snprintf (msg, sizeof (msg), "vlongmem: block %lu on page %lu\r\n",
              (unsigned long) m_head_seq, (unsigned long) page);
    LOGI (msg);
    return err_code;
}

/** @brief Encode one sample for the grid slot at slot_uptime_s and put it in the buffer. */
static rd_status_t append_sample (const vlmc_sample_t * const p_sample,
                                  const uint32_t slot_uptime_s)
{
    rd_status_t err_code = RD_SUCCESS;
    uint8_t tmp[VLMC_MAX_SAMPLE_BYTES];

    if (!m_block_open)
    {
        err_code |= open_block (slot_uptime_s);
    }

    vlmc_state_t saved = m_enc;
    size_t len = vlmc_encode (&m_enc, p_sample, tmp);

    if ( (m_flash_len + ROUND_UP4 (m_pending_len + len)) > VLM_DATA_SIZE)
    {
        m_enc = saved;
        err_code |= close_block();
        err_code |= open_block (slot_uptime_s);
        len = vlmc_encode (&m_enc, p_sample, tmp);
    }

    memcpy (&m_pending[m_pending_len], tmp, len);
    m_pending_len += (uint16_t) len;
    m_pending_samples++;

    if (m_pending_samples >= APP_VLONGMEM_FLUSH_SAMPLES)
    {
        err_code |= flush();
    }

    return err_code;
}

rd_status_t app_log_init (void)
{
    rd_status_t err_code = RD_SUCCESS;
    ret_code_t rc = nrf_fstorage_init (&m_vlongmem_fs, &nrf_fstorage_sd, NULL);

    if (NRF_SUCCESS != rc)
    {
        LOGE ("vlongmem: fstorage init failed\r\n");
        return RD_ERROR_INTERNAL;
    }

    // The boot session id comes from a counter in FDS.
    uint32_t boot_count = 0;
    err_code = rt_flash_load (APP_FLASH_LOG_FILE, APP_FLASH_LOG_BOOT_COUNTER_RECORD,
                              &boot_count, sizeof (boot_count));

    if (RD_ERROR_NOT_FOUND == err_code)
    {
        boot_count = 0;
        err_code = RD_SUCCESS;
    }

    boot_count++;

    while (rt_flash_busy()) { ri_yield(); }

    err_code |= rt_flash_store (APP_FLASH_LOG_FILE, APP_FLASH_LOG_BOOT_COUNTER_RECORD,
                                &boot_count, sizeof (boot_count));

    while (rt_flash_busy()) { ri_yield(); }

    m_boot_id = boot_count;
    // The time anchors of earlier sessions.
    memset (&m_anchors, 0, sizeof (m_anchors));
    rd_status_t anchor_status = rt_flash_load (APP_FLASH_LOG_FILE,
                                APP_FLASH_LOG_TIME_ANCHORS_RECORD,
                                &m_anchors, sizeof (m_anchors));

    if (RD_SUCCESS != anchor_status)
    {
        memset (&m_anchors, 0, sizeof (m_anchors));
    }

    // Find the newest block. The next block continues from there.
    uint32_t max_seq = 0;

    for (uint32_t page = 0; page < VLM_PAGES; page++)
    {
        const vlm_header_t * const p_h = page_header (page);

        if (header_valid (p_h) && (p_h->seq > max_seq))
        {
            max_seq = p_h->seq;
        }
    }

    m_next_seq = max_seq + 1U;
    m_session_first_seq = m_next_seq;
    m_epoch_offset_s = VLM_UNKNOWN;
    m_block_open = false;
    m_grid_started = false;
    m_initialized = true;
#if APP_VLONGMEM_FAKE_DATA

    if (0U == max_seq)
    {
        // Empty ring: write the synthetic year. The live grid continues after it.
        LOGI ("vlongmem: fake data\r\n");

        for (uint32_t slot = 0; slot < APP_VLONGMEM_FAKE_SAMPLES; slot++)
        {
            vlmc_sample_t s;
            vlmf_sample (slot, &s);
            err_code |= append_sample (&s, slot * APP_LOG_INTERVAL_S);
        }

        m_grid_started = true;
        m_next_sample_ms = VLM_UPTIME_BIAS_MS;
    }

#endif
    char msg[96];
    snprintf (msg, sizeof (msg), "vlongmem: boot %lu, %lu pages, next block %lu\r\n",
              (unsigned long) m_boot_id, (unsigned long) VLM_PAGES, (unsigned long) m_next_seq);
    LOGI (msg);
    return err_code;
}

rd_status_t app_log_process (const rd_sensor_data_t * const sample)
{
    rd_status_t err_code = RD_SUCCESS;

    if (!m_initialized || (NULL == sample))
    {
        return RD_ERROR_INVALID_STATE;
    }

    const uint64_t ts = sample->timestamp_ms + VLM_UPTIME_BIAS_MS;

    if (!m_grid_started)
    {
        m_next_sample_ms = ts;
        m_grid_started = true;
    }

    if (ts < m_next_sample_ms)
    {
        return RD_SUCCESS;
    }

    // Slots without a sample, for example during a GATT transfer.
    uint64_t missed = (ts - m_next_sample_ms) / VLM_INTERVAL_MS;

    if (missed > APP_VLONGMEM_MAX_MISSING)
    {
        err_code |= close_block();
        m_next_sample_ms = ts;
        missed = 0;
    }

    const vlmc_sample_t missing = { .valid = { false, false, false } };

    for (uint64_t ii = 0; ii < missed; ii++)
    {
        err_code |= append_sample (&missing, (uint32_t) (m_next_sample_ms / 1000ULL));
        m_next_sample_ms += VLM_INTERVAL_MS;
    }

    vlmc_sample_t s;
    vlmc_sample_from_units (&s,
                            rd_sensor_data_parse (sample, RD_SENSOR_TEMP_FIELD), true,
                            rd_sensor_data_parse (sample, RD_SENSOR_HUMI_FIELD), true,
                            rd_sensor_data_parse (sample, RD_SENSOR_PRES_FIELD), true);
    err_code |= append_sample (&s, (uint32_t) (m_next_sample_ms / 1000ULL));
    m_next_sample_ms += VLM_INTERVAL_MS;
    return err_code;
}

void app_log_time_set (const uint32_t epoch_now_s)
{
    if (!m_initialized) { return; }

    const uint32_t uptime_s = uptime_now_s();
    // The oldest session with data in the ring: its anchors must stay.
    uint32_t oldest_boot_id = m_boot_id;

    for (uint32_t page = 0; page < VLM_PAGES; page++)
    {
        const vlm_header_t * const p_h = page_header (page);

        if (header_valid (p_h) && (p_h->boot_id < oldest_boot_id))
        {
            oldest_boot_id = p_h->boot_id;
        }
    }

    if (vlmt_anchor_add (&m_anchors, m_boot_id, uptime_s, epoch_now_s, oldest_boot_id))
    {
        while (rt_flash_busy()) { ri_yield(); }

        (void) rt_flash_store (APP_FLASH_LOG_FILE, APP_FLASH_LOG_TIME_ANCHORS_RECORD,
                               &m_anchors, sizeof (m_anchors));

        while (rt_flash_busy()) { ri_yield(); }
    }

    if (epoch_now_s < VLMT_MIN_EPOCH_S) { return; } // The phone clock is not set.

    m_epoch_offset_s = epoch_now_s - uptime_s;
    m_word_buf[0] = m_epoch_offset_s;

    // The blocks of this session are the blocks with seq >= m_session_first_seq.
    for (uint32_t page = 0; page < VLM_PAGES; page++)
    {
        const vlm_header_t * const p_h = page_header (page);

        if (header_valid (p_h)
                && (p_h->seq >= m_session_first_seq)
                && (p_h->boot_id == m_boot_id)
                && (VLM_UNKNOWN == p_h->epoch_offset_s))
        {
            (void) fs_write (page_addr (page) + offsetof (vlm_header_t, epoch_offset_s),
                             m_word_buf, sizeof (uint32_t));
        }
    }
}

/**
 * @brief Move the read state to the next block that has a date.
 *
 * @return true if a block was found, false when there are no more blocks.
 */
static bool next_block (app_log_read_state_t * const p_rs)
{
    const uint32_t oldest_seq = (m_next_seq > VLM_PAGES) ? (m_next_seq - VLM_PAGES) : 1U;
    uint32_t seq = (0U == p_rs->seq) ? oldest_seq : (p_rs->seq + 1U);

    for (; seq < m_next_seq; seq++)
    {
        const uint32_t page = seq % VLM_PAGES;
        const vlm_header_t * const p_h = page_header (page);

        if (!header_valid (p_h) || (p_h->seq != seq)) { continue; }

        uint32_t offset_s = p_h->epoch_offset_s;

        if ( (VLM_UNKNOWN == offset_s) && (seq >= m_session_first_seq))
        {
            offset_s = m_epoch_offset_s; // VLM_UNKNOWN when this session has not connected.
        }

        // The last possible sample of the block, with a margin for the drift correction.
        const uint32_t last_uptime_s = p_h->start_uptime_s
                                       + (VLM_MAX_SAMPLES_PER_BLOCK * p_h->interval_s)
                                       + 86400U;
        uint32_t latest_s = 0;

        if (!vlmt_epoch_get (&m_anchors, p_h->boot_id, last_uptime_s, offset_s, &latest_s))
        {
            continue; // The session never connected to a phone, the block has no date.
        }

        if ( ( (uint64_t) latest_s * 1000ULL) < p_rs->oldest_element_ms) { continue; }

        p_rs->seq = seq;
        p_rs->pos = 0;
        p_rs->element_idx = 0;
        p_rs->interval_s = p_h->interval_s;
        p_rs->block_start_uptime_s = p_h->start_uptime_s;
        p_rs->block_boot_id = p_h->boot_id;
        p_rs->block_offset_s = offset_s;
        memset (&p_rs->dec, 0, sizeof (p_rs->dec));
        p_rs->block_valid = true;
        return true;
    }

    p_rs->seq = m_next_seq;
    return false;
}

/**
 * @brief Find if the sample at pos is the last one in the block and can be cut.
 *
 * A power loss during a write stops at a word boundary. The bytes after it
 * are 0xFF. A cut in a field before the last field makes the next field
 * start with 0xFF, and the decoder stops. A cut in the absolute bytes of an
 * escape in the last field (pressure) decodes as a value, because 0xFF is a
 * valid absolute byte. Thus the last sample of a block from an earlier
 * session is not used when its pressure is an escape with 0xFF as the high
 * byte. A real pressure never has that value (more than 115 kPa), only the
 * missing sentinel does. That loss is one missing marker at most.
 */
static bool last_sample_is_cut (const uint8_t * const p_flash, const uint16_t pos,
                                const size_t n, const uint16_t flash_len)
{
    uint16_t next = (uint16_t) (pos + n);

    if (0U != (next % 4U)) { next = (uint16_t) ROUND_UP4 (next); }

    if ( (next < flash_len) && (VLMC_END != p_flash[next])) { return false; }

    // The last field is an escape when the sample ends with three escape bytes.
    return (n >= 3U)
           && (VLMC_ESCAPE == p_flash[pos + n - 3U])
           && (VLMC_END == p_flash[pos + n - 1U]);
}

/**
 * @brief Decode the next sample of the current block.
 *
 * @return true if a sample was decoded, false when the block has no more samples.
 */
static bool decode_next (app_log_read_state_t * const p_rs, vlmc_sample_t * const p_out)
{
    const uint32_t page = p_rs->seq % VLM_PAGES;
    const bool is_open_head = m_block_open && (p_rs->seq == m_head_seq);
    const uint8_t * const p_flash = page_data (page);
    const uint16_t flash_len = is_open_head ? m_flash_len : (uint16_t) VLM_DATA_SIZE;
    uint16_t pos = p_rs->pos;

    if (pos < flash_len)
    {
        // The word padding after a write shows as 0xFF at the start of a sample.
        if (VLMC_END == p_flash[pos])
        {
            pos = (uint16_t) ROUND_UP4 (pos);
        }

        if (pos < flash_len)
        {
            const size_t n = vlmc_decode (&p_rs->dec, &p_flash[pos], flash_len - pos, p_out);

            if (0U == n)
            {
                // The end of the data (closed block), or a power loss cut the data.
                if (!is_open_head) { return false; }

                pos = flash_len;
            }
            else if ( (p_rs->block_boot_id != m_boot_id)
                      && last_sample_is_cut (p_flash, pos, n, flash_len))
            {
                // A power loss in an earlier session cut this sample. Its
                // absolute bytes are erased flash, thus its value is not known.
                return false;
            }
            else
            {
                p_rs->pos = (uint16_t) (pos + n);
                return true;
            }
        }
    }

    if (is_open_head)
    {
        const uint16_t ram_pos = (uint16_t) (pos - flash_len);

        if (ram_pos < m_pending_len)
        {
            const size_t n = vlmc_decode (&p_rs->dec, &m_pending[ram_pos],
                                          m_pending_len - ram_pos, p_out);

            if (0U != n)
            {
                p_rs->pos = (uint16_t) (pos + n);
                return true;
            }
        }
    }

    return false;
}

rd_status_t app_log_read (rd_sensor_data_t * const sample,
                          app_log_read_state_t * const p_rs)
{
    if ( (NULL == sample) || (NULL == p_rs))
    {
        return RD_ERROR_NULL;
    }

    if (!m_initialized)
    {
        return RD_ERROR_NOT_FOUND;
    }

    while (true)
    {
        if (!p_rs->block_valid && !next_block (p_rs))
        {
            return RD_ERROR_NOT_FOUND;
        }

        vlmc_sample_t s = {0};

        if (!decode_next (p_rs, &s))
        {
            p_rs->block_valid = false;
            continue;
        }

        const uint32_t idx = p_rs->element_idx++;
        const uint32_t uptime_s = p_rs->block_start_uptime_s + (idx * p_rs->interval_s);
        uint32_t epoch_s = 0;

        if (!vlmt_epoch_get (&m_anchors, p_rs->block_boot_id, uptime_s,
                             p_rs->block_offset_s, &epoch_s))
        {
            p_rs->block_valid = false;
            continue;
        }

        const uint64_t epoch_ms = (uint64_t) epoch_s * 1000ULL;

        if (epoch_ms < p_rs->oldest_element_ms) { continue; }

        float t = 0, h = 0, p = 0;
        vlmc_sample_to_units (&s, &t, &h, &p);

        if (s.valid[VLMC_FIELD_TEMPERATURE]) { rd_sensor_data_set (sample, RD_SENSOR_TEMP_FIELD, t); }

        if (s.valid[VLMC_FIELD_HUMIDITY]) { rd_sensor_data_set (sample, RD_SENSOR_HUMI_FIELD, h); }

        if (s.valid[VLMC_FIELD_PRESSURE]) { rd_sensor_data_set (sample, RD_SENSOR_PRES_FIELD, p); }

        sample->timestamp_ms = epoch_ms;
        return RD_SUCCESS;
    }
}

rd_status_t app_log_config_set (const app_log_config_t * const configuration)
{
    (void) configuration;
    return RD_ERROR_NOT_SUPPORTED;
}

rd_status_t app_log_config_get (app_log_config_t * const configuration)
{
    if (NULL == configuration) { return RD_ERROR_NULL; }

    configuration->interval_s = APP_LOG_INTERVAL_S;
    configuration->overflow = true;
    configuration->fields.datas.temperature_c = 1;
    configuration->fields.datas.humidity_rh = 1;
    configuration->fields.datas.pressure_pa = 1;
    return RD_SUCCESS;
}

void app_log_purge_flash (void)
{
    m_block_open = false;
    m_pending_len = 0;
    m_pending_samples = 0;
    m_grid_started = false;
    m_next_seq = 1;
    m_session_first_seq = 1;
    ri_flash_purge();
}

#endif // APP_VLONGMEM_ENABLED
