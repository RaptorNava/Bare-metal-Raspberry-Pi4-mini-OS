/**
 * shell.c — Интерактивный командный интерпретатор
 * Обновлён: вывод дублируется на HDMI через console_mux.h
 */

#include "../../include/shell.h"
#include "../../include/uart.h"
#include "../../include/gpio.h"
#include "../../include/timer.h"
#include "../../include/memory.h"
#include "../../include/video.h"
#include "../../include/string.h"
#include "../../include/printf.h"
#include "../../include/console_mux.h"
#include <stddef.h>

/* ----------------------------------------------------------------
 * Внешние символы из kernel.c
 * ---------------------------------------------------------------- */
extern volatile int      g_video_available;
extern volatile uint64_t g_uptime_ticks;
extern uint64_t          kernel_get_uptime_ms(void);

/* ----------------------------------------------------------------
 * Константы
 * ---------------------------------------------------------------- */
#define SHELL_MAX_CMD_LEN   256
#define SHELL_MAX_ARGS      16
#define SHELL_HISTORY_SIZE  8

/* ----------------------------------------------------------------
 * Вывод: UART всегда, HDMI если доступен
 * ---------------------------------------------------------------- */
static void sh_puts(const char *s) {
    uart_puts(s);
    if (g_video_available) console_puts(s);
}

static void sh_putc(char c) {
    uart_putc(c);
    if (g_video_available) console_putc(c);
}

/* Вывод числа в обоих каналах */
static void sh_putdec(uint32_t v) {
    /* В UART */
    uart_putdec(v);
    /* В HDMI */
    if (!g_video_available) return;
    char buf[12]; int i = 10; buf[10] = '\0';
    if (v == 0) { buf[--i] = '0'; }
    else { while (v > 0) { buf[--i] = '0' + (v % 10); v /= 10; } }
    console_puts(&buf[i]);
}

static void sh_puthex(uint64_t v) {
    uart_puthex(v);
    if (!g_video_available) return;
    char buf[19]; int i;
    buf[0] = '0'; buf[1] = 'x'; buf[18] = '\0';
    for (i = 17; i >= 2; i--) {
        buf[i] = "0123456789ABCDEF"[v & 0xF];
        v >>= 4;
    }
    console_puts(buf);
}

/* Цветной вывод: UART через ANSI, HDMI через console_set_color */
static void sh_color_puts(uint32_t hdmi_color, const char *ansi, const char *s) {
    uart_puts(ansi); uart_puts(s); uart_puts("\033[0m");
    if (g_video_available) {
        console_set_color(hdmi_color, COLOR_BLACK);
        console_puts(s);
        console_set_color(COLOR_WHITE, COLOR_BLACK);
    }
}

/* ----------------------------------------------------------------
 * ANSI коды
 * ---------------------------------------------------------------- */
#define C_RED    "\033[31m"
#define C_GREEN  "\033[32m"
#define C_YELLOW "\033[33m"
#define C_CYAN   "\033[36m"
#define C_BOLD   "\033[1m"
#define C_RESET  "\033[0m"

/* ----------------------------------------------------------------
 * Структура команды
 * ---------------------------------------------------------------- */
typedef struct {
    const char *name;
    const char *description;
    int (*handler)(int argc, char *argv[]);
} shell_command_t;

/* ----------------------------------------------------------------
 * История команд
 * ---------------------------------------------------------------- */
static char history[SHELL_HISTORY_SIZE][SHELL_MAX_CMD_LEN];
static int  history_count = 0;

static void history_add(const char *cmd) {
    if (!cmd[0]) return;
    int i;
    for (i = SHELL_HISTORY_SIZE - 1; i > 0; i--) {
        str_copy(history[i], history[i-1], SHELL_MAX_CMD_LEN);
    }
    str_copy(history[0], cmd, SHELL_MAX_CMD_LEN);
    if (history_count < SHELL_HISTORY_SIZE) history_count++;
}

/* ----------------------------------------------------------------
 * Разбор аргументов
 * ---------------------------------------------------------------- */
static int parse_args(char *line, char *argv[], int max) {
    int argc = 0;
    char *p = line;
    while (*p && argc < max) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = '\0'; p++; }
    }
    return argc;
}

/* ================================================================
 * Обработчики команд
 * ================================================================ */

