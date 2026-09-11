#ifndef APPLICATION_MODE_VLONGMEM_H
#define APPLICATION_MODE_VLONGMEM_H

/**
 * @file application_mode_vlongmem.h
 * @brief Environmental logger for one year. Refer to docs/vlongmem/README.md.
 *
 * The firmware records temperature, humidity and pressure each 10 minutes
 * into a raw flash ring buffer (app_log_vlongmem.c). It sends the history
 * through the standard GATT log-read protocol.
 */

#define APP_FW_VARIANT "+vlongmem"

#define APP_VLONGMEM_ENABLED (1U)

/** @brief Log interval in seconds. The samples are on a fixed grid of this period. */
#define APP_LOG_INTERVAL_S (10U * 60U)

/**
 * @brief FDS keeps only the settings and the boot counter: 2 virtual pages
 * and one page for garbage collection, 12 kB at the top of the storage section.
 */
#define APP_FLASH_PAGES (2U)

/** @brief Raw log ring: 46 pages below FDS. Must agree with ruuvitag_b_vlongmem.ld. */
#define APP_VLONGMEM_REGION_START (0x44000U)
#define APP_VLONGMEM_REGION_END   (0x72000U)

/** @brief Number of samples in RAM before a flash write. */
#define APP_VLONGMEM_FLUSH_SAMPLES (16U)

/**
 * @brief Maximum number of missing markers for a gap. For a longer gap, a
 * new block is started. 144 slots = 1 day at 10 min.
 */
#define APP_VLONGMEM_MAX_MISSING (144U)

#endif
