/**
 * timer.c — Системный таймер, защита от зависаний, планировщик
 *
 * На RPi4/AArch64 доступны несколько таймеров:
 *   1. ARM Generic Timer (рекомендуемый)  ← Используем этот
 *   2. BCM2711 System Timer (устаревший)
 *   3. ARM Cortex Local Timer
 *
 * ARM Generic Timer:
 *   - Встроен в ядро ARM Cortex-A72
 *   - Регистры доступны через системные инструкции (MSR/MRS)
 *   - Тактируется от CNTFRQ_EL0 (обычно 54 МГц на RPi4)
 *   - Генерирует IRQ через GIC или локальный контроллер
 *
 * Важные регистры:
 *   CNTFRQ_EL0   — Частота таймера (Hz)
 *   CNTPCT_EL0   — Текущее значение счётчика (64 бит)
 *   CNTP_TVAL_EL0 — Значение до следующего прерывания
 *   CNTP_CTL_EL0  — Управление (Enable, Mask, ISTATUS)
 *
 * Для QEMU используется Generic Timer, который работает корректно.
 *
 * ЗАЩИТА ОТ БЕСКОНЕЧНЫХ ЦИКЛОВ (Watchdog):
 *   Таймер прерывает выполнение каждые TIMER_INTERVAL_US мкс.
 *   Если задача не «кормит» watchdog за WATCHDOG_TIMEOUT тиков,
 *   мы принудительно завершаем её (или выводим предупреждение).
 *
 * ПЛАНИРОВЩИК (Round-Robin):
 *   Простой алгоритм: каждой задаче выделяется TIME_SLICE тиков.
 *   После истечения кванта — переключение на следующую задачу.
 */

#include "../../include/timer.h"
#include "../../include/uart.h"
#include <stddef.h>
// ---------------------------------------------------------------
// Константы таймера
// ---------------------------------------------------------------
// CNTP_CTL_EL0 биты управления
#define CNTP_CTL_ENABLE     (1 << 0)    // Включить таймер
#define CNTP_CTL_IMASK      (1 << 1)    // Маскировать прерывание
#define CNTP_CTL_ISTATUS    (1 << 2)    // Статус: 1 = условие сработало

// ARM GIC (Generic Interrupt Controller) регистры
// На RPi4 прерывания управляются через локальный таймер ARM
#define ARM_LOCAL_BASE          0xFF800000UL
#define ARM_LOCAL_TIMER_CTL     (ARM_LOCAL_BASE + 0x08)
#define ARM_LOCAL_TIMER_IRQ     (ARM_LOCAL_BASE + 0x40)

// Для QEMU используем CNTP (физический таймер)
// Период прерываний: TIMER_INTERVAL_US = 10000 мкс = 10 мс = 100 Hz

// ---------------------------------------------------------------
// Структура задачи (упрощённая)
// ---------------------------------------------------------------
typedef enum {
    TASK_IDLE    = 0,   // Задача ожидает
    TASK_RUNNING = 1,   // Задача выполняется
    TASK_BLOCKED = 2,   // Задача заблокирована
} task_state_t;

typedef struct {
    uint32_t     id;            // Идентификатор задачи
    const char  *name;          // Имя задачи
    task_state_t state;         // Состояние
    uint32_t     time_slice;    // Оставшийся квант времени
    uint32_t     total_ticks;   // Всего использовано тиков
    uint32_t     watchdog;      // Счётчик watchdog (сбрасывается задачей)
} task_t;

// ---------------------------------------------------------------
// Глобальное состояние планировщика
// ---------------------------------------------------------------
#define MAX_TASKS           8
#define TIME_SLICE_TICKS    10          // Квант времени: 10 тиков (~100 мс)
#define WATCHDOG_TIMEOUT    500         // Таймаут watchdog: 500 тиков (~5 сек)

static task_t    tasks[MAX_TASKS];
static int       current_task  = 0;    // Индекс текущей задачи
static int       task_count    = 0;    // Количество задач
static uint32_t  timer_ticks   = 0;    // Локальный счётчик прерываний

// Статистика таймера
static uint32_t  scheduler_switches = 0;   // Количество переключений
static uint32_t  watchdog_events    = 0;   // Количество watchdog срабатываний

// Частота таймера (читается из CNTFRQ_EL0)
static uint64_t  timer_freq = 0;

// ---------------------------------------------------------------
// Вспомогательные функции для работы с ARM Generic Timer
// ---------------------------------------------------------------

// Читаем частоту таймера
static uint64_t read_cntfrq(void) {
    uint64_t freq;
    asm volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq;
}

