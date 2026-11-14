# Timer 调度

Toad 提供一次性定时和固定延迟 interval。每个脚本 context 拥有单 shard 的 Salts coroutine executor；定时器在 executor 协程中等待，回调在该 shard 执行。当前已移除 Cron API 和依赖。

## 脚本 API

| API | 返回值 | 错误条件 |
| --- | --- | --- |
| `timer.after(delay_ms, callback)` | 正整数 job ID | delay 不是非负整数、超过 `UINT_MAX - 1` 毫秒、callback 不是函数、context 已关闭、容量耗尽或 executor 拒绝提交 |
| `timer.every(interval_ms, callback)` | 正整数 job ID | interval 不是正整数，或同上 |
| `timer.cancel(job_id)` | `true` / `false` | ID 类型非法时终止当前脚本；未知、已取消或已结束的 ID 返回 `false` |
| `timer.status(job_id)` | 状态字符串 | ID 类型非法时终止当前脚本；未知 ID 返回 `"invalid"` |

参数和提交错误通过脚本错误契约使当前执行失败。`after` 成功执行一次后进入 `completed`；`every` 等本次回调返回后再等待下一段 interval，不补放历史触发。零延迟 `after` 仍异步提交，等待至少一毫秒。

```javascript
var heartbeat = timer.every(30000, () => {
    print("heartbeat");
});
// 后续可调用 timer.cancel(heartbeat)。
```

状态值包括 `scheduled`、`pending`、`running`、`completed`、`cancelled`、`failed` 和 `invalid`。终态记录保留到容量槽被复用。未捕获的回调错误使 job 进入 `failed`，不自动重试；宿主通过 `toad_script_timer_failed_count()` 查询保留的失败记录。

## 所有权、容量与取消

调度器拥有固定容量的 job 表、状态、回调引用和错误摘要；executor 任务借用 job 槽。提交前保留 callback closure，任务结束或提交失败时释放。宿主通过 `toad_script_set_timer_capacity()` 在没有活动 timer 时调整容量；合法范围为 1 到 65536，容量耗尽直接失败，不自动增长。

取消请求在调度器 mutex 下更新状态，并完成活动 await 以唤醒等待协程。运行中的回调只能在现有协作边界观察取消，不能强行终止 CPU 代码。停止再次调度和释放 callback 保持现有生命周期协议。

`toad_script_free()` 先关闭 timer/task 注册表并请求取消，再等待 executor drain，最后销毁调度器、executor 和 Engine/MIR 状态。不得在回调内部释放所属 context。

## 宿主与迁移

`toad_script_init()` 创建 executor；宿主无需注入 CoroNet context 或 callback-post adapter。使用 `toad_script_timer_active_count()` 观察活动 timer 数，调用 context 销毁入口完成关闭。

原 `timer.cron` 已删除，脚本调用会失败。需要周期性检查时可显式改用 `timer.every`，但固定延迟不提供日历调度语义。完整 executor 协议见 [Toad Executor Runtime](../architecture/toad-executor-runtime.md)。现有正式 timer 测试覆盖一次性触发、interval 取消和移除 API 的拒绝行为；运行结果以对应构建记录为准。
