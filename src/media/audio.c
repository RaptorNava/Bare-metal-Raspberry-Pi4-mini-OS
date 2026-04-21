/**
 * audio.c — Драйвер аудио (заглушка + PWM пищалка)
 *
 * Полноценный аудио на RPi4 требует:
 *   - I2S интерфейс (для внешних DAC)
 *   - PWM через GPIO (для простых звуков)
 *   - HDMI аудио (через mailbox)
 *   - USB аудио (через драйвер USB)
 *
 * Наша реализация: PWM пищалка на GPIO 18 (PCM_CLK / ALT5 = PWM0)
 *
 * PWM (Pulse Width Modulation) на RPi4:
 *   Базовый адрес PWM: 0xFE20C000
 *   Регистры:
 *     CTL   (0x00): Control (каналы, режимы)
 *     STA   (0x04): Status
 *     DMAC  (0x08): DMA Configuration
 *     RNG1  (0x10): Channel 1 Range (длина периода)
 *     DAT1  (0x14): Channel 1 Data (длина импульса)
 *     FIF1  (0x18): FIFO input
 *     RNG2  (0x20): Channel 2 Range
 *     DAT2  (0x24): Channel 2 Data
 *
 * Частота PWM = Clock / Range
 * Для ноты A4 (440 Hz): Range = PWMclock / 440
 *
 * Тактирование PWM через Clock Manager:
 *   Базовый адрес CM: 0xFE101000
 *   CM_PWMCTL (0x0A0): Control
 *   CM_PWMDIV (0x0A4): Divisor
 *   Пароль для записи: 0x5A000000
 */

#include "../../include/audio.h"
#include "../../include/mmio.h"
#include "../../include/uart.h"
#include "../../include/gpio.h"
#include "../../include/timer.h"
#include <stddef.h>
// ---------------------------------------------------------------
// Базовые адреса
// ---------------------------------------------------------------
#define PERIPHERAL_BASE     0xFE000000UL
#define PWM_BASE            (PERIPHERAL_BASE + 0x20C000)
#define CM_BASE             (PERIPHERAL_BASE + 0x101000)

// Регистры PWM
#define PWM_CTL             (PWM_BASE + 0x00)
#define PWM_STA             (PWM_BASE + 0x04)
#define PWM_RNG1            (PWM_BASE + 0x10)
#define PWM_DAT1            (PWM_BASE + 0x14)
#define PWM_RNG2            (PWM_BASE + 0x20)
#define PWM_DAT2            (PWM_BASE + 0x24)

// Биты PWM_CTL
#define PWM_CTL_PWEN1       (1 << 0)    // Канал 1 включён
#define PWM_CTL_MODE1       (1 << 1)    // Serializer mode
#define PWM_CTL_RPTL1       (1 << 2)    // Repeat last data
#define PWM_CTL_MSEN1       (1 << 7)    // Use M/S algorithm

// Регистры Clock Manager для PWM
#define CM_PWMCTL           (CM_BASE + 0x0A0)
#define CM_PWMDIV           (CM_BASE + 0x0A4)
#define CM_PWM_PASSWORD     0x5A000000
#define CM_PWM_ENAB         (1 << 4)
#define CM_PWM_SRC_OSC      1           // Oscillator (19.2 MHz)

// GPIO 18 = PCM_CLK, ALT5 = PWM0
#define GPIO_18             18
#define GPIO_FUNC_ALT5      2

// Базовая частота осциллятора RPi4 = 19.2 МГц
#define OSC_FREQ_HZ         19200000

// ---------------------------------------------------------------
// Таблица нот (частоты в Гц)
// ---------------------------------------------------------------
typedef struct {
    const char *name;
    uint32_t    freq_hz;
} note_t;

static const note_t notes[] = {
    { "C4",  262 },
    { "D4",  294 },
    { "E4",  330 },
    { "F4",  349 },
    { "G4",  392 },
    { "A4",  440 },
    { "B4",  494 },
    { "C5",  523 },
    { "D5",  587 },
    { "E5",  659 },
    { "F5",  698 },
    { "G5",  784 },
    { "A5",  880 },
    { "B5",  988 },
    { "C6", 1047 },
    { NULL,    0 },
};

