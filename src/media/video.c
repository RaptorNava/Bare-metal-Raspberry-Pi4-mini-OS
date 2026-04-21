#include "../../include/video.h"
#include "../../include/mailbox.h"
#include "../../include/uart.h"
#include "../../include/timer.h"
#include "../../include/string.h"

#ifndef NULL
#define NULL ((void*)0)
#endif

// Сообщаем компилятору, что эти данные лежат где-то в другом месте
extern const int BAD_APPLE_TOTAL_FRAMES;
extern const char *bad_apple_frames[];

static int video_mode = 0;

int video_init(void) {
    video_mode = 2;
    return 1;
}

void video_put_pixel(uint32_t x, uint32_t y, uint32_t color) { (void)x;(void)y;(void)color; }
void video_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) { (void)x;(void)y;(void)w;(void)h;(void)color; }
void video_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg) { (void)x;(void)y;(void)c;(void)fg;(void)bg; }
void video_draw_text(uint32_t x, uint32_t y, const char *text, uint32_t fg, uint32_t bg) { (void)x;(void)y;(void)text;(void)fg;(void)bg; }
void video_clear(uint32_t color) { (void)color; uart_puts("\033[2J\033[H"); }

void video_play_bad_apple(void) {
    uart_puts("\033[?25l\033[2J\033[H");
    for (int i = 0; i < BAD_APPLE_TOTAL_FRAMES; i++) {
        uart_puts("\033[H");
        uart_puts(bad_apple_frames[i]);
        timer_delay_ms(40);
        if (uart_getc_nonblock() != 0) break;
    }
    uart_puts("\033[?25h\r\nDone.\r\n");
}

void video_print_info(void) {
    uart_puts("Mode: ASCII\r\nFrames: ");
    uart_putdec(BAD_APPLE_TOTAL_FRAMES);
    uart_puts("\r\n");
}
