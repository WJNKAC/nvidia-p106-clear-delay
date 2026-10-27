# 23 · 在 525 上顺栈下挖：等待点落在 glcore 命令队列 worker，不在 vblank / DRM

> # ⚠️ 一处归因错误已被报告 32 修正（2026-10-25 21:40）
> 本文 §6.2 写"唤醒者是**驱动内部**的 1ms 定时轮询线程"。**错了。**
> 实测：那个 1ms `usleep(1000)`（周期 1091µs）来自 **`libUnigine_x64.so+0x26a7fd`**，是 **Unigine 引擎自己的线程**，
> 与 nvidia 驱动无关。而 glcore 的 4 个 `nanosleep@plt` 调用点（0x146d742 / 0x19499f7 / 0x194e04c / 0x194e060）
> 在真实场景下**命中全部为 0**。
>
> 另外，报告 32 用 futex 配对确认：map 的 3ms 里 98.3% 睡在 futex 上，
> 且**全进程 ioctl 只有 0.4% 的窗口有活动、>1ms 的 ioctl = 0%** → **nvidia 内核驱动完全没参与**。
> 唤醒链是 **tid 5609 ↔ 5610 两个用户态线程互相握手成环**（uaddr 相差 0x40，同一结构体）。
>
> **仍然有效**：§3 的等待点指令级定位（`0x123b314: call *0x2d0(%rbp)` = `wait_ms(cond,100ms)`）、
> §3.4 的"超时 100ms 从未到期、是被 signal 唤醒"、§5 的启动校准（1000×2ms，解释首次启动 18s）。

> 2026-10-25 15:35
> 前提：报告 22 已把粒度做到"主线程等同一个 futex、有 1ms 轮询线程"。本文按"不换驱动、在 525 上动态定位 + 515 静态对比"的路线继续。
> **五条新结论（第 5 条推翻了前两份报告的"轮询"表述）：**
> 1. **主线程等的是 primus 的 worker，不是 vblank、不是 DRM 事件**——栈是 `glXSwapBuffers(primus) → libc cond_wait`，中间没有 glcore；且 `poll` 在主线程上无任何 >1ms 调用。
> 2. **glcore worker 的等待点已定位到指令级**：`0x123b314: call *0x2d0(%rbp)` = `wait_ms(cond, ms)`，紧接一个**等待次数计数器** `ctx+0x8e0d24`。
> 3. **525 把时间换算从 ns 精度改成了 ms 精度**：`imul $0x10624dd3`(÷1000) 在 525 有 3 处、**515 有 0 处**、**580 有 0 处**；515 用 `1e9` 做 ns 换算（16 处）。
> 4. **525 有一套启动校准**：`pthread_once` → 1000×`wait_ms(cond,2ms)` → 把实测均值写入全局 `0x2a68f0c`。这解释了 525 首次启动 18s vs 515 的 8s。
> 5. **超时值是 100ms，不是 1ms**（§3.4 实测 2456/2456 次 `esi=100`）。
>    → **worker 是被 signal 正常唤醒的，不是超时轮询。** "1ms 轮询"这个描述必须修正：
>    那 1ms 来自 **Mesa 的 DRI3**（§6），不是 glcore。

---

## 0. 最重要的一条修正：这不是"轮询"，是"等一个来晚了的 signal"

§3.4 的实测（2456 次断点命中，`esi` 全为 **100**）说明：

```
worker: wait_ms(cond, 100ms)   ← 上限 100ms
实际等待: 23.7ms（525）/ 10.8ms（515）
→ 在 100ms 到期之前就被 cond_signal 唤醒
```

**这彻底改变了归因方向：**

| 原表述（报告 22/23 初稿） | 修正后 |
|---|---|
| "1ms 定时轮询，525 多转 16 圈" | **错**。超时 100ms，从未到期；等待时长 = **signal 到达的时刻** |
| "轮询间隔决定唤醒延迟" | **错**。没有轮询，是被唤醒 |
| "525 判定周期更长" | **对，但原因不是周期**——是**发出 signal 的一方晚了一个 vblank** |

**所以真正要问的问题变成：谁该 signal 这个 cond？它为什么晚 16ms？**

