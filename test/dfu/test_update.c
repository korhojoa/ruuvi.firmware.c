#include "sdk_stubs.h"
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#define APP 0x26000u
#define LOG 0x44000u
#define LOG_END 0x72000u
settings_t s_dfu_settings;
static settings_t saved;
static uint8_t history[LOG_END - LOG];
static jmp_buf restart;
static unsigned writes, fail_at;

static void checkpoint(void)
{
    if (++writes == fail_at) longjmp(restart, 1);
}

uint32_t nrf_dfu_flash_erase(uint32_t addr, uint32_t pages, flash_callback_t cb)
{
    assert(addr >= APP && addr + pages * 4096 <= LOG);
    assert(addr % 4096 == 0);
    memset((void *)(uintptr_t)addr, 255, pages * 4096);
    checkpoint();
    if (cb) cb(NULL);
    return NRF_SUCCESS;
}

uint32_t nrf_dfu_flash_store(uint32_t addr, const void *data, uint32_t len, flash_callback_t cb)
{
    uint8_t *dst = (void *)(uintptr_t)addr;
    const uint8_t *src = data;
    assert(addr >= APP && addr + len <= LOG);
    for (uint32_t i = 0; i < len; ++i)
    {
        assert((dst[i] & src[i]) == src[i]);
        dst[i] &= src[i];
    }
    checkpoint();
    if (cb) cb(NULL);
    return NRF_SUCCESS;
}

uint32_t nrf_dfu_settings_write_and_backup(flash_callback_t cb)
{
    saved = s_dfu_settings;
    checkpoint();
    if (cb) cb(NULL);
    return NRF_SUCCESS;
}

uint32_t crc32_compute(const uint8_t *data, uint32_t len, const uint32_t *initial)
{
    uint32_t crc = initial ? ~*initial : 0xffffffffu;
    for (uint32_t i = 0; i < len; ++i)
    {
        crc ^= data[i];
        for (unsigned j = 0; j < 8; ++j)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void nrf_bootloader_wdt_init(void) {}
void nrf_bootloader_wdt_feed(void) {}
uint32_t nrf_dfu_mbr_copy_bl(uint32_t *src, uint32_t len)
{
    (void)src; (void)len;
    abort();
}

static unsigned update(uint32_t old_size, const uint8_t *image, uint32_t size,
                       unsigned fault)
{
    memset((void *)(uintptr_t)APP, 0xff, LOG - APP);
    memset(&s_dfu_settings, 0, sizeof(s_dfu_settings));
    s_dfu_settings.bank_0.image_size = old_size;
    s_dfu_settings.bank_0.bank_code = NRF_DFU_BANK_VALID_APP;
    assert(nrf_dfu_cache_prepare(size, false, false, true) == NRF_SUCCESS);
    uint32_t staging = nrf_dfu_bank1_start_addr();
    assert(staging + ALIGN_TO_PAGE(size) <= LOG);
    memcpy((void *)(uintptr_t)staging, image, size);
    s_dfu_settings.bank_1 = (nrf_dfu_bank_t){size, crc32_compute(image, size, NULL), NRF_DFU_BANK_VALID_APP};
    s_dfu_settings.progress.update_start_address = staging;
    saved = s_dfu_settings;
    writes = 0;
    fail_at = fault;
    if (setjmp(restart)) s_dfu_settings = saved;
    nrf_bootloader_fw_activation_result_t result = nrf_bootloader_fw_activate();
    assert(result == ACTIVATION_SUCCESS || result == ACTIVATION_NONE);
    assert(s_dfu_settings.bank_0.image_size == size);
    assert(s_dfu_settings.bank_0.image_crc == crc32_compute(image, size, NULL));
    assert(memcmp((void *)(uintptr_t)APP, image, size) == 0);
    assert(memcmp((void *)(uintptr_t)LOG, history, sizeof(history)) == 0);
    return writes;
}

static uint8_t *read_image(const char *path, uint32_t *size)
{
    FILE *file = fopen(path, "rb");
    assert(file);
    assert(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length > 0 && length < LOG - APP);
    *size = (uint32_t)length;
    rewind(file);
    uint8_t *data = malloc(*size);
    assert(data && fread(data, 1, *size, file) == *size);
    fclose(file);
    return data;
}

int main(int argc, char **argv)
{
    assert(argc == 4);
    assert(mmap((void *)0x10000, 0x70000, PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) != MAP_FAILED);
    for (unsigned i = 0; i < sizeof(history); ++i) history[i] = (i * 37 + i / 97) & 255;
    memcpy((void *)LOG, history, sizeof(history));
    uint32_t old_size = (uint32_t)strtoul(argv[1], NULL, 0), bridge_size, final_size;
    uint8_t *bridge = read_image(argv[2], &bridge_size);
    uint8_t *final = read_image(argv[3], &final_size);
    assert(bridge_size <= 4096);
    assert(bridge_size % 4096 == 0 && final_size % 4096 == 0);
    assert(APP + ALIGN_TO_PAGE(old_size) + ALIGN_TO_PAGE(final_size) > LOG);
    printf("Direct update overlaps history; two-stage ranges: 0x%05x..0x%05x, 0x%05x..0x%05x\n",
           APP + ALIGN_TO_PAGE(old_size), APP + ALIGN_TO_PAGE(old_size) + bridge_size,
           APP + bridge_size, APP + bridge_size + final_size);
    unsigned first = update(old_size, bridge, bridge_size, 0);
    unsigned second = update(bridge_size, final, final_size, 0);
    for (unsigned fault = 1; fault <= first; ++fault) update(old_size, bridge, bridge_size, fault);
    for (unsigned fault = 1; fault <= second; ++fault) update(bridge_size, final, final_size, fault);
    printf("SDK activation: two updates, %u interruption points, history identical\n", first + second);
    free(bridge);
    free(final);
    return 0;
}
