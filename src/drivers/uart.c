/**
 * uart.c — Драйвер UART (Universal Asynchronous Receiver/Transmitter)
 *
 * UART позволяет общаться с компьютером через последовательный порт.
 * В QEMU вывод UART автоматически перенаправляется в терминал.
 * На реальном RPi4 нужен USB-UART адаптер (3.3V!) на GPIO 14/15.
 *
 * Raspberry Pi 4 имеет два UART:
 *   - PL011 (UART0): Аппаратный, точный, используем его
 *   - Mini UART: Упрощённый, зависит от тактовой частоты
 *
 * Базовый адрес периферии RPi4: 0xFE000000
 * UART0 смещение: 0x201000
 * Итого: 0xFE201000
 *
 * Скорость: 115200 бод, 8N1 (8 бит данных, нет чётности, 1 стоп-бит)
 *
 * Регистры PL011:
 *   DR    (0x00): Data Register — чтение/запись байта
 *   FR    (0x18): Flag Register — статусные флаги
 *   IBRD  (0x24): Integer Baud Rate Divisor
 *   FBRD  (0x28): Fractional Baud Rate Divisor
 *   LCRH  (0x2C): Line Control Register
 *   CR    (0x30): Control Register
 *   IMSC  (0x38): Interrupt Mask Set/Clear
 *   ICR   (0x44): Interrupt Clear Register
 */

#include "../../include/uart.h"
#include "../../include/gpio.h"
#include "../../include/mmio.h"
#include <stddef.h>
// ---------------------------------------------------------------
// Базовые адреса
// ---------------------------------------------------------------
#define PERIPHERAL_BASE     0xFE000000UL    // Базовый адрес периферии RPi4
#define UART0_BASE          (PERIPHERAL_BASE + 0x201000)

// Регистры UART0 (смещения от UART0_BASE)
#define UART0_DR            (UART0_BASE + 0x00)  // Data Register
#define UART0_FR            (UART0_BASE + 0x18)  // Flag Register
#define UART0_IBRD          (UART0_BASE + 0x24)  // Integer Baud Rate
#define UART0_FBRD          (UART0_BASE + 0x28)  // Fractional Baud Rate
#define UART0_LCRH          (UART0_BASE + 0x2C)  // Line Control
#define UART0_CR            (UART0_BASE + 0x30)  // Control Register
#define UART0_IMSC          (UART0_BASE + 0x38)  // Interrupt Mask
#define UART0_ICR           (UART0_BASE + 0x44)  // Interrupt Clear

// Биты регистра FR (Flag Register)
#define FR_RXFE             (1 << 4)    // Receive FIFO Empty
#define FR_TXFF             (1 << 5)    // Transmit FIFO Full
#define FR_BUSY             (1 << 3)    // UART Busy

// Биты регистра LCRH (Line Control)
#define LCRH_FEN            (1 << 4)    // Enable FIFO
#define LCRH_WLEN_8BIT      (3 << 5)   // 8-bit word length

// Биты регистра CR (Control Register)
#define CR_UARTEN           (1 << 0)    // UART Enable
#define CR_TXE              (1 << 8)    // Transmit Enable
#define CR_RXE              (1 << 9)    // Receive Enable

// GPIO регистры для настройки альтернативных функций
#define GPFSEL1             (PERIPHERAL_BASE + 0x200004)
#define GPPUD              (PERIPHERAL_BASE + 0x200094)
#define GPPUDCLK0          (PERIPHERAL_BASE + 0x200098)

// ---------------------------------------------------------------
// Вспомогательные функции задержки (простые счётчики)
// ---------------------------------------------------------------
static void delay_cycles(uint32_t count) {
    // Простой цикл задержки — используется при инициализации GPIO
    // Не точный, но достаточный для настройки периферии
    volatile uint32_t i;
    for (i = 0; i < count; i++) {
        asm volatile("nop");    // No Operation — одна пустая инструкция
    }
}

// ---------------------------------------------------------------
// Функция: uart_init()
// Инициализация UART0 для работы на скорости 115200 бод
//
// Формула делителя скорости:
//   Делитель = тактовая_частота / (16 * скорость_передачи)
//   Для RPi4: тактовая = 48 МГц, скорость = 115200
//   Делитель = 48000000 / (16 * 115200) = 26.041666...
//   IBRD = 26 (целая часть)
//   FBRD = round(0.041666 * 64) = round(2.666) = 3
// ---------------------------------------------------------------
void uart_init(void) {
    // Шаг 1: Отключаем UART перед настройкой
    mmio_write(UART0_CR, 0);

    // Шаг 2: Настройка GPIO 14 и 15 для UART
    // GPIO 14 = TXD0 (передача), GPIO 15 = RXD0 (приём)
    // Альтернативная функция ALT0 = UART для этих пинов
    
    uint32_t gpfsel = mmio_read(GPFSEL1);
    
    // GPIO 14: биты [14:12] FSEL14 = 100 (ALT0)
    gpfsel &= ~(7 << 12);      // Очищаем биты 12-14
    gpfsel |=  (4 << 12);      // Устанавливаем ALT0 (binary 100)
    
    // GPIO 15: биты [17:15] FSEL15 = 100 (ALT0)
    gpfsel &= ~(7 << 15);      // Очищаем биты 15-17
    gpfsel |=  (4 << 15);      // Устанавливаем ALT0 (binary 100)
    
    mmio_write(GPFSEL1, gpfsel);

    // Шаг 3: Отключаем pull-up/pull-down резисторы для GPIO 14, 15
    // На RPi4 используется новая схема через GPPUPPDN регистры
    // Для совместимости с QEMU используем старый метод
    mmio_write(GPPUD, 0);           // No pull-up/down
    delay_cycles(150);               // Минимум 150 тактов ожидания
    
    // Применяем к GPIO 14 и 15 (биты 14 и 15)
    mmio_write(GPPUDCLK0, (1 << 14) | (1 << 15));
    delay_cycles(150);
    
    mmio_write(GPPUDCLK0, 0);       // Сбрасываем тактирование

    // Шаг 4: Очищаем все прерывания UART
    mmio_write(UART0_ICR, 0x7FF);   // Сбрасываем все биты прерываний

    // Шаг 5: Устанавливаем скорость передачи (115200 бод)
    // Тактовая UART = 48 МГц (задаётся в config.txt через init_uart_clock)
    mmio_write(UART0_IBRD, 26);     // Integer Baud Rate Divisor
    mmio_write(UART0_FBRD, 3);      // Fractional Baud Rate Divisor

    // Шаг 6: Настройка формата данных 8N1 + включение FIFO
    mmio_write(UART0_LCRH, LCRH_FEN | LCRH_WLEN_8BIT);

    // Шаг 7: Маскируем все прерывания UART (не используем их)
    mmio_write(UART0_IMSC, 0x7F2);  // Маскируем все источники прерываний

    // Шаг 8: Включаем UART (TX + RX + UART Enable)
    mmio_write(UART0_CR, CR_UARTEN | CR_TXE | CR_RXE);
}