结合报告 10（580 上"HOST-GPU 完成标记 2.64ms 就写好，但驱动 30.24ms 后才发现"），
最一致的解释是：**GPU 完成通知到达驱动的延迟，在 525 上变成了 ~16ms（一个 vblank）。**
本报告尚未直接观测到这一点，是下一步的核心任务。

---

## 1. 为什么 fp 模式抓不到栈，改用 gdb

`perf record --call-graph fp` 对 glcore 完全失效（栈帧全是 `[unknown]`）——**glcore 没有帧指针**。
改用 gdb 批量 attach（5 次采样，每次 `thread apply all bt 12`），靠 `.eh_frame` 展开，成功。

> 代价：gdb attach 会短暂暂停进程。每次 attach→bt→detach 约 0.1~0.3s，5 次采样对 23ms/帧 的稳态有扰动，
> 但**只用于取栈、不用于取时序**，所以可接受。

---

## 2. 主线程在等什么：等 primus 的 worker

5 次采样里主线程（`Thread 1`，LWP == PID）有 4 次在同一位置：

```
#0  libc  (futex)
#1  libc
#2  libc
#3  libc
#4  glXSwapBuffers () from /usr/lib/x86_64-linux-gnu/primus/libGL.so.1
#5  libUnigine_x64.so
#6  libUnigine_x64.so
#7  0x400755 (main)
```

**关键：`glXSwapBuffers` 之下、之上一层 glcore 帧都没有。**
而报告 22 的 strace 显示主线程的阻塞全是 **futex**、`poll` 无 >1ms 调用。

两条独立证据合起来：

| 候选 | 是否成立 |
|---|---|
| 等 vblank | ✗ vblank 要 `poll`/`ioctl`（DRM） |
| 等 DRM 事件 | ✗ 同上 |
| 等 GPU 中断 | ✗ 同上 |
| **等进程内 worker** | **✓ futex + 栈在 primus 内部** |

→ **主线程卡在 primus 自己的线程同步上，等它的 worker 把活干完。**

---

## 3. glcore worker 的等待点（指令级）

worker 线程栈（`Thread 5`/`Thread 11`，comm 都是 `heaven_x64`）：

```
#0-2  libc
#3    pthread_cond_timedwait
#4    glcore+0x146d340      ← wait_ms 函数内（146d280-146d39a）
#5    glcore+0x123b31a      ← 调用者（命令队列处理器 123ab90-123b4dd，2381B）
#6    glcore+0x14702fa      ← 线程入口 wrapper（末尾调 dlerror 清理）
```

### 3.1 `0x146d280` 是 `wait_ms(cond, ms)`

```asm
146d291: imul $0x10624dd3,%r13,%r13   ; ÷1000  → 秒
146d29b: imul $0xf4240,%ebp,%r14d     ; ×1e6   → ns
146d301: call gettimeofday@plt
146d30b: add (%rsp),%rdx              ; now.tv_sec + ms/1000
146d318: add 0x8(%rsp),%rax           ; now.tv_usec*1000 + (ms%1000)*1e6
146d321: cmp $0x3b9ac9ff,%rax         ; 归一（>1e9 则进位）
146d338: call *(%rax)                 ; pthread_cond_timedwait
```

**语义确认：`wait_ms(cond, ms)`，用 ms 精度构造绝对超时。**

### 3.2 调用点与判定条件（`0x123b2f0` ~ `0x123b33a`）

```asm
123b2f0: cmpb $0x0,0x17(%rsp)        ; 条件 A
123b2f5: jne  123b309
123b2f7: mov  0x2c(%rsp),%eax        ; 条件 B
123b2fb: test %eax,%eax
123b2fd: jne  123b32f
123b2ff: cmpb $0x0,0x8e0dc8(%r15)    ; 条件 C（ctx 标志）
123b307: jne  123b32f
123b309:  mov 0x8e0950(%r15),%rdi     ; cond = ctx+0x8e0950
123b310:  mov 0x28(%rsp),%esi         ; ms = 栈变量
123b314:  call *0x2d0(%rbp)           ; ← wait_ms  [vtable 槽 _nv028glcore+0x2d0]
123b31a:  test %eax,%eax              ; 返回值
123b322:  addl $0x1,0x8e0d24(%r15)    ; ← **等待次数计数器 ++**
123b32f:  mov $0x4,%edi
123b334:  call *0x280(%rbp)           ; 另一分支：调 [槽+0x280]，参数 4
```

### 3.3 这个 worker 是命令队列处理器

