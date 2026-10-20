#ifndef LUME_FREESTANDING_STDLIB_H
#define LUME_FREESTANDING_STDLIB_H
#include <stddef.h>
void *malloc(size_t);
void *calloc(size_t,size_t);
void free(void *);
_Noreturn void abort(void);
int atoi(const char *);
long strtol(const char *,char **,int);
unsigned long strtoul(const char *,char **,int);
#endif
