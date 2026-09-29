#include <stdint.h>
#include "power.h"

#define MMIO_BASE       0xFE000000UL
#define PM_BASE         (MMIO_BASE + 0x100000UL)
#define PM_RSTC         (*(volatile uint32_t *)(PM_BASE + 0x1C))
#define PM_WDOG         (*(volatile uint32_t *)(PM_BASE + 0x24))
#define PM_PASSWORD     0x5A000000u
#define PM_RSTC_FULL_RESET  0x20u

void system_reset(void) {
    PM_WDOG = PM_PASSWORD | 10;                     /* time out almost at once */
    PM_RSTC = PM_PASSWORD | PM_RSTC_FULL_RESET;     /* start the watchdog: full reset */
    __asm__ volatile("dsb sy" ::: "memory");
    for (;;)
        __asm__ volatile("wfe");
}
