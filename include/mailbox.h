#ifndef MAILBOX_H
#define MAILBOX_H

/**
 * mailbox.h — VideoCore Mailbox interface
 */

#include <stdint.h>

int      mbox_call(uint8_t channel);
uint32_t mbox_get_board_revision(void);
int      mbox_get_arm_memory(uint32_t *base, uint32_t *size);
uint32_t mbox_get_clock_rate(uint32_t clock_id);
uint32_t mbox_get_temperature(void);
int      mbox_init_framebuffer(uint32_t width, uint32_t height,
                               uint32_t *fb_addr, uint32_t *fb_pitch);

/* Общий буфер Mailbox (выровнен на 16 байт) */
extern volatile uint32_t mbox_buffer[36];

#endif /* MAILBOX_H */
