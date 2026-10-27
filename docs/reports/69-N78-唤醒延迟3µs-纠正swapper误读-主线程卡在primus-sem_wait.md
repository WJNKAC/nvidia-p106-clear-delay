# 69 · N77-B + N78：唤醒延迟 **3µs**（正常）；**纠正上一轮"定时器唤醒"的误读**；主线程卡在 primus 的 `sem_wait`

> 2026-10-26 11:10
> 用户判据：
> | 唤醒延迟中位 | 含义 |
> |---|---|
> | **< 100µs** | 调度正常，27ms 是「等条件满足」→ 回到 N77-A |
> | ms 级 | 调度延迟本身就是差异 |
> | 双峰 | 混合原因 |
>
> **结果：中位 3.0 µs，73.9% < 10µs → 第一档（调度完全正常）。**
> **同时必须纠正我上一轮的一处误读，并发现主线程的真正等待点。**

---

## 1. 判据回答：调度正常，且全系统都正常

### 1.1 渲染线程（全系统采集，3.03s）

| 分位 | 唤醒延迟 |
|---|---|
| min | 1.0 µs |
| p10 / p25 | 2.0 µs |
| **中位** | **3.0 µs** |
| p75 | 15.0 µs |
| p90 | 597 µs |
| p99 | 4966 µs |

```
<10µs       218  73.9%  ##########################
10-50µs      11   3.7%
50-100µs     10   3.4%
100-500µs    23   7.8%
0.5-1ms       7   2.4%
1-3ms        18   6.1%
3-10ms        6   2.0%
10-50ms       2   0.7%
```

**合计只占 4.42% 的时间。**

### 1.2 对照组：全系统其它线程

| tid | wakeup | 中位延迟 |
|---|---|---|
| 64807 | 2725 | **4.0 µs** |
| 2588 | 1480 | **3.0 µs** |
| 64773 | 923 | **3.0 µs** |
| 64772 | 892 | **3.0 µs** |
| 1926 | 864 | **4.0 µs** |
| 3733 | 742 | **2.0 µs** |

**所有线程都是 2~4 µs。这台机器的调度健康。**

→ **「525 的线程被唤醒后要等 ms 级才上 CPU」这个假设被彻底否定。**

---

## 2. ⚠️ 纠正上一轮的一处误读：`swapper` ≠ 定时器

我在起草本轮报告时曾写过：

> 「72.5% 的唤醒来自 `swapper`(tid 0) = 中断上下文 = hrtimer 超时」

**这是错的。**拿到唤醒的**内核栈**后真相是：

```
__traceiter_sched_wakeup
ttwu_do_wakeup
sched_ttwu_pending                    ← ★
flush_smp_call_function_queue         ← ★
do_idle
cpu_startup_entry
start_secondary
```

`sched_ttwu_pending` + `flush_smp_call_function_queue` 的含义是：

**另一个 CPU 上的线程调了 `try_to_wake_up`，因为目标在同一 CPU 之外，内核用 IPI 把唤醒请求投到目标 CPU 的 pending 队列；目标 CPU 当时在 idle，于是在处理这个 IPI 时执行唤醒。**

**→ `swapper` 只代表「目标 CPU 当时空闲」，不代表「唤醒源是定时器」。**

这一点很关键：它推翻了我基于 `common_pid==0` 做的那套"超时唤醒"推断。

### 2.1 正确的做法：看唤醒栈里的关键函数

| 事件 | 目标 | 样本 | `sched_ttwu_pending`（跨 CPU） | `futex_wake`（同 CPU） |
|---|---|---|---|---|
| `sched_wakeup` | **主线程** | 184 | **100.0%** | **0.0%** |
| `sched_wakeup` | **渲染线程** | 197 | 91.4% | **8.6%** |

栈模式：

| 线程 | 栈 | 次数 |
|---|---|---|
| 主线程 | `ttwu_do_wakeup > sched_ttwu_pending > flush_smp_call_function_queue > do_idle` | **181 / 184（98%）** |
| 主线程 | `ttwu_do_wakeup > sched_ttwu_pending > __sysvec_call_function_single` | 3（2%） |
| 渲染线程 | 同上（跨 CPU） | 171 / 197（87%） |
| **渲染线程** | **`ttwu_do_wakeup > try_to_wake_up > wake_up_q > futex_wake > do_futex`** | **17（9%）** |