static int cmd_help(int argc, char *argv[]) {
    (void)argc; (void)argv;
    sh_puts(C_BOLD "Available commands:\r\n" C_RESET);
    const char *cmds[] = {
        "  help              - Show this help",
        "  clear             - Clear screen",
        "  info              - System information",
        "  mem               - Memory usage",
        "  uptime            - System uptime",
        "  timer             - Timer & scheduler info",
        "  echo <text>       - Print text",
        "  hex <addr> [n]    - Memory dump",
        "  led <on|off|blink>- Control GPIO LED",
        "  video             - Video system info",
        "  play              - Play Bad Apple",
        "  history           - Command history",
        "  reboot            - Reboot system",
        "  panic             - Test kernel panic",
        NULL
    };
    int i;
    for (i = 0; cmds[i]; i++) {
        sh_puts(cmds[i]); sh_puts("\r\n");
    }
    return 0;
}

static int cmd_clear(int argc, char *argv[]) {
    (void)argc; (void)argv;
    uart_puts("\033[2J\033[H");
    if (g_video_available) console_clear();
    return 0;
}

static int cmd_info(int argc, char *argv[]) {
    (void)argc; (void)argv;
    sh_puts(C_BOLD "=== System Information ===\r\n" C_RESET);
    sh_puts("  OS      : RaspberryOS Mini v0.1\r\n");
    sh_puts("  Arch    : AArch64 (ARM64)\r\n");
    sh_puts("  Board   : Raspberry Pi 4\r\n");
    sh_puts("  Output  : ");
    if (g_video_available) {
        sh_puts("UART + HDMI Framebuffer\r\n");
    } else {
        sh_puts("UART only\r\n");
    }
    sh_puts("  Uptime  : ");
    uint64_t ms = kernel_get_uptime_ms();
    uint32_t sec = (uint32_t)(ms / 1000);
    sh_putdec(sec / 3600); sh_puts("h ");
    sh_putdec((sec % 3600) / 60); sh_puts("m ");
    sh_putdec(sec % 60); sh_puts("s\r\n");
    sh_puts("  Ticks   : "); sh_putdec((uint32_t)g_uptime_ticks); sh_puts("\r\n");
    return 0;
}

static int cmd_mem(int argc, char *argv[]) {
    (void)argc; (void)argv;
    memory_stats_t stats;
    memory_get_stats(&stats);
    sh_puts(C_BOLD "=== Memory Status ===\r\n" C_RESET);
    sh_puts("  Total   : "); sh_putdec(stats.total / 1024); sh_puts(" KB\r\n");
    sh_puts("  Used    : "); sh_putdec(stats.used  / 1024); sh_puts(" KB\r\n");
    sh_puts("  Free    : "); sh_putdec(stats.free  / 1024); sh_puts(" KB\r\n");
    sh_puts("  Allocs  : "); sh_putdec(stats.alloc_count);  sh_puts("\r\n");
    /* Прогресс-бар */
    uint32_t pct = stats.total ? (stats.used * 20) / stats.total : 0;
    sh_puts("  [");
    uint32_t i;
    for (i = 0; i < 20; i++) sh_putc(i < pct ? '#' : '.');
    sh_puts("] ");
    sh_putdec(stats.total ? (stats.used * 100) / stats.total : 0);
    sh_puts("%\r\n");
    return 0;
}

static int cmd_uptime(int argc, char *argv[]) {
    (void)argc; (void)argv;
    uint64_t ms = kernel_get_uptime_ms();
    uint32_t sec = (uint32_t)(ms / 1000);
    sh_puts("Uptime: ");
    sh_putdec(sec / 3600); sh_puts("h ");
    sh_putdec((sec % 3600) / 60); sh_puts("m ");
    sh_putdec(sec % 60); sh_puts("s\r\n");
    return 0;
}

static int cmd_timer(int argc, char *argv[]) {
    (void)argc; (void)argv;
    sh_puts(C_BOLD "=== Timer & Scheduler ===\r\n" C_RESET);
    sh_puts("  Interval : "); sh_putdec(TIMER_INTERVAL_US); sh_puts(" us\r\n");
    sh_puts("  Ticks    : "); sh_putdec((uint32_t)g_uptime_ticks); sh_puts("\r\n");
    timer_print_stats();
    return 0;
}

