#ifndef NRF_FSTORAGE_H
#define NRF_FSTORAGE_H
/**
 * Host simulation shim of the Nordic SDK 15 nrf_fstorage API. Only the
 * part that app_log_vlongmem.c uses. The functions are in sim_vlongmem.c
 * and operate on a fake flash in RAM.
 */
#include <stdbool.h>
#include <stdint.h>

typedef uint32_t ret_code_t;
#define NRF_SUCCESS      (0U)
#define NRF_ERROR_NO_MEM (4U)

typedef enum
{
    NRF_FSTORAGE_EVT_READ_RESULT,
    NRF_FSTORAGE_EVT_WRITE_RESULT,
    NRF_FSTORAGE_EVT_ERASE_RESULT
} nrf_fstorage_evt_id_t;

typedef struct
{
    nrf_fstorage_evt_id_t id;
    ret_code_t result;
    uint32_t addr;
    void const * p_src;
    uint32_t len;
    void * p_param;
} nrf_fstorage_evt_t;

typedef void (*nrf_fstorage_evt_handler_t) (nrf_fstorage_evt_t * p_evt);

typedef struct
{
    int unused;
} nrf_fstorage_api_t;

typedef struct
{
    nrf_fstorage_evt_handler_t evt_handler;
    uint32_t start_addr;
    uint32_t end_addr;
} nrf_fstorage_t;

#define NRF_FSTORAGE_DEF(inst) inst

ret_code_t nrf_fstorage_init (nrf_fstorage_t * p_fs, nrf_fstorage_api_t * p_api, void * p_param);
ret_code_t nrf_fstorage_write (nrf_fstorage_t const * p_fs, uint32_t dest, void const * p_src,
                               uint32_t len, void * p_param);
ret_code_t nrf_fstorage_erase (nrf_fstorage_t const * p_fs, uint32_t page_addr, uint32_t len,
                               void * p_param);
bool nrf_fstorage_is_busy (nrf_fstorage_t const * p_fs);

#endif
