/**
 * video.c — Framebuffer драйвер + текстовая консоль
 *
 * Инициализация:
 *   1. Запрашиваем framebuffer через VideoCore Mailbox
 *   2. GPU выделяет память и возвращает адрес + pitch
 *   3. Рисуем пиксели напрямую в память (MMIO-style)
 *
 * Текстовая консоль:
 *   - Символы 8x8 пикселей, масштаб x2 → 16x16 на экране
 *   - Автоматический перенос строки
 *   - Скроллинг: копируем строки вверх через kmemcpy
 *   - Курсор: позиция в символьных единицах
 *
 * Поддержка ANSI escape-кодов (базовая):
 *   \033[2J   — очистить экран
 *   \033[H    — курсор в начало
 *   \033[Xm   — цвет (31=red, 32=green, 33=yellow, 36=cyan, 0=reset, 1=bold)
 *   \r        — возврат каретки
 *   \n        — перевод строки
 *   \b        — backspace
 */

#include "../../include/video.h"
#include "../../include/mailbox.h"
#include "../../include/font.h"
#include "../../include/memory.h"
#include "../../include/uart.h"
#include "../../include/timer.h"
#include <stddef.h>
#include <stdint.h>

/* ----------------------------------------------------------------
 * Состояние framebuffer
 * ---------------------------------------------------------------- */
static uint32_t  fb_width   = 0;
static uint32_t  fb_height  = 0;
static uint32_t  fb_pitch   = 0;   /* Байт на строку */
static uint32_t *fb_addr    = NULL; /* Указатель на начало FB */
static int       fb_ready   = 0;

/* ----------------------------------------------------------------
 * Состояние текстовой консоли
 * ---------------------------------------------------------------- */
static int       con_cols   = 0;    /* Символов по горизонтали */
static int       con_rows   = 0;    /* Символов по вертикали   */
static int       con_cur_x  = 0;    /* Текущий столбец */
static int       con_cur_y  = 0;    /* Текущая строка  */
static uint32_t  con_fg     = COLOR_WHITE;
static uint32_t  con_bg     = COLOR_BLACK;

/* Разбор ANSI escape-последовательностей */
typedef enum {
    ANSI_NORMAL,       /* Обычный режим */
    ANSI_ESC,          /* Получили \033 */
    ANSI_BRACKET,      /* Получили \033[ */
    ANSI_PARAM,        /* Разбираем параметры */
} ansi_state_t;

static ansi_state_t ansi_state  = ANSI_NORMAL;
static int          ansi_params[8];
static int          ansi_param_count = 0;
static int          ansi_cur_param   = 0;

/* ----------------------------------------------------------------
 * Таблица ANSI цветов → наши 32-bit цвета
 * ---------------------------------------------------------------- */
static const uint32_t ansi_colors[8] = {
    COLOR_BLACK,    /* 0 = black  */
    COLOR_RED,      /* 1 = red    */
    COLOR_GREEN,    /* 2 = green  */
    COLOR_YELLOW,   /* 3 = yellow */
    COLOR_BLUE,     /* 4 = blue   */
    COLOR_MAGENTA,  /* 5 = magenta*/
    COLOR_CYAN,     /* 6 = cyan   */
    COLOR_WHITE,    /* 7 = white  */
};

/* ================================================================
 * НИЗКОУРОВНЕВЫЕ ФУНКЦИИ
 * ================================================================ */

/**
 * video_init — Инициализация framebuffer через Mailbox
 * Возвращает 0 при успехе, -1 при ошибке.
 */
int video_init(void) {
    uint32_t addr, pitch;

    /* Пробуем 1920x1080, затем 1280x720, затем 1024x768 */
    static const uint32_t try_w[] = { 1920, 1280, 1024 };
    static const uint32_t try_h[] = { 1080,  720,  768 };
    int i;

    for (i = 0; i < 3; i++) {
        if (mbox_init_framebuffer(try_w[i], try_h[i], &addr, &pitch) && addr != 0) {
            fb_width  = try_w[i];
            fb_height = try_h[i];
            fb_pitch  = pitch;
            /* Адрес GPU → адрес ARM: убираем биты кеша */
            fb_addr   = (uint32_t *)(unsigned long)(addr & 0x3FFFFFFF);
            fb_ready  = 1;
            break;
        }
    }

    if (!fb_ready) return -1;

    /* Очищаем экран */
    video_clear(COLOR_BLACK);

    /* Инициализируем текстовую консоль */
    console_init();

    return 0;
}

int video_is_available(void) { return fb_ready; }

