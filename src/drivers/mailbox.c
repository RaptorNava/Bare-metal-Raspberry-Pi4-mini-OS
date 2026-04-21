/**
 * mailbox.c — Драйвер почтового ящика (Mailbox) VideoCore
 *
 * На Raspberry Pi CPU (ARM) и GPU (VideoCore) — два отдельных процессора.
 * Они общаются через механизм "Mailbox" (почтовый ящик).
 *
 * Mailbox позволяет:
 *   - Получить разрешение экрана и framebuffer
 *   - Узнать тактовую частоту CPU/GPU
 *   - Управлять питанием устройств
 *   - Включать/выключать LED через firmware
 *   - Получить серийный номер платы
 *
 * Базовый адрес: PERIPHERAL_BASE + 0xB880
 *
 * Регистры:
 *   MBOX_READ   (0x00): Чтение сообщения
 *   MBOX_STATUS (0x18): Статус (полный/пустой)
 *   MBOX_WRITE  (0x20): Запись сообщения
 *
 * Каналы Mailbox:
 *   Канал 8 = ARM to VC (запросы от CPU к GPU)
 *   Канал 9 = Framebuffer
 *
 * Формат сообщения:
 *   Сообщение — это массив uint32_t в памяти, выровненный на 16 байт.
 *   Структура:
 *     [0] = Общий размер буфера в байтах
 *     [1] = Код запроса (0 = запрос, 0x80000000 = успех)
 *     [2] = Тег (что мы хотим узнать)
 *     [3] = Размер буфера значения
 *     [4] = Размер запроса (0)
 *     [5..N] = Данные
 *     [конец] = 0 (END TAG)
 */

#include "../../include/mailbox.h"
#include "../../include/mmio.h"
#include <stddef.h>
// ---------------------------------------------------------------
// Базовые адреса Mailbox
// ---------------------------------------------------------------
#define PERIPHERAL_BASE     0xFE000000UL
#define MBOX_BASE           (PERIPHERAL_BASE + 0xB880)

#define MBOX_READ           (MBOX_BASE + 0x00)
#define MBOX_POLL           (MBOX_BASE + 0x10)
#define MBOX_SENDER         (MBOX_BASE + 0x14)
#define MBOX_STATUS         (MBOX_BASE + 0x18)
#define MBOX_CONFIG         (MBOX_BASE + 0x1C)
#define MBOX_WRITE          (MBOX_BASE + 0x20)

// Биты статусного регистра
#define MBOX_FULL           (1 << 31)   // Ящик для записи полный
#define MBOX_EMPTY          (1 << 30)   // Ящик для чтения пустой

// Каналы
#define MBOX_CHANNEL_PROP   8           // Канал свойств (Property Channel)
#define MBOX_CHANNEL_FB     1           // Канал фреймбуфера

// Коды запросов/ответов
#define MBOX_REQUEST        0x00000000  // Это запрос
#define MBOX_RESPONSE_OK    0x80000000  // Ответ: успех
#define MBOX_RESPONSE_ERR   0x80000001  // Ответ: ошибка

// Теги Property Channel
#define MBOX_TAG_GET_BOARD_REVISION     0x00010002
#define MBOX_TAG_GET_BOARD_SERIAL       0x00010004
#define MBOX_TAG_GET_ARM_MEMORY         0x00010005
#define MBOX_TAG_GET_VC_MEMORY          0x00010006
#define MBOX_TAG_GET_CLOCK_RATE         0x00030002
#define MBOX_TAG_SET_CLOCK_RATE         0x00038002
#define MBOX_TAG_GET_TEMPERATURE        0x00030006
#define MBOX_TAG_ALLOCATE_FRAMEBUF      0x00040001
#define MBOX_TAG_RELEASE_FRAMEBUF       0x00048001
#define MBOX_TAG_BLANK_SCREEN           0x00040002
#define MBOX_TAG_GET_PHYS_DISPLAY       0x00040003
#define MBOX_TAG_SET_PHYS_DISPLAY       0x00048003
#define MBOX_TAG_GET_VIRT_DISPLAY       0x00040004
#define MBOX_TAG_SET_VIRT_DISPLAY       0x00048004
#define MBOX_TAG_GET_DEPTH              0x00040005
#define MBOX_TAG_SET_DEPTH              0x00048005
#define MBOX_TAG_GET_PIXEL_ORDER        0x00040006
#define MBOX_TAG_SET_PIXEL_ORDER        0x00048006
#define MBOX_TAG_GET_ALPHA_MODE         0x00040007
#define MBOX_TAG_SET_ALPHA_MODE         0x00048007
#define MBOX_TAG_GET_PITCH              0x00040008
#define MBOX_TAG_SET_VIRT_OFFSET        0x00048009
#define MBOX_TAG_END                    0x00000000  // Конец списка тегов

// Идентификаторы тактовых источников
#define MBOX_CLOCK_ID_EMMC  1
#define MBOX_CLOCK_ID_UART  2
#define MBOX_CLOCK_ID_ARM   3
#define MBOX_CLOCK_ID_CORE  4
#define MBOX_CLOCK_ID_V3D   5

