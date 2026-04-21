/**
 * printf.c — Реализация printf для bare-metal окружения
 *
 * Стандартный printf требует libc и системных вызовов.
 * Здесь мы реализуем упрощённую версию, которая выводит через UART.
 *
 * Поддерживаемые форматные спецификаторы:
 *   %d, %i  — знаковое целое (int)
 *   %u      — беззнаковое целое (uint32_t)
 *   %x, %X  — шестнадцатеричное (строчные/прописные)
 *   %o      — восьмеричное
 *   %b      — двоичное
 *   %c      — символ (char)
 *   %s      — строка
 *   %p      — указатель (как %016X)
 *   %l, %ll — long и long long варианты
 *   %%      — знак процента
 *
 * Модификаторы ширины:
 *   %5d     — поле шириной 5, выравнивание вправо
 *   %-5d    — поле шириной 5, выравнивание влево
 *   %05d    — заполнение нулями
 *   %*d     — ширина из аргумента
 *
 * Архитектура:
 *   kprintf_buf()   — внутренний буферизованный вывод
 *   kvprintf()      — форматирование с va_list
 *   kprintf()       — основная функция printf
 *   ksprintf()      — вывод в строку
 *   ksnprintf()     — вывод в строку с ограничением длины
 */

#include "../../include/printf.h"
#include "../../include/uart.h"
#include <stdarg.h>
#include <stddef.h>
// ---------------------------------------------------------------
// Вспомогательные функции
// ---------------------------------------------------------------

// Буфер для временного хранения чисел при форматировании
#define NUM_BUF_SIZE    64

// Вспомогательная функция: записывает символ в вывод
// output_ctx: если NULL — в UART, иначе в буфер строки
typedef struct {
    char    *buf;       // Буфер (для sprintf)
    int      pos;       // Текущая позиция
    int      max;       // Максимальный размер (0 = не ограничено)
} output_ctx_t;

static void write_char(output_ctx_t *ctx, char c) {
    if (ctx == NULL) {
        // Выводим в UART
        uart_putc(c);
    } else {
        // Выводим в буфер строки
        if (ctx->max == 0 || ctx->pos < ctx->max - 1) {
            ctx->buf[ctx->pos++] = c;
        }
    }
}

static void write_string(output_ctx_t *ctx, const char *s) {
    while (*s) write_char(ctx, *s++);
}

// ---------------------------------------------------------------
// Форматирование числа в строку
// Возвращает длину строки в buf
// ---------------------------------------------------------------
static int format_number(
    char *buf,          // Выходной буфер
    uint64_t value,     // Число
    int base,           // Основание (2, 8, 10, 16)
    int is_signed,      // Знаковое ли число
    int upper,          // Прописные буквы для hex
    int width,          // Минимальная ширина
    char pad_char,      // Символ заполнения (' ' или '0')
    int left_align      // Выравнивание влево
) {
    const char *digits_lower = "0123456789abcdef";
    const char *digits_upper = "0123456789ABCDEF";
    const char *digits = upper ? digits_upper : digits_lower;
    
    char tmp[NUM_BUF_SIZE];
    int  tmp_len = 0;
    int  is_negative = 0;
    
    // Обрабатываем знак
    if (is_signed && (int64_t)value < 0) {
        is_negative = 1;
        value = (uint64_t)(-(int64_t)value);
    }
    
    // Специальный случай: 0
    if (value == 0) {
        tmp[tmp_len++] = '0';
    } else {
        while (value > 0) {
            tmp[tmp_len++] = digits[value % base];
            value /= base;
        }
    }
    
    // Собираем строку (числа в tmp — в обратном порядке)
    int buf_len = 0;
    
    int num_len = tmp_len + (is_negative ? 1 : 0);
    
    // Заполнение слева (если выравнивание вправо)
    if (!left_align && width > num_len) {
        int pad_count = width - num_len;
        int i;
        for (i = 0; i < pad_count; i++) {
            buf[buf_len++] = pad_char;
        }
    }
    
    // Знак минус
    if (is_negative) buf[buf_len++] = '-';
    
    // Цифры (в правильном порядке)
    int i;
    for (i = tmp_len - 1; i >= 0; i--) {
        buf[buf_len++] = tmp[i];
    }
    
    // Заполнение справа (если выравнивание влево)
    if (left_align && width > num_len) {
        int pad_count = width - num_len;
        for (i = 0; i < pad_count; i++) {
            buf[buf_len++] = ' ';  // Слева всегда пробелы
        }
    }
    
    buf[buf_len] = '\0';
    return buf_len;
}