// Читаем текущее значение счётчика
static uint64_t read_cntpct(void) {
    uint64_t count;
    asm volatile("mrs %0, cntpct_el0" : "=r"(count));
    return count;
}

// Устанавливаем значение до следующего прерывания (в тиках)
static void write_cntp_tval(uint32_t tval) {
    asm volatile("msr cntp_tval_el0, %0" :: "r"((uint64_t)tval));
}

// Управление таймером (включить/выключить, маскировать IRQ)
static void write_cntp_ctl(uint32_t ctl) {
    asm volatile("msr cntp_ctl_el0, %0" :: "r"((uint64_t)ctl));
}

// Для QEMU: разрешаем прерывание от физического таймера ядру
static void enable_cntp_irq(void) {
    uint64_t el;
    asm volatile("mrs %0, CurrentEL" : "=r"(el));
    el = (el >> 2) & 3;

    if (el == 2) {
        // Мы в EL2, теперь можно трогать cnthctl_el2
        uint64_t cnthctl;
        asm volatile("mrs %0, cnthctl_el2" : "=r"(cnthctl));
        cnthctl |= 3;
        asm volatile("msr cnthctl_el2, %0" :: "r"(cnthctl));
    }
    // Если мы в EL1, пропускаем — прошивка или эмулятор уже должны были дать нам права
}

// ---------------------------------------------------------------
// Функция: timer_init()
// Инициализирует ARM Generic Timer
// ---------------------------------------------------------------
void timer_init(void) {
    // Инициализируем структуры задач
    int i;
    for (i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_IDLE;
        tasks[i].watchdog = 0;
    }
    
    // Читаем частоту таймера (CNTFRQ_EL0)
    timer_freq = read_cntfrq();
    
    // Если QEMU не настроил частоту — используем стандартную для RPi4
    if (timer_freq == 0 || timer_freq > 1000000000UL) {
        timer_freq = 54000000;  // 54 МГц — стандарт для RPi4
    }
    
    // Вычисляем количество тиков для нашего интервала
    // ticks = freq * interval_us / 1000000
    uint32_t tval = (uint32_t)((timer_freq * TIMER_INTERVAL_US) / 1000000);
    
    // Разрешаем доступ к таймеру из EL1 (если запускаемся из EL2)
    enable_cntp_irq();
    
    // Устанавливаем первый интервал
    write_cntp_tval(tval);
    
    // Включаем таймер и разрешаем прерывание (ENABLE=1, IMASK=0)
    write_cntp_ctl(CNTP_CTL_ENABLE);
    
    // Добавляем "задачу ядра" как первую задачу
    tasks[0].id         = 0;
    tasks[0].name       = "kernel";
    tasks[0].state      = TASK_RUNNING;
    tasks[0].time_slice = TIME_SLICE_TICKS;
    tasks[0].total_ticks= 0;
    tasks[0].watchdog   = 0;
    task_count          = 1;
    current_task        = 0;
}

// ---------------------------------------------------------------
// Функция: timer_add_task()
// Регистрирует задачу в планировщике
// В реальной ОС здесь был бы полноценный процесс/поток
// ---------------------------------------------------------------
int timer_add_task(uint32_t id, const char *name) {
    if (task_count >= MAX_TASKS) return -1;
    
    tasks[task_count].id          = id;
    tasks[task_count].name        = name;
    tasks[task_count].state       = TASK_IDLE;
    tasks[task_count].time_slice  = TIME_SLICE_TICKS;
    tasks[task_count].total_ticks = 0;
    tasks[task_count].watchdog    = 0;
    
    return task_count++;
}

// ---------------------------------------------------------------
// Функция: timer_watchdog_kick()
// "Кормит" watchdog — сбрасывает счётчик текущей задачи
// Задача должна вызывать это регулярно, если выполняет длинную работу
// ---------------------------------------------------------------
void timer_watchdog_kick(void) {
    if (current_task < task_count) {
        tasks[current_task].watchdog = 0;
    }
}

