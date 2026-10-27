# 65 · N74-A：`uaddr` 归属查清了 —— **不在 primus 的 .bss，在 glcore 的条件变量上**

> 2026-10-26 10:20
> 用户指令：抓渲染线程 futex 的 uaddr，用 `/proc/<pid>/maps` 确认落在 primus 的 `.bss`、glcore 映射还是堆上。
>
> **结论（三档判据全中）**：
> 1. **不在 primus 的 `libGL.so.1` 段** —— 0 条。primus 的 rw 段只有 4KB，且**栈上从未出现 primus**。
> 2. **不在 glcore 的映射段** —— 也在 0 条（glcore 的 rw 段只占 WAIT 的 0%）。
> 3. **★ 97.9% 在匿名大段上，是 glibc 的 malloc thread arena（堆）**；而**等它的代码在 glcore 里**。
>
> **一句话**：**对象在堆上，代码在 glcore 里，等的是 glcore 自己的 `pthread_cond_timedwait`。**

---

## 1. uaddr 归属表（659 条 WAIT）

| 归属 | 条数 | 占比 |
|---|---|---|
| **匿名大段（64.8MB，glibc thread arena）** | **645** | **97.9%** |
| `libc.so.6` 的 rw 段 | 13 | 2.0% |
| `[heap]`（主 arena） | 1 | 0.2% |
| **`primus/libGL.so.1` 的 .bss** | **0** | **0%** |
| **`libnvidia-glcore` 的映射** | **0** | **0%** |

### 1.1 三个主要 uaddr 的完整画像

| uaddr | 次数 | op | val 特征 | 归属 |
|---|---|---|---|---|
| `0x7f9028000dc4` | **301** | `WAIT_BITSET\|PRIV\|REALTIME` | **单调递增 3402..4002** | 64.8MB 匿名段 内 0x100dc4 |
| `0x7f9028000dc0` | **300** | 同上 | 单调递增 3403..4001 | 同一段 内 0x100dc0 |
| `0x3f87e9f0` | 602 | `WAKE\|PRIV` + `WAIT\|PRIV` | val ∈ {1,2} | `[heap]` |
| `0x7f903ee688e0` | 108 | `WAKE\|PRIV` + `WAIT\|PRIV` | val ∈ {1,2} | 260KB 匿名段 |
| `0x7f905a5f1ac0` | 30 | `WAKE\|PRIV` + `WAIT\|PRIV` | val ∈ {1,2} | `libc.so.6` rw |

### 1.2 `val` 递增这个细节是判别关键

**`sem_t` 的 futex val 只有 0/1（信号量计数）**，而实测两个主力地址的 val **从 3402 单调涨到 4002**（跨度正好 ≈ 等待次数）。

这是 **`pthread_cond_t` 的 `__wseq`（等待序号）** 的特征 —— 每次有 waiter 进入，序号 +1。

配合 `op = 0x189 = FUTEX_WAIT_BITSET|FUTEX_PRIVATE_FLAG|FUTEX_CLOCK_REALTIME`，这就是 **`pthread_cond_timedwait`** 的标准签名（不是 `sem_wait` 用的 `FUTEX_WAIT`）。

**分类统计**：

| 类型 | 条数 | 占比 | 对应 |
|---|---|---|---|
| **条件变量型**（val 递增） | **601** | **91.2%** | **`pthread_cond_timedwait`** |
| 信号量/互斥型（val 恒 0/1） | 58 | 8.8% | `sem_wait` / mutex |

**而 primus 源码里只有 `sem_t`（`sem_t acqsem, relsem`），从头到尾没有 `pthread_cond_t`。**
→ **这 91.2% 不可能是 primus 的等待。**

### 1.3 那个 64.8MB 匿名段是什么：glibc 的 thread arena

| 判据 | 结果 |
|---|---|
| 大小 | **66344 KB = 64.79 MB**（glibc `HEAP_MAX_SIZE` = 64MB） |
| 属性 | `rw-p`，`inode 0`，无文件名 |
| `MALLOC_ARENA_MAX=1` 对照 | **该尺寸段消失**（base 有 63.8MB 段，arena1 没有） |

