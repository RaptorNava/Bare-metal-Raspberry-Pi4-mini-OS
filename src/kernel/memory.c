/**
 * memory.c — Подсистема управления памятью
 *
 * Реализует два уровня управления памятью:
 *
 * 1. BUMP ALLOCATOR (простейший аллокатор)
 *    - Указатель просто двигается вперёд при каждом malloc()
 *    - Освобождение памяти не поддерживается (только сброс всего)
 *    - Очень быстрый, нулевые накладные расходы
 *    - Подходит для ранней инициализации
 *
 * 2. FREE-LIST ALLOCATOR (связный список свободных блоков)
 *    - Поддерживает malloc() и free()
 *    - Каждый блок имеет заголовок с размером и флагом занятости
 *    - Объединяет соседние свободные блоки (coalescing)
 *    - Подходит для общего использования
 *
 * Карта памяти RPi4 (упрощённая):
 *   0x00000000 - 0x0007FFFF: Зарезервировано для GPU (512KB)
 *   0x00080000: Точка входа ядра (_start)
 *   0x00080000 - 0x000FFFFF: Код и данные ядра (~512KB)
 *   0x00100000 - 0x1FFFFFFF: Свободная память (наша куча)
 *   0x20000000 - 0x3FFFFFFF: Периферийные устройства (RPi3)
 *   0xFC000000 - 0xFFFFFFFF: Периферийные устройства (RPi4)
 *
 * Наш аллокатор работает в диапазоне HEAP_START - HEAP_END
 */

#include "../../include/memory.h"
#include <stddef.h>
#include <stdint.h>
// ---------------------------------------------------------------
// Параметры кучи
// ---------------------------------------------------------------
#define HEAP_START      0x00200000UL    // 2 MB — начало кучи (после ядра)
#define HEAP_END        0x03F00000UL    // ~63 MB — конец кучи (до периферии)
#define HEAP_SIZE       (HEAP_END - HEAP_START)

// Минимальный размер блока (выравнивание)
#define ALIGN_SIZE      16              // Выравниваем на 16 байт (требование AArch64)
#define ALIGN(x)        (((x) + ALIGN_SIZE - 1) & ~(ALIGN_SIZE - 1))

// Маски для заголовка блока
#define BLOCK_USED      0x1             // Бит занятости в поле size
#define BLOCK_MAGIC     0xDEAD0000      // Магическое число для проверки целостности

// ---------------------------------------------------------------
// Структура заголовка блока памяти
// Каждый аллоцированный блок имеет этот заголовок ПЕРЕД данными
// ---------------------------------------------------------------
typedef struct block_header {
    uint32_t magic;                 // Магическое число (проверка целостности)
    uint32_t size;                  // Размер блока (включая заголовок) | BLOCK_USED
    struct block_header *next;      // Следующий блок в списке
    struct block_header *prev;      // Предыдущий блок в списке
} block_header_t;

#define HEADER_SIZE     (sizeof(block_header_t))

// ---------------------------------------------------------------
// Глобальное состояние аллокатора
// ---------------------------------------------------------------
static block_header_t *heap_start_ptr = NULL;   // Начало кучи
static block_header_t *heap_free_list = NULL;   // Список свободных блоков

// Статистика
static uint32_t alloc_count = 0;    // Количество выделений
static uint32_t free_count  = 0;    // Количество освобождений
static uint32_t total_used  = 0;    // Суммарно занято байт

// ---------------------------------------------------------------
// Вспомогательные функции
// ---------------------------------------------------------------
static int block_is_used(block_header_t *b) {
    return (b->size & BLOCK_USED) != 0;
}

static uint32_t block_size(block_header_t *b) {
    return b->size & ~BLOCK_USED;   // Убираем бит занятости
}

static void block_set_used(block_header_t *b) {
    b->size |= BLOCK_USED;
}

static void block_set_free(block_header_t *b) {
    b->size &= ~BLOCK_USED;
}

// Следующий блок в памяти (не в списке, а физически за текущим)
static block_header_t *block_next_physical(block_header_t *b) {
    return (block_header_t *)((uint8_t *)b + block_size(b));
}

// ---------------------------------------------------------------
// Функция: memory_init()
// Инициализирует кучу как один большой свободный блок
// ---------------------------------------------------------------
void memory_init(void) {
    // Инициализируем весь диапазон кучи как один блок
    heap_start_ptr = (block_header_t *)HEAP_START;
    
    heap_start_ptr->magic = BLOCK_MAGIC;
    heap_start_ptr->size  = HEAP_SIZE;  // Весь размер, бит USED = 0 (свободен)
    heap_start_ptr->next  = NULL;
    heap_start_ptr->prev  = NULL;
    
    heap_free_list = heap_start_ptr;
    
    alloc_count = 0;
    free_count  = 0;
    total_used  = 0;
}

