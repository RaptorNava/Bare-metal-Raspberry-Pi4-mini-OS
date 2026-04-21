/**
 * string.c — Библиотека строковых функций
 *
 * Стандартная библиотека C (libc) недоступна в bare-metal окружении.
 * Поэтому мы реализуем все нужные строковые функции самостоятельно.
 *
 * Реализованные функции:
 *   str_len()     — длина строки (strlen)
 *   str_copy()    — копирование строки (strncpy)
 *   str_cat()     — конкатенация (strncat)
 *   str_cmp()     — сравнение (strcmp)
 *   str_ncmp()    — сравнение N символов (strncmp)
 *   str_chr()     — поиск символа (strchr)
 *   str_rchr()    — поиск символа с конца (strrchr)
 *   str_str()     — поиск подстроки (strstr)
 *   str_tok()     — разбивка на токены (strtok)
 *   str_to_int()  — конвертация строки в целое число (atoi)
 *   str_to_hex()  — конвертация строки в hex число
 *   int_to_str()  — конвертация целого в строку (itoa)
 *   str_upper()   — в верхний регистр
 *   str_lower()   — в нижний регистр
 *   str_trim()    — удаление пробелов
 *   str_starts_with() — проверка префикса
 *   str_ends_with()   — проверка суффикса
 *   mem_set()     — заполнение памяти (memset)
 *   mem_copy()    — копирование памяти (memcpy)
 *   mem_cmp()     — сравнение памяти (memcmp)
 */

#include "../../include/string.h"
#include <stddef.h>
// ---------------------------------------------------------------
// Основные строковые функции
// ---------------------------------------------------------------

/**
 * str_len — Возвращает длину строки без нулевого терминатора
 * Аналог: strlen()
 */
