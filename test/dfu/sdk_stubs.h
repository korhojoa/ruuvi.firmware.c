#ifndef SDK_STUBS_H
#define SDK_STUBS_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define BLE_STACK_SUPPORT_REQD 1
#define CODE_PAGE_SIZE 4096u
#define SD_PRESENT 1
#define SD_SIZE_GET(x) 0x26000u
#define MBR_SIZE 4096u
#define ALIGN_TO_PAGE(x) (((x) + 4095u) & ~4095u)
#define ALIGN_NUM(n, x) (((x) + (n) - 1u) & ~((n) - 1u))
#define CEIL_DIV(x, n) (((x) + (n) - 1u) / (n))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define BOOTLOADER_START_ADDR 0x78000u
#define DFU_REGION_END(x) ((x) - 20480u)
#define NRF_BL_FW_COPY_PROGRESS_STORE_STEP 8u
#define NRF_DFU_BANK_INVALID 0u
#define NRF_DFU_BANK_VALID_APP 1u
#define NRF_DFU_BANK_VALID_SD 2u
#define NRF_DFU_BANK_VALID_BL 3u
#define NRF_DFU_BANK_VALID_SD_BL 4u
#define NRF_SUCCESS 0u
#define NRF_ERROR_NO_MEM 4u
#define NRF_ERROR_INTERNAL 3u
#define NRF_ERROR_INVALID_DATA 11u
#define SD_MAGIC_NUMBER 0x51b1e5dbu
#define SD_MAGIC_NUMBER_GET(x) 0u
#define ASSERT(x) assert(x)
#define UNUSED_PARAMETER(x) (void)(x)
#define NRF_LOG_DEBUG(...) ((void)0)
#define NRF_LOG_INFO(...) ((void)0)
#define NRF_LOG_WARNING(...) ((void)0)
#define NRF_LOG_ERROR(...)
#define NRF_LOG_FLUSH() ((void)0)
typedef uint32_t ret_code_t;
typedef struct { uint32_t image_size, image_crc, bank_code; } nrf_dfu_bank_t;
typedef struct
{
    nrf_dfu_bank_t bank_0, bank_1;
    uint32_t write_offset, sd_size;
    struct { uint32_t update_start_address; } progress;
} settings_t;
extern settings_t s_dfu_settings;
typedef enum
{
    ACTIVATION_NONE, ACTIVATION_SUCCESS, ACTIVATION_ERROR,
    ACTIVATION_SUCCESS_EXPECT_ADDITIONAL_UPDATE
} nrf_bootloader_fw_activation_result_t;
typedef void (*flash_callback_t)(void *);
uint32_t nrf_dfu_flash_erase(uint32_t, uint32_t, flash_callback_t);
uint32_t nrf_dfu_flash_store(uint32_t, const void *, uint32_t, flash_callback_t);
uint32_t nrf_dfu_settings_write_and_backup(flash_callback_t);
uint32_t nrf_dfu_mbr_copy_bl(uint32_t *, uint32_t);
uint32_t crc32_compute(const uint8_t *, uint32_t, const uint32_t *);
void nrf_bootloader_wdt_init(void);
void nrf_bootloader_wdt_feed(void);
uint32_t nrf_dfu_bank0_start_addr(void);
uint32_t nrf_dfu_bank1_start_addr(void);
uint32_t nrf_dfu_softdevice_start_address(void);
void nrf_dfu_bank_invalidate(nrf_dfu_bank_t *);
uint32_t nrf_dfu_cache_prepare(uint32_t, bool, bool, bool);
nrf_bootloader_fw_activation_result_t nrf_bootloader_fw_activate(void);
#endif
