#ifndef APP_SETTINGS_H
#define APP_SETTINGS_H

#include "app_config.h"
#include "ruuvi_driver_error.h"
#include "ruuvi_interface_communication.h"

#if APP_VLONGMEM_ENABLED

typedef struct
{
    uint32_t announce_ms; //!< Advertisement interval in milliseconds.
    uint32_t storage_s;   //!< Storage interval in seconds.
} app_settings_t;

/** @brief Load the saved intervals, or use the defaults. */
rd_status_t app_settings_init (void);
/** @brief Get the effective intervals. */
app_settings_t app_settings_get (void);
/** @brief Validate, save and apply both intervals. Call with the heartbeat stopped. */
rd_status_t app_settings_set (const app_settings_t * const settings);
/** @brief Handle an authorized system configuration request. */
rd_status_t app_settings_handle (const ri_comm_xfer_fp_t reply_fp,
                                 const uint8_t * const message, const size_t length);

#endif
#endif
