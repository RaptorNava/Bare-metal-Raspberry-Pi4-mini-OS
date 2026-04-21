#ifndef TIMER_H
#define TIMER_H

/**
 * timer.h — ARM Generic Timer interface
 *
 * TIMER_INTERVAL_US: интервал таймерного прерывания в микросекундах.
 * Используется в kernel.c (print_sysinfo) и shell.c (cmd_timer).
 */

#include <stdint.h>

#define TIMER_INTERVAL_US   10000   /* 10 мс = 100 Hz */

void     timer_init(void);
void     timer_handle_irq(void);
void     timer_delay_ms(uint32_t ms);
void     timer_watchdog_kick(void);
int      timer_add_task(uint32_t id, const char *name);
uint32_t timer_get_ticks(void);
uint64_t timer_get_freq(void);
void     timer_print_stats(void);

#endif /* TIMER_H */
