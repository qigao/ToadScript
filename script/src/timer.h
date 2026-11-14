#ifndef TOAD_SCRIPT_TIMER_H
#define TOAD_SCRIPT_TIMER_H

#include "internal.h"

enum { TS_TIMER_DEFAULT_CAPACITY = 64 };

int ts_timer_scheduler_init(toad_script_ctx_t *ctx, size_t capacity);
void ts_timer_scheduler_shutdown(toad_script_ctx_t *ctx);
void ts_timer_scheduler_destroy(toad_script_ctx_t *ctx);
void ts_timer_register_functions(toad_script_ctx_t *ctx);
size_t ts_timer_scheduler_capacity(toad_script_ctx_t *ctx);

#endif /* TOAD_SCRIPT_TIMER_H */
