#include "tinytest.h"
#include <toad/script.h>
#include <threads/threads.h>

static void wait_for_tasks(toad_script_ctx_t *ctx) {
  for (int i = 0; i < 200 && toad_script_task_active_count(ctx); ++i)
    sleep_ms(5);
}

suite("Toad Executor tasks") {
  it("runs without a host coroutine context") {
    toad_script_ctx_t *ctx = toad_script_init(TOAD_SCRIPT_INIT_BARE);
    check_not_null(ctx);
    if (!ctx) return;
    check_equal(
        toad_script_run(ctx,
                         "id = task.spawn(() => { task.sleep(10); return 7; });"),
        0);
    wait_for_tasks(ctx);
    check_equal(toad_script_task_active_count(ctx), (size_t)0);
    toad_script_free(ctx);
  }
}