→ **是 glibc 的 per-thread malloc arena。** 所以在它上面分配的 `pthread_cond_t` 是**堆对象**（某个 `malloc` 出来的结构体成员）。

**这解释了一个悖论**：为什么"落在堆上"却能确定是 glcore 的 —— **因为要看的是"谁在等"，不是"对象在哪分配"。**

---

## 2. 抓栈：83% 的样本给出同一条链

12 份 gdb 栈样本中，渲染线程的栈：

| 样本数 | 栈 |
|---|---|
| **10 / 12** | `pthread_cond_timedwait` → **glcore ×3** |
| 1 / 12 | 全在 glcore（抓在 glcore 内部执行中） |
| 1 / 12 | `sched_yield` → glcore ×2 → `libGLX_nvidia` → glcore ×2 → `libGLX_nvidia` |

**primus 的 `libGL.so.1` 在 12 份样本里一次都没出现。**

### 2.1 六份带 maps 的样本，栈完全一致

```
#3  pthread_cond_timedwait    libc.so.6+0x9205d
#4  ??                        glcore+0x146d340     ← 调用点
#5  ??                        glcore+0x123b31a     ← 上一层
#6  ??                        glcore+0x14702fa     ← 线程入口
#7  ??                        libc.so.6+0x928bb    (start_thread)
```

4/6 样本逐帧相同（另 2 份抓在别的时刻）。

---

## 3. ★★★ `0x146d340` 直接接上了报告 45 / 52

### 3.1 偏移算术

| 量 | 值 | 来源 |
|---|---|---|
| `wait_ms` 函数入口 | **`0x146d280`** | 报告 52（vtable 槽 `0x280/0x290` 的等待实现） |
| 本报告栈帧 #4 | **`0x146d340`** | N74 |
| 差 | **`0xC0`** | 同一个函数内部 |

### 3.2 反汇编确认

```asm
146d280: push %r14                    ← 函数入口（报告 52 的 wait_ms）
146d282: push %r13
...
146d322: cmp  $0x3b9ac9ff,%eax        ← 999999999 秒的参数上限检查
146d327: jg   146d360
146d329: mov  %rax,0x8(%rsp)
146d32e: lea  0x15fb2eb(%rip),%rax    # 2a68620  ← vtable 槽
146d335: mov  %rsp,%rdx
146d338: mov  %r12,%rsi
146d33b: mov  %rbx,%rdi
146d33e: call *(%rax)                 ← ★ 经 vtable 调用（= cond_timedwait）
146d340: test %eax,%eax               ← 栈帧 #4 显示的返回地址正是这里
146d342: je   146d2df
```

**`0x146d340` 就是 `call *(%rax)` 的返回地址。** 栈帧里显示的返回地址，正是这条 call 的下一句。

### 3.3 完整链条

```
线程入口            glcore+0x14702fa
  → 命令队列处理器   glcore+0x123b31a     （0x123xxxx 段 = 报告 35/37 的队列处理器区域）
    → wait_ms        glcore+0x146d340     （= wait_ms 内 +0xC0，vtable call）
      → pthread_cond_timedwait            ← 27ms 停在这里
```

**这就是报告 44~55 追了 12 轮的那个 `wait_ms`。**当时追的是另一个线程（20µs 自旋的那个 tid），现在证明：**渲染线程自己也在同一个函数上等。**

---

## 4. 三条判据的完整回答

| 用户判据 | 实测 | 判定 |
|---|---|---|
| 落在 **primus 的 .bss** → 等的是 primus 的 sem | **0 条**；primus rw 段仅 4KB；12 份栈里 primus **从未出现** | **❌** |
| 落在 **glcore 的映射** → 驱动内部锁 | **0 条**（WAIT 全部不在 glcore 的 rw 段） | **❌** |
| 落在 **堆上** | **97.9%**（glibc thread arena） | **✅** |