uint32_t video_get_width(void)  { return fb_width;  }
uint32_t video_get_height(void) { return fb_height; }
uint32_t video_get_pitch(void)  { return fb_pitch;  }

/**
 * video_put_pixel — Рисуем один пиксель (ARGB 32-bit)
 */
void video_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!fb_ready || x >= fb_width || y >= fb_height) return;
    /* pitch в байтах → делим на 4 для uint32_t индексации */
    fb_addr[y * (fb_pitch / 4) + x] = color;
}

/**
 * video_clear — Заливка всего экрана одним цветом
 * Оптимизировано: пишем uint32_t, а не побайтово.
 */
void video_clear(uint32_t color) {
    if (!fb_ready) return;
    uint32_t row, col;
    uint32_t stride = fb_pitch / 4;
    for (row = 0; row < fb_height; row++) {
        for (col = 0; col < fb_width; col++) {
            fb_addr[row * stride + col] = color;
        }
    }
}

/**
 * video_fill_rect — Заливка прямоугольника
 */
void video_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (!fb_ready) return;
    uint32_t stride = fb_pitch / 4;
    uint32_t row, col;
    uint32_t x_end = x + w; if (x_end > fb_width)  x_end = fb_width;
    uint32_t y_end = y + h; if (y_end > fb_height) y_end = fb_height;
    for (row = y; row < y_end; row++) {
        for (col = x; col < x_end; col++) {
            fb_addr[row * stride + col] = color;
        }
    }
}

/**
 * video_draw_rect — Рамка (только контур)
 */
void video_draw_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    uint32_t i;
    for (i = x; i < x + w; i++) {
        video_put_pixel(i, y,         color);
        video_put_pixel(i, y + h - 1, color);
    }
    for (i = y; i < y + h; i++) {
        video_put_pixel(x,         i, color);
        video_put_pixel(x + w - 1, i, color);
    }
}

/**
 * video_draw_char — Рисуем один ASCII символ шрифтом 8x8, масштаб x2
 * Итого занимает 16x16 пикселей на экране.
 */
void video_draw_char(uint32_t x, uint32_t y, char c, uint32_t fg, uint32_t bg) {
    if (!fb_ready) return;
    if ((uint8_t)c < FONT_FIRST || (uint8_t)c > FONT_LAST) c = '?';

    const uint8_t *glyph = font_8x8[(uint8_t)c - FONT_FIRST];
    uint32_t row, col;

    for (row = 0; row < FONT_HEIGHT; row++) {
        /* Каждая строка шрифта — это строка из 8 бит */
        uint8_t line = glyph[row / 2];  /* x2 вертикальный масштаб */
        for (col = 0; col < FONT_WIDTH; col++) {
            /* x2 горизонтальный масштаб */
            uint32_t bit = (line >> (7 - (col / 2))) & 1;
            uint32_t color = bit ? fg : bg;
            video_put_pixel(x + col, y + row, color);
        }
    }
}

/**
 * video_draw_text — Рисуем строку начиная с (x, y)
 */
void video_draw_text(uint32_t x, uint32_t y, const char *text, uint32_t fg, uint32_t bg) {
    if (!fb_ready || !text) return;
    uint32_t cx = x;
    while (*text) {
        video_draw_char(cx, y, *text++, fg, bg);
        cx += CONSOLE_FONT_W;
    }
}

/* ================================================================
 * ТЕКСТОВАЯ КОНСОЛЬ
 * ================================================================ */

/**
 * console_scroll — Скроллинг содержимого консоли на одну строку вверх
 * Копируем всё содержимое экрана вверх на CONSOLE_FONT_H пикселей.
 */
static void console_scroll(void) {
    if (!fb_ready) return;

    uint32_t stride = fb_pitch / 4;
    uint32_t top_y  = CONSOLE_MARGIN_Y;
    uint32_t bot_y  = CONSOLE_MARGIN_Y + (uint32_t)con_rows * CONSOLE_FONT_H;
    uint32_t line_h = CONSOLE_FONT_H;

    /* Сдвигаем все строки вверх на line_h пикселей */
    uint32_t row, col;
    for (row = top_y; row < bot_y - line_h; row++) {
        for (col = 0; col < fb_width; col++) {
            fb_addr[row * stride + col] = fb_addr[(row + line_h) * stride + col];
        }
    }

    /* Очищаем последнюю строку */
    for (row = bot_y - line_h; row < bot_y; row++) {
        for (col = 0; col < fb_width; col++) {
            fb_addr[row * stride + col] = con_bg;
        }
    }
}