// Простая мелодия "Jingle Bells" (нотное имя + длительность в мс)
typedef struct {
    const char *note;
    uint32_t    duration_ms;
} melody_note_t;

static const melody_note_t jingle_bells[] = {
    {"E4", 200}, {"E4", 200}, {"E4", 400},
    {"E4", 200}, {"E4", 200}, {"E4", 400},
    {"E4", 200}, {"G4", 200}, {"C4", 300}, {"D4", 100}, {"E4", 600},
    {"F4", 200}, {"F4", 200}, {"F4", 200}, {"F4", 200},
    {"F4", 200}, {"E4", 200}, {"E4", 200}, {"E4", 100}, {"E4", 100},
    {"E4", 200}, {"D4", 200}, {"D4", 200}, {"E4", 200}, {"D4", 400}, {"G4", 400},
    {NULL, 0}
};

// ---------------------------------------------------------------
// Состояние аудиосистемы
// ---------------------------------------------------------------
static int audio_initialized = 0;
static int audio_muted = 0;

// ---------------------------------------------------------------
// Функция: audio_set_pwm_clock(uint32_t divisor)
// Настраивает тактирование PWM через Clock Manager
// Делитель вычисляется как: divisor = OSC_FREQ / (desired_freq * range)
// ---------------------------------------------------------------
static void audio_set_pwm_clock(uint32_t divisor) {
    // Шаг 1: Останавливаем PWM
    mmio_write(PWM_CTL, 0);
    
    // Шаг 2: Останавливаем тактирование PWM
    mmio_write(CM_PWMCTL, CM_PWM_PASSWORD | 0x01);  // KILL бит
    
    // Ждём пока часы остановятся
    volatile int i;
    for (i = 0; i < 100; i++) asm volatile("nop");
    
    // Шаг 3: Устанавливаем делитель
    // Формат PWMDIV: биты [23:12] = DIVI (целая часть), биты [11:0] = DIVF (дробная)
    mmio_write(CM_PWMDIV, CM_PWM_PASSWORD | (divisor << 12));
    
    // Шаг 4: Запускаем тактирование от осциллятора
    mmio_write(CM_PWMCTL, CM_PWM_PASSWORD | CM_PWM_SRC_OSC | CM_PWM_ENAB);
    
    // Ждём запуска
    for (i = 0; i < 1000; i++) asm volatile("nop");
}

// ---------------------------------------------------------------
// Функция: audio_init()
// Инициализирует PWM аудио подсистему
// ---------------------------------------------------------------
void audio_init(void) {
    // Настраиваем GPIO 18 в режим ALT5 (PWM0)
    gpio_set_function(GPIO_18, GPIO_FUNC_ALT5);
    
    // Настраиваем тактирование PWM
    // Делитель = 1 (максимальная точность, частота ~19.2 МГц)
    audio_set_pwm_clock(1);
    
    // Настраиваем PWM канал 1 в M/S режим
    // M/S (Mark-Space): DAT1/RNG1 = скважность
    mmio_write(PWM_CTL, PWM_CTL_MSEN1 | PWM_CTL_PWEN1);
    
    audio_initialized = 1;
    audio_muted = 0;
}

// ---------------------------------------------------------------
// Функция: audio_play_tone(freq_hz, duration_ms)
// Воспроизводит тон указанной частоты
// freq_hz: частота в Гц (0 = пауза/тишина)
// ---------------------------------------------------------------
void audio_play_tone(uint32_t freq_hz, uint32_t duration_ms) {
    if (!audio_initialized || audio_muted) {
        // Аудио не инициализировано — просто делаем паузу
        timer_delay_ms(duration_ms);
        return;
    }
    
    if (freq_hz == 0) {
        // Пауза — выключаем PWM
        mmio_write(PWM_CTL, 0);
        timer_delay_ms(duration_ms);
        mmio_write(PWM_CTL, PWM_CTL_MSEN1 | PWM_CTL_PWEN1);
        return;
    }
    
    // Вычисляем Range (количество тиков на период)
    // Range = OSC_FREQ / freq_hz
    uint32_t range = OSC_FREQ_HZ / freq_hz;
    if (range == 0) range = 1;
    
    // Data = Range / 2 (скважность 50% = прямоугольный сигнал)
    uint32_t data = range / 2;
    
    // Устанавливаем параметры PWM
    mmio_write(PWM_RNG1, range);
    mmio_write(PWM_DAT1, data);
    
    // Включаем канал
    mmio_write(PWM_CTL, PWM_CTL_MSEN1 | PWM_CTL_PWEN1);
    
    // Ждём заданное время
    timer_delay_ms(duration_ms);
    
    // Выключаем после паузы между нотами
    mmio_write(PWM_DAT1, 0);
}

