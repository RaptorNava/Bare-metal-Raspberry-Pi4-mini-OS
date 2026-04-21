/**
 * gpio.c — Драйвер GPIO (General Purpose Input/Output)
 *
 * GPIO позволяет управлять физическими пинами Raspberry Pi.
 * RPi4 имеет 58 GPIO пинов (0-57), доступных через регистры.
 *
 * Базовый адрес GPIO: 0xFE200000
 *
 * Регистры GPIO (важнейшие):
 *   GPFSEL0-5  (0x00-0x14): Function Select — режим каждого пина
 *   GPSET0-1   (0x1C-0x20): Set Output — устанавливаем HIGH
 *   GPCLR0-1   (0x28-0x2C): Clear Output — устанавливаем LOW
 *   GPLEV0-1   (0x34-0x38): Level — читаем состояние пина
 *   GPPUPPDN0-3(0xE4-0xF0): Pull-up/Pull-down (только RPi4)
 *
 * Каждый пин имеет 3-битный код функции (GPFSEL):
 *   000 = Input, 001 = Output, 100 = ALT0, 101 = ALT1, ...
 *
 * ACT LED на RPi4 — это GPIO 42 (через расширитель, не прямой)
 * Для QEMU используем GPIO 16 как условный LED
 *
 * На борту RPi4 ACT LED управляется через почтовый ящик (mailbox),
 * но для простоты мы имитируем его через UART-сообщения в QEMU.
 */
#include <stddef.h>
#include "../../include/gpio.h"
#include "../../include/mmio.h"

// ---------------------------------------------------------------
// Базовые адреса
// ---------------------------------------------------------------
#define PERIPHERAL_BASE     0xFE000000UL
#define GPIO_BASE           (PERIPHERAL_BASE + 0x200000)

// Регистры функций выбора (по 10 пинов на регистр, 3 бита на пин)
#define GPFSEL0     (GPIO_BASE + 0x00)  // GPIO 0-9
#define GPFSEL1     (GPIO_BASE + 0x04)  // GPIO 10-19
#define GPFSEL2     (GPIO_BASE + 0x08)  // GPIO 20-29
#define GPFSEL3     (GPIO_BASE + 0x0C)  // GPIO 30-39
#define GPFSEL4     (GPIO_BASE + 0x10)  // GPIO 40-49
#define GPFSEL5     (GPIO_BASE + 0x14)  // GPIO 50-57

// Регистры установки и сброса выходов
#define GPSET0      (GPIO_BASE + 0x1C)  // GPIO 0-31: установить HIGH
#define GPSET1      (GPIO_BASE + 0x20)  // GPIO 32-53
#define GPCLR0      (GPIO_BASE + 0x28)  // GPIO 0-31: установить LOW
#define GPCLR1      (GPIO_BASE + 0x2C)  // GPIO 32-53

// Регистры чтения уровня
#define GPLEV0      (GPIO_BASE + 0x34)  // GPIO 0-31
#define GPLEV1      (GPIO_BASE + 0x38)  // GPIO 32-53

// Регистры pull-up/pull-down (RPi4 новый формат)
#define GPPUPPDN0   (GPIO_BASE + 0xE4)  // GPIO 0-15
#define GPPUPPDN1   (GPIO_BASE + 0xE8)  // GPIO 16-31
#define GPPUPPDN2   (GPIO_BASE + 0xEC)  // GPIO 32-47
#define GPPUPPDN3   (GPIO_BASE + 0xF0)  // GPIO 48-57

// Коды функций GPIO
#define GPIO_FUNC_INPUT     0   // 000
#define GPIO_FUNC_OUTPUT    1   // 001
#define GPIO_FUNC_ALT0      4   // 100
#define GPIO_FUNC_ALT1      5   // 101
#define GPIO_FUNC_ALT2      6   // 110
#define GPIO_FUNC_ALT3      7   // 111
#define GPIO_FUNC_ALT4      3   // 011
#define GPIO_FUNC_ALT5      2   // 010

// Коды pull-up/pull-down (RPi4 формат)
#define GPIO_PULL_NONE      0   // 00 = No pull
#define GPIO_PULL_UP        1   // 01 = Pull up
#define GPIO_PULL_DOWN      2   // 10 = Pull down

// ---------------------------------------------------------------
// Получить адрес регистра GPFSEL для нужного пина
// ---------------------------------------------------------------
static uint32_t gpio_get_fsel_addr(uint32_t pin) {
    // Каждый GPFSEL регистр контролирует 10 пинов
    uint32_t reg_index = pin / 10;
    return GPFSEL0 + (reg_index * 4);
}

// ---------------------------------------------------------------
// Функция: gpio_set_function(uint32_t pin, uint32_t func)
// Устанавливает функцию для GPIO пина
// pin:  номер GPIO (0-57)
// func: GPIO_FUNC_INPUT, GPIO_FUNC_OUTPUT, GPIO_FUNC_ALT0...
// ---------------------------------------------------------------
void gpio_set_function(uint32_t pin, uint32_t func) {
    if (pin > 57) return;   // Защита от некорректных значений
    
    uint32_t reg_addr = gpio_get_fsel_addr(pin);
    uint32_t shift = (pin % 10) * 3;   // 3 бита на пин, смещение внутри регистра
    
    uint32_t val = mmio_read(reg_addr);
    val &= ~(7 << shift);               // Очищаем 3 бита функции
    val |=  (func & 7) << shift;        // Устанавливаем новую функцию
    mmio_write(reg_addr, val);
}

