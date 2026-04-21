/**
 * shell.c — Интерактивный командный интерпретатор
 *
 * Shell — это пользовательский интерфейс нашей мини-ОС.
 * Он читает команды из UART и выполняет их.
 *
 * Поддерживаемые команды:
 *   help        — список команд
 *   clear       — очистка экрана
 *   info        — информация о системе
 *   mem         — состояние памяти
 *   uptime      — время работы
 *   timer       — информация о таймере и планировщике
 *   echo <text> — вывод текста
 *   hex <addr>  — дамп памяти по адресу
 *   led on/off  — управление LED
 *   video       — информация о видео
 *   play        — воспроизвести Bad Apple
 *   reboot      — перезапуск (программный)
 *   panic       — тест kernel panic
 *
 * Архитектура shell:
 *   - Таблица команд (массив структур с именем и функцией)
 *   - Разбор аргументов командной строки
 *   - История команд (простая, последние N команд)
 */

#include "../../include/shell.h"
#include "../../include/uart.h"
#include "../../include/gpio.h"
#include "../../include/timer.h"
#include "../../include/memory.h"
#include "../../include/video.h"
#include "../../include/string.h"
#include "../../include/printf.h"
#include <stddef.h>
// ---------------------------------------------------------------
// Константы
// ---------------------------------------------------------------
#define SHELL_MAX_CMD_LEN   256     // Максимальная длина команды
#define SHELL_MAX_ARGS      16      // Максимум аргументов
#define SHELL_HISTORY_SIZE  8       // Количество команд в истории
#define SHELL_PROMPT        "\033[36mRPiOS\033[0m\033[33m>\033[0m "

// ANSI коды цветов
#define C_RED    "\033[31m"
#define C_GREEN  "\033[32m"
#define C_YELLOW "\033[33m"
#define C_CYAN   "\033[36m"
#define C_BOLD   "\033[1m"
#define C_RESET  "\033[0m"

// ---------------------------------------------------------------
// Структура команды
// ---------------------------------------------------------------
typedef struct {
    const char *name;           // Имя команды
    const char *description;    // Краткое описание
    int (*handler)(int argc, char *argv[]);  // Функция-обработчик
} shell_command_t;

// ---------------------------------------------------------------
// История команд
// ---------------------------------------------------------------
static char history[SHELL_HISTORY_SIZE][SHELL_MAX_CMD_LEN];
static int history_count = 0;

static void history_add(const char *cmd) {
    if (cmd[0] == '\0') return;  // Не сохраняем пустые команды
    
    // Сдвигаем историю
    int i;
    for (i = SHELL_HISTORY_SIZE - 1; i > 0; i--) {
        str_copy(history[i], history[i-1], SHELL_MAX_CMD_LEN);
    }
    str_copy(history[0], cmd, SHELL_MAX_CMD_LEN);
    
    if (history_count < SHELL_HISTORY_SIZE) {
        history_count++;
    }
}

// ---------------------------------------------------------------
// Разбор командной строки на аргументы
// Поддерживает пробелы как разделители
// Возвращает количество аргументов
// ---------------------------------------------------------------
static int parse_args(char *cmdline, char *argv[], int max_args) {
    int argc = 0;
    char *p = cmdline;
    
    while (*p && argc < max_args) {
        // Пропускаем пробелы
        while (*p == ' ' || *p == '\t') p++;
        
        if (*p == '\0') break;
        
        // Начало аргумента
        argv[argc++] = p;
        
        // Находим конец аргумента
        while (*p && *p != ' ' && *p != '\t') p++;
        
        if (*p) {
            *p = '\0';  // Завершаем аргумент нулём
            p++;
        }
    }
    
    return argc;
}

// ---------------------------------------------------------------
// Обработчики команд
// ---------------------------------------------------------------