**但"落在堆上"不等于"对象属于应用"** —— 抓栈证明调用者是 glcore。所以准确表述是：

> **对象在堆上（glcore `malloc` 出来含 `pthread_cond_t` 的结构），调用者在 glcore（`wait_ms`），等的是 glcore 内部的条件变量。**

### 4.1 这修正了报告 57 的一个推断

报告 57 曾写「主线程在 `glXSwapBuffers` 里 `sem_wait(&di.r.relsem)` 等 readback worker 提交 `glReadPixels`」。

**那是主线程。渲染线程不在 primus 的 sem 上。** 两者是两条独立的等待：

| 线程 | 等什么 | 证据 |
|---|---|---|
| **主线程** | primus 的 `sem`（等 readback worker 提交） | 报告 57（源码 + `st` 段实测） |
| **渲染线程** | **glcore 的 `cond_timedwait`**（`wait_ms`） | **本报告（uaddr + 栈双证）** |

---

## 5. 这次也修正了一个方法论误判：不能靠"归属段"反推"属于谁"

我原本的判据是「落在 primus 的 .bss → primus 的 sem」。这个判据**在这条链上失效了**，因为：

**primus 的 `sem_t` 根本不在 `.bss` 里** —— 它是 `DrawableInfo` 的成员，而 `DrawableInfo` 是 `primus.drawables[drawable]`（`std::map` 节点）的成员，**堆分配**。

所以正确的判据顺序是：

| 判据 | 可靠性 | 说明 |
|---|---|---|
| **`val` 递增 vs 恒 0/1** | **高** | 递增 = `__wseq` = 条件变量；0/1 = 信号量/互斥 |
| **`op` 的 flag 组合** | 中 | `WAIT_BITSET\|REALTIME` 两者都可能（`sem_timedwait` 也用） |
| **gdb 栈** | **最高** | 直接给出调用者模块 |
| 归属段（.bss / 堆 / 库映射） | **低** | 堆对象无法区分模块 |

**"对象在哪"和"谁在等它"是两个独立问题。** 前者靠 maps，后者必须靠栈。

---

## 6. 结论与下一步

### 6.1 现在确定的事实

| # | 事实 | 证据 |
|---|---|---|
| 1 | 渲染线程 87% 时间在 futex 上睡 | 报告 64（utime 12.4% + wchan） |
| 2 | 其中 **91.2% 是 `pthread_cond_timedwait`** | uaddr 的 val 递增 + op 0x189 |
| 3 | 调用者是 **glcore 的 `wait_ms`（`0x146d280`+0xC0）** | gdb 栈 10/12 + 反汇编 |
| 4 | 再上层是 **`glcore+0x123b31a`（队列处理器区）** | gdb 栈 |
| 5 | **primus 不在这条链上** | 12 份栈 0 次出现 |
| 6 | 等 9.4ms/次、2.86 次/帧 | 27.0ms ÷ 2.86 |

### 6.2 下一步：只剩"为什么这次等待这么久"

链条已经完整，问题变成：**`wait_ms` 在等一个什么条件，为什么 9.4ms 才满足？**

| 选项 | 内容 | 代价 |
|---|---|---|
| **A（推荐）** | 反汇编 `glcore+0x123b31a` 所在函数（`0x123xxxx` 段 = 队列处理器），看它调 `wait_ms` 前在检查什么、`wait_ms` 返回后做什么 | **零成本**，纯静态 |
| B | 读 `wait_ms` 的第二个参数（超时值）—— 报告 45 说是硬编码 2ms，但实测单次 9.4ms，矛盾需澄清 | 一次 uprobe 读 `%esi` |
| C | 对 515 做同样测量（需换驱动），看它的渲染线程是否也在 `wait_ms` 上 | 需换驱动 |

**建议 A**：因为报告 35 已经静态看过这个区域（`0x123aef0`），现在有了**确切的返回地址 `0x123b31a`**，可以直接定位到"哪一条 call 调了 wait_ms"。