**主线程 100% 是跨 CPU 唤醒，一次 `futex_wake` 都没有。**
**渲染线程 9% 是同 CPU 的 `futex_wake`（能直接看到源是 `do_futex`）。**

**跨 CPU 唤醒的栈上看不到原始 waker**（链路在 IPI 处断了）。要看源必须用 `sched_waking`（在源 CPU 触发）。

---

## 3. 主线程卡在哪：primus 的 `sem_wait`（不是超时）

### 3.1 gdb 栈（14 份样本，13 份同栈）

```
#0  libc+0x9a72e
#1  libc+0x8f388
#2  libc+0x8f9ac
#3  libc+0x9a660            ← 落在 sem_timedwait(0x9a4b0) 范围内
#4  glXSwapBuffers+0x20d    primus/libGL.so.1
#5  libUnigine_x64.so+0x2714b0
#6  libUnigine_x64.so+0x151de4
```

### 3.2 反汇编确认调用点

primus `glXSwapBuffers` @ `0x19220`，栈帧 `0x1942d` = **+0x20d**：

```asm
19419: mov  %r14,%rdi
1941c: mov  %rax,0x60(%rbx)
19420: call sem_post@plt        ; sem_post(&di.r.acqsem)  —— 唤醒 readback worker
19425: mov  %r13,%rdi
19428: call sem_wait@plt        ; sem_wait(&di.r.relsem)  —— ★ 等它放行
1942d: mov  0x60(%rbx),%rdi     ; ← 栈帧 #4 正是这条（sem_wait 的返回地址）
```

**与 primus 源码第 744/745 行完全对应。**

### 3.3 `#3` 显示 `sem_timedwait` 但源码是 `sem_wait` —— 不矛盾

glibc 里 `sem_wait`(0x9a6a0) 与 `sem_timedwait`(0x9a4b0) 共享实现（`sem_wait` 是 `sem_timedwait(sem, NULL)` 的入口）。
`#3 = 0x9a660` 落在 `sem_timedwait` 的代码范围（0x9a4b0~0x9a6a0）内，是**共享实现里的返回地址**。

**源码是 `sem_wait`（无超时）→ 它只能被 `sem_post` 唤醒，不可能是超时。**

### 3.4 所以

| 项 | 值 |
|---|---|
| 主线程长睡眠 | **84 次 ≥10ms，平均 29.8 ms**，占 off-CPU 时间的 **99.2%** |
| 睡眠时长 | **29.8ms ≈ 帧时间 30.84ms** |
| 睡在哪 | **primus `glXSwapBuffers` 的 `sem_wait(&di.r.relsem)`** |
| 等谁 | **readback worker 完成 `glWaitSync + glReadPixels` 后的 `sem_post`** |

**→ 主线程每帧有约 29.8ms 在等 readback worker 放行。这就是那 20.55ms「真空」的主体。**

---

## 4. ★ 一个必须纠正的命名：之前叫"渲染线程"的其实是 **readback worker**

按 primus 源码，进程里有三条链：

| 线程 | 职责 | 我此前的称呼 |
|---|---|---|
| **主线程**（= PID） | 应用 GL 调用 → 转发到 `adpy`；`glXSwapBuffers` 里 `sem_post`/`sem_wait` | 主线程 ✓ |
| **readback_work** | `glWaitSync` → `glReadPixels` → `sem_post(relsem)` → `usleep` → `glMapBuffer` → 通知 display | **"渲染线程"** ❌ |
| **display_work** | `glTexSubImage2D` + `glXSwapBuffers(ddpy)` | display worker ✓ |

我此前用"NVIDIA 侧 ioctl 最多的 tid"来定位，那个 tid **不是主线程**（主线程 = PID，另算），所以它是 **`readback_work`**。

**报告 63/64/65/66/67 里所有"渲染线程"应读作"readback worker"。**

