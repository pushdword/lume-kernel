#ifndef LUME_PATH_H
#define LUME_PATH_H
#include "lume/files.h"
#include <stdbool.h>

/* Path copies own a slot until release. Allocation does not suspend a worker. */
char *lume_path_duplicate(const char *);
void lume_path_release(char *);
bool lume_path_equal_folded(const char *, const char *);
uint32_t lume_path_hash_folded(const char *);
int lume_path_final_spelling(const char *, char[LUME_FILES_PATH_MAX]);
#endif
