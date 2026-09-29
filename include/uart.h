#ifndef UART_H
#define UART_H

#include <stdint.h>

// UART0 (PL011): bootloader upload and the GDB link
void uart_init(void);
void uart_putc(char c);
char uart_getc(void);
int uart_rx_ready(void);   /* 1 if a received byte is waiting (does not consume it) */
void uart_puts(const char *s);
void uart_hex8(unsigned v);
void uart_hex32(unsigned v);

// UART2 (PL011; the mini UART in the QEMU build): the kernel's console
void uart2_init(void);
void uart2_putc(char c);
void uart2_puts(const char *s);
char uart2_getc(void);
int uart2_rx_ready(void);
void uart2_hex32(uint32_t v);

#endif
