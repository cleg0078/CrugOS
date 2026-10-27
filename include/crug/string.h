/* Freestanding memory and string routines. */
#ifndef CRUG_STRING_H
#define CRUG_STRING_H

#include <crug/types.h>

void *memcpy(void *dst, const void *src, usize n);
void *memmove(void *dst, const void *src, usize n);
void *memset(void *dst, int value, usize n);
int memcmp(const void *a, const void *b, usize n);
usize strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, usize n);

#endif /* CRUG_STRING_H */
