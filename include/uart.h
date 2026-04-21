#ifndef UART_H
#define UART_H

/**
 * uart.h — UART driver interface
 */

#include <stdint.h>

void  uart_init(void);
void  uart_putc(char c);
char  uart_getc(void);
char  uart_getc_nonblock(void);
void  uart_puts(const char *str);
void  uart_puthex(uint64_t value);
void  uart_putdec(uint32_t value);
int   uart_readline(char *buf, int max_len);

#endif /* UART_H */
