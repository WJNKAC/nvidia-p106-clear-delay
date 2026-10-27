# 24 · signal 来源定位 + 相位检验：525 的节拍与 vblank 无关，不是"等 2 拍"

> 2026-10-25 16:05
> 前提：报告 23 把等待点定位到 `wait_ms(cond, 100ms)`，推翻"1ms 轮询"叙事。
> 本文按计划做第 2 步（抓 signal 来源）并追加一个决定性实验（相位检验）。
> **五条结论，第 5 条推翻了"半速 vsync 锁"模型：**
> 1. **主线程不用 `pthread_cond_wait`**（0 次），用**裸 futex** `FUTEX_WAIT_BITSET_PRIVATE|CLOCK_REALTIME`。
> 2. **主线程等的 uaddr 在 `[heap]`**，每次 swap 等一次（94 次 = 94 次 swap），**94/94 全由同一个 glcore worker 线程唤醒**。
> 3. **两个 glcore worker 形成环形握手**（14385 ↔ 14386），握手率 ≈87/s，远高于帧率 31/s → **它们在空转，不是算力瓶颈**。
> 4. **signal/broadcast 的来源**：glcore 的 notify 函数 `0x146c680`（82%）、Unigine 线程池（4 个 cond）、libxcb/libX11（X 连接）。**没有 primus 发出的 signal。**
> 5. **唤醒时刻的 vblank 相位完全均匀**（8 桶各 11.5~14.4%，标准差 = 28.1% ≈ 均匀的 28.9%）
>    → **节拍与显示刷新无关。525 不是"每帧等 2 个 vblank"，而是"等一个异步完成事件，其耗时≈1.76 拍且随机抖动"。**
>
> **另有 §5.3 的佐证**：GPU 满频 1911MHz、util 100%，但功耗仅 48~64W、温度 41°C（515 同条件 116W）
> → **util 100% 是"有 kernel 在跑"，不是算力饱和；瓶颈在同步，不在计算。**

---

## 1. 仪器：三个语法坑（都用实测确认了）

| 坑 | 现象 | 正确写法 |
|---|---|---|
| **uprobe fetch 不支持 `$arg1`** | `echo 'p:x libc:0x914f0 cond=$arg1' >> uprobe_events` → `I/O error`，事件注册失败 | 用**寄存器名** `%di`；64 位要带宽度 `%di:u64` |
| **libc 的 cond 符号是版本化的** | `p:sig libc:pthread_cond_signal` 失败 | 用**文件偏移**：`pthread_cond_signal@@GLIBC_2.3.2` = `0x91880`（633B 实现）/ `0x90730`（101B 旧 shim）；`broadcast` = `0x914f0` / `0x90680` |
| **`sys_enter_futex` 的实际打印格式** | 用 `uaddr=0x..., op=0x...` 匹配不到任何行 | 实际是 `sys_futex(uaddr: 7fe57c000d84, op: 81, val: ..., uaddr2: ..., val3: ...)` —— **事件名 `sys_futex`，值是无前缀十六进制** |

> `op: 81` = 0x81 = `WAKE|PRIVATE`；`op: 189` = 0xbd = `WAIT_BITSET|PRIVATE|CLOCK_REALTIME`。
> 核对方法：`op & 0x7f` 是基操作，`& 0x80` 是 PRIVATE，`& 0x100` 是 CLOCK_REALTIME。

---

## 2. 主线程到底在用哪个原语

报告 23 的 gdb 栈显示主线程 `glXSwapBuffers → libc`，我误以为是 `pthread_cond_wait`。用 uprobe 挂上去数：

| 探针 | 事件数（3s） |
|---|---|
| `pthread_cond_wait`（0x90830） | **0** |
| `pthread_cond_timedwait`（0x907a0） | 863（全部来自 `glcore+0x146d340` = wait_ms） |

**主线程不用 cond 变量，用裸 futex。** 与 strace 一致：

```
futex(uaddr, FUTEX_WAIT_BITSET_PRIVATE|FUTEX_CLOCK_REALTIME, 0, NULL, FUTEX_BITSET_MATCH_ANY)
```

`timeout=NULL` + `BITSET_MATCH_ANY` → 无限等待，等一个 `FUTEX_WAKE`。

---

## 3. signal 来源：glcore 自己，不是 primus

按 cond 地址分组（完整 maps 归属，`glcore 段内 + 0x2fc000 = vaddr`）：

| cond | 次数(3s) | 事件 | 来源 | vaddr |
|---|---|---|---|---|
| `0x7f358c000da0` | 2379 | **broadcast** | **libnvidia-glcore** | `0x146c6aa` |
| `0x7f358c000d60` | 607 | **broadcast** | **libnvidia-glcore** | `0x146c6aa` |
| 4 个连续 cond | 各 908 | signal | libUnigine_x64 | `0x1cade2` |
| `0x1b17f730` | 564 | signal | libxcb | `0xea39` |
| `0x1b17f4e0` | 376 | broadcast | libX11 | `0x464a6` |