// ---------------------------------------------------------------
// Функция: audio_find_note_freq(name)
// Ищет частоту ноты по имени
// ---------------------------------------------------------------
static uint32_t audio_find_note_freq(const char *name) {
    const note_t *n = notes;
    while (n->name != NULL) {
        // Сравниваем имена нот
        int i = 0;
        while (n->name[i] && name[i] && n->name[i] == name[i]) i++;
        if (!n->name[i] && !name[i]) return n->freq_hz;
        n++;
    }
    return 0;  // Нота не найдена
}

// ---------------------------------------------------------------
// Функция: audio_play_melody()
// Воспроизводит встроенную мелодию Jingle Bells
// В QEMU: вывод нот в UART (аудио не работает без звуковой карты)
// ---------------------------------------------------------------
void audio_play_melody(void) {
    uart_puts("♪ Playing melody (PWM audio)...\r\n");
    uart_puts("  Note sequence: ");
    
    const melody_note_t *note = jingle_bells;
    int count = 0;
    
    while (note->note != NULL && count < 30) {
        uint32_t freq = audio_find_note_freq(note->note);
        
        // Выводим ноту в UART для визуализации
        uart_puts(note->note);
        uart_putc(' ');
        
        // Воспроизводим (или имитируем)
        audio_play_tone(freq, note->duration_ms);
        
        // Небольшая пауза между нотами
        audio_play_tone(0, 30);
        
        note++;
        count++;
    }
    
    uart_puts("\r\n♪ Done!\r\n");
}

// ---------------------------------------------------------------
// Функция: audio_beep()
// Короткий звуковой сигнал (beep)
// Используется для уведомлений системы
// ---------------------------------------------------------------
void audio_beep(void) {
    audio_play_tone(880, 100);  // 100 мс ноты A5
}

// ---------------------------------------------------------------
// Функция: audio_mute() / audio_unmute()
// ---------------------------------------------------------------
void audio_mute(void) {
    audio_muted = 1;
    if (audio_initialized) {
        mmio_write(PWM_CTL, 0);
    }
}

void audio_unmute(void) {
    audio_muted = 0;
    if (audio_initialized) {
        mmio_write(PWM_CTL, PWM_CTL_MSEN1 | PWM_CTL_PWEN1);
    }
}

// ---------------------------------------------------------------
// Функция: audio_print_info()
// Выводит информацию об аудиосистеме
// ---------------------------------------------------------------
void audio_print_info(void) {
    uart_puts("\033[1m=== Audio Subsystem ===\033[0m\r\n");
    uart_puts("  Mode     : PWM (GPIO 18, ALT5)\r\n");
    uart_puts("  Status   : ");
    
    if (!audio_initialized) {
        uart_puts("\033[33mNot initialized\033[0m\r\n");
    } else if (audio_muted) {
        uart_puts("\033[33mMuted\033[0m\r\n");
    } else {
        uart_puts("\033[32mActive\033[0m\r\n");
    }
    
    uart_puts("  Clock    : ");
    uart_putdec(OSC_FREQ_HZ / 1000000);
    uart_puts(" MHz (oscillator)\r\n");
    uart_puts("  Note A4  : 440 Hz, Range=");
    uart_putdec(OSC_FREQ_HZ / 440);
    uart_puts("\r\n");
    uart_puts("  Note A5  : 880 Hz, Range=");
    uart_putdec(OSC_FREQ_HZ / 880);
    uart_puts("\r\n");
    uart_puts("  Note C4  : 262 Hz (middle C)\r\n");
    uart_puts("  Notes available: C4-B5 (24 notes)\r\n");
}
