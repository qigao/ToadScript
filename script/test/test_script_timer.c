#include "tinytest.h"
#include <toad/script.h>
#include <threads/threads.h>

static void wait_for_timers(toad_script_ctx_t *ctx) {
  for (int i = 0; i < 200 && toad_script_timer_active_count(ctx); ++i)
    sleep_ms(5);
}

suite("Toad Executor timers") {
  it("fires after without a host coroutine context") {
    toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
    check_not_null(ctx);
    if (!ctx) return;
    check_equal(toad_script_run(ctx, "timer.after(10, () => { fired = 1; });"), 0);
    wait_for_timers(ctx);
    check_equal(toad_script_timer_active_count(ctx), (size_t)0);
    toad_script_free(ctx);
  }

  it("cancels an interval") {
    toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
    check_not_null(ctx);
    if (!ctx) return;
    check_equal(
        toad_script_run(ctx, "id = timer.every(1000, () => 1); timer.cancel(id);"),
        0);
    wait_for_timers(ctx);
    check_equal(toad_script_timer_active_count(ctx), (size_t)0);
    toad_script_free(ctx);
  }
}
