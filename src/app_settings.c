/** @brief Persistent intervals for the vlongmem variant. */
#include "app_settings.h"

#if APP_VLONGMEM_ENABLED

#include "app_log.h"
#include "ruuvi_endpoints.h"
#include "ruuvi_interface_rtc.h"
#include "ruuvi_interface_yield.h"
#include "ruuvi_task_flash.h"
#include <string.h>

#define SETTINGS_MAGIC (0x314D4C56U) //!< VLM1, little endian.
#define SETTINGS_WAIT_MS (4000ULL)

typedef struct
{
    uint32_t magic;
    app_settings_t intervals;
} settings_record_t;

_Static_assert (sizeof (settings_record_t) == 12U, "settings record size");

static app_settings_t m_settings = {APP_BLE_INTERVAL_MS, APP_LOG_INTERVAL_S};
// FDS requires an aligned source that stays valid until the write completes.
static settings_record_t m_record;

static bool valid (const app_settings_t * const s)
{
    return (s->announce_ms >= 100U) && (s->announce_ms <= 10000U)
           && (s->storage_s >= 1U) && (s->storage_s <= UINT16_MAX)
           && (s->storage_s * 1000U >= s->announce_ms * APP_NUM_REPEATS);
}

static rd_status_t wait_flash (void)
{
    const uint64_t start = ri_rtc_millis();

    while (rt_flash_busy())
    {
        if (ri_rtc_millis() - start >= SETTINGS_WAIT_MS) { return RD_ERROR_TIMEOUT; }

        ri_yield();
    }

    return RD_SUCCESS;
}

rd_status_t app_settings_init (void)
{
    m_settings = (app_settings_t) {APP_BLE_INTERVAL_MS, APP_LOG_INTERVAL_S};
    rd_status_t status = wait_flash();

    if (RD_SUCCESS != status) { return status; }

    settings_record_t saved = {0};
    status = rt_flash_load (APP_FLASH_LOG_FILE, APP_FLASH_LOG_SETTINGS_RECORD,
                            &saved, sizeof (saved));

    if (RD_ERROR_NOT_FOUND == status || RD_ERROR_DATA_SIZE == status) { return RD_SUCCESS; }

    if (RD_SUCCESS != status) { return status; }

    if (SETTINGS_MAGIC == saved.magic && valid (&saved.intervals))
    {
        m_settings = saved.intervals;
    }

    return RD_SUCCESS;
}

app_settings_t app_settings_get (void)
{
    return m_settings;
}

rd_status_t app_settings_set (const app_settings_t * const settings)
{
    if (NULL == settings) { return RD_ERROR_NULL; }

    if (!valid (settings)) { return RD_ERROR_INVALID_PARAM; }

    if (settings->announce_ms == m_settings.announce_ms
            && settings->storage_s == m_settings.storage_s) { return RD_SUCCESS; }

    rd_status_t status = wait_flash();

    if (RD_SUCCESS != status) { return status; }

    // Keep all pending samples before the record can commit a different interval.
    if (settings->storage_s != m_settings.storage_s)
    {
        status = app_log_flush();

        if (RD_SUCCESS != status) { return status; }
    }

    m_record.magic = SETTINGS_MAGIC;
    m_record.intervals = *settings;
    status = rt_flash_store (APP_FLASH_LOG_FILE, APP_FLASH_LOG_SETTINGS_RECORD,
                             &m_record, sizeof (m_record));

    if (RD_SUCCESS != status) { return status; }

    status = wait_flash();

    if (RD_SUCCESS != status) { return status; }

    settings_record_t saved = {0};
    status = rt_flash_load (APP_FLASH_LOG_FILE, APP_FLASH_LOG_SETTINGS_RECORD,
                            &saved, sizeof (saved));

    if (RD_SUCCESS != status) { return status; }

    if (0 != memcmp (&saved, &m_record, sizeof (saved))) { return RD_ERROR_INTERNAL; }

    app_log_config_t config = {0};
    config.interval_s = (uint16_t) settings->storage_s;
    config.overflow = true;
    config.fields.datas.temperature_c = 1;
    config.fields.datas.humidity_rh = 1;
    config.fields.datas.pressure_pa = 1;
    status = app_log_config_set (&config);

    if (RD_SUCCESS == status) { m_settings = *settings; }

    return status;
}

static uint32_t read_u32 (const uint8_t * p)
{
    return ( (uint32_t) p[0] << 24U) | ( (uint32_t) p[1] << 16U)
           | ( (uint32_t) p[2] << 8U) | p[3];
}

static void write_u32 (uint8_t * p, uint32_t value)
{
    for (size_t i = 4U; i > 0U; i--)
    {
        p[i - 1U] = (uint8_t) value;
        value >>= 8U;
    }
}

rd_status_t app_settings_handle (const ri_comm_xfer_fp_t reply_fp,
                                 const uint8_t * const message, const size_t length)
{
    if (NULL == reply_fp || NULL == message) { return RD_ERROR_NULL; }

    if (RE_STANDARD_MESSAGE_LENGTH != length) { return RD_ERROR_DATA_SIZE; }

    rd_status_t status = RD_SUCCESS;
    const uint8_t op = message[RE_STANDARD_OPERATION_INDEX];

    if (RE_STANDARD_LOG_CONFIGURATION_WRITE == op)
    {
        const app_settings_t settings = {read_u32 (&message[3]), read_u32 (&message[7]) };
        status = app_settings_set (&settings);
    }
    else if (RE_STANDARD_LOG_CONFIGURATION_READ != op)
    {
        status = RD_ERROR_INVALID_PARAM;
    }

    ri_comm_message_t reply = {0};
    reply.data_length = RE_STANDARD_MESSAGE_LENGTH;
    reply.repeat_count = 1;
    reply.data[0] = message[1];
    reply.data[1] = RE_STANDARD_DESTINATION_SYS_CONFIG;
    reply.data[2] = (RD_SUCCESS == status) ? RE_STANDARD_LOG_CONFIGURATION_WRITE
                    : RE_STANDARD_OP_ERROR;
    write_u32 (&reply.data[3], m_settings.announce_ms);
    write_u32 (&reply.data[7], m_settings.storage_s);
    return status | reply_fp (&reply);
}

#endif