static int cmd_help(int argc, char *argv[]) {
    (void)argc; (void)argv;
    
    uart_puts(C_BOLD);
    uart_puts("Available commands:\r\n");
    uart_puts(C_RESET);
    uart_puts("  " C_CYAN "help" C_RESET "              — Show this help\r\n");
    uart_puts("  " C_CYAN "clear" C_RESET "             — Clear screen\r\n");
    uart_puts("  " C_CYAN "info" C_RESET "              — System information\r\n");
    uart_puts("  " C_CYAN "mem" C_RESET "               — Memory usage\r\n");
    uart_puts("  " C_CYAN "uptime" C_RESET "            — System uptime\r\n");
    uart_puts("  " C_CYAN "timer" C_RESET "             — Timer & scheduler info\r\n");
    uart_puts("  " C_CYAN "echo <text>" C_RESET "       — Print text\r\n");
    uart_puts("  " C_CYAN "hex <addr> [n]" C_RESET "    — Memory dump (hex)\r\n");
    uart_puts("  " C_CYAN "led <on|off|blink>" C_RESET " — Control GPIO LED\r\n");
    uart_puts("  " C_CYAN "video" C_RESET "             — Video system info\r\n");
    uart_puts("  " C_CYAN "play" C_RESET "              — Play Bad Apple animation\r\n");
    uart_puts("  " C_CYAN "history" C_RESET "           — Command history\r\n");
    uart_puts("  " C_CYAN "reboot" C_RESET "            — Reboot system\r\n");
    uart_puts("  " C_CYAN "panic" C_RESET "             — Test kernel panic\r\n");
    return 0;
}

static int cmd_clear(int argc, char *argv[]) {
    (void)argc; (void)argv;
    uart_puts("\033[2J\033[H");
    return 0;
}

extern uint64_t kernel_get_uptime_ms(void);
extern volatile uint64_t g_uptime_ticks;
extern volatile int g_video_available;

static int cmd_info(int argc, char *argv[]) {
    (void)argc; (void)argv;
    
    uart_puts(C_BOLD "=== System Information ===\r\n" C_RESET);
    uart_puts("  OS      : RaspberryOS Mini v0.1\r\n");
    uart_puts("  Arch    : AArch64 (ARM64)\r\n");
    uart_puts("  Board   : Raspberry Pi 4B (QEMU)\r\n");
    uart_puts("  Uptime  : ");
    
    uint64_t ms = kernel_get_uptime_ms();
    uint32_t sec = (uint32_t)(ms / 1000);
    uint32_t min = sec / 60;
    uint32_t hr  = min / 60;
    
    uart_putdec(hr);  uart_puts("h ");
    uart_putdec(min % 60); uart_puts("m ");
    uart_putdec(sec % 60); uart_puts("s\r\n");
    
    uart_puts("  Ticks   : ");
    uart_putdec((uint32_t)g_uptime_ticks);
    uart_puts("\r\n");
    
    uart_puts("  Video   : ");
    uart_puts(g_video_available ? C_GREEN "Available" C_RESET : C_YELLOW "Not available" C_RESET);
    uart_puts("\r\n");
    
    return 0;
}

static int cmd_mem(int argc, char *argv[]) {
    (void)argc; (void)argv;
    
    memory_stats_t stats;
    memory_get_stats(&stats);
    
    uart_puts(C_BOLD "=== Memory Status ===\r\n" C_RESET);
    uart_puts("  Total   : ");
    uart_putdec(stats.total / 1024);
    uart_puts(" KB\r\n");
    uart_puts("  Used    : ");
    uart_putdec(stats.used / 1024);
    uart_puts(" KB\r\n");
    uart_puts("  Free    : ");
    uart_putdec(stats.free / 1024);
    uart_puts(" KB\r\n");
    uart_puts("  Allocs  : ");
    uart_putdec(stats.alloc_count);
    uart_puts("\r\n");
    
    // Графическая полоска использования памяти
    uint32_t pct = (stats.used * 20) / stats.total;
    uart_puts("  [");
    uint32_t i;
    for (i = 0; i < 20; i++) {
        uart_putc(i < pct ? '#' : '.');
    }
    uart_puts("] ");
    uart_putdec((stats.used * 100) / stats.total);
    uart_puts("%\r\n");
    
    return 0;
}

static int cmd_uptime(int argc, char *argv[]) {
    (void)argc; (void)argv;
    
    uint64_t ms = kernel_get_uptime_ms();
    uint32_t sec = (uint32_t)(ms / 1000);
    
    uart_puts("Uptime: ");
    uart_putdec(sec / 3600); uart_puts("h ");
    uart_putdec((sec % 3600) / 60); uart_puts("m ");
    uart_putdec(sec % 60); uart_puts("s (");
    uart_putdec((uint32_t)ms); uart_puts(" ms)\r\n");
    
    return 0;
}