**没有 primus 发出的 signal。**

`0x146c6aa` 属于 `0x146c680`（64 字节），就是一个 **notify 函数**：

```asm
146c680: lea 0x30(%rdi),%r12        ; r12 = ctx+0x30（mutex/cond 起始）
146c691: call *(2a68680)            ; 槽+0x160 = lock
146c6a4: movb $0x1,0x58(%rbp)       ; ctx+0x58 = 1  ← 置完成标志
146c6a8: call *(2a68630)            ; 槽+0x110 = signal/broadcast  ← 82% 的 broadcast 来自这里
146c6be: jmp  *(2a68670)            ; 槽+0x150 = unlock
```

它的调用者只有两个：`0x146d3a0` 与 `0x146ec00`（报告 23 定位的 1000×2ms 校准循环）。
→ 说明这对 cond（`0x...d60` / `0x...da0`，相差 0x40）就是**两个 GPU context 的完成队列**。

---

## 4. 唤醒链：主线程 ← 一个 worker ← 另一个 worker（成环）

按 (uaddr, tid) 精确配对，主线程 tid = PID：

```
L0: 主线程 14350 等 uaddr 0x8221d8（94 次）← 94 次由 tid 14385 唤醒（100%）
L1: tid 14385 等 0x7f69dc000d44（162 次）← 119 次由 tid 14386 唤醒
L2: tid 14386 等 0x7f69dc000d80（141 次）← 141 次由 tid 14385 唤醒
    （14385 已在链上 → 形成环）
```

- `0x8221d8` 在 **[heap]**（`maps` 显示 heap 段内 `+0x13b1d8`）→ 无法从地址反推库，是进程内分配的同步对象
- 14385/14386 等的地址在 glcore 的匿名 arena（`0x7f69dc000d40~d84`）
- **环形握手率 ≈ (119+141)/3s ≈ 87/s**，而 swap 只有 31/s → **worker 之间的对齐比帧率快得多，它们在空转等外部事件**

---

## 5. 决定性实验：唤醒时刻与 vblank 无关

如果 525 真的"每帧等 2 个 vblank"，那唤醒时刻应当**锁在 vblank 网格上**（相位集中）。
实测（10s，340 次唤醒，597 个 vblank，周期 16.660ms）：

```
相位直方图（8 桶，均匀应各约 12.5%）:
  [ 0.00- 2.08)   39  11.5%  #####
  [ 2.08- 4.17)   41  12.1%  ######
  [ 4.17- 6.25)   43  12.6%  ######
  [ 6.25- 8.33)   49  14.4%  #######
  [ 8.33-10.41)   47  13.8%  ######
  [10.41-12.50)   39  11.5%  #####
  [12.50-14.58)   41  12.1%  ######
  [14.58-16.66)   41  12.1%  ######

相位标准差 4.68ms = 周期的 28.1%     （均匀分布理论值 28.9%）
```

**完全均匀，与 vblank 无任何锁定关系。**

### 5.1 这推翻了什么

报告 16 提出的"**30fps = 半速 vsync 锁**（每帧吃 2 个 vblank）"，其物理含义是"每帧都错过一次 vblank 截止"。
若是那样，唤醒必然锁在 vblank 相位上。**实测不锁。**

正确描述应改为：

> **525 每帧在等一个异步完成事件（GPU 完成），这个事件耗时 ≈ 29.4ms ≈ 1.76 个 vblank，且逐帧抖动
> （报告 22 的三峰：1.5× / 1.75× / 2.0× 拍）。因为耗时不是 vblank 的整数倍，相位才会均匀漂移。**

### 5.2 与"每帧 0.06 次中断"的一致性

报告 23 实测 `nvidia` IRQ 只有 2.0/s（帧率 34.7fps）。
→ 驱动不靠中断，靠**轮询 host 内存标记**。
→ 那么"完成事件耗时"应该由**标记被写入的时刻**决定，而 GPU 写标记的时刻由 GPU 队列排空决定，与显示刷新无关。

**两条独立观测（相位均匀 + IRQ 极低）指向同一结论：瓶颈在 GPU 侧的异步完成，不在显示刷新。**

### 5.3 GPU 利用率 100% 是假象：满频但功耗只有一半、温度 41°C

连续 10 秒采样（525，primus，vsync off，稳态）：

| 项 | 值 |
|---|---|
| `utilization.gpu` | **100%**（10/10 次） |
| **`clocks.sm`** | **1911 MHz（满频）** |
| `clocks.mem` | 4006 MHz（满频） |
| `power.draw` | **48 ~ 64 W**（515 同条件 116 W） |
| 温度 | **41~42 °C** |
| `pstate` | P0 |