static int cmd_echo(int argc, char *argv[]) {
    int i;
    for (i = 1; i < argc; i++) {
        sh_puts(argv[i]);
        if (i < argc - 1) sh_putc(' ');
    }
    sh_puts("\r\n");
    return 0;
}

static int cmd_hex(int argc, char *argv[]) {
    if (argc < 2) { sh_puts("Usage: hex <address> [count]\r\n"); return 1; }

    const char *p = argv[1];
    uint64_t addr = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    while (*p) {
        char c = *p++;
        uint8_t n = 0;
        if (c >= '0' && c <= '9') n = (uint8_t)(c - '0');
        else if (c >= 'a' && c <= 'f') n = (uint8_t)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') n = (uint8_t)(c - 'A' + 10);
        else break;
        addr = (addr << 4) | n;
    }

    int count = 4;
    if (argc >= 3) {
        count = 0;
        for (p = argv[2]; *p >= '0' && *p <= '9'; p++)
            count = count * 10 + (*p - '0');
    }
    if (count > 32) count = 32;

    int row, col;
    for (row = 0; row < count; row++) {
        sh_puthex(addr + (uint32_t)row * 16); sh_puts(": ");
        for (col = 0; col < 16; col++) {
            uint8_t byte = *(volatile uint8_t *)(addr + (uint32_t)row * 16 + (uint32_t)col);
            sh_putc("0123456789ABCDEF"[byte >> 4]);
            sh_putc("0123456789ABCDEF"[byte & 0xF]);
            sh_putc(' ');
        }
        sh_puts("  ");
        for (col = 0; col < 16; col++) {
            char c = *(volatile char *)(addr + (uint32_t)row * 16 + (uint32_t)col);
            sh_putc((c >= 32 && c < 127) ? c : '.');
        }
        sh_puts("\r\n");
    }
    return 0;
}

static int cmd_led(int argc, char *argv[]) {
    if (argc < 2) { sh_puts("Usage: led <on|off|blink>\r\n"); return 1; }
    if (str_cmp(argv[1], "on") == 0) {
        gpio_led_on(); sh_puts("LED: ON\r\n");
    } else if (str_cmp(argv[1], "off") == 0) {
        gpio_led_off(); sh_puts("LED: OFF\r\n");
    } else if (str_cmp(argv[1], "blink") == 0) {
        sh_puts("Blinking 5 times...\r\n"); gpio_led_blink(5);
    } else {
        sh_puts("Unknown led command\r\n"); return 1;
    }
    return 0;
}

static int cmd_video(int argc, char *argv[]) {
    (void)argc; (void)argv;
    video_print_info();
    if (g_video_available) {
        /* Дополнительно вывести консольные параметры на HDMI */
        int cols, rows;
        console_get_size(&cols, &rows);
        sh_puts("  Console : ");
        sh_putdec((uint32_t)cols); sh_puts(" x ");
        sh_putdec((uint32_t)rows); sh_puts(" chars\r\n");
    }
    return 0;
}

static int cmd_play(int argc, char *argv[]) {
    (void)argc; (void)argv;
    sh_puts("Starting Bad Apple...\r\n");
    video_play_bad_apple();
    return 0;
}

static int cmd_history(int argc, char *argv[]) {
    (void)argc; (void)argv;
    if (!history_count) { sh_puts("No history.\r\n"); return 0; }
    int i;
    for (i = history_count - 1; i >= 0; i--) {
        sh_puts("  "); sh_putdec((uint32_t)(history_count - i));
        sh_puts("  "); sh_puts(history[i]); sh_puts("\r\n");
    }
    return 0;
}

static int cmd_reboot(int argc, char *argv[]) {
    (void)argc; (void)argv;
    sh_puts("Rebooting...\r\n");
    timer_delay_ms(500);
    #define PM_BASE  0xFE100000UL
    volatile uint32_t *pm_rstc = (volatile uint32_t *)(PM_BASE + 0x1C);
    volatile uint32_t *pm_wdog = (volatile uint32_t *)(PM_BASE + 0x24);
    *pm_wdog = 0x5A000000 | 1;
    *pm_rstc = 0x5A000000 | 0x20;
    while (1) asm volatile("nop");
    return 0;
}

