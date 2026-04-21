#ifndef MMIO_H
#define MMIO_H

/**
 * mmio.h — Memory-Mapped I/O
 *
 * На ARM периферийные устройства доступны через обычные адреса памяти.
 * Volatile гарантирует, что компилятор не оптимизирует эти обращения.
 */

#include <stdint.h>

/**
 * mmio_write — Записывает 32-битное значение по адресу
 */
static inline void mmio_write(uint64_t reg, uint32_t val) {
    *(volatile uint32_t *)reg = val;
}

/**
 * mmio_read — Читает 32-битное значение по адресу
 */
static inline uint32_t mmio_read(uint64_t reg) {
    return *(volatile uint32_t *)reg;
}

#endif /* MMIO_H */