同一函数里的主循环（`0x123b288`）：

```asm
123b2aa: mov  0x8e0988(%r13),%rax    ; 队头指针
123b2b1: mov  (%rax),%eax            ; 读命令状态字
123b2b3: test $0x10,%ah              ; 检查 0x1000 位（未完成标志）
123b2b6: je   123b288
123b288:   and $0x7ff,%eax           ; opcode
123b293:   call *(%r14,%rax,8)       ; ← 按 opcode 走跳转表分发
```

**队头命令的 0x1000 位置位 = 还没完成 → 走 `wait_ms` 等；清掉 → 继续分发下一条。**

### 3.4 实测：超时值 100ms，且有两个 context 在等

在 `0x123b314`（`call *0x2d0(%rbp)` 前）下断点，10 秒采样：

| 项 | 实测 |
|---|---|
| 断点命中 | **2456 次**（≈245 次/秒；34.7fps 下约 **7 次/帧**） |
| **`esi`（ms 超时）** | **2456/2456 次全部 = 100** |
| `r15`（ctx） | **两个不同值**：`0x7f0bcc000d40`（982 次）、`0x7f0bf991e040`（1474 次） |

**两条推论：**

1. **等待不是超时到期，是被 signal 唤醒**（100ms ≫ 实际 23.7ms）。
   → 报告 22 §6.2 的"1ms 定时轮询驱动"和 §8 的"多转 16 圈"表述**作废**（详见 §0）。
2. **有两个 context 在各自等**，且次数比约 1:1.5。
   说明这条等待不是单一 GPU 上下文，而是"每 context 一个 worker 都在等"。
   （`ctx+0x8e0d24` 计数器两次读取均为 0，未读到有效增长——该偏移可能不属于此 ctx，或计数在别处累加，**这一点未解决**。）

> 每次命中的断点开销会拖慢进程，所以 **245 次/秒不能当作精确频率**，只能作为量级参考。
> 但 `esi` 的取值不受影响（它是调用约定决定的入参）。

---

## 4. 静态对比：525 的时间处理方式与 515 不同

用 `÷1000`（ms→s，构造 timespec 的必经步骤）做指纹：

| 版本 | `imul $0x10624dd3` | `imul $0xf4240` | `$0x3b9aca00`(1e9, ns 归一) |
|---|---|---|---|
| **515.105.01** | **0** | 4 | 16 |
| **525.116.03** | **3** | 3 | 14 |
| **580.178.04** | **0** | 4 | — |

525 的 3 处：
- `0x146d291` — `wait_ms(cond, ms)`
- `0x146d6d7` — **`sleep_ms(ms)`**（纯 nanosleep，515 里没有对应物）
- `0x17c69ec` — 第三处（在 present barrier 管理函数 `0x17c4e40` 的邻近区域，852B 函数）

**判读**：525 引入了 **ms 精度的超时抽象**（`wait_ms` / `sleep_ms` 两个封装）；
515 有 ns 精度的时间处理（`1e9` 归一 16 处），但没有这对 ms 封装。

> **谨慎标注**：这不等于"515 不做带超时等待"——三版二进制里都有 `pthread_cond_timedwait`
> 的 dlsym 名字串（各 1 处），说明三版都能做。差异在**封装风格与精度**，是否造成行为差异需实测。
> 而 580 也是 0 处 ÷1000，说明这是 **525 分支窗口特有的写法**，不是从 525 之后一路延续
> （与 `RMForcePstateBug3811584War` 同样的分布，进一步支持"525 是特殊的一代"）。

---

## 5. 副产品：525 有一套启动校准，解释"首次启动 18s"

`0x146d760`：

```c
pthread_once(&once_ctl_0x2a6880c, calibrate_0x146ec00);   // 只跑一次
while (!calib_done_0x2a68f10) sleep_ms(1);                // 未完成则 1ms 轮询
*out_flag = calib_flag_0x2a68f08;
return calib_value_0x2a68f0c;
```

而 `calibrate_0x146ec00`：

```asm
146ec67: mov $0x3e8,%ebx          ; 循环 1000 次
146ec87: mov $0x2,%esi            ; 每次 wait_ms(cond, 2ms)
146ec8c: call 146d280
...
146ecba: sub 0x10(%rsp),%rax      ; 结束 - 开始
146ecc5: cvtsi2ss / divss ×2      ; 算平均
146ece8: movss %xmm0,0x2a68f0c    ; 存校准值
146ed0d: movb $0x1,0x2a68f10      ; 置完成标志
```