/**
 * console_init — Вычисляем размер консоли и рисуем фон
 */
void console_init(void) {
    if (!fb_ready) return;

    con_cols  = (int)(fb_width  - 2 * CONSOLE_MARGIN_X) / CONSOLE_FONT_W;
    con_rows  = (int)(fb_height - 2 * CONSOLE_MARGIN_Y) / CONSOLE_FONT_H;
    con_cur_x = 0;
    con_cur_y = 0;
    con_fg    = COLOR_WHITE;
    con_bg    = COLOR_BLACK;

    /* Заливаем фон */
    video_clear(con_bg);

    ansi_state       = ANSI_NORMAL;
    ansi_param_count = 0;
    ansi_cur_param   = 0;
}

void console_clear(void) {
    con_cur_x = 0;
    con_cur_y = 0;
    video_clear(con_bg);
}

void console_set_cursor(int col, int row) {
    con_cur_x = col;
    con_cur_y = row;
}

void console_get_size(int *cols, int *rows) {
    if (cols) *cols = con_cols;
    if (rows) *rows = con_rows;
}

void console_set_color(uint32_t fg, uint32_t bg) {
    con_fg = fg;
    con_bg = bg;
}

/**
 * console_render_char — Рисуем символ в позиции курсора и двигаем его
 */
static void console_render_char(char c) {
    uint32_t px = (uint32_t)(CONSOLE_MARGIN_X + con_cur_x * CONSOLE_FONT_W);
    uint32_t py = (uint32_t)(CONSOLE_MARGIN_Y + con_cur_y * CONSOLE_FONT_H);
    video_draw_char(px, py, c, con_fg, con_bg);
    con_cur_x++;
    if (con_cur_x >= con_cols) {
        con_cur_x = 0;
        con_cur_y++;
        if (con_cur_y >= con_rows) {
            console_scroll();
            con_cur_y = con_rows - 1;
        }
    }
}

/**
 * console_newline — Обработка \n
 */
static void console_newline(void) {
    con_cur_x = 0;
    con_cur_y++;
    if (con_cur_y >= con_rows) {
        console_scroll();
        con_cur_y = con_rows - 1;
    }
}

/**
 * console_backspace — Обработка \b
 */
static void console_backspace(void) {
    if (con_cur_x > 0) {
        con_cur_x--;
    } else if (con_cur_y > 0) {
        con_cur_y--;
        con_cur_x = con_cols - 1;
    }
    /* Затираем символ под курсором */
    uint32_t px = (uint32_t)(CONSOLE_MARGIN_X + con_cur_x * CONSOLE_FONT_W);
    uint32_t py = (uint32_t)(CONSOLE_MARGIN_Y + con_cur_y * CONSOLE_FONT_H);
    video_fill_rect(px, py, CONSOLE_FONT_W, CONSOLE_FONT_H, con_bg);
}

/**
 * ansi_apply_sgr — Применяем параметры ANSI SGR (цвет, стиль)
 */
static void ansi_apply_sgr(void) {
    int i;
    for (i = 0; i < ansi_param_count; i++) {
        int p = ansi_params[i];
        if (p == 0) {
            /* Сброс */
            con_fg = COLOR_WHITE;
            con_bg = COLOR_BLACK;
        } else if (p == 1) {
            /* Bold — делаем чуть ярче (просто оставляем цвет) */
        } else if (p >= 30 && p <= 37) {
            con_fg = ansi_colors[p - 30];
        } else if (p >= 40 && p <= 47) {
            con_bg = ansi_colors[p - 40];
        } else if (p == 90) {
            con_fg = COLOR_DARKGRAY;
        } else if (p == 97) {
            con_fg = COLOR_WHITE;
        }
    }
}

/**
 * console_putc — Вывод одного символа с поддержкой ANSI escape
 */
