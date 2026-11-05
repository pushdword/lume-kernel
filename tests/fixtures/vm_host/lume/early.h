#ifndef MOCK_EARLY_H
#define MOCK_EARLY_H
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define LUME_TASK_LIMIT 64u
uint64_t lume_page_alloc(unsigned);
bool lume_page_free(uint64_t);
void lume_fail(const char *);
bool lume_user_buffer(uint64_t, uint64_t, bool);
void lume_sync_code(uint64_t, size_t);
void lume_sync_code_defer(uint64_t, size_t);
void lume_sync_code_clean_finish(void);
void lume_sync_code_finish(void);
#endif
