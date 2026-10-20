#ifndef LUME_FREESTANDING_STRING_H
#define LUME_FREESTANDING_STRING_H
#include <stddef.h>
void *memcpy(void *,const void *,size_t);
void *memset(void *,int,size_t);
int memcmp(const void *,const void *,size_t);
char *strchr(const char *,int);
#endif