// ---------------------------------------------------------------
// Функция: uart_putc(char c)
// Отправка одного символа через UART
// Ждём, пока буфер передатчика не освободится (busy-wait)
// ---------------------------------------------------------------
void uart_putc(char c) {
    // Ждём, пока FIFO передатчика не освободится
    // FR_TXFF = 1: буфер полный, нельзя писать
    while (mmio_read(UART0_FR) & FR_TXFF) {
        asm volatile("nop");    // Ждём
    }
    
    // Записываем символ в регистр данных
    mmio_write(UART0_DR, (uint32_t)c);
}

// ---------------------------------------------------------------
// Функция: uart_getc()
// Получение одного символа из UART (блокирующее)
// ---------------------------------------------------------------
char uart_getc(void) {
    // Ждём появления данных в приёмном FIFO
    // FR_RXFE = 1: приёмный буфер пуст
    while (mmio_read(UART0_FR) & FR_RXFE) {
        asm volatile("nop");    // Ждём символа
    }
    
    // Читаем символ (биты [7:0] регистра DR)
    return (char)(mmio_read(UART0_DR) & 0xFF);
}

// ---------------------------------------------------------------
// Функция: uart_getc_nonblock()
// Неблокирующее получение символа
// Возвращает 0 если нет данных
// ---------------------------------------------------------------
char uart_getc_nonblock(void) {
    if (mmio_read(UART0_FR) & FR_RXFE) {
        return 0;   // Буфер пуст — нет символа
    }
    return (char)(mmio_read(UART0_DR) & 0xFF);
}

// ---------------------------------------------------------------
// Функция: uart_puts(const char* str)
// Отправка строки (завершённой нулём)
// ---------------------------------------------------------------
void uart_puts(const char* str) {
    while (*str) {
        // Преобразуем \n в \r\n (требование терминалов)
        if (*str == '\n') {
            uart_putc('\r');
        }
        uart_putc(*str++);
    }
}

// ---------------------------------------------------------------
// Функция: uart_puthex(uint64_t value)
// Вывод числа в шестнадцатеричном формате (0xXXXXXXXXXXXXXXXX)
// Полезно для отладки адресов и регистров
// ---------------------------------------------------------------
void uart_puthex(uint64_t value) {
    const char hex_chars[] = "0123456789ABCDEF";
    char buf[19];   // "0x" + 16 цифр + '\0'
    int i;
    
    buf[0] = '0';
    buf[1] = 'x';
    buf[18] = '\0';
    
    // Заполняем с конца
    for (i = 17; i >= 2; i--) {
        buf[i] = hex_chars[value & 0xF];  // Берём младшие 4 бита
        value >>= 4;                        // Сдвигаем вправо на 4 бита
    }
    
    uart_puts(buf);
}

// ---------------------------------------------------------------
// Функция: uart_putdec(uint32_t value)
// Вывод числа в десятичном формате
// ---------------------------------------------------------------
void uart_putdec(uint32_t value) {
    char buf[11];   // Максимум 10 цифр для uint32 + '\0'
    int i = 10;
    
    buf[10] = '\0';
    
    if (value == 0) {
        uart_putc('0');
        return;
    }
    
    while (value > 0 && i > 0) {
        buf[--i] = '0' + (value % 10);
        value /= 10;
    }
    
    uart_puts(&buf[i]);
}

// ---------------------------------------------------------------
// Функция: uart_readline(char* buf, int max_len)
// Чтение строки от пользователя с поддержкой Backspace
// Возвращает количество прочитанных символов
// ---------------------------------------------------------------
int uart_readline(char* buf, int max_len) {
    int pos = 0;
    char c;
    
    while (1) {
        c = uart_getc();    // Ждём символ
        
        if (c == '\r' || c == '\n') {
            // Enter — конец строки
            uart_puts("\r\n");
            buf[pos] = '\0';
            return pos;
        }
        else if (c == 127 || c == '\b') {
            // Backspace — удаляем последний символ
            if (pos > 0) {
                pos--;
                uart_puts("\b \b");     // Курсор назад, пробел, снова назад
            }
        }
        else if (c >= 32 && c < 127 && pos < max_len - 1) {
            // Обычный печатный символ
            buf[pos++] = c;
            uart_putc(c);       // Эхо — показываем введённый символ
        }
        // Остальные символы (управляющие) игнорируем
    }
}
