/**
 * kernel.c — Главный файл ядра
 *
 * v0.2: Добавлена инициализация USB HID клавиатуры.
 *       Новый глобальный флаг: g_usb_available.
 *       USB инициализируется после video, перед shell.
 */

#include <stddef.h>
#include "../../include/uart.h"
#include "../../include/gpio.h"
#include "../../include/timer.h"
#include "../../include/memory.h"
#include "../../include/shell.h"
#include "../../include/mailbox.h"
#include "../../include/printf.h"
#include "../../include/video.h"
#include "../../include/console_mux.h"
#include "../../include/usb.h"          /* <<<< ДОБАВЛЕНО */

#define OS_NAME     "RaspberryOS Mini"
#define OS_VERSION  "0.2.0"

volatile int      g_kernel_panic    = 0;
volatile uint64_t g_uptime_ticks    = 0;
volatile int      g_video_available = 0;
volatile int      g_usb_available   = 0;   /* <<<< ДОБАВЛЕНО */

/* ----------------------------------------------------------------
 * ANSI цвета для UART
 * ---------------------------------------------------------------- */
#define COLOR_RED_A    "\033[31m"
#define COLOR_GREEN_A  "\033[32m"
#define COLOR_YELLOW_A "\033[33m"
#define COLOR_CYAN_A   "\033[36m"
#define COLOR_BOLD_A   "\033[1m"
#define COLOR_RESET_A  "\033[0m"

/* ----------------------------------------------------------------
 * Вывод: UART + HDMI (если доступен)
 * ---------------------------------------------------------------- */
static void kputs(const char *s) {
    uart_puts(s);
    if (g_video_available) console_puts(s);
}

/* ----------------------------------------------------------------
 * Баннер
 * ---------------------------------------------------------------- */
static void print_banner(void) {
    uart_puts("\r\n");
    uart_puts(COLOR_CYAN_A);
    uart_puts("  ____  ____  _   _ ___  ___ \r\n");
    uart_puts(" |  _ \\|  _ \\(_) | |  \\/  | |\r\n");
    uart_puts(" | |_) | |_) | | | | |\\/| | |\r\n");
    uart_puts(" |  _ <|  __/| | | | |  | | |\r\n");
    uart_puts(" |_| \\_\\_|   |_| |_|_|  |_|_|\r\n");
    uart_puts(COLOR_RESET_A);
    uart_puts(COLOR_BOLD_A);
    uart_puts("  RaspberryOS Mini v0.2\r\n");
    uart_puts(COLOR_RESET_A);
    uart_puts("  Bare-metal OS for Raspberry Pi 4\r\n");
    uart_puts("  ----------------------------------------\r\n\r\n");
}

/* ----------------------------------------------------------------
 * Вывод на HDMI-консоль без ANSI (framebuffer не понимает все коды)
 * ---------------------------------------------------------------- */
static void print_banner_hdmi(void) {
    console_set_color(COLOR_CYAN, COLOR_BLACK);
    console_puts("  ____  ____  _   _ ___  ___ \r\n");
    console_puts(" |  _ \\|  _ \\(_) | |  \\/  | |\r\n");
    console_puts(" | |_) | |_) | | | | |\\/| | |\r\n");
    console_puts(" |  _ <|  __/| | | | |  | | |\r\n");
    console_puts(" |_| \\_\\_|   |_| |_|_|  |_|_|\r\n");
    console_set_color(COLOR_WHITE, COLOR_BLACK);
    console_puts("  RaspberryOS Mini v0.2\r\n");
    console_puts("  Bare-metal OS for Raspberry Pi 4\r\n");
    console_puts("  ----------------------------------------\r\n\r\n");
}

/* ----------------------------------------------------------------
 * print_sysinfo
 * ---------------------------------------------------------------- */