**1000 × 2ms = 2 秒**（另有 `0x146d3a0` 也是 1000×2ms，可能是第二轮）。

→ 这解释了报告 22 §1.1 的现象：**525 首次启动要等 18s 才进稳态，而 515 只要 8s。**
→ 也解释了报告 22 里为什么第一次采集（固定 `sleep 22`）会采到 LOADING 期。

**方法论收益**：以后换版本测 525，稳态门的超时必须放宽到 ≥30s，或直接用 GPU util 门（已实现）。

---

## 6. 一个必须修正的前一轮表述

报告 22 §6.2 写"唤醒者是**驱动内部**的 1ms 定时轮询线程"。
本轮栈显示：`clock_nanosleep` 出现在 **libgallium 的 `loader_dri3_swap_buffers_msc`** 里（Thread 7/8/9），
那是 **Mesa 的 DRI3 present 等待**，不是 glcore 的。

修正为：
- **glcore 侧**：`sleep_ms(1)` 只在启动校准期间用（§5）
- **Mesa 侧**：稳态下的 1ms `clock_nanosleep` 来自 `loader_dri3_swap_buffers_msc`

**两套 1ms 是不同来源，报告 22 把它们混为一谈了。**

这带来一个重要的新问题：**primus 最终是通过 Mesa 的 DRI3 present 上屏的。**
那么 525 慢的原因可能在"primus 何时把 buffer 交给 Mesa"，而不在 NVIDIA 的上屏路径。

---

## 7. 一个未完成的支线：同步抽象表的槽位解析失败

想把 `_nv028glcore+0x100/+0x108/+0x150/+0x160`（wait/signal/lock/unlock）各自对应到具体函数，
以便在 signal 上打 uprobe。**失败**，原因记录如下，供下次避坑。

### 7.1 表基址推算是对的

从 `0x146d2b6: lea 0x15fb3c3(%rip),%rax # 2a68680 <_nv028glcore@@Base+0x160>`
反推 `_nv028glcore = 0x2a68520`（`0x2a68680 - 0x160`），落在 `.bss`（`0x2a3bb40` + `0x40c28`）内 ✓

### 7.2 但读出来的值落在 `.rodata` 而不是 `.text`

运行时读 `base+0x2a68620`（= `_nv028glcore+0x100`），得 `0x1ee9c7a0`（相对基址）。
而 525 的 section 布局：

```
.text          0x2fc000 - 0x1951ee1
.rodata        0x1952f00 - 0x23f8685   ← 读出的值落在这里
.eh_frame_hdr  0x23f8688
...
.bss           0x2a3bb40 - 0x2a7c768
```

**一个会被 `call *(%rax)` 直接调用的槽，不可能指向 `.rodata`。**

### 7.3 根因：glcore 是多段映射，`base + vaddr` 在段边界处错位

```
7f0bcc800000-7f0bccadb000 r-xp offset 0        ← 段1
7f0bccadb000-7f0bccafc000 rwxp offset 0x2db000
7f0bccafc000-7f0bcf001000 r-xp offset 0x2fc000  ← 真正的 .text
7f0bcf001000-7f0bcf002000 ---p offset 0x2801000
7f0bcf002000-7f0bcf1cd000 r--p offset 0x2801000
7f0bcf1cd000-7f0bcf23c000 rw-p offset 0x29cc000  ← .data/.bss
```

`.text` 段的 offset（0x2fc000）与 vaddr 相同，所以 `base+0x123b314` 这类**代码地址**是对的
（报告 §3.4 的断点确实命中了）。但 `.bss` 段 offset（0x29cc000）与 vaddr（0x2a3bb40）**不等**，
差约 0x6ff40 → 用 `base + vaddr` 去读 `.bss` 会落到错误的页。

**下次正确做法**：不推算 base，而是**按 maps 条目逐段换算**
`runtime_addr = map_start + (vaddr - map_vaddr)`，其中 `map_vaddr` 由该条目的 file offset 反推
（对 offset==vaddr 的段直接相等；对 `.data`/`.bss` 段要用 `map_start - offset + vaddr` 的等价形式验证两次）。

---

