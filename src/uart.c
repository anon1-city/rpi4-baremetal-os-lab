#include <stdint.h>
#include "uart.h"

#define MMIO_BASE       0xFE000000UL
#define GPIO_BASE       (MMIO_BASE + 0x200000UL)
#define UART0_BASE      (MMIO_BASE + 0x201000UL)
#define UART2_BASE      (MMIO_BASE + 0x201400UL)

/* GPIO */
#define GPFSEL0         (*(volatile uint32_t *)(GPIO_BASE + 0x00))
#define GPFSEL1         (*(volatile uint32_t *)(GPIO_BASE + 0x04))
#define GPPUPPDN0       (*(volatile uint32_t *)(GPIO_BASE + 0xE4))

/* UART0 PL011 */
#define U0_DR           (*(volatile uint32_t *)(UART0_BASE + 0x00))
#define U0_FR           (*(volatile uint32_t *)(UART0_BASE + 0x18))
#define U0_IBRD         (*(volatile uint32_t *)(UART0_BASE + 0x24))
#define U0_FBRD         (*(volatile uint32_t *)(UART0_BASE + 0x28))
#define U0_LCRH         (*(volatile uint32_t *)(UART0_BASE + 0x2C))
#define U0_CR           (*(volatile uint32_t *)(UART0_BASE + 0x30))
#define U0_ICR          (*(volatile uint32_t *)(UART0_BASE + 0x44))

/* UART2 PL011 */
#define U2_DR           (*(volatile uint32_t *)(UART2_BASE + 0x00))
#define U2_FR           (*(volatile uint32_t *)(UART2_BASE + 0x18))
#define U2_IBRD         (*(volatile uint32_t *)(UART2_BASE + 0x24))
#define U2_FBRD         (*(volatile uint32_t *)(UART2_BASE + 0x28))
#define U2_LCRH         (*(volatile uint32_t *)(UART2_BASE + 0x2C))
#define U2_CR           (*(volatile uint32_t *)(UART2_BASE + 0x30))
#define U2_ICR          (*(volatile uint32_t *)(UART2_BASE + 0x44))

/* QEMU's raspi4b has no PL011 UART2, but it does have the mini UART (AUX), its
 * second serial port. A build with -DQEMU_CONSOLE puts the console (uart2_*) on
 * it, so QEMU can simulate the real board's two channels: UART0 for upload and
 * GDB, and a separate console. */
#define AUX_BASE        (MMIO_BASE + 0x215000UL)
#define AUX_ENABLES     (*(volatile uint32_t *)(AUX_BASE + 0x04))
#define AUX_MU_IO       (*(volatile uint32_t *)(AUX_BASE + 0x40))
#define AUX_MU_IER      (*(volatile uint32_t *)(AUX_BASE + 0x44))
#define AUX_MU_LCR      (*(volatile uint32_t *)(AUX_BASE + 0x4C))
#define AUX_MU_LSR      (*(volatile uint32_t *)(AUX_BASE + 0x54))
#define AUX_MU_CNTL     (*(volatile uint32_t *)(AUX_BASE + 0x60))
#define AUX_MU_BAUD     (*(volatile uint32_t *)(AUX_BASE + 0x68))

static inline void mmio_barrier(void) {
    __asm__ volatile("dsb sy" ::: "memory");
}

void uart_init(void) {
    uint32_t r;

    /* GPIO14/GPIO15 -> ALT0 for UART0 */
    r = GPFSEL1;
    r &= ~((7u << 12) | (7u << 15));
    r |=  ((4u << 12) | (4u << 15));
    GPFSEL1 = r;

    /* Disable pulls on GPIO14/GPIO15 */
    r = GPPUPPDN0;
    r &= ~((3u << 28) | (3u << 30));
    GPPUPPDN0 = r;

    mmio_barrier();

    U0_CR = 0;
    U0_ICR = 0x7FF;
    U0_IBRD = 26;                 /* 115200 @ 48 MHz */
    U0_FBRD = 3;
    U0_LCRH = (1u << 4) | (3u << 5);   /* FIFO + 8N1 */
    U0_CR = (1u << 0) | (1u << 8) | (1u << 9);

    mmio_barrier();
}

