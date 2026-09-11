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

#define APP_VLONGMEM_ENABLED (1U)

/**
 * @brief Test build: at the first boot with an empty ring, the firmware
 * writes one year of synthetic data (app_log_vlongmem_fake.c) into the
 * ring. The uptime gets a bias of one year, thus the live samples continue
 * after the synthetic year. Variant "vlongmemfake" sets this.
 */
#ifndef APP_VLONGMEM_FAKE_DATA
#   define APP_VLONGMEM_FAKE_DATA (0U)
#endif

#if APP_VLONGMEM_FAKE_DATA
#   define APP_FW_VARIANT "+vlongmemfake"
#   define APP_VLONGMEM_FAKE_SAMPLES (365U * 144U)
#else
#   define APP_FW_VARIANT "+vlongmem"
#endif

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