## 8. 零成本交叉验证：nvidia IRQ 只有 2 次/秒

报告 10 在 580 上用过这个判别（"完成率 30/s 而 IRQ 只有 1.8/s → 标记是 GPU 直接写内存，中断路径被排除"）。
在 525 上重做：

```
nvidia IRQ: 9363 -> 9383   差 20 次 / 10.01s = 2.0/s
  若 34.7 fps → 每帧 0.06 次中断
```

| 观测 | 含义 |
|---|---|
| **IRQ 2.0/s，帧率 34.7fps** | **驱动不靠 GPU 中断获知完成** |
| 每帧 0.06 次中断 | 完成是靠**轮询 host 内存标记**发现的（与报告 10 在 580 上的结论一致） |

**这条把"什么时候发现完成"的责任完全推给了用户态的轮询逻辑**——
既然是轮询，那"晚了 16ms"就只能是：
1. 轮询间隔太长（但实测 1ms），或
2. **轮询读到的标记本身晚了 16ms（GPU 真的 16ms 后才写）**

而 `wait_ms` 的超时是 100ms、且被 signal 唤醒（§3.4），说明轮询线程发现标记后立即 signal 了。
→ **嫌疑重点转向"GPU 何时真正写完标记"。**

这正是报告 10 的 E2（完成标记往返法）要测的量：在 580 上得到 **GPU 2.64ms 完成 / 驱动 30.24ms 才收到**。
**525 的 23.7ms 与 580 的 30.24ms 是同一量级，很可能是同一现象。**

---

## 9. 产物

```
analysis/traces/t5/
├── gdb_stacks.txt              5 次全线程栈采样（1809 行）
├── cg_clock_nanosleep.data     perf fp 模式（失败，仅存档）
└── cg_futex.data               同上

analysis/diffs/glcore-swap/
├── t5_146d340.func.asm         wait_ms(cond, ms)  完整实现
├── t5f_146d6d7.func.asm        sleep_ms(ms)
├── t5f_17c69ec.func.asm        第三处 ÷1000（barrier 邻近区域，852B）
├── t5g_worker.func.asm         命令队列处理器（2381B，含等待点）
└── t5_14702fa.func.asm         线程入口 wrapper

analysis/probes/t5c_callstack.sh   perf tracepoint + call-graph（fp 对闭源库失效）
analysis/probes/t5d_gdbstack.sh     gdb 批量全线程栈采样（有效路线）
analysis/probes/t5b_wakeup.sh/.py   sched_wakeup + stacktrace
```

---

## 10. 下一步（都在 525 上，不换驱动）

| 优先级 | 动作 | 能答什么 | 判据 |
|---|---|---|---|
| **1（最短闭环）** | **GPU 完成标记往返法**（报告 10 的方法，在 525 上重做）。若嫌找标记地址麻烦，可先跑 E1 的 CUDA 空转往返（不依赖标记地址） | **GPU 何时真正完成 vs 驱动何时察觉**。报告 10 在 580 上得 "GPU 2.64ms / 驱动 30.24ms"，525 的 23.7ms 同量级 | E1 <50µs → 通知路径健康，问题在"何时去看"；≥500µs → 通知机制回归 |
| 2 | 按 §7.3 的正确换算重读 vtable，找到 signal 函数后在它上面打 uprobe 取时间戳 | signal 相对 vblank / swap 的时刻。若 signal 及时而 worker 未醒 → futex 唤醒路径问题 | 需先解决多段映射换算 |
| 3 | 抓 `loader_dri3_swap_buffers_msc` 的 `target_msc` 入参 | primus 交给 Mesa 的 present 目标拍（§6 修正带来的新问题） | gdb 断点读入参 |
| 4 | 补 `$stack` 抓 dmabuf 那 48% 重复 swap | 报告 22 §2.4 遗留 | 栈里看是否同一调用点 |

> **优先级 1 是唯一能把"晚了 16ms"钉死在 GPU 侧还是驱动侧的实验。**
> 报告 10 的结论（GPU 快、驱动慢）在 580 上已成立；若 525 也成立，
> 则 515→525 的回归点就是**"驱动察觉完成"的那段逻辑**，而这正是 §3~§4 定位到的 `wait_ms`/signal 路径。
>
> 优先级 2/3/4 都是在 525 上的廉价补充，但优先级 1 的信息量最大。