// ---------------------------------------------------------------
// Буфер для Mailbox сообщений
// ВАЖНО: Буфер должен быть выровнен на 16 байт (требование GPU)
// Используем __attribute__((aligned(16))) для выравнивания
// ---------------------------------------------------------------
volatile uint32_t mbox_buffer[36] __attribute__((aligned(16)));

// ---------------------------------------------------------------
// Функция: mbox_call(uint8_t channel)
// Отправляет буфер mbox_buffer через Mailbox на указанный канал
// Возвращает: 1 = успех, 0 = ошибка
//
// Протокол:
//   1. Ждём, пока Mailbox освободится (не FULL)
//   2. Составляем сообщение: адрес буфера | канал (нижние 4 бита)
//   3. Записываем в MBOX_WRITE
//   4. Ждём ответа (не EMPTY)
//   5. Читаем и проверяем ответ
// ---------------------------------------------------------------
int mbox_call(uint8_t channel) {
    uint32_t r;
    
    // Адрес буфера должен быть выровнен на 16 байт
    // Канал занимает биты [3:0], адрес — биты [31:4]
    uint32_t msg = ((uint32_t)(unsigned long)mbox_buffer & ~0xF) | (channel & 0xF);
    
    // Шаг 1: Ждём пока Mailbox для записи освободится
    do {
        asm volatile("nop");
    } while (mmio_read(MBOX_STATUS) & MBOX_FULL);
    
    // Шаг 2: Отправляем сообщение
    mmio_write(MBOX_WRITE, msg);
    
    // Шаг 3: Ждём ответа от GPU
    while (1) {
        // Ждём пока ящик чтения не станет непустым
        do {
            asm volatile("nop");
        } while (mmio_read(MBOX_STATUS) & MBOX_EMPTY);
        
        // Читаем ответ
        r = mmio_read(MBOX_READ);
        
        // Проверяем, это ответ на наш запрос (совпадает канал)
        if ((r & 0xF) == channel) {
            // Проверяем код ответа в буфере
            return (mbox_buffer[1] == MBOX_RESPONSE_OK) ? 1 : 0;
        }
        // Иначе — это ответ для другого запроса, продолжаем ждать
    }
}

// ---------------------------------------------------------------
// Функция: mbox_get_board_revision()
// Получает номер ревизии платы от firmware
// Полезно для определения модели RPi
// ---------------------------------------------------------------
uint32_t mbox_get_board_revision(void) {
    mbox_buffer[0] = 7 * 4;            // Размер буфера: 7 слов * 4 байта
    mbox_buffer[1] = MBOX_REQUEST;     // Это запрос
    mbox_buffer[2] = MBOX_TAG_GET_BOARD_REVISION;  // Тег
    mbox_buffer[3] = 4;                // Размер буфера значения (4 байта)
    mbox_buffer[4] = 0;                // Размер запроса
    mbox_buffer[5] = 0;                // Здесь GPU запишет ревизию
    mbox_buffer[6] = MBOX_TAG_END;     // Конец тегов
    
    if (mbox_call(MBOX_CHANNEL_PROP)) {
        return mbox_buffer[5];          // Возвращаем ревизию
    }
    return 0;   // Ошибка
}

