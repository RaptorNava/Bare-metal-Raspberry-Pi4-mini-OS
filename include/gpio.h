#ifndef GPIO_H
#define GPIO_H

/**
 * gpio.h — GPIO driver interface
 */

#include <stdint.h>

/* Коды функций GPIO */
#define GPIO_FUNC_INPUT     0
#define GPIO_FUNC_OUTPUT    1
#define GPIO_FUNC_ALT0      4
#define GPIO_FUNC_ALT1      5
#define GPIO_FUNC_ALT2      6
#define GPIO_FUNC_ALT3      7
#define GPIO_FUNC_ALT4      3
#define GPIO_FUNC_ALT5      2

/* Режимы pull-up/pull-down */
#define GPIO_PULL_NONE      0
#define GPIO_PULL_UP        1
#define GPIO_PULL_DOWN      2

void     gpio_init(void);
void     gpio_set_function(uint32_t pin, uint32_t func);
void     gpio_set_pull(uint32_t pin, uint32_t pull);
void     gpio_set_high(uint32_t pin);
void     gpio_set_low(uint32_t pin);
uint32_t gpio_read(uint32_t pin);

void     gpio_led_on(void);
void     gpio_led_off(void);
void     gpio_led_blink(uint32_t count);

void     gpio_configure_uart_pins(void);
uint32_t gpio_get_fsel_register(uint32_t reg_num);

#endif /* GPIO_H */