**满频 + 100% util + 一半功耗 + 41°C。**

41°C 这个温度本身就说明问题：如果真有 100% 算力负载，功耗不会只有 55W、温度不会这么低。

`utilization.gpu` 的定义是"**至少有一个 kernel 在 GPU 上运行的时间比例**"，
**它不衡量算力饱和**。所以：

> **525 下 GPU 满频运行着一批低功耗的等待型 kernel，绝大部分时间在轮询/等待，不是在算。**

这同时解释了 §5.3 提出的三重矛盾（100% util + 功耗一半 + 完成晚 1.76 拍）：
**没有真正的计算瓶颈，慢的是同步环节。**

---

## 6. 与用户判读表的对照

用户给的第 2 步判读表：

| signal 栈顶 | 判读 | 实测 |
|---|---|---|
| glcore/RM 帧 | 驱动内部 signal | **✔ 命中**：82% 的 broadcast 来自 glcore `0x146c6aa` |
| loader_dri3_swap_buffers_msc（Mesa） | Mesa 呈现路径 | ✘ 未观测到 Mesa 的 signal（Mesa 只是报告 22 里那个 `clock_nanosleep` 的来源） |
| primus 内部 | primus 设计行为 | ✘ 无 primus 的 signal |
| 无 signal（100ms 超时） | 与 23.7ms 矛盾 | ✘ signal 大量存在，且 worker 的 wait 是被正常唤醒的 |

**但"驱动内部 signal"这一格需要更精确的解读**：
signal 确实来自 glcore，但它是 **glcore 的 notify 函数在通知它在等的 context**，
而**主线程等的那个 heap 上的 futex 由 worker 14385 唤醒**——
即 **glcore worker 是"中转站"：它收 glcore 内部的通知，再转告主线程**。

所以链条是：

```
GPU 写完成标记（异步，时刻不锁 vblank）
   ↓ 驱动轮询内存标记发现（IRQ 仅 2/s）
glcore notify 函数 0x146c680 → broadcast 两个 context 的 cond
   ↓
worker 14385（收 cond）→ futex_wake(主线程的 heap uaddr)
   ↓
主线程 glXSwapBuffers 返回
```

---

## 7. 下一步

| 优先级 | 动作 | 能答什么 |
|---|---|---|
| **1（首选）** | **完成标记往返法**（报告 10 的方法）：在 525 上找 host 内存里的完成标记，高频轮询测"GPU 写入时刻"与"驱动清零时刻" | 把 29.4ms 拆成"GPU 真完成" vs "驱动察觉延迟"。报告 10 在 580 上得 **2.64ms / 30.24ms** |
| 2 | 找 14385/14386 是哪个线程（`prctl` 名字全是 `heaven_x64`，可用 uprobe 栈定位其在 glcore 的函数） | 确认它们是 GPU 提交线程还是纯同步线程 |
| 3 | 读 primus 源码看 `glXSwapBuffers` 在 heap uaddr 上等的语义 | 确认主线程等的是"buffer 可重用"还是"渲染完成" |
| ~~4~~ | ~~验证 GPU 利用率 100% 是否假象~~ **已做（§5.3）**：满频 1911MHz 但只有 48~64W、41°C → **util 100% 是"有 kernel 在跑"，不是算力饱和** | — |

> **§5.3 的结果让第 1 项更有针对性**：既然 GPU 没有算力瓶颈，
> 那 29.4ms 就应当拆成"GPU 完成一个轻量 kernel 花多久" + "驱动多久才发现"。
> 报告 10 在 580 上量到的是 **2.64ms vs 30.24ms**——如果 525 也是这个比例，
> 那么"回归"就在**驱动察觉环节**，与报告 18/19 缩小的代码范围完全吻合。

---

## 8. 产物

```
analysis/traces/t6/
├── t6_signal.trace            首轮（含全系统噪声）
├── t6_signal_stack.trace      stacktrace 选项（只有内核栈，无效）
├── t6_cond.trace              cond 地址分组
├── t6_pairship.trace          wait/signal 配对（发现 cond_wait=0）
├── t6_fp2.trace + .maps       futex 配对（主数据）
└── t6_phase.trace + .maps     相位检验（决定性）

analysis/probes/
├── t6_signal.sh               signal/broadcast uprobe（符号名→偏移的修正）
├── t6_signal_stack.sh         pid filter + stacktrace
├── t6_dwarf.sh                perf dwarf 尝试（事件名不符，未成功）
├── t6_cond.sh                 cond 地址分组 + maps 归属
├── t6_pairship.sh             wait/signal 配对
├── t6_futex_pair.sh           sys_futex uaddr 配对（含 op 解码）
├── t6_tidpair.py              按 tid 精确配对
├── t6_chain.py                链式追溯（发现环形握手）
└── t6_phase.sh                相位检验（本轮决定性实验）
```