// ---------------------------------------------------------------
// Функция: kmalloc(uint32_t size)
// Выделяет блок памяти размером size байт
// Возвращает указатель на данные (после заголовка) или NULL
//
// Алгоритм: First-Fit — берём первый подходящий свободный блок
// ---------------------------------------------------------------
void *kmalloc(uint32_t size) {
    if (size == 0) return NULL;
    
    // Выравниваем размер и добавляем место под заголовок
    uint32_t needed = ALIGN(size + HEADER_SIZE);
    if (needed < size) return NULL;     // Защита от переполнения
    
    // Ищем первый подходящий свободный блок
    block_header_t *current = heap_free_list;
    
    while (current != NULL) {
        // Проверяем целостность блока
        if (current->magic != BLOCK_MAGIC) {
            // Куча повреждена!
            return NULL;
        }
        
        if (!block_is_used(current) && block_size(current) >= needed) {
            // Нашли подходящий блок!
            
            uint32_t remaining = block_size(current) - needed;
            
            // Если остаток достаточно большой — разбиваем блок
            // Минимальный остаток: заголовок + 16 байт данных
            if (remaining >= HEADER_SIZE + ALIGN_SIZE) {
                // Создаём новый свободный блок из остатка
                block_header_t *new_block = (block_header_t *)((uint8_t *)current + needed);
                new_block->magic = BLOCK_MAGIC;
                new_block->size  = remaining;   // Свободный (бит USED = 0)
                new_block->next  = current->next;
                new_block->prev  = current;
                
                if (current->next) current->next->prev = new_block;
                current->next = new_block;
                
                // Устанавливаем размер текущего блока
                current->size = needed;
            }
            
            // Помечаем блок как занятый
            block_set_used(current);
            
            alloc_count++;
            total_used += block_size(current);
            
            // Возвращаем указатель на данные (после заголовка)
            return (void *)(current + 1);
        }
        
        current = current->next;
    }
    
    return NULL;    // Нет подходящего блока — нехватка памяти
}

// ---------------------------------------------------------------
// Функция: kfree(void *ptr)
// Освобождает ранее выделенный блок
// Объединяет соседние свободные блоки (coalescing)
// ---------------------------------------------------------------
void kfree(void *ptr) {
    if (ptr == NULL) return;
    
    // Получаем заголовок блока (он стоит прямо перед данными)
    block_header_t *block = (block_header_t *)ptr - 1;
    
    // Проверяем целостность
    if (block->magic != BLOCK_MAGIC) {
        // Попытка освободить невалидный указатель!
        return;
    }
    
    if (!block_is_used(block)) {
        // Двойное освобождение! Игнорируем (в реальной ОС — panic)
        return;
    }
    
    total_used -= block_size(block);
    
    // Помечаем блок как свободный
    block_set_free(block);
    free_count++;
    
    // Объединение с следующим блоком (если тот тоже свободен)
    if (block->next != NULL && !block_is_used(block->next)) {
        block_header_t *next = block->next;
        // Присоединяем размер следующего блока
        block->size += block_size(next);    // Бит USED уже = 0
        // Удаляем следующий блок из списка
        block->next = next->next;
        if (next->next) next->next->prev = block;
        // Затираем заголовок объединённого блока (безопасность)
        next->magic = 0;
    }
    
    // Объединение с предыдущим блоком (если тот тоже свободен)
    if (block->prev != NULL && !block_is_used(block->prev)) {
        block_header_t *prev = block->prev;
        // Присоединяем наш размер к предыдущему
        prev->size += block_size(block);
        // Удаляем текущий блок из списка
        prev->next = block->next;
        if (block->next) block->next->prev = prev;
        // Затираем наш заголовок
        block->magic = 0;
    }
}

// ---------------------------------------------------------------
// Функция: kmemset(void *ptr, int value, uint32_t size)
// Заполняет память указанным байтом
// ---------------------------------------------------------------
void *kmemset(void *ptr, int value, uint32_t size) {
    uint8_t *p = (uint8_t *)ptr;
    uint8_t v = (uint8_t)value;
    uint32_t i;
    
    for (i = 0; i < size; i++) {
        p[i] = v;
    }
    
    return ptr;
}

// ---------------------------------------------------------------
// Функция: kmemcpy(void *dst, const void *src, uint32_t size)
// Копирует блок памяти
// ---------------------------------------------------------------
void *kmemcpy(void *dst, const void *src, uint32_t size) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    uint32_t i;
    
    // Если буферы не перекрываются — копируем вперёд
    if (d < s || d >= s + size) {
        for (i = 0; i < size; i++) d[i] = s[i];
    } else {
        // Перекрытие — копируем назад
        for (i = size; i > 0; i--) d[i-1] = s[i-1];
    }
    
    return dst;
}

// ---------------------------------------------------------------
// Функция: memory_get_stats()
// Заполняет структуру статистики памяти
// ---------------------------------------------------------------
void memory_get_stats(memory_stats_t *stats) {
    if (!stats) return;
    
    stats->total       = HEAP_SIZE;
    stats->used        = total_used;
    stats->free        = HEAP_SIZE - total_used;
    stats->alloc_count = alloc_count;
    stats->free_count  = free_count;
    
    // Считаем фрагментацию — количество свободных фрагментов
    uint32_t free_frags = 0;
    block_header_t *b = heap_free_list;
    while (b != NULL) {
        if (!block_is_used(b)) free_frags++;
        b = b->next;
    }
    stats->free_fragments = free_frags;
}