#ifndef QEMU_CONSOLE
void uart2_init(void) {
    uint32_t r;

    /* GPIO0/GPIO1 -> ALT4 for UART2 */
    r = GPFSEL0;
    r &= ~((7u << 0) | (7u << 3));
    r |=  ((3u << 0) | (3u << 3));     /* ALT4 */
    GPFSEL0 = r;

    /* Disable pulls on GPIO0/GPIO1 */
    r = GPPUPPDN0;
    r &= ~((3u << 0) | (3u << 2));
    GPPUPPDN0 = r;

    mmio_barrier();

    U2_CR = 0;
    U2_ICR = 0x7FF;
    U2_IBRD = 26;                 /* 115200 @ 48 MHz */
    U2_FBRD = 3;
    U2_LCRH = (1u << 4) | (3u << 5);   /* FIFO + 8N1 */
    U2_CR = (1u << 0) | (1u << 8) | (1u << 9);

    mmio_barrier();
}
#else
void uart2_init(void) {
    AUX_ENABLES |= 1;           /* enable the mini UART */
    AUX_MU_CNTL = 0;
    AUX_MU_IER = 0;
    AUX_MU_LCR = 3;             /* 8 bits */
    AUX_MU_BAUD = 270;          /* 115200 @ 250 MHz (ignored by QEMU) */
    AUX_MU_CNTL = 3;            /* TX and RX on */
    mmio_barrier();
}
#endif

void uart_putc(char c) {
    while (U0_FR & (1u << 5)) { }
    U0_DR = (uint32_t)(uint8_t)c;
}

int uart_rx_ready(void) {
    return !(U0_FR & (1u << 4));
}

char uart_getc(void) {
    while (U0_FR & (1u << 4)) { }
    return (char)(U0_DR & 0xFF);
}

void uart_puts(const char *s) {
    while (*s) {
        if (*s == '\n')
            uart_putc('\r');
        uart_putc(*s++);
    }
}

#ifndef QEMU_CONSOLE
void uart2_putc(char c) {
    while (U2_FR & (1u << 5)) { }
    U2_DR = (uint32_t)(uint8_t)c;
}

int uart2_rx_ready(void) {
    return !(U2_FR & (1u << 4));
}

char uart2_getc(void) {
    while (U2_FR & (1u << 4)) { }
    return (char)(U2_DR & 0xFF);
}
#else
void uart2_putc(char c) {
    while (!(AUX_MU_LSR & (1u << 5))) { }
    AUX_MU_IO = (uint32_t)(uint8_t)c;
}

int uart2_rx_ready(void) {
    return AUX_MU_LSR & 1u;
}

char uart2_getc(void) {
    while (!(AUX_MU_LSR & 1u)) { }
    return (char)(AUX_MU_IO & 0xFF);
}
#endif

void uart2_puts(const char *s) {
    while (*s) {
        if (*s == '\n')
            uart2_putc('\r');
        uart2_putc(*s++);
    }
}

void uart_hex8(unsigned v) {
    const char *h = "0123456789ABCDEF";
    uart_putc(h[(v >> 4) & 0xF]);
    uart_putc(h[v & 0xF]);
}

void uart_hex32(unsigned v) {
    for (int i = 28; i >= 0; i -= 4) {
        unsigned n = (v >> i) & 0xF;
        uart_putc((char)(n < 10 ? ('0' + n) : ('A' + n - 10)));
    }
}

void uart2_hex32(uint32_t v) {
    for (int i = 28; i >= 0; i -= 4) {
        unsigned n = (v >> i) & 0xF;
        uart2_putc(n < 10 ? ('0' + n) : ('A' + n - 10));
    }
}
