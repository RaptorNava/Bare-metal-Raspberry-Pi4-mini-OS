#ifndef SHELL_H
#define SHELL_H

/**
 * shell.h — Interactive command shell interface
 */

/* Объявляем kernel_panic здесь, чтобы shell.c мог её вызвать
 * без отдельного kernel.h */
void kernel_panic(const char *msg);

void shell_run(void);

#endif /* SHELL_H */