// ---------------------------------------------------------------
// Функция: timer_handle_irq()
// Вызывается из обработчика IRQ в boot.S
// Здесь: обновление планировщика + watchdog проверки
// ---------------------------------------------------------------
void timer_handle_irq(void) {
    // Перезапускаем таймер для следующего прерывания
    uint32_t tval = (uint32_t)((timer_freq * TIMER_INTERVAL_US) / 1000000);
    write_cntp_tval(tval);
    
    timer_ticks++;
    
    // -----------------------------------------------------------
    // WATCHDOG: Проверяем, не завис ли текущий процесс
    // -----------------------------------------------------------
    if (current_task < task_count && tasks[current_task].state == TASK_RUNNING) {
        tasks[current_task].watchdog++;
        tasks[current_task].total_ticks++;
        
        if (tasks[current_task].watchdog >= WATCHDOG_TIMEOUT) {
            // Задача не отвечала WATCHDOG_TIMEOUT тиков подряд!
            watchdog_events++;
            tasks[current_task].watchdog = 0;
            
            // В реальной ОС здесь: принудительно завершаем задачу
            // В нашей реализации: просто логируем предупреждение
            // (uart_puts в прерывании — не идеально, но допустимо для отладки)
            uart_puts("\r\n[WATCHDOG] Warning: task '");
            uart_puts(tasks[current_task].name);
            uart_puts("' may be stuck!\r\n");
        }
    }
    
    // -----------------------------------------------------------
    // ПЛАНИРОВЩИК (Round-Robin): Уменьшаем квант времени
    // -----------------------------------------------------------
    if (task_count > 1 && current_task < task_count) {
        if (tasks[current_task].time_slice > 0) {
            tasks[current_task].time_slice--;
        }
        
        // Квант истёк — переключаемся на следующую задачу
        if (tasks[current_task].time_slice == 0) {
            tasks[current_task].time_slice = TIME_SLICE_TICKS;  // Восстанавливаем квант
            
            // Ищем следующую готовую задачу по кругу
            int next = (current_task + 1) % task_count;
            int checked = 0;
            
            while (checked < task_count) {
                if (tasks[next].state == TASK_RUNNING || 
                    tasks[next].state == TASK_IDLE) {
                    break;
                }
                next = (next + 1) % task_count;
                checked++;
            }
            
            if (next != current_task) {
                current_task = next;
                scheduler_switches++;
            }
            
            // В настоящей ОС здесь был бы context switch:
            // сохранение регистров текущей задачи и загрузка регистров новой
        }
    }
}

// ---------------------------------------------------------------
// Функция: timer_delay_ms(uint32_t ms)
// Задержка на указанное количество миллисекунд
// Использует счётчик CNTPCT_EL0 для точного измерения времени
// ---------------------------------------------------------------
void timer_delay_ms(uint32_t ms) {
    uint64_t target;
    uint64_t ticks_per_ms;
    
    if (timer_freq == 0) {
        // Fallback: простой цикл (неточный)
        volatile uint32_t i;
        for (i = 0; i < ms * 10000; i++) {
            asm volatile("nop");
        }
        return;
    }
    
    ticks_per_ms = timer_freq / 1000;
    target = read_cntpct() + (uint64_t)ms * ticks_per_ms;
    
    // Крутимся пока не достигнем цели
    // "Кормим" watchdog чтобы не получить ложное срабатывание
    while (read_cntpct() < target) {
        timer_watchdog_kick();
        asm volatile("nop");
    }
}

// ---------------------------------------------------------------
// Функция: timer_get_ticks()
// Возвращает количество тиков таймера с момента запуска
// ---------------------------------------------------------------
uint32_t timer_get_ticks(void) {
    return timer_ticks;
}

// ---------------------------------------------------------------
// Функция: timer_get_freq()
// Возвращает частоту таймера в Гц
// ---------------------------------------------------------------
uint64_t timer_get_freq(void) {
    return timer_freq;
}

// ---------------------------------------------------------------
// Функция: timer_print_stats()
// Выводит статистику таймера и планировщика через UART
// ---------------------------------------------------------------
void timer_print_stats(void) {
    uart_puts("  Freq       : ");
    uart_putdec((uint32_t)(timer_freq / 1000000));
    uart_puts(" MHz\r\n");
    uart_puts("  IRQ ticks  : ");
    uart_putdec(timer_ticks);
    uart_puts("\r\n");
    uart_puts("  Sched sw.  : ");
    uart_putdec(scheduler_switches);
    uart_puts("\r\n");
    uart_puts("  WD events  : ");
    uart_putdec(watchdog_events);
    uart_puts("\r\n");
    uart_puts("  Tasks      : ");
    uart_putdec(task_count);
    uart_puts("\r\n");
    
    // Выводим информацию о каждой задаче
    int i;
    for (i = 0; i < task_count; i++) {
        uart_puts("    [");
        uart_putdec(tasks[i].id);
        uart_puts("] ");
        uart_puts(tasks[i].name);
        uart_puts(" - ticks=");
        uart_putdec(tasks[i].total_ticks);
        uart_puts(i == current_task ? " *CURRENT*" : "");
        uart_puts("\r\n");
    }
}