int str_len(const char *s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

/**
 * str_copy — Копирует строку src в dst, не более max_len символов
 * Всегда завершает dst нулём
 * Аналог: strncpy() с гарантированным нулём
 */
char *str_copy(char *dst, const char *src, int max_len) {
    int i = 0;
    
    if (max_len <= 0) return dst;
    
    while (i < max_len - 1 && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';  // Гарантируем нулевой терминатор
    
    return dst;
}

/**
 * str_cat — Добавляет строку src к dst
 * max_len — максимальная длина итогового dst
 * Аналог: strncat()
 */
char *str_cat(char *dst, const char *src, int max_len) {
    int dst_len = str_len(dst);
    int i = 0;
    
    while (dst_len + i < max_len - 1 && src[i]) {
        dst[dst_len + i] = src[i];
        i++;
    }
    dst[dst_len + i] = '\0';
    
    return dst;
}

/**
 * str_cmp — Лексикографическое сравнение строк
 * Возвращает: 0 если равны, <0 если s1 < s2, >0 если s1 > s2
 * Аналог: strcmp()
 */
int str_cmp(const char *s1, const char *s2) {
    while (*s1 && *s2 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return (unsigned char)*s1 - (unsigned char)*s2;
}

/**
 * str_ncmp — Сравнение первых n символов
 * Аналог: strncmp()
 */
int str_ncmp(const char *s1, const char *s2, int n) {
    while (n > 0 && *s1 && *s2 && *s1 == *s2) {
        s1++; s2++; n--;
    }
    if (n == 0) return 0;
    return (unsigned char)*s1 - (unsigned char)*s2;
}

/**
 * str_chr — Поиск первого вхождения символа c в строке s
 * Аналог: strchr()
 */
char *str_chr(const char *s, int c) {
    while (*s) {
        if (*s == (char)c) return (char *)s;
        s++;
    }
    // Проверяем нулевой терминатор (c может быть '\0')
    if (c == '\0') return (char *)s;
    return NULL;
}

/**
 * str_rchr — Поиск последнего вхождения символа c
 * Аналог: strrchr()
 */
char *str_rchr(const char *s, int c) {
    const char *last = NULL;
    
    while (*s) {
        if (*s == (char)c) last = s;
        s++;
    }
    if (c == '\0') return (char *)s;
    return (char *)last;
}

/**
 * str_str — Поиск подстроки needle в строке haystack
 * Аналог: strstr()
 */
char *str_str(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;     // Пустая подстрока
    
    int needle_len = str_len(needle);
    
    while (*haystack) {
        if (str_ncmp(haystack, needle, needle_len) == 0) {
            return (char *)haystack;
        }
        haystack++;
    }
    return NULL;
}

/**
 * str_tok — Разбивка строки на токены по разделителям
 * При первом вызове: str != NULL
 * При последующих:  str == NULL (работает со сохранённым состоянием)
 * Аналог: strtok_r() (потокобезопасная версия)
 */
char *str_tok(char *str, const char *delim, char **saveptr) {
    char *start;
    
    if (str != NULL) {
        *saveptr = str;
    }
    
    if (*saveptr == NULL) return NULL;
    
    // Пропускаем разделители в начале
    while (**saveptr && str_chr(delim, **saveptr)) {
        (*saveptr)++;
    }
    
    if (**saveptr == '\0') {
        *saveptr = NULL;
        return NULL;
    }
    
    start = *saveptr;
    
    // Ищем следующий разделитель
    while (**saveptr && !str_chr(delim, **saveptr)) {
        (*saveptr)++;
    }
    
    if (**saveptr) {
        **saveptr = '\0';
        (*saveptr)++;
    } else {
        *saveptr = NULL;
    }
    
    return start;
}

/**
 * str_to_int — Конвертация строки в целое число
 * Поддерживает знак +/- и пробелы в начале
 * Аналог: atoi() / strtol()
 */
int str_to_int(const char *s) {
    int result = 0;
    int sign = 1;
    
    // Пропускаем ведущие пробелы
    while (*s == ' ' || *s == '\t') s++;
    
    // Знак
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') { s++; }
    
    // Цифры
    while (*s >= '0' && *s <= '9') {
        result = result * 10 + (*s - '0');
        s++;
    }
    
    return result * sign;
}

/**
 * str_to_hex — Конвертация hex-строки в число
 * Принимает строки вида "0xFF", "FF", "ff"
 */
uint32_t str_to_hex(const char *s) {
    uint32_t result = 0;
    
    // Пропускаем "0x" или "0X"
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    
    while (*s) {
        char c = *s++;
        uint8_t nibble;
        
        if (c >= '0' && c <= '9')      nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
        else break;  // Не hex символ
        
        result = (result << 4) | nibble;
    }
    
    return result;
}

/**
 * int_to_str — Конвертация числа в строку
 * base: основание (10 = десятичная, 16 = шестнадцатеричная)
 * Буфер buf должен быть достаточно большим (минимум 12 символов для uint32)
 */
char *int_to_str(int value, char *buf, int base) {
    const char digits[] = "0123456789ABCDEF";
    char tmp[32];
    int i = 0;
    int is_negative = 0;
    uint32_t uval;
    
    if (value == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return buf;
    }
    
    if (value < 0 && base == 10) {
        is_negative = 1;
        uval = (uint32_t)(-value);
    } else {
        uval = (uint32_t)value;
    }
    
    while (uval > 0) {
        tmp[i++] = digits[uval % base];
        uval /= base;
    }
    
    int j = 0;
    if (is_negative) buf[j++] = '-';
    
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
    
    return buf;
}

/**
 * str_upper — Преобразование строки в верхний регистр (in-place)
 */
void str_upper(char *s) {
    while (*s) {
        if (*s >= 'a' && *s <= 'z') *s -= 32;
        s++;
    }
}

/**
 * str_lower — Преобразование строки в нижний регистр (in-place)
 */
void str_lower(char *s) {
    while (*s) {
        if (*s >= 'A' && *s <= 'Z') *s += 32;
        s++;
    }
}

/**
 * str_trim — Удаление пробелов в начале и конце строки (in-place)
 */
char *str_trim(char *s) {
    // Удаляем пробелы в начале
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    
    // Удаляем пробелы в конце
    int len = str_len(start);
    char *end = start + len - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end = '\0';
        end--;
    }
    
    // Перемещаем строку в начало (если удалили ведущие пробелы)
    if (start != s) {
        char *dst = s;
        while (*start) *dst++ = *start++;
        *dst = '\0';
    }
    
    return s;
}

/**
 * str_starts_with — Проверяет, начинается ли строка s с prefix
 * Возвращает: 1 если да, 0 если нет
 */
int str_starts_with(const char *s, const char *prefix) {
    int prefix_len = str_len(prefix);
    return str_ncmp(s, prefix, prefix_len) == 0;
}

/**
 * str_ends_with — Проверяет, заканчивается ли строка s на suffix
 */
int str_ends_with(const char *s, const char *suffix) {
    int s_len      = str_len(s);
    int suffix_len = str_len(suffix);
    
    if (suffix_len > s_len) return 0;
    return str_cmp(s + s_len - suffix_len, suffix) == 0;
}

// ---------------------------------------------------------------
// Функции работы с памятью
// ---------------------------------------------------------------

/**
 * mem_set — Заполняет блок памяти значением
 * Аналог: memset()
 */
void *mem_set(void *ptr, int value, int size) {
    uint8_t *p = (uint8_t *)ptr;
    uint8_t v = (uint8_t)value;
    int i;
    for (i = 0; i < size; i++) p[i] = v;
    return ptr;
}

/**
 * mem_copy — Копирует блок памяти
 * Аналог: memcpy()
 */
void *mem_copy(void *dst, const void *src, int size) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    int i;
    for (i = 0; i < size; i++) d[i] = s[i];
    return dst;
}

/**
 * mem_cmp — Сравнивает блоки памяти
 * Аналог: memcmp()
 */
int mem_cmp(const void *a, const void *b, int size) {
    const uint8_t *pa = (const uint8_t *)a;
    const uint8_t *pb = (const uint8_t *)b;
    int i;
    for (i = 0; i < size; i++) {
        if (pa[i] != pb[i]) return pa[i] - pb[i];
    }
    return 0;
}

/**
 * is_digit — Проверка на цифру
 */
int is_digit(char c) {
    return c >= '0' && c <= '9';
}

/**
 * is_alpha — Проверка на букву
 */
int is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/**
 * is_alnum — Буква или цифра
 */
int is_alnum(char c) {
    return is_digit(c) || is_alpha(c);
}

/**
 * is_space — Пробельный символ
 */
int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}