**结论层面不受影响**（它确实 87% 在 futex、确实只占 12.4% CPU、STOP 它 fps 掉 91%），但**角色定位要改**：

- 它等的 `cond_timedwait`（报告 65/66）是等主线程发来的 `di.r.acqsem`
- 它 87% 在等，是因为主线程每帧只发一次
- **不是它在等 GPU**

---

## 5. 完整链条（现在每一步都有实测）

```
主线程
  ├─ 渲染本帧（CPU 5.0ms）
  ├─ glXSwapBuffers:
  │    ├─ sem_post(&di.r.acqsem)      → 唤醒 readback worker【80 次/3s ≈ 26/s ≈ fps】
  │    └─ sem_wait(&di.r.relsem)      → 等 readback worker 放行【★ 29.8ms】
  │
readback worker（"渲染线程"）
  ├─ sem_wait(&di.r.acqsem)           ← 等主线程【87% 时间，中位 22.6ms】
  ├─ glWaitSync(di.sync)              ← 等 GPU 完成 fence
  ├─ glReadPixels(...,NULL)           ← 提交，137µs
  ├─ sem_post(&di.r.relsem)           → 放行主线程
  ├─ usleep(sleep_usec)               ← primus autosleep
  ├─ glMapBuffer(...)                 ← 等 DMA 落地
  └─ sem_post(&di.d.acqsem)           → 通知 display worker
  │
display worker
  ├─ glTexSubImage2D + glXSwapBuffers(ddpy)  → Intel 上屏
```

### 5.1 一个仍然矛盾的地方

| 量 | 值 |
|---|---|
| 主线程等 readback worker | **29.8 ms** |
| readback worker 的 CPU 时间 | **3.82 ms**（报告 64） |
| readback worker 等主线程 | 22.6 ms（它的 cond_timedwait） |

**readback worker 只干 3.82ms 的活，主线程却等 29.8ms —— 中间差了 26ms。**

候选：
1. **`glWaitSync` 等 GPU 完成要 20+ms**（在 readback worker 的 3.82ms CPU 之外，因为 `glWaitSync` 是阻塞等待，CPU 不占）
2. 或者 `sem_wait/acqsem` 的相位错开

**如果是 1，那 20.4ms 固定项就找到了归属：它等于 `glWaitSync` 的等待时间。**

这与报告 56 的 `T_land = 20.39 + 1.335×MiB` 高度吻合——**20.39ms 固定项就是 GPU 完成 fence 的时间，与传输字节无关。**

**这条需要用 uprobe 直接测 `glWaitSync` 的时长来确认。**

---

## 6. 结论

| # | 结论 | 强度 |
|---|---|---|
| 1 | **唤醒延迟中位 3.0 µs，73.9% <10µs** | **铁证** |
| 2 | 全系统线程都是 2~4 µs → **调度健康** | **铁证** |
| 3 | **「等被调度」这个假设否定** | **铁证** |
| 4 | **⚠️ `swapper` 唤醒 = 跨 CPU IPI + 目标 CPU idle，不是定时器**（纠正误读） | **铁证** |
| 5 | 主线程 **100%** 跨 CPU 唤醒、**0 次** `futex_wake` | **铁证** |
| 6 | 主线程栈在 **primus `glXSwapBuffers` 的 `sem_wait(&di.r.relsem)`** | **铁证** |
| 7 | 主线程长睡眠 **29.8ms ≈ 帧时间**，占 off-CPU 的 99.2% | **铁证** |
| 8 | **此前称"渲染线程"实为 `readback_work`**（命名纠正） | **铁证** |
| 9 | **主线程等 readback worker 29.8ms，而它只干 3.82ms 活 → 缺口 26ms 应在 `glWaitSync`** | **强（推断）** |

---

## 7. 下一步（预期能定案）