static int cmd_timer(int argc, char *argv[]) {
    (void)argc; (void)argv;
    
    uart_puts(C_BOLD "=== Timer & Scheduler ===\r\n" C_RESET);
    uart_puts("  Mode     : Interrupt-driven (ARM Generic Timer)\r\n");
    uart_puts("  Interval : ");
    uart_putdec(TIMER_INTERVAL_US);
    uart_puts(" us (");
    uart_putdec(1000000 / TIMER_INTERVAL_US);
    uart_puts(" Hz)\r\n");
    uart_puts("  Watchdog : ");
    uart_puts(C_GREEN "Active" C_RESET);
    uart_puts(" (prevents infinite loops)\r\n");
    uart_puts("  Ticks    : ");
    uart_putdec((uint32_t)g_uptime_ticks);
    uart_puts("\r\n");
    timer_print_stats();
    return 0;
}

static int cmd_echo(int argc, char *argv[]) {
    int i;
    for (i = 1; i < argc; i++) {
        uart_puts(argv[i]);
        if (i < argc - 1) uart_putc(' ');
    }
    uart_puts("\r\n");
    return 0;
}

static int cmd_hex(int argc, char *argv[]) {
    if (argc < 2) {
        uart_puts("Usage: hex <address> [count]\r\n");
        return 1;
    }
    
    // Парсим hex адрес (0x...)
    const char *p = argv[1];
    uint64_t addr = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    
    while (*p) {
        char c = *p++;
        uint8_t nibble = 0;
        if (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
        else break;
        addr = (addr << 4) | nibble;
    }
    
    int count = 4;  // По умолчанию 4 строки по 16 байт
    if (argc >= 3) {
        count = 0;
        for (p = argv[2]; *p >= '0' && *p <= '9'; p++) {
            count = count * 10 + (*p - '0');
        }
    }
    if (count > 32) count = 32;
    
    // Дамп памяти: по 16 байт в строке
    int row, col;
    for (row = 0; row < count; row++) {
        uart_puthex(addr + row * 16);
        uart_puts(": ");
        
        for (col = 0; col < 16; col++) {
            uint8_t byte = *(volatile uint8_t *)(addr + row * 16 + col);
            uart_putc("0123456789ABCDEF"[byte >> 4]);
            uart_putc("0123456789ABCDEF"[byte & 0xF]);
            uart_putc(' ');
        }
        
        uart_puts("  ");
        for (col = 0; col < 16; col++) {
            char c = *(volatile char *)(addr + row * 16 + col);
            uart_putc(c >= 32 && c < 127 ? c : '.');
        }
        uart_puts("\r\n");
    }
    
    return 0;
}

static int cmd_led(int argc, char *argv[]) {
    if (argc < 2) {
        uart_puts("Usage: led <on|off|blink>\r\n");
        return 1;
    }
    
    if (str_cmp(argv[1], "on") == 0) {
        gpio_led_on();
        uart_puts("LED: ON\r\n");
    } else if (str_cmp(argv[1], "off") == 0) {
        gpio_led_off();
        uart_puts("LED: OFF\r\n");
    } else if (str_cmp(argv[1], "blink") == 0) {
        uart_puts("Blinking 5 times...\r\n");
        gpio_led_blink(5);
    } else {
        uart_puts("Unknown led command\r\n");
        return 1;
    }
    return 0;
}

static int cmd_video(int argc, char *argv[]) {
    (void)argc; (void)argv;
    video_print_info();
    return 0;
}

static int cmd_play(int argc, char *argv[]) {
    (void)argc; (void)argv;
    uart_puts(C_CYAN "Starting Bad Apple playback...\r\n" C_RESET);
    uart_puts("(ASCII terminal animation - press any key to stop)\r\n\r\n");
    video_play_bad_apple();
    return 0;
}

static int cmd_history(int argc, char *argv[]) {
    (void)argc; (void)argv;
    
    if (history_count == 0) {
        uart_puts("No history.\r\n");
        return 0;
    }
    
    int i;
    for (i = history_count - 1; i >= 0; i--) {
        uart_puts("  ");
        uart_putdec(history_count - i);
        uart_puts("  ");
        uart_puts(history[i]);
        uart_puts("\r\n");
    }
    return 0;
}

static int cmd_reboot(int argc, char *argv[]) {
    (void)argc; (void)argv;
    uart_puts("Rebooting...\r\n");
    
    // На RPi4 можно перезапуститься через watchdog таймер
    // Адрес PM_WDOG (Power Management Watchdog)
    #define PM_BASE      0xFE100000UL
    #define PM_WDOG      (PM_BASE + 0x24)
    #define PM_RSTC      (PM_BASE + 0x1C)
    #define PM_PASSWORD  0x5A000000
    
    volatile uint32_t *pm_rstc = (volatile uint32_t *)PM_RSTC;
    volatile uint32_t *pm_wdog = (volatile uint32_t *)PM_WDOG;
    
    *pm_wdog = PM_PASSWORD | 1;             // Watchdog timeout = 1
    *pm_rstc = PM_PASSWORD | 0x00000020;    // RSTC_WRCFG_FULL_RESET
    
    // Ждём перезагрузки
    while (1) asm volatile("nop");
    return 0;
}

static int cmd_panic(int argc, char *argv[]) {
    (void)argc; (void)argv;
    kernel_panic("Manual panic triggered by user via shell");
    return 0;
}

// ---------------------------------------------------------------
// Таблица команд
// ---------------------------------------------------------------
static const shell_command_t commands[] = {
    { "help",    "Show help",               cmd_help    },
    { "clear",   "Clear screen",            cmd_clear   },
    { "info",    "System info",             cmd_info    },
    { "mem",     "Memory stats",            cmd_mem     },
    { "uptime",  "Show uptime",             cmd_uptime  },
    { "timer",   "Timer info",              cmd_timer   },
    { "echo",    "Print text",              cmd_echo    },
    { "hex",     "Memory dump",             cmd_hex     },
    { "led",     "LED control",             cmd_led     },
    { "video",   "Video info",              cmd_video   },
    { "play",    "Play Bad Apple",          cmd_play    },
    { "history", "Command history",         cmd_history },
    { "reboot",  "Reboot system",           cmd_reboot  },
    { "panic",   "Trigger kernel panic",    cmd_panic   },
    { NULL, NULL, NULL }    // Конец таблицы
};

// ---------------------------------------------------------------
// Поиск и выполнение команды
// ---------------------------------------------------------------
static int execute_command(char *cmdline) {
    char *argv[SHELL_MAX_ARGS];
    char cmdcopy[SHELL_MAX_CMD_LEN];
    
    str_copy(cmdcopy, cmdline, SHELL_MAX_CMD_LEN);
    
    int argc = parse_args(cmdcopy, argv, SHELL_MAX_ARGS);
    if (argc == 0) return 0;
    
    // Ищем команду в таблице
    const shell_command_t *cmd = commands;
    while (cmd->name != NULL) {
        if (str_cmp(argv[0], cmd->name) == 0) {
            return cmd->handler(argc, argv);
        }
        cmd++;
    }
    
    // Команда не найдена
    uart_puts(C_YELLOW "Unknown command: " C_RESET);
    uart_puts(argv[0]);
    uart_puts("  (type 'help' for list)\r\n");
    return 1;
}

// ---------------------------------------------------------------
// ГЛАВНАЯ ФУНКЦИЯ SHELL
// Бесконечный цикл чтения и выполнения команд
// ---------------------------------------------------------------
void shell_run(void) {
    char cmdline[SHELL_MAX_CMD_LEN];
    
    uart_puts(SHELL_PROMPT);
    
    while (1) {
        // Читаем команду от пользователя (с echo и backspace)
        uart_readline(cmdline, SHELL_MAX_CMD_LEN);
        
        // Сохраняем в истории
        history_add(cmdline);
        
        // Выполняем команду
        execute_command(cmdline);
        
        // Выводим приглашение снова
        uart_puts(SHELL_PROMPT);
    }
}
