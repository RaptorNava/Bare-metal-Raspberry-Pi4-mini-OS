/**
 * kernel.c — Главный файл ядра операционной системы
 *
 * Это сердце ОС. После инициализации в boot.S управление
 * передаётся сюда, в функцию kernel_main().
 *
 * Порядок инициализации:
 *   1. UART — чтобы сразу видеть вывод
 *   2. GPIO — настройка пинов
 *   3. Таймер — для прерываний и планировщика
 *   4. Память — инициализация кучи
 *   5. Видео — если доступен framebuffer
 *   6. Shell — интерактивный терминал
 *
 * Подсистемы ОС:
 *   - Монолитное ядро (всё в одном адресном пространстве)
 *   - Кооперативная многозадачность через планировщик таймера
 *   - Базовое управление памятью (bump allocator)
 *   - Видеовывод через framebuffer
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

// ---------------------------------------------------------------
// Версия ОС
// ---------------------------------------------------------------
#define OS_NAME     "RaspberryOS Mini"
#define OS_VERSION  "0.1.0"
#define OS_AUTHOR   "QEMU Test Build"

// ---------------------------------------------------------------
// Глобальные флаги состояния системы
// ---------------------------------------------------------------
volatile int g_kernel_panic = 0;        // Флаг критической ошибки
volatile uint64_t g_uptime_ticks = 0;  // Счётчик тиков с момента запуска
volatile int g_video_available = 0;    // Доступен ли framebuffer

// ---------------------------------------------------------------
// Прообраз таблицы системных вызовов (расширяется позже)
// ---------------------------------------------------------------
typedef struct {
    const char *name;
    void (*handler)(void);
} syscall_entry_t;

// ---------------------------------------------------------------
// Вспомогательная функция: вывод цветной строки
// Используем ANSI escape-коды (работают в большинстве терминалов)
// ---------------------------------------------------------------
static void print_colored(const char *color, const char *msg) {
    uart_puts(color);   // Код цвета (ANSI)
    uart_puts(msg);
    uart_puts("\033[0m");  // Сброс цвета
}

// ANSI цветовые коды
#define COLOR_RED     "\033[31m"
#define COLOR_GREEN   "\033[32m"
#define COLOR_YELLOW  "\033[33m"
#define COLOR_BLUE    "\033[34m"
#define COLOR_CYAN    "\033[36m"
#define COLOR_BOLD    "\033[1m"
#define COLOR_RESET   "\033[0m"

// ---------------------------------------------------------------
// Вывод баннера при загрузке
// ---------------------------------------------------------------
static void print_banner(void) {
    uart_puts("\r\n");
    uart_puts(COLOR_CYAN);
    uart_puts("  ____  ____  _   _ ___  ___ \r\n");
    uart_puts(" |  _ \\|  _ \\(_) | |  \\/  | |\r\n");
    uart_puts(" | |_) | |_) | | | | |\\/| | |\r\n");
    uart_puts(" |  _ <|  __/| | | | |  | | |\r\n");
    uart_puts(" |_| \\_\\_|   |_| |_|_|  |_|_|\r\n");
    uart_puts(COLOR_RESET);
    uart_puts(COLOR_BOLD);
    uart_puts("  RaspberryOS Mini v0.1\r\n");
    uart_puts(COLOR_RESET);
    uart_puts("  Bare-metal OS for Raspberry Pi 4\r\n");
    uart_puts("  Running on QEMU (aarch64-virt or raspi4b)\r\n");
    uart_puts("\r\n");
    uart_puts("  ----------------------------------------\r\n");
}

// ---------------------------------------------------------------
// Вывод информации о системе
// ---------------------------------------------------------------
static void print_sysinfo(void) {
    uint32_t board_rev;
    uint32_t arm_base, arm_size;
    uint32_t uart_clock;
    
    uart_puts(COLOR_GREEN);
    uart_puts("[BOOT] ");
    uart_puts(COLOR_RESET);
    uart_puts("System information:\r\n");
    
    // Получаем информацию через Mailbox
    board_rev = mbox_get_board_revision();
    uart_puts("  Board revision : 0x");
    uart_puthex(board_rev);
    uart_puts("\r\n");
    
    if (mbox_get_arm_memory(&arm_base, &arm_size)) {
        uart_puts("  ARM memory     : ");
        uart_putdec(arm_size / (1024 * 1024));
        uart_puts(" MB at 0x");
        uart_puthex(arm_base);
        uart_puts("\r\n");
    }
    
    uart_clock = mbox_get_clock_rate(2);  // UART clock
    if (uart_clock) {
        uart_puts("  UART clock     : ");
        uart_putdec(uart_clock / 1000000);
        uart_puts(" MHz\r\n");
    }
    
    // Информация о таймере
    uart_puts("  Timer interval : ");
    uart_putdec(TIMER_INTERVAL_US / 1000);
    uart_puts(" ms\r\n");
    
    uart_puts("\r\n");
}

// ---------------------------------------------------------------
// Функция: kernel_panic(const char *msg)
// Вызывается при фатальной ошибке
// Выводит сообщение об ошибке и останавливает систему
// ---------------------------------------------------------------
void kernel_panic(const char *msg) {
    // Запрещаем все прерывания
    asm volatile("msr daifset, #0xF");  // Маскируем D, A, I, F
    
    g_kernel_panic = 1;
    
    uart_puts("\r\n");
    uart_puts(COLOR_RED);
    uart_puts("╔══════════════════════════════════════╗\r\n");
    uart_puts("║          KERNEL PANIC                ║\r\n");
    uart_puts("╚══════════════════════════════════════╝\r\n");
    uart_puts(COLOR_RESET);
    uart_puts("PANIC: ");
    uart_puts(msg);
    uart_puts("\r\n");
    uart_puts("System halted. Reset required.\r\n");
    
    // Мигаем LED — быстрое мигание означает критическую ошибку
    gpio_led_blink(10);
    
    // Останавливаем систему
    while (1) {
        asm volatile("wfe");
    }
}

// ---------------------------------------------------------------
// Функция: handle_irq()
// Вызывается из boot.S при IRQ прерывании
// Перенаправляет обработку к таймеру и планировщику
// ---------------------------------------------------------------
void handle_irq(void) {
    // Увеличиваем счётчик тиков
    g_uptime_ticks++;
    
    // Обрабатываем таймерное прерывание
    timer_handle_irq();
    
    // Здесь можно добавить обработку других IRQ:
    // - UART RX (получение данных)
    // - GPIO события
    // - USB
}

// ---------------------------------------------------------------
// Функция: kernel_get_uptime_ms()
// Возвращает время работы системы в миллисекундах
// ---------------------------------------------------------------
uint64_t kernel_get_uptime_ms(void) {
    // TIMER_INTERVAL_US — интервал таймера в микросекундах
    return (g_uptime_ticks * TIMER_INTERVAL_US) / 1000;
}

// ---------------------------------------------------------------
// Инициализация подсистем ОС (с диагностическим выводом)
// ---------------------------------------------------------------
static void subsystem_ok(const char *name) {
    print_colored(COLOR_GREEN, "[  OK  ] ");
    uart_puts(name);
    uart_puts("\r\n");
}

static void subsystem_fail(const char *name, const char *reason) {
    print_colored(COLOR_YELLOW, "[ WARN ] ");
    uart_puts(name);
    uart_puts(": ");
    uart_puts(reason);
    uart_puts("\r\n");
}

// ---------------------------------------------------------------
// ГЛАВНАЯ ФУНКЦИЯ ЯДРА
// Сюда попадаем из boot.S после базовой настройки процессора
// ---------------------------------------------------------------
void kernel_main(void) {
    // =============================================================
    // ШАГ 1: UART — должен быть первым, чтобы видеть остальные сообщения
    // =============================================================
    uart_init();
    
    // Очищаем экран и выводим баннер
    uart_puts("\033[2J\033[H");     // ANSI: очистить экран, курсор в начало
    print_banner();
    
    // =============================================================
    // ШАГ 2: GPIO
    // =============================================================
    gpio_init();
    gpio_led_on();      // LED горит во время инициализации
    subsystem_ok("GPIO driver initialized");
    
    // =============================================================
    // ШАГ 3: Системный таймер + прерывания
    // =============================================================
    // Настраиваем векторную таблицу прерываний
    // Регистр VBAR_EL1 указывает на нашу таблицу из boot.S
    extern void vectors(); 
    asm volatile("msr vbar_el1, %0" :: "r"((uint64_t)vectors) : "memory");
    
    // Инициализируем таймер (настраивает регистры и включает прерывания)
    timer_init();
    
    // Разрешаем IRQ прерывания (сбрасываем I-бит в DAIF)
    //asm volatile("msr daifclr, #2");    // Бит 2 = I (IRQ)
    
    subsystem_ok("Timer initialized (watchdog + scheduler)");
    
    // =============================================================
    // ШАГ 4: Система памяти
    // =============================================================
    memory_init();
    subsystem_ok("Memory manager initialized");
    
    // =============================================================
    // ШАГ 5: Получаем информацию о системе
    // =============================================================
    print_sysinfo();
    
    // =============================================================
    // ШАГ 6: Инициализация видео (framebuffer)
    // =============================================================
    if (video_init() == 0) {
        g_video_available = 1;
        subsystem_ok("Video framebuffer initialized (1024x768)");
    } else {
        g_video_available = 0;
        subsystem_fail("Video", "framebuffer not available in QEMU virt mode");
    }
    
    // =============================================================
    // ШАГ 7: Всё готово
    // =============================================================
    gpio_led_off();     // Выключаем LED — инициализация завершена
    
    uart_puts(COLOR_GREEN);
    uart_puts("\r\n[BOOT] Kernel initialized successfully!\r\n");
    uart_puts(COLOR_RESET);
    uart_puts("[BOOT] Type 'help' for available commands.\r\n\r\n");
    
    // =============================================================
    // ШАГ 8: Запуск интерактивного командного интерпретатора
    // shell_run() — бесконечный цикл, обрабатывает команды пользователя
    // =============================================================
    shell_run();
    
    // =============================================================
    // Сюда попасть не должны
    // =============================================================
    kernel_panic("shell_run() returned unexpectedly");
}