### 6.3 一个必须先解决的矛盾

报告 45 说 `wait_ms` 的超时是**硬编码 2ms**（`mov $0x2,%esi`），但**实测单次等待 9.4ms**（27.0ms ÷ 2.86 次）。

如果超时真是 2ms，那 9.4ms ≈ **4.7 次超时轮询**。这与报告 44 的"15 次 2.065ms 超时轮询/帧"数量级一致（4.7 × 2ms ≈ 9.4ms）。

**但要小心**：`27.0 ÷ 2.86 = 9.4ms` 是从"总时长 ÷ 进入次数"推的，如果存在"进入前已满足、瞬间返回"的情况，这个平均值会失真。**B 选项能直接量出分布。**

---

## 7. 产物

```
analysis/probes/n74_futex.sh      ftrace 版（失败，记录了 two 个坑）
analysis/probes/n74_perf.sh       perf 版（成功，1373 条 futex）
analysis/probes/n74_arena.sh      MALLOC_ARENA_MAX=1 对照
analysis/probes/n74_bt.sh         gdb 抓栈（12 份）
analysis/probes/n74_bt2.sh        gdb 抓栈 + maps（6 份，用于算偏移）
analysis/n64/n74/an.py            uaddr 归属分析
analysis/n64/n74/raw.py           目标地址的原始 maps 行
analysis/n64/n74/prof.py          按 uaddr 的对象画像（op / val / 归属）
analysis/n64/n74/off.py           栈帧 → glcore 内偏移
analysis/traces/n74/              futex.txt / maps*.txt / bt*.txt / io*.txt
```

### 7.1 方法论（五条，已入技能库）

1. **★ 判断"等的是谁"，`val` 比 `uaddr` 归属更可靠。**
   `val` 单调递增（几千的序列号）= `pthread_cond_t.__wseq`；`val` 恒 0/1 = `sem_t`/mutex。
   本例靠这一条就排除了 primus（primus 只用 `sem_t`）。

2. **★ `uaddr` 落在堆上 ≠ 对象属于应用。**
   glibc thread arena 上的对象，无法从 maps 判断归属模块。**必须抓栈。**

3. **primus 的 `sem_t` 不在 `.bss`。**
   它在 `DrawableInfo` 里，而 `DrawableInfo` 是 `std::map` 节点 → **堆分配**。
   「落在 primus 的 .bss 就是 primus 的 sem」这个判据**本身是错的**。

4. **`MALLOC_ARENA_MAX=1` 是零成本判"匿名大段是不是 malloc arena"的方法。**
   64MB 量级的匿名 rw 段 + `MALLOC_ARENA_MAX=1` 后消失 → 是 glibc thread arena。

5. **gdb 栈里的返回地址能直接接上之前的静态分析。**
   本例 `glcore+0x146d340` 减去报告 52 的 `wait_ms(0x146d280)` = **0xC0**，
   反汇编确认那正是 `call *(%rax)` 的下一条。**栈帧地址和静态偏移可以精确对上，别浪费。**

### 7.2 环境坑（本轮踩了两个，都很花时间）

| 坑 | 表现 | 根因 | 解法 |
|---|---|---|---|
| **`echo 0 > filter` 破坏 filter** | 之后所有采集 **0 事件**，`cat filter` 显示 `Error: (0)`，连 `printf ''` 都清不掉 | 写 `0` 是**设置筛选条件为常量 0 = 恒假**，不是"清空" | **清空必须写空行**；已被写坏时写一个恒真表达式覆盖（`common_pid > 0`） |
| **`events/enable = 0` 屏蔽子事件** | 子事件 `enable=1` 但采集 0 条；`echo 1 > events/enable` 后立刻 1.5M 条 | 全局开关与子事件不是独立的 | **先用 perf**（不受此影响），或先开 `events/enable` |
| 上次残留的 filter 未清 | 采集 0 条，`cat filter` 显示上一轮的 `common_pid >= 21448 && ...` | 采集前没复位 | **每次采集前显式复位 filter** |
