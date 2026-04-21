#ifndef STRING_H
#define STRING_H

/**
 * string.h — String and memory utility functions
 */

#include <stdint.h>

/* Строковые функции */
int   str_len(const char *s);
char *str_copy(char *dst, const char *src, int max_len);
char *str_cat(char *dst, const char *src, int max_len);
int   str_cmp(const char *s1, const char *s2);
int   str_ncmp(const char *s1, const char *s2, int n);
char *str_chr(const char *s, int c);
char *str_rchr(const char *s, int c);
char *str_str(const char *haystack, const char *needle);
char *str_tok(char *str, const char *delim, char **saveptr);
int   str_to_int(const char *s);
uint32_t str_to_hex(const char *s);
char *int_to_str(int value, char *buf, int base);
void  str_upper(char *s);
void  str_lower(char *s);
char *str_trim(char *s);
int   str_starts_with(const char *s, const char *prefix);
int   str_ends_with(const char *s, const char *suffix);

/* Функции памяти */
void *mem_set(void *ptr, int value, int size);
void *mem_copy(void *dst, const void *src, int size);
int   mem_cmp(const void *a, const void *b, int size);

/* Классификация символов */
int is_digit(char c);
int is_alpha(char c);
int is_alnum(char c);
int is_space(char c);

#endif /* STRING_H */