// ---------------------------------------------------------------
// Функция: gpio_set_pull(uint32_t pin, uint32_t pull)
// Устанавливает pull-up/pull-down резистор для RPi4
// На RPi4 используется новый механизм (отличается от RPi3!)
// ---------------------------------------------------------------
void gpio_set_pull(uint32_t pin, uint32_t pull) {
    if (pin > 57) return;
    
    // На RPi4: 2 бита на пин, 16 пинов на регистр
    uint32_t reg_index = pin / 16;
    uint32_t shift = (pin % 16) * 2;
    
    uint32_t reg_addr = GPPUPPDN0 + (reg_index * 4);
    uint32_t val = mmio_read(reg_addr);
    val &= ~(3 << shift);           // Очищаем 2 бита
    val |=  (pull & 3) << shift;   // Устанавливаем режим
    mmio_write(reg_addr, val);
}

// ---------------------------------------------------------------
// Функция: gpio_set_high(uint32_t pin)
// Устанавливает выходной пин в HIGH (логическая 1)
// ---------------------------------------------------------------
void gpio_set_high(uint32_t pin) {
    if (pin > 57) return;
    
    if (pin < 32) {
        // GPIO 0-31: используем GPSET0
        mmio_write(GPSET0, 1 << pin);
    } else {
        // GPIO 32-57: используем GPSET1
        mmio_write(GPSET1, 1 << (pin - 32));
    }
}

// ---------------------------------------------------------------
// Функция: gpio_set_low(uint32_t pin)
// Устанавливает выходной пин в LOW (логический 0)
// ---------------------------------------------------------------
void gpio_set_low(uint32_t pin) {
    if (pin > 57) return;
    
    if (pin < 32) {
        mmio_write(GPCLR0, 1 << pin);
    } else {
        mmio_write(GPCLR1, 1 << (pin - 32));
    }
}

// ---------------------------------------------------------------
// Функция: gpio_read(uint32_t pin)
// Читает текущее состояние пина
// Возвращает: 0 (LOW) или 1 (HIGH)
// ---------------------------------------------------------------
uint32_t gpio_read(uint32_t pin) {
    if (pin > 57) return 0;
    
    if (pin < 32) {
        return (mmio_read(GPLEV0) >> pin) & 1;
    } else {
        return (mmio_read(GPLEV1) >> (pin - 32)) & 1;
    }
}

// ---------------------------------------------------------------
// Функция: gpio_init()
// Базовая инициализация GPIO подсистемы
// ---------------------------------------------------------------
void gpio_init(void) {
    // Для QEMU большинство настроек не нужны,
    // но на реальном железе здесь можно сбросить
    // важные пины в безопасное состояние
    
    // GPIO 4 используем как статусный индикатор (DEBUG LED)
    // На физическом RPi4 к нему можно подключить LED
    gpio_set_function(4, GPIO_FUNC_OUTPUT);
    gpio_set_low(4);    // Начальное состояние — выключен
}

// ---------------------------------------------------------------
// Функция: gpio_led_on() / gpio_led_off()
// Управление отладочным LED (GPIO 4)
// ---------------------------------------------------------------
void gpio_led_on(void) {
    gpio_set_high(4);
}

void gpio_led_off(void) {
    gpio_set_low(4);
}

// ---------------------------------------------------------------
// Функция: gpio_led_blink(uint32_t count)
// Мигание LED указанное количество раз
// Используется для сигнализации ошибок (panic)
// ---------------------------------------------------------------
void gpio_led_blink(uint32_t count) {
    volatile uint32_t i, j;
    
    for (i = 0; i < count; i++) {
        gpio_set_high(4);
        // Задержка ~0.5 секунды (приблизительно)
        for (j = 0; j < 500000; j++) {
            asm volatile("nop");
        }
        gpio_set_low(4);
        // Задержка ~0.5 секунды
        for (j = 0; j < 500000; j++) {
            asm volatile("nop");
        }
    }
}

// ---------------------------------------------------------------
// Функция: gpio_configure_uart_pins()
// Настройка GPIO 14 и 15 для UART0
// Вызывается из uart.c через gpio.h интерфейс
// ---------------------------------------------------------------
void gpio_configure_uart_pins(void) {
    // GPIO 14 = TXD0, GPIO 15 = RXD0 — ALT0 функция
    gpio_set_function(14, GPIO_FUNC_ALT0);
    gpio_set_function(15, GPIO_FUNC_ALT0);
    
    // Отключаем pull-up/down для UART пинов
    gpio_set_pull(14, GPIO_PULL_NONE);
    gpio_set_pull(15, GPIO_PULL_NONE);
}

// ---------------------------------------------------------------
// Функция: gpio_get_info()
// Диагностическая функция — возвращает состояние GPFSEL регистров
// Полезна для отладки при проверке конфигурации пинов
// ---------------------------------------------------------------
uint32_t gpio_get_fsel_register(uint32_t reg_num) {
    if (reg_num > 5) return 0;
    return mmio_read(GPFSEL0 + (reg_num * 4));
}