static void print_sysinfo(void) {
    uint32_t board_rev;
    uint32_t arm_base = 0, arm_size = 0;
    uint32_t uart_clock;

    kputs("[BOOT] System information:\r\n");

    board_rev = mbox_get_board_revision();
    uart_puts("  Board revision : 0x"); uart_puthex(board_rev); uart_puts("\r\n");
    if (g_video_available) {
        console_puts("  Board revision : 0x");
        char hbuf[20];
        int hi = 18; hbuf[18] = '\0';
        uint64_t v = board_rev;
        hbuf[0] = '0'; hbuf[1] = 'x';
        for (; hi >= 2; hi--) {
            hbuf[hi] = "0123456789ABCDEF"[v & 0xF]; v >>= 4;
        }
        console_puts(hbuf); console_puts("\r\n");
    }

    if (mbox_get_arm_memory(&arm_base, &arm_size)) {
        uart_puts("  ARM memory     : ");
        uart_putdec(arm_size / (1024 * 1024));
        uart_puts(" MB\r\n");
        if (g_video_available) {
            console_puts("  ARM memory     : ");
            uint32_t mb = arm_size / (1024 * 1024);
            char dbuf[12]; int di = 10; dbuf[10] = '\0';
            if (mb == 0) { dbuf[--di] = '0'; }
            else { while (mb > 0) { dbuf[--di] = '0' + (mb % 10); mb /= 10; } }
            console_puts(&dbuf[di]); console_puts(" MB\r\n");
        }
    }

    uart_clock = mbox_get_clock_rate(2);
    if (uart_clock) {
        uart_puts("  UART clock     : ");
        uart_putdec(uart_clock / 1000000);
        uart_puts(" MHz\r\n");
    }

    uart_puts("  Timer interval : ");
    uart_putdec(TIMER_INTERVAL_US / 1000);
    uart_puts(" ms\r\n\r\n");
    if (g_video_available) {
        console_puts("  UART + HDMI output active\r\n\r\n");
    }
}

/* ----------------------------------------------------------------
 * kernel_panic
 * ---------------------------------------------------------------- */
void kernel_panic(const char *msg) {
    asm volatile("msr daifset, #0xF");
    g_kernel_panic = 1;

    uart_puts("\r\n" COLOR_RED_A);
    uart_puts("╔══════════════════════════════════════╗\r\n");
    uart_puts("║          KERNEL PANIC                ║\r\n");
    uart_puts("╚══════════════════════════════════════╝\r\n");
    uart_puts(COLOR_RESET_A "PANIC: ");
    uart_puts(msg);
    uart_puts("\r\nSystem halted.\r\n");

    if (g_video_available) {
        console_set_color(COLOR_RED, COLOR_BLACK);
        console_puts("\r\n*** KERNEL PANIC ***\r\n");
        console_puts(msg);
        console_puts("\r\nSystem halted.\r\n");
        console_set_color(COLOR_WHITE, COLOR_BLACK);
    }

    gpio_led_blink(10);
    while (1) asm volatile("wfe");
}

/* ----------------------------------------------------------------
 * handle_irq
 * ---------------------------------------------------------------- */
void handle_irq(void) {
    g_uptime_ticks++;
    timer_handle_irq();
}

/* ----------------------------------------------------------------
 * kernel_get_uptime_ms
 * ---------------------------------------------------------------- */
uint64_t kernel_get_uptime_ms(void) {
    return (g_uptime_ticks * TIMER_INTERVAL_US) / 1000;
}

/* ----------------------------------------------------------------
 * Вспомогательные статусные сообщения
 * ---------------------------------------------------------------- */
static void subsystem_ok(const char *name) {
    uart_puts(COLOR_GREEN_A "[  OK  ] " COLOR_RESET_A);
    uart_puts(name); uart_puts("\r\n");
    if (g_video_available) {
        console_set_color(COLOR_GREEN, COLOR_BLACK);
        console_puts("[  OK  ] ");
        console_set_color(COLOR_WHITE, COLOR_BLACK);
        console_puts(name); console_puts("\r\n");
    }
}

static void subsystem_warn(const char *name, const char *reason) {
    uart_puts(COLOR_YELLOW_A "[ WARN ] " COLOR_RESET_A);
    uart_puts(name); uart_puts(": "); uart_puts(reason); uart_puts("\r\n");
    if (g_video_available) {
        console_set_color(COLOR_YELLOW, COLOR_BLACK);
        console_puts("[ WARN ] ");
        console_set_color(COLOR_WHITE, COLOR_BLACK);
        console_puts(name); console_puts(": "); console_puts(reason);
        console_puts("\r\n");
    }
}

