#ifndef VIDEO_H
#define VIDEO_H

/**
 * video.h — Framebuffer видео + текстовая консоль
 *
 * Два слоя:
 *   1. Низкий уровень: пиксели, прямоугольники, символы
 *   2. Высокий уровень: текстовая консоль со скроллингом
 *
 * Инициализация через VideoCore Mailbox.
 * Разрешение: 1920x1080 или 1280x720 (fallback).
 * Формат пикселя: 32 бит ARGB (0xAARRGGBB).
 */

#include <stdint.h>

/* ----------------------------------------------------------------
 * Цвета (ARGB 32-bit)
 * ---------------------------------------------------------------- */
#define COLOR_BLACK     0xFF000000
#define COLOR_WHITE     0xFFFFFFFF
#define COLOR_RED       0xFFFF0000
#define COLOR_GREEN     0xFF00FF00
#define COLOR_BLUE      0xFF0000FF
#define COLOR_CYAN      0xFF00FFFF
#define COLOR_YELLOW    0xFFFFFF00
#define COLOR_MAGENTA   0xFFFF00FF
#define COLOR_GRAY      0xFF808080
#define COLOR_DARKGRAY  0xFF404040
#define COLOR_ORANGE    0xFFFF8800
#define COLOR_LIME      0xFF88FF00

/* ----------------------------------------------------------------
 * Параметры текстовой консоли
 * ---------------------------------------------------------------- */
#define CONSOLE_FONT_W   8    /* Ширина символа в пикселях  */
#define CONSOLE_FONT_H   16   /* Высота символа в пикселях (8x8 x2 scale) */
#define CONSOLE_MARGIN_X 8    /* Отступ слева/справа */
#define CONSOLE_MARGIN_Y 8    /* Отступ сверху/снизу */

/* ----------------------------------------------------------------
 * Низкоуровневое API (пиксели)
 * ---------------------------------------------------------------- */
int      video_init(void);
int      video_is_available(void);

void     video_clear(uint32_t color);
void     video_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void     video_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void     video_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);

/* Рисование символа шрифтом 8x8 (масштаб x2 = 16x16 на экране) */
void     video_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg);
void     video_draw_text(uint32_t x, uint32_t y, const char *text, uint32_t fg, uint32_t bg);

/* ----------------------------------------------------------------
 * Высокоуровневое API — текстовая консоль
 * Автоматический перенос строк, скроллинг, курсор
 * ---------------------------------------------------------------- */
void     console_init(void);
void     console_putc(char c);
void     console_puts(const char *s);
void     console_set_color(uint32_t fg, uint32_t bg);
void     console_clear(void);
void     console_set_cursor(int col, int row);
void     console_get_size(int *cols, int *rows);

/* ----------------------------------------------------------------
 * Информация и Bad Apple (заглушка)
 * ---------------------------------------------------------------- */
void     video_print_info(void);
void     video_play_bad_apple(void);

/* ----------------------------------------------------------------
 * Получить параметры framebuffer (для отладки)
 * ---------------------------------------------------------------- */
uint32_t video_get_width(void);
uint32_t video_get_height(void);
uint32_t video_get_pitch(void);
/* Bad Apple data (определены в video_data.c) */
extern const int BAD_APPLE_TOTAL_FRAMES;
extern const char *bad_apple_frames[];
#endif /* VIDEO_H */