| 选项 | 内容 | 代价 | 回答 |
|---|---|---|---|
| **A（推荐）** | **uprobe 直接测 readback worker 的 `glWaitSync` 时长**：在 primus 里 `glWaitSync` 调用前后打点（源码 566 行） | 一次 uprobe trace | **20.4ms 固定项是不是 `glWaitSync` 等 GPU** ← 如果是，案子本质上收口 |
| B | 测 `glReadPixels` → `sem_post(relsem)` 的间隔，确认主线程等的到底是 waitSync 还是 readPixels | 同一次 trace 可兼得 | 同上 |
| C | 用 `sched_waking` 抓主线程唤醒的**源 CPU**，看 waker 是哪个 tid | 一次 trace | 确认放行人确实是 readback worker |

**建议 A**。因为它直指报告 56 那个 20.39ms 固定项，而这是唯一还没被直接测量过的量。

### 7.1 如果 A 命中

那整条链就闭合了：

```
primus glXSwapBuffers 里 sem_wait 等 29.8ms
  → readback worker 卡在 glWaitSync
    → GPU 完成这一帧的 fence 要 20.4ms
      → 而 GPU util 100% / 55W / 满频 = 空转等（报告 07）
        → 「为什么 GPU 要 20.4ms 才完成」= 最终问题
```

**那时问题会收敛到「GPU 侧的完成延迟为什么是 20.4ms」，而不再是"用户态谁在等"。**

---

## 8. 产物

```
analysis/probes/n77_wakeup.sh     perf -p 版（漏 97% wakeup，作废）
analysis/probes/n77_all.sh        perf -a 全系统版（成功）
analysis/probes/n78_main.sh       主线程 wchan + 全系统 sched
analysis/probes/n78_bt.sh         14 份 gdb 栈
analysis/probes/n78_waker.sh      sched_wakeup 的内核栈（★ 定稿关键）
analysis/n64/n77_*.py             N77 分析链
analysis/n64/n78_an.py            主线程 off-CPU + 唤醒者
analysis/n64/n78/off.py           栈帧→库内偏移
analysis/n64/n78/wk_an.py         唤醒栈的关键函数统计
analysis/traces/{n77,n78}/        原始数据
```

### 8.1 环境坑（本轮共三个，都花了时间）

| 坑 | 表现 | 根因 | 解法 |
|---|---|---|---|
| **`perf -p PID` 采 `sched_wakeup` 漏 97%** | 切出 1236 次，只记录 38 次 | `-p` 按事件发生的 `common_pid` 过滤，**跨 CPU/其它上下文触发的被丢** | 用 `-a` 全系统采，事后按 pid 筛 |
| **`perf record` 不加 `-p` 也不加 `-a`** | 只有 3 行 | 默认只采当前进程（`sleep`） | 全系统必须显式 `-a` |
| 正则分组号错 / `int(str)` 比较 | 全 0 或 ValueError | `\S+` 先吃了 comm，`(\d+)` 才是 tid | **先 `head` 看实际格式，再写分组** |

### 8.2 方法论（四条，已入技能库）

1. **★★ `sched_wakeup` 的 `common_pid == 0`（`swapper`）不代表定时器超时。**
   本例栈是 `sched_ttwu_pending + flush_smp_call_function_queue + do_idle`
   = **另一个 CPU 发起唤醒，目标 CPU 当时 idle**。
   **判据：必须看唤醒栈，不能靠 `common_pid` 猜。**

2. **★ 跨 CPU 唤醒的栈上看不到原始 waker。**
   本例主线程 100% 跨 CPU、0 次 `futex_wake`；渲染线程 9% 同 CPU（能看到 `do_futex`）。
   **要看源必须用 `sched_waking`（在源 CPU 触发），或在源码侧打 uprobe。**

3. **★ 用"ioctl 最多的 tid"定位到的可能是 worker，不是主线程。**
   本例主线程 = PID，而 NVIDIA ioctl 最多的是 `readback_work`。
   **判据：先看 `PID` 是不是也在那个 ioctl 列表里；若不在，那个 tid 是 worker。**

4. **`sem_wait` 与 `sem_timedwait` 在 libc 里共享实现，栈上可能显示后者。**
   glibc：`sem_timedwait`@0x9a4b0、`sem_wait`@0x9a6a0，两者范围相邻且共享代码。
   **别因为栈显示 `sem_timedwait` 就断言"有超时"——要看调用点的反汇编是不是 `call sem_wait@plt`。**
