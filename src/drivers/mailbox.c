/**
 * mailbox.c — Драйвер почтового ящика (Mailbox) VideoCore
 *
 * Исправлена функция mbox_init_framebuffer:
 *   - Явно размечаем буфер с фиксированными индексами
 *   - Правильно читаем ответ (адрес и pitch)
 */

#include "../../include/mailbox.h"
#include "../../include/mmio.h"
#include <stddef.h>

/* ---------------------------------------------------------------
 * Базовые адреса Mailbox
 * --------------------------------------------------------------- */
#define PERIPHERAL_BASE     0xFE000000UL
#define MBOX_BASE           (PERIPHERAL_BASE + 0xB880)

#define MBOX_READ           (MBOX_BASE + 0x00)
#define MBOX_STATUS         (MBOX_BASE + 0x18)
#define MBOX_WRITE          (MBOX_BASE + 0x20)

#define MBOX_FULL           (1U << 31)
#define MBOX_EMPTY          (1U << 30)

#define MBOX_CHANNEL_PROP   8

#define MBOX_REQUEST        0x00000000
#define MBOX_RESPONSE_OK    0x80000000

/* Теги */
#define TAG_SET_PHYS_WH     0x00048003
#define TAG_SET_VIRT_WH     0x00048004
#define TAG_SET_VIRT_OFF    0x00048009
#define TAG_SET_DEPTH       0x00048005
#define TAG_SET_PIXEL_ORDER 0x00048006
#define TAG_GET_FB          0x00040001
#define TAG_GET_PITCH       0x00040008
#define TAG_END             0x00000000

#define TAG_GET_BOARD_REV   0x00010002
#define TAG_GET_ARM_MEM     0x00010005
#define TAG_GET_CLOCK_RATE  0x00030002
#define TAG_GET_TEMPERATURE 0x00030006

/* ---------------------------------------------------------------
 * Буфер (выровнен на 16 байт — требование GPU)
 * --------------------------------------------------------------- */
volatile uint32_t mbox_buffer[36] __attribute__((aligned(16)));

/* ---------------------------------------------------------------
 * mbox_call — отправить буфер и получить ответ
 * --------------------------------------------------------------- */
int mbox_call(uint8_t channel) {
    uint32_t msg = ((uint32_t)(unsigned long)mbox_buffer & ~0xFU) | (channel & 0xFU);

    /* Ждём освобождения ящика */
    while (mmio_read(MBOX_STATUS) & MBOX_FULL) {
        asm volatile("nop");
    }
    mmio_write(MBOX_WRITE, msg);

    /* Ждём ответа */
    while (1) {
        while (mmio_read(MBOX_STATUS) & MBOX_EMPTY) {
            asm volatile("nop");
        }
        uint32_t r = mmio_read(MBOX_READ);
        if ((r & 0xFU) == channel) {
            return (mbox_buffer[1] == MBOX_RESPONSE_OK) ? 1 : 0;
        }
    }
}

/* ---------------------------------------------------------------
 * mbox_get_board_revision
 * --------------------------------------------------------------- */
uint32_t mbox_get_board_revision(void) {
    mbox_buffer[0] = 7 * 4;
    mbox_buffer[1] = MBOX_REQUEST;
    mbox_buffer[2] = TAG_GET_BOARD_REV;
    mbox_buffer[3] = 4;
    mbox_buffer[4] = 0;
    mbox_buffer[5] = 0;
    mbox_buffer[6] = TAG_END;
    if (mbox_call(MBOX_CHANNEL_PROP)) return mbox_buffer[5];
    return 0;
}

/* ---------------------------------------------------------------
 * mbox_get_arm_memory
 * --------------------------------------------------------------- */