/* ================================================================
 * ГЛАВНАЯ ФУНКЦИЯ ЯДРА
 * ================================================================ */
void kernel_main(void) {

    /* 1. UART — первым, чтобы видеть отладку */
    uart_init();
    uart_puts("\033[2J\033[H");
    print_banner();

    /* 2. GPIO */
    gpio_init();
    gpio_led_on();
    uart_puts(COLOR_GREEN_A "[  OK  ] " COLOR_RESET_A "GPIO initialized\r\n");

    /* 3. Таймер */
    extern void vectors(void);
    asm volatile("msr vbar_el1, %0" :: "r"((uint64_t)vectors) : "memory");
    timer_init();
    uart_puts(COLOR_GREEN_A "[  OK  ] " COLOR_RESET_A "Timer initialized\r\n");

    /* 4. Память */
    memory_init();
    uart_puts(COLOR_GREEN_A "[  OK  ] " COLOR_RESET_A "Memory manager initialized\r\n");

    /* 5. Видео */
    uart_puts("[ ... ] Initializing framebuffer (HDMI)...\r\n");
    if (video_init() == 0) {
        g_video_available = 1;

        print_banner_hdmi();

        console_set_color(COLOR_GREEN, COLOR_BLACK);
        console_puts("[  OK  ] ");
        console_set_color(COLOR_WHITE, COLOR_BLACK);
        console_puts("GPIO initialized\r\n");

        console_set_color(COLOR_GREEN, COLOR_BLACK);
        console_puts("[  OK  ] ");
        console_set_color(COLOR_WHITE, COLOR_BLACK);
        console_puts("Timer initialized\r\n");

        console_set_color(COLOR_GREEN, COLOR_BLACK);
        console_puts("[  OK  ] ");
        console_set_color(COLOR_WHITE, COLOR_BLACK);
        console_puts("Memory initialized\r\n");

        subsystem_ok("Video framebuffer initialized");
        uart_puts(COLOR_GREEN_A "[  OK  ] " COLOR_RESET_A "Video framebuffer initialized\r\n");
    } else {
        g_video_available = 0;
        subsystem_warn("Video", "framebuffer unavailable — UART only mode");
    }

    /* 6. Системная информация */
    print_sysinfo();

    /* ================================================================
     * 7. USB HID клавиатура                         <<<< ДОБАВЛЕНО
     *
     * Инициализируем после video чтобы статус был виден на HDMI.
     * usb_init() ждёт подключения до 3 секунд — это нормально,
     * пользователь видит сообщение "[ ... ] Waiting for USB...".
     * Если клавиатура не подключена — продолжаем без неё (WARN).
     * ================================================================ */
    uart_puts("[ ... ] Initializing USB host (DWC2 OTG)...\r\n");
    if (g_video_available) console_puts("[ ... ] Initializing USB host...\r\n");

    if (usb_init() == 0) {
        g_usb_available = 1;
        subsystem_ok("USB HID keyboard ready");
    } else {
        g_usb_available = 0;
        subsystem_warn("USB", "no keyboard detected — connect to lower USB 2.0 port");
    }
    /* ================================================================
     * КОНЕЦ ДОБАВЛЕНИЯ
     * ================================================================ */

    /* 8. Готово */
    gpio_led_off();

    kputs(COLOR_GREEN_A "\r\n[BOOT] Kernel initialized successfully!\r\n" COLOR_RESET_A);
    kputs("[BOOT] Type 'help' for available commands.\r\n");

    /* Подсказка про клавиатуру */
    if (g_usb_available) {
        kputs("[BOOT] USB keyboard active — type directly!\r\n");
    }
    kputs("\r\n");

    /* Prompt на HDMI */
    if (g_video_available) {
        console_set_color(COLOR_CYAN, COLOR_BLACK);
        console_puts("RPiOS");
        console_set_color(COLOR_YELLOW, COLOR_BLACK);
        console_puts("> ");
        console_set_color(COLOR_WHITE, COLOR_BLACK);
    }

    /* 9. Shell */
    shell_run();

    kernel_panic("shell_run() returned unexpectedly");
}
