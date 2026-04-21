#ifndef VIDEO_H
#define VIDEO_H

/**
 * video.h — Video subsystem (framebuffer + ASCII Bad Apple)
 */

#include <stdint.h>

/* Параметры анимации Bad Apple */
#define BAD_APPLE_FRAMES    5       /* Количество кадров в демо */
#define BAD_APPLE_DELAY_MS  200     /* Задержка между кадрами (5 FPS) */

int  video_init(void);
void video_clear(uint32_t color);
void video_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void video_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void video_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg);
void video_draw_text(uint32_t x, uint32_t y, const char *text, uint32_t fg, uint32_t bg);
void video_play_bad_apple(void);
void video_print_info(void);

#endif /* VIDEO_H */
