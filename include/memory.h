#ifndef MEMORY_H
#define MEMORY_H

/**
 * memory.h — Heap memory manager interface
 */

#include <stdint.h>

/* Статистика памяти */
typedef struct {
    uint32_t total;
    uint32_t used;
    uint32_t free;
    uint32_t alloc_count;
    uint32_t free_count;
    uint32_t free_fragments;
} memory_stats_t;

void  memory_init(void);
void *kmalloc(uint32_t size);
void  kfree(void *ptr);
void *kmemset(void *ptr, int value, uint32_t size);
void *kmemcpy(void *dst, const void *src, uint32_t size);
void  memory_get_stats(memory_stats_t *stats);

#endif /* MEMORY_H */