int mbox_get_arm_memory(uint32_t *base, uint32_t *size) {
    mbox_buffer[0] = 8 * 4;
    mbox_buffer[1] = MBOX_REQUEST;
    mbox_buffer[2] = TAG_GET_ARM_MEM;
    mbox_buffer[3] = 8;
    mbox_buffer[4] = 0;
    mbox_buffer[5] = 0;
    mbox_buffer[6] = 0;
    mbox_buffer[7] = TAG_END;
    if (mbox_call(MBOX_CHANNEL_PROP)) {
        if (base) *base = mbox_buffer[5];
        if (size) *size = mbox_buffer[6];
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------------
 * mbox_get_clock_rate
 * --------------------------------------------------------------- */
uint32_t mbox_get_clock_rate(uint32_t clock_id) {
    mbox_buffer[0] = 8 * 4;
    mbox_buffer[1] = MBOX_REQUEST;
    mbox_buffer[2] = TAG_GET_CLOCK_RATE;
    mbox_buffer[3] = 8;
    mbox_buffer[4] = 0;
    mbox_buffer[5] = clock_id;
    mbox_buffer[6] = 0;
    mbox_buffer[7] = TAG_END;
    if (mbox_call(MBOX_CHANNEL_PROP)) return mbox_buffer[6];
    return 0;
}

/* ---------------------------------------------------------------
 * mbox_get_temperature
 * --------------------------------------------------------------- */
uint32_t mbox_get_temperature(void) {
    mbox_buffer[0] = 8 * 4;
    mbox_buffer[1] = MBOX_REQUEST;
    mbox_buffer[2] = TAG_GET_TEMPERATURE;
    mbox_buffer[3] = 8;
    mbox_buffer[4] = 0;
    mbox_buffer[5] = 0;
    mbox_buffer[6] = 0;
    mbox_buffer[7] = TAG_END;
    if (mbox_call(MBOX_CHANNEL_PROP)) return mbox_buffer[6];
    return 0;
}

/* ---------------------------------------------------------------
 * mbox_init_framebuffer
 *
 * Буфер с фиксированной разметкой (проверен на RPi4):
 *
 * [0]  size
 * [1]  request code
 *
 * TAG: SET_PHYS_WH (индексы 2..8)
 * [2]  tag id
 * [3]  value buf size = 8
 * [4]  request/response indicator
 * [5]  width
 * [6]  height
 *
 * TAG: SET_VIRT_WH (индексы 7..13)
 * [7]  tag id
 * [8]  8
 * [9]  0
 * [10] width
 * [11] height
 *
 * TAG: SET_VIRT_OFFSET (12..18)
 * [12] tag id
 * [13] 8
 * [14] 0
 * [15] 0   offset x
 * [16] 0   offset y
 *
 * TAG: SET_DEPTH (17..21)
 * [17] tag id
 * [18] 4
 * [19] 0
 * [20] 32  bpp
 *
 * TAG: SET_PIXEL_ORDER (21..25)
 * [21] tag id
 * [22] 4
 * [23] 0
 * [24] 1   (RGB)
 *
 * TAG: GET_FB (25..31) — GPU запишет адрес + размер
 * [25] tag id
 * [26] 8
 * [27] 0
 * [28] 16  alignment (request) → адрес FB (response)
 * [29] 0   (response: size)
 *
 * TAG: GET_PITCH (30..34)
 * [30] tag id
 * [31] 4
 * [32] 0
 * [33] 0   → pitch (response)
 *
 * [34] TAG_END
 * [35] (padding)
 * --------------------------------------------------------------- */
int mbox_init_framebuffer(uint32_t width, uint32_t height,
                           uint32_t *fb_addr, uint32_t *fb_pitch) {
    mbox_buffer[0]  = 35 * 4;          /* Размер буфера */
    mbox_buffer[1]  = MBOX_REQUEST;

    mbox_buffer[2]  = TAG_SET_PHYS_WH;
    mbox_buffer[3]  = 8;
    mbox_buffer[4]  = 0;
    mbox_buffer[5]  = width;
    mbox_buffer[6]  = height;

    mbox_buffer[7]  = TAG_SET_VIRT_WH;
    mbox_buffer[8]  = 8;
    mbox_buffer[9]  = 0;
    mbox_buffer[10] = width;
    mbox_buffer[11] = height;

    mbox_buffer[12] = TAG_SET_VIRT_OFF;
    mbox_buffer[13] = 8;
    mbox_buffer[14] = 0;
    mbox_buffer[15] = 0;   /* x offset */
    mbox_buffer[16] = 0;   /* y offset */

    mbox_buffer[17] = TAG_SET_DEPTH;
    mbox_buffer[18] = 4;
    mbox_buffer[19] = 0;
    mbox_buffer[20] = 32;  /* Bits per pixel */

    mbox_buffer[21] = TAG_SET_PIXEL_ORDER;
    mbox_buffer[22] = 4;
    mbox_buffer[23] = 0;
    mbox_buffer[24] = 1;   /* RGB (не BGR) */

    mbox_buffer[25] = TAG_GET_FB;
    mbox_buffer[26] = 8;
    mbox_buffer[27] = 0;
    mbox_buffer[28] = 16;  /* Выравнивание (запрос) */
    mbox_buffer[29] = 0;   /* GPU запишет размер сюда */

    mbox_buffer[30] = TAG_GET_PITCH;
    mbox_buffer[31] = 4;
    mbox_buffer[32] = 0;
    mbox_buffer[33] = 0;   /* GPU запишет pitch сюда */

    mbox_buffer[34] = TAG_END;

    if (!mbox_call(MBOX_CHANNEL_PROP)) return 0;

    /* Адрес FB: убираем биты GPU-кеша → получаем ARM-адрес */
    uint32_t addr  = mbox_buffer[28] & 0x3FFFFFFFU;
    uint32_t pitch = mbox_buffer[33];

    if (addr == 0 || pitch == 0) return 0;

    if (fb_addr)  *fb_addr  = addr;
    if (fb_pitch) *fb_pitch = pitch;
    return 1;
}