static int cmd_panic(int argc, char *argv[]) {
    (void)argc; (void)argv;
    kernel_panic("Manual panic triggered via shell");
    return 0;
}

/* ----------------------------------------------------------------
 * Таблица команд
 * ---------------------------------------------------------------- */
static const shell_command_t commands[] = {
    { "help",    "Show help",            cmd_help    },
    { "clear",   "Clear screen",         cmd_clear   },
    { "info",    "System info",          cmd_info    },
    { "mem",     "Memory stats",         cmd_mem     },
    { "uptime",  "Show uptime",          cmd_uptime  },
    { "timer",   "Timer info",           cmd_timer   },
    { "echo",    "Print text",           cmd_echo    },
    { "hex",     "Memory dump",          cmd_hex     },
    { "led",     "LED control",          cmd_led     },
    { "video",   "Video info",           cmd_video   },
    { "play",    "Play Bad Apple",       cmd_play    },
    { "history", "Command history",      cmd_history },
    { "reboot",  "Reboot system",        cmd_reboot  },
    { "panic",   "Trigger kernel panic", cmd_panic   },
    { NULL, NULL, NULL }
};

/* ----------------------------------------------------------------
 * Выполнение команды
 * ---------------------------------------------------------------- */
static int execute_command(char *line) {
    char  *argv[SHELL_MAX_ARGS];
    char   copy[SHELL_MAX_CMD_LEN];
    str_copy(copy, line, SHELL_MAX_CMD_LEN);
    int argc = parse_args(copy, argv, SHELL_MAX_ARGS);
    if (!argc) return 0;

    const shell_command_t *cmd = commands;
    while (cmd->name) {
        if (str_cmp(argv[0], cmd->name) == 0)
            return cmd->handler(argc, argv);
        cmd++;
    }

    sh_puts(C_YELLOW "Unknown command: " C_RESET);
    sh_puts(argv[0]); sh_puts("  (type 'help')\r\n");
    return 1;
}

/* ----------------------------------------------------------------
 * Вывод приглашения командной строки
 * ---------------------------------------------------------------- */
static void print_prompt(void) {
    /* UART — ANSI цвета */
    uart_puts("\033[36mRPiOS\033[0m\033[33m>\033[0m ");
    /* HDMI — через console_set_color */
    if (g_video_available) {
        console_set_color(COLOR_CYAN, COLOR_BLACK);
        console_puts("RPiOS");
        console_set_color(COLOR_YELLOW, COLOR_BLACK);
        console_puts("> ");
        console_set_color(COLOR_WHITE, COLOR_BLACK);
    }
}

/* ----------------------------------------------------------------
 * Чтение строки: эхо дублируется на HDMI
 * ---------------------------------------------------------------- */

static int shell_readline(char *buf, int max_len) {
    int pos = 0;
    buf[0] = '\0';

    sh_puts("\r\n");                    // гарантируем перевод строки

    while (1) {
        timer_watchdog_kick();          // обязательно!

        char c = uart_getc_nonblock();

        if (c != 0) {                   // пришёл символ
            if (c == '\r' || c == '\n') {
                sh_puts("\r\n");
                buf[pos] = '\0';
                return pos;
            }
            else if ((c == 127 || c == '\b') && pos > 0) {
                pos--;
                uart_puts("\b \b");
                if (g_video_available) {
                    console_putc('\b');
                    console_putc(' ');
                    console_putc('\b');
                }
            }
            else if (c >= 32 && c < 127 && pos < max_len - 1) {
                buf[pos++] = c;
                sh_putc(c);             // эхо сразу в оба канала
            }
        }
        // Tight poll — БЕЗ timer_delay_ms! Это главное исправление
        asm volatile("nop");            // минимальная пауза, чтобы не грузить хост
    }
}
/* ----------------------------------------------------------------
 * ГЛАВНАЯ ФУНКЦИЯ SHELL
 * ---------------------------------------------------------------- */
void shell_run(void) {
    char cmdline[SHELL_MAX_CMD_LEN];
    print_prompt();

    while (1) {
        shell_readline(cmdline, SHELL_MAX_CMD_LEN);
        history_add(cmdline);
        execute_command(cmdline);
        print_prompt();
    }
}