void console_putc(char c) {
    if (!fb_ready) return;

    switch (ansi_state) {

    case ANSI_NORMAL:
        if (c == '\033') {
            ansi_state = ANSI_ESC;
        } else if (c == '\r') {
            con_cur_x = 0;
        } else if (c == '\n') {
            console_newline();
        } else if (c == '\b' || c == 127) {
            console_backspace();
        } else if (c >= 32 && (uint8_t)c < 128) {
            console_render_char(c);
        }
        break;

    case ANSI_ESC:
        if (c == '[') {
            ansi_state       = ANSI_BRACKET;
            ansi_param_count = 0;
            ansi_cur_param   = 0;
            int j;
            for (j = 0; j < 8; j++) ansi_params[j] = 0;
        } else {
            ansi_state = ANSI_NORMAL;
        }
        break;

    case ANSI_BRACKET:
        ansi_state = ANSI_PARAM;
        /* fall through */

    case ANSI_PARAM:
        if (c >= '0' && c <= '9') {
            ansi_cur_param = ansi_cur_param * 10 + (c - '0');
        } else if (c == ';') {
            if (ansi_param_count < 8) {
                ansi_params[ansi_param_count++] = ansi_cur_param;
            }
            ansi_cur_param = 0;
        } else {
            /* Финальный символ команды */
            if (ansi_param_count < 8) {
                ansi_params[ansi_param_count++] = ansi_cur_param;
            }
            ansi_cur_param = 0;

            if (c == 'm') {
                /* SGR: цвет/стиль */
                ansi_apply_sgr();
            } else if (c == 'J') {
                /* ED: Erase Display */
                if (ansi_params[0] == 2) {
                    console_clear();
                }
            } else if (c == 'H' || c == 'f') {
                /* CUP: Cursor Position */
                int row = (ansi_param_count >= 1) ? ansi_params[0] - 1 : 0;
                int col = (ansi_param_count >= 2) ? ansi_params[1] - 1 : 0;
                if (row < 0) row = 0;
                if (col < 0) col = 0;
                if (row >= con_rows) row = con_rows - 1;
                if (col >= con_cols) col = con_cols - 1;
                con_cur_x = col;
                con_cur_y = row;
            } else if (c == 'A') {
                /* CUU: Cursor Up */
                con_cur_y -= (ansi_params[0] > 0) ? ansi_params[0] : 1;
                if (con_cur_y < 0) con_cur_y = 0;
            } else if (c == 'B') {
                /* CUD: Cursor Down */
                con_cur_y += (ansi_params[0] > 0) ? ansi_params[0] : 1;
                if (con_cur_y >= con_rows) con_cur_y = con_rows - 1;
            } else if (c == 'l' || c == 'h') {
                /* ?25l = hide cursor, ?25h = show cursor — игнорируем */
            }
            ansi_state = ANSI_NORMAL;
        }
        break;
    }
}

/**
 * console_puts — Вывод строки
 */
void console_puts(const char *s) {
    if (!s) return;
    while (*s) console_putc(*s++);
}

/* ================================================================
 * ИНФОРМАЦИОННЫЕ ФУНКЦИИ
 * ================================================================ */

void video_print_info(void) {
    /* Вывод в UART всегда работает */
    uart_puts("=== Video Subsystem ===\r\n");
    if (!fb_ready) {
        uart_puts("  Status  : NOT available\r\n");
        return;
    }
    uart_puts("  Status  : Active\r\n");
    uart_puts("  Width   : "); uart_putdec(fb_width);  uart_puts(" px\r\n");
    uart_puts("  Height  : "); uart_putdec(fb_height); uart_puts(" px\r\n");
    uart_puts("  Pitch   : "); uart_putdec(fb_pitch);  uart_puts(" bytes/line\r\n");
    uart_puts("  BPP     : 32 (ARGB)\r\n");
    uart_puts("  Console : ");
    uart_putdec((uint32_t)con_cols); uart_puts(" x ");
    uart_putdec((uint32_t)con_rows); uart_puts(" chars\r\n");
    uart_puts("  FB addr : "); uart_puthex((uint64_t)(unsigned long)fb_addr); uart_puts("\r\n");
}

void video_play_bad_apple(void) {
    if (!fb_ready) {
        uart_puts("[video] Bad Apple: framebuffer not available, falling back to UART\r\n");
        // можно оставить простой вывод в UART, если хочешь
        return;
    }

    console_clear();
    console_set_color(COLOR_WHITE, COLOR_BLACK);

    uart_puts("\r\n[video] Playing Bad Apple... (3477 frames)\r\n");
    uart_puts("Press any key to stop\r\n");

    for (int i = 0; i < BAD_APPLE_TOTAL_FRAMES; i++) {
        console_clear();                    // очищаем экран
        console_puts(bad_apple_frames[i]);  // выводим готовый ASCII-фрейм

        // ~30 FPS (можно подкрутить)
        timer_delay_ms(33);

        // возможность прервать по клавише (неблокирующее чтение)
        if (uart_getc_nonblock() != 0) {
            uart_puts("\r\n[video] Stopped by user\r\n");
            break;
        }
    }

    console_clear();
    console_set_color(COLOR_CYAN, COLOR_BLACK);
    console_puts("Bad Apple finished!\r\n");
    console_set_color(COLOR_WHITE, COLOR_BLACK);
}
