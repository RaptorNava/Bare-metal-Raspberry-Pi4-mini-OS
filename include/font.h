#ifndef FONT_H
#define FONT_H

/**
 * font.h — Встроенный bitmap шрифт 8x8
 * Каждый символ = 8 байт, каждый байт = одна строка пикселей (старший бит = левый)
 * Поддерживаются ASCII символы 32-126
 */

#include <stdint.h>

/* Размеры символа */
#define FONT_WIDTH   8
#define FONT_HEIGHT  8
#define FONT_FIRST   32   /* Первый символ (пробел) */
#define FONT_LAST    126  /* Последний символ (~) */

/* Таблица шрифта: объявлена в font.c */
extern const uint8_t font_8x8[95][8];

#endif /* FONT_H */