// ---------------------------------------------------------------
// Главная функция форматирования
// ---------------------------------------------------------------
static int kvprintf_impl(output_ctx_t *ctx, const char *fmt, va_list args) {
    int count = 0;      // Количество выведенных символов
    char num_buf[NUM_BUF_SIZE];
    
    while (*fmt) {
        if (*fmt != '%') {
            // Обычный символ — выводим как есть
            write_char(ctx, *fmt++);
            count++;
            continue;
        }
        
        fmt++;  // Пропускаем '%'
        
        // -------------------------------------------------------
        // Разбор модификаторов
        // -------------------------------------------------------
        int  left_align = 0;    // '-' — выравнивание влево
        int  zero_pad   = 0;    // '0' — заполнение нулями
        int  width      = 0;    // Ширина поля
        int  is_long    = 0;    // 'l' или 'll' модификатор
        
        // Флаги
        while (*fmt == '-' || *fmt == '0' || *fmt == '+' || *fmt == ' ') {
            if (*fmt == '-') left_align = 1;
            if (*fmt == '0') zero_pad   = 1;
            fmt++;
        }
        
        // Ширина поля
        if (*fmt == '*') {
            // Ширина из аргумента
            width = va_arg(args, int);
            if (width < 0) { left_align = 1; width = -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt++ - '0');
            }
        }
        
        // Точность (просто пропускаем для простоты)
        if (*fmt == '.') {
            fmt++;
            while (*fmt >= '0' && *fmt <= '9') fmt++;
        }
        
        // Модификатор длины
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
            if (*fmt == 'l') {
                is_long = 2;    // long long
                fmt++;
            }
        }
        
        char pad_char = (zero_pad && !left_align) ? '0' : ' ';
        
        // -------------------------------------------------------
        // Форматный спецификатор
        // -------------------------------------------------------
        char spec = *fmt++;
        int len;
        
        switch (spec) {
        
        case 'd': case 'i': {
            // Знаковое целое
            int64_t val;
            if (is_long == 2) val = va_arg(args, long long);
            else if (is_long) val = va_arg(args, long);
            else              val = va_arg(args, int);
            
            len = format_number(num_buf, (uint64_t)val, 10, 1, 0,
                               width, pad_char, left_align);
            write_string(ctx, num_buf);
            count += len;
            break;
        }
        
        case 'u': {
            // Беззнаковое целое
            uint64_t val;
            if (is_long == 2) val = va_arg(args, unsigned long long);
            else if (is_long) val = va_arg(args, unsigned long);
            else              val = va_arg(args, unsigned int);
            
            len = format_number(num_buf, val, 10, 0, 0,
                               width, pad_char, left_align);
            write_string(ctx, num_buf);
            count += len;
            break;
        }
        
        case 'x': case 'X': {
            // Шестнадцатеричное
            uint64_t val;
            if (is_long == 2) val = va_arg(args, unsigned long long);
            else if (is_long) val = va_arg(args, unsigned long);
            else              val = va_arg(args, unsigned int);
            
            len = format_number(num_buf, val, 16, 0, (spec == 'X'),
                               width, pad_char, left_align);
            write_string(ctx, num_buf);
            count += len;
            break;
        }
        
        case 'o': {
            // Восьмеричное
            uint32_t val = va_arg(args, unsigned int);
            len = format_number(num_buf, val, 8, 0, 0,
                               width, pad_char, left_align);
            write_string(ctx, num_buf);
            count += len;
            break;
        }
        
        case 'b': {
            // Двоичное (нестандартное расширение)
            uint32_t val = va_arg(args, unsigned int);
            len = format_number(num_buf, val, 2, 0, 0,
                               width, pad_char, left_align);
            write_string(ctx, num_buf);
            count += len;
            break;
        }
        
        case 'c': {
            // Символ
            char c = (char)va_arg(args, int);
            if (!left_align && width > 1) {
                int i;
                for (i = 1; i < width; i++) {
                    write_char(ctx, ' ');
                    count++;
                }
            }
            write_char(ctx, c);
            count++;
            if (left_align && width > 1) {
                int i;
                for (i = 1; i < width; i++) {
                    write_char(ctx, ' ');
                    count++;
                }
            }
            break;
        }
        
        case 's': {
            // Строка
            const char *s = va_arg(args, const char *);
            if (!s) s = "(null)";
            
            int slen = 0;
            const char *p = s;
            while (*p++) slen++;
            
            if (!left_align && width > slen) {
                int i;
                for (i = slen; i < width; i++) {
                    write_char(ctx, ' ');
                    count++;
                }
            }
            write_string(ctx, s);
            count += slen;
            if (left_align && width > slen) {
                int i;
                for (i = slen; i < width; i++) {
                    write_char(ctx, ' ');
                    count++;
                }
            }
            break;
        }
        
        case 'p': {
            // Указатель — как 0x + hex 16 символов
            void *ptr = va_arg(args, void *);
            write_string(ctx, "0x");
            len = format_number(num_buf, (uint64_t)(unsigned long)ptr,
                               16, 0, 0, 16, '0', 0);
            write_string(ctx, num_buf);
            count += len + 2;
            break;
        }
        
        case '%': {
            // Буквальный знак процента
            write_char(ctx, '%');
            count++;
            break;
        }
        
        case '\0':
            // Неожиданный конец строки формата
            fmt--;
            break;
        
        default:
            // Неизвестный спецификатор — выводим как есть
            write_char(ctx, '%');
            write_char(ctx, spec);
            count += 2;
            break;
        }
    }
    
    // Завершаем строку (для sprintf/snprintf)
    if (ctx != NULL) {
        ctx->buf[ctx->pos] = '\0';
    }
    
    return count;
}

// ---------------------------------------------------------------
// Публичные функции
// ---------------------------------------------------------------

/**
 * kprintf — Вывод форматированной строки через UART
 * Аналог printf()
 */
int kprintf(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    int result = kvprintf_impl(NULL, fmt, args);
    va_end(args);
    return result;
}

/**
 * ksprintf — Вывод форматированной строки в буфер
 * Аналог sprintf()
 */
int ksprintf(char *buf, const char *fmt, ...) {
    output_ctx_t ctx = { buf, 0, 0 };
    va_list args;
    va_start(args, fmt);
    int result = kvprintf_impl(&ctx, fmt, args);
    va_end(args);
    return result;
}

/**
 * ksnprintf — Вывод форматированной строки в буфер с ограничением
 * Аналог snprintf()
 */
int ksnprintf(char *buf, int size, const char *fmt, ...) {
    output_ctx_t ctx = { buf, 0, size };
    va_list args;
    va_start(args, fmt);
    int result = kvprintf_impl(&ctx, fmt, args);
    va_end(args);
    return result;
}

/**
 * kvprintf — Вывод с va_list (аналог vprintf)
 */
int kvprintf(const char *fmt, va_list args) {
    return kvprintf_impl(NULL, fmt, args);
}
