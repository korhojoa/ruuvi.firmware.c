/* This application requests DFU again. It does not write flash. */
#include <stdint.h>

__attribute__((noreturn)) void bridge_reset(void)
{
    __asm volatile ("cpsid i" ::: "memory");
    *(volatile uint32_t *)0x4000051c = 0xb1;
    __asm volatile ("dsb" ::: "memory");
    *(volatile uint32_t *)0xe000ed0c = 0x05fa0004;
    __asm volatile ("dsb" ::: "memory");
    for (;;) {}
}

__attribute__((used, section(".vectors")))
const uintptr_t bridge_vectors[] =
{
    0x20010000, (uintptr_t)bridge_reset,
    (uintptr_t)bridge_reset, (uintptr_t)bridge_reset,
    (uintptr_t)bridge_reset, (uintptr_t)bridge_reset,
    (uintptr_t)bridge_reset, 0, 0, 0, 0,
    (uintptr_t)bridge_reset, (uintptr_t)bridge_reset, 0,
    (uintptr_t)bridge_reset, (uintptr_t)bridge_reset
};