// ---------------------------------------------------------------
// Функция: mbox_get_arm_memory()
// Получает размер памяти ARM (не GPU)
// Заполняет структуру mbox_memory_info
// ---------------------------------------------------------------
int mbox_get_arm_memory(uint32_t *base, uint32_t *size) {
    mbox_buffer[0] = 8 * 4;
    mbox_buffer[1] = MBOX_REQUEST;
    mbox_buffer[2] = MBOX_TAG_GET_ARM_MEMORY;
    mbox_buffer[3] = 8;    // Ответ: 2 слова (base + size)
    mbox_buffer[4] = 0;
    mbox_buffer[5] = 0;    // base
    mbox_buffer[6] = 0;    // size
    mbox_buffer[7] = MBOX_TAG_END;
    
    if (mbox_call(MBOX_CHANNEL_PROP)) {
        if (base) *base = mbox_buffer[5];
        if (size) *size = mbox_buffer[6];
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------
// Функция: mbox_get_clock_rate(uint32_t clock_id)
// Получает тактовую частоту указанного источника
// clock_id: MBOX_CLOCK_ID_ARM, MBOX_CLOCK_ID_CORE и т.д.
// Возвращает: частоту в Гц или 0 при ошибке
// ---------------------------------------------------------------
uint32_t mbox_get_clock_rate(uint32_t clock_id) {
    mbox_buffer[0] = 8 * 4;
    mbox_buffer[1] = MBOX_REQUEST;
    mbox_buffer[2] = MBOX_TAG_GET_CLOCK_RATE;
    mbox_buffer[3] = 8;
    mbox_buffer[4] = 0;
    mbox_buffer[5] = clock_id;   // Указываем какую частоту хотим
    mbox_buffer[6] = 0;          // GPU запишет сюда частоту
    mbox_buffer[7] = MBOX_TAG_END;
    
    if (mbox_call(MBOX_CHANNEL_PROP)) {
        return mbox_buffer[6];
    }
    return 0;
}

// ---------------------------------------------------------------
// Функция: mbox_init_framebuffer()
// Инициализирует фреймбуфер через Mailbox
// Запрашивает у GPU выделение видеопамяти
//
// Параметры запроса:
//   - Физическое разрешение: width x height
//   - Виртуальное разрешение: то же самое
//   - Глубина: 32 бит на пиксель (RGBA)
//   - Порядок байт: RGB
//
// GPU вернёт:
//   - Адрес фреймбуфера (в памяти GPU)
//   - Шаг строки (pitch) в байтах
// ---------------------------------------------------------------
int mbox_init_framebuffer(uint32_t width, uint32_t height,
                           uint32_t *fb_addr, uint32_t *fb_pitch) {
    uint32_t idx = 0;
    
    // Формируем пакет с несколькими тегами
    // Размер будет вычислен в конце
    
    mbox_buffer[idx++] = 0;                 // [0] Размер (заполним позже)
    mbox_buffer[idx++] = MBOX_REQUEST;      // [1] Запрос
    
    // Тег 1: Установить физическое разрешение
    mbox_buffer[idx++] = MBOX_TAG_SET_PHYS_DISPLAY;  // Тег
    mbox_buffer[idx++] = 8;                 // Размер буфера
    mbox_buffer[idx++] = 0;                 // Размер запроса
    mbox_buffer[idx++] = width;             // Ширина
    mbox_buffer[idx++] = height;            // Высота
    
    // Тег 2: Установить виртуальное разрешение (=физическому)
    mbox_buffer[idx++] = MBOX_TAG_SET_VIRT_DISPLAY;
    mbox_buffer[idx++] = 8;
    mbox_buffer[idx++] = 0;
    mbox_buffer[idx++] = width;
    mbox_buffer[idx++] = height;
    
    // Тег 3: Установить глубину цвета (32 бит = 4 байта на пиксель)
    mbox_buffer[idx++] = MBOX_TAG_SET_DEPTH;
    mbox_buffer[idx++] = 4;
    mbox_buffer[idx++] = 0;
    mbox_buffer[idx++] = 32;               // Бит на пиксель
    
    // Тег 4: Выделить фреймбуфер
    mbox_buffer[idx++] = MBOX_TAG_ALLOCATE_FRAMEBUF;
    mbox_buffer[idx++] = 8;
    mbox_buffer[idx++] = 4;
    mbox_buffer[idx++] = 16;               // Выравнивание в байтах
    mbox_buffer[idx++] = 0;               // GPU запишет сюда размер
    
    // Тег 5: Получить pitch (шаг строки)
    mbox_buffer[idx++] = MBOX_TAG_GET_PITCH;
    mbox_buffer[idx++] = 4;
    mbox_buffer[idx++] = 0;
    mbox_buffer[idx++] = 0;               // GPU запишет pitch
    
    // Конец тегов
    mbox_buffer[idx++] = MBOX_TAG_END;
    
    // Устанавливаем размер буфера
    mbox_buffer[0] = idx * 4;
    
    if (!mbox_call(MBOX_CHANNEL_PROP)) {
        return 0;   // Ошибка инициализации фреймбуфера
    }
    
    // Извлекаем результаты из буфера
    // Адрес фреймбуфера — в теге ALLOCATE (смещение 5 от начала тега)
    // Индекс: 0(size) + 1(req) + 3(tag1 header)+2 + 3(tag2)+2 + 3(tag3)+1 = 15
    // Адрес фреймбуфера находится в теге ALLOCATE — 5-й параметр
    // GPU возвращает физический адрес GPU памяти
    // Маскируем биты [31:30] для получения адреса ARM
    
    uint32_t addr = mbox_buffer[17] & 0x3FFFFFFF;  // Убираем биты GPU cache
    uint32_t pitch = mbox_buffer[23];
    
    if (fb_addr)  *fb_addr  = addr;
    if (fb_pitch) *fb_pitch = pitch;
    
    return (addr != 0) ? 1 : 0;
}

// ---------------------------------------------------------------
// Функция: mbox_get_temperature()
// Получает температуру чипа от firmware
// Возвращает: температуру в тысячных долях градуса Цельсия
//   (например, 50000 = 50.000°C)
// ---------------------------------------------------------------
uint32_t mbox_get_temperature(void) {
    mbox_buffer[0] = 8 * 4;
    mbox_buffer[1] = MBOX_REQUEST;
    mbox_buffer[2] = MBOX_TAG_GET_TEMPERATURE;
    mbox_buffer[3] = 8;
    mbox_buffer[4] = 0;
    mbox_buffer[5] = 0;    // ID термодатчика (0 = основной)
    mbox_buffer[6] = 0;    // GPU запишет температуру
    mbox_buffer[7] = MBOX_TAG_END;
    
    if (mbox_call(MBOX_CHANNEL_PROP)) {
        return mbox_buffer[6];
    }
    return 0;
}
