# 60 · N64：**客户端 GLX 两版等价，XInitThreads 被 Qt 抹平，唯一分界是那 19.10ms 固定项**

> 2026-10-26 09:20
> 用户指令：N62 已说明瓶颈不在"读哪个 API"，换 `glGetTexImage` 大概率一样慢。真正剩下的差异在 primus 之下的 `libGLX_nvidia` —— **先零成本静态对比 515/525 这个库的 GLX 转发/回读路径**。
>
> **结论三句话**：
> 1. **转发/同步路径两版等价**：`glXSwapBuffers` / `glXWaitGL` / `glXWaitX` / `glXMakeCurrent` 语义指纹完全一致；X 协议收发调用点 `_XSend 27 / _XReply 224 / _XFlush 300` **两版逐个相同**。
> 2. **唯一新增的运行时行为 `XInitThreads` 被 Qt 抹平**：Heaven 自带的 `libQtGuiUnigine_x64.so.4` 也导入它，且早于 primus 打开 display → 两版实际进程都跑在线程安全模式。
> 3. **★ 顺手把 515 也拟合成二元式**：`515: 1.29 + 1.227×MiB` vs `525: 20.40 + 1.329×MiB` —— **斜率只差 8%，固定项差 15.8 倍**。**带宽从来不是分界，那 19.10ms 才是。**

---

## 1. 结论表

| # | 结论 | 证据 | 强度 |
|---|---|---|---|
| 1 | 客户端 `libGLX_nvidia` 导入符号 **只多 `XInitThreads`** 一个 | 122 vs 123，逐条 diff | **铁证** |
| 2 | 导出符号集完全相同（差 1 个 typo 函数名 `glTexParametervNVX`） | 2674 vs 2673 | **铁证** |
| 3 | X 协议发送/接收调用点**逐个相同** | `_XFlush` 300、`_XReply` 224、`_XSend` 27，两版一致 | **铁证** |
| 4 | **转发/同步四函数语义等价** | `glXSwapBuffers` 108→105（cond 9→9，calls 同）、`glXWaitGL` 0.983、`glXWaitX` 0.981、`glXMakeCurrent` 1.000 | **强** |
| 5 | 17/81 个 glX 函数有差异，但集中在**创建期**与**编译优化** | 见 §3 | **强** |
| 6 | **`XInitThreads` 线不成立**：Qt 也调，且更早 | `libQtGuiUnigine_x64.so.4` 导入 1 处 | **铁证** |
| 7 | 服务端 `libglxserver_nvidia` 导入符号 **67 条逐条相同**；`DRI3`/`DRI2`/`present`/`vblank`/`shm` **字符串 0 命中（两版都是）** | `strings`/`nm` | **铁证** |
| 8 | **报告 59 §4.2 的"DRI2 阻塞式缓冲"假设作废** | 同上（该库根本不走 DRI2/DRI3） | **铁证** |
| 9 | **★ 515 固定项 1.29ms vs 525 20.40ms，斜率差 8%** | 515 两点 / 525 三点拟合 | **强（515 只有两点，无残差）** |
| 10 | **带宽不是分界，19.10ms 纯延迟才是** | 由 9 推出 | **强** |
| 11 | 把固定项降到 515 水平 → 1080p 上限 **84.7 fps** | `1.29+1.329×7.91=11.81ms` | **推算** |

---

## 2. 方法：静态对比的三层，与一层不能用的

对两个 stripped 的闭源 .so 做"转发路径有没有变"，必须分三层，**且要避开一层陷阱**：

| 层 | 手段 | 能回答什么 | 灵敏度 |
|---|---|---|---|
| **L1 符号层** | `nm -D` 导入/导出集合 diff | 有没有新增外部能力、有没有新增导出 | **极高**（±1 就报警） |
| **L2 调用层** | 统计每个导入函数的 `@plt` 调用点数量 | 收发/同步**次数与分布**有没有变 | **高**（结构性） |
| **L3 语义层** | 逐函数提取指纹（调用序列 + 条件跳转数 + 内存偏移集合） | 单个函数逻辑有没有变 | **中**（需归一化） |
| ~~L0 字节层~~ | 指令序列 difflib 相似度 | ~~逻辑是否相同~~ | **不可用** |

**L0 不可用，这是本轮踩到的坑**：`glXSwapBuffers` 指令级 difflib 只得 **0.761**，但实际 diff 出来**每一行差异都是寄存器互换**（`%r13↔%r12`、`%rbp↔%rbx`）+ vtable 槽偏移平移（`0x4e0→0x5d0`）。

> **规矩**：两个不同编译器版本（或不同优化决策）编出的同一个函数，指令相似度天然落在 **0.7~0.9**。
> **这个区间没有信息量**——既不能证明相同，也不能证明不同。
> 要判"逻辑是否相同"，只能用**对寄存器分配和指令选择不敏感的指纹**。

寄存器归一化后升到 0.743，仍然低。**真正起作用的是 L3 语义指纹**：一次把 80 个"疑似不同"降到 17 个。

---

## 3. L3 结果：81 个 glX 函数，64 个等价，17 个不同

**判等条件**（三条同时满足）：`call` 目标序列完全相同 + 条件跳转数差 ≤1 + 指令数比在 0.75~1.33。

### 3.1 转发/同步核心：全部等价

| 函数 | 515 指令 | 525 指令 | 条件跳转 | 判读 |
|---|---|---|---|---|
| **`glXSwapBuffers`** | 108 | 105 | 9 → 9 | **等价**（diff 全是寄存器互换） |
| **`glXWaitGL`** | 59 | 59 | 7 → 7 | **等价**（0.983） |
| **`glXWaitX`** | 52 | 52 | 8 → 8 | **等价**（0.981） |
| **`glXMakeCurrent`** | 4 | 4 | 0 → 0 | **等价**（1.000） |
| `glXMakeContextCurrent` | 2 | 2 | 0 → 0 | 等价 |

**这就是"GLX 转发/回读路径"本身。四个全部等价。**

### 3.2 17 个不同的，按性质分四类

| 类 | 函数 | 数据 | **判读** |
|---|---|---|---|
| **A. drawable 创建重构** | `glXCreatePbuffer` 36→34<br>`glXCreateGLXPbufferSGIX` 42→53 | 515 直接构造 GLX 协议包（`movl $0x11009,(%rsp)`）→ vtable 派发；525 走统一入口 `0x7fec0` | **只影响创建时（一次性）**，不影响每帧 |
| **B. vsync / swap group**（6 个全变） | `glXSwapIntervalEXT` 104→76<br>`glXJoinSwapGroupNV` / `glXBindSwapBarrierNV` / `glXQueryFrameCountNV` / `glXResetFrameCountNV` / `glXQueryMaxSwapGroupsNV` 均 73→54 | offs 18→9，cond 9→8 | pbuffer 不走 swap group；且 `noswap` 已证伪 swap 是闸门 |
| **C. 编译优化**（逻辑等价） | `glXGetFBConfigAttrib` 42→33<br>`glXChooseVisual` 297→88 | **switch 从二分比较树变成跳转表**：`lea 0xe1734,%rcx; movslq (%rcx,%rbx,4),%rax; add %rcx,%rax; jmp *%rax` | **不是逻辑变化**，是编译器把 switch 编成了 table |
| **D. 与本路径无关** | `glXBindTexImageEXT` 51→132<br>`glXReleaseTexImageEXT` 135→103<br>`glXAllocateMemoryNV` / `glXCopyBufferSubDataNV` / `glXCopyImageSubDataNV` / `glXGetProcAddress` / `glXQueryContext` / `glXFreeContextEXT` | — | 纹理绑定 / 显存分配 / 扩展查询，primus 走的是 `glTexSubImage2D` |

### 3.3 A 类值得多看一眼，但它救不了

525 的 `glXCreatePbuffer` 经过 `0x7fec0`，这个统一入口**会解析 attribs 里的两个 GLX 常量**：

```
7ff04: cmp $0x801c,%eax      ← GLX_LARGEST_PBUFFER
7ff15: cmp $0x801b,%eax      ← GLX_PRESERVED_CONTENTS
```

`GLX_PRESERVED_CONTENTS` 正是"swap 后内容是否保留"，理论上会影响 back buffer 分配。**525 在用户态先解析它，515 直接把 attribs 原样发给 X server。**

但这条只能解释"pbuffer 的**类型**可能不同"，解释不了"每帧 20ms"：
- 创建只发生一次（primus 每 context 一个 pbuffer）
- `n62_noswap` 里完全不 swap，map 仍等 30ms → **PRESERVED_CONTENTS 的语义（swap 后保留）在这个场景下不起作用**

**记录为线索，不作为方向。**

---

## 4. `XInitThreads` 线：提出 → 反证 → 关掉

这是本轮最值得写下来的一段，因为它看起来非常像根因。

### 4.1 提出：一个 15.8 倍的差异，起因可能只是"锁有没有真的生效"

L1 唯一的差异是 `XInitThreads`（515 导入 0 处，525 导入 1 处），而两版**都导入** `XLockDisplay` / `XUnlockDisplay`（各 1 处调用）。

Xlib 的经典行为：
```c
#define LockDisplay(dpy) if ((dpy)->lock) (*(dpy)->lock->lock_display)((dpy))
```
**`dpy->lock` 只在 `XInitThreads()` 被调用过、且 display 在其之后创建时才非 NULL。**
→ 515 的 `XLockDisplay` 可能是**空操作**，525 是真锁。

而且 525 的调用点形态很配：`0x8170b` 调 `XInitThreads` → 返回后立刻读全局指针 `0x123608` 做双重检查（幂等初始化）。

### 4.2 反证：Qt 也调，而且更早

逐个查 515 全家桶：

| 库 | 515 | 525 |
|---|---|---|
| `libGLX_nvidia` | 0 | **1** |
| `libGL.so.1` | 0 | 0 |
| `libGLX.so.0` | 0 | 0 |
| `libglxserver_nvidia` | 0 | 0 |
| `libnvidia-glcore` | 0 | 0 |
| `libEGL` | 0 | 0 |
| **Heaven 的 `libQtGuiUnigine_x64.so.4`** | **1** | **1** |

Heaven 4.0 的 UI 是 Unigine 定制的 Qt，**Qt 的 xcb 后端会调 `XInitThreads`**。
而 Heaven 的启动顺序是"先建 Qt 窗口 → 再初始化 GL（dlopen libGL，primus 介入）"。

→ **Qt 的 `XInitThreads` 早于 primus 的 `XOpenDisplay`。**
→ primus 那两个 display 在 515 下**也是线程安全的**。
→ **两版的实际运行时行为相同，这条线关掉。**

### 4.3 方法论：一个"看起来是根因"的差异，要先问"它在这个进程里生效吗"

`XInitThreads` 的生效条件很苛刻：**必须在 `XOpenDisplay` 之前**。仅凭"525 导入了它"就推"525 加了锁"，会漏掉两件事：
1. 谁先调（应用可能已经调了）
2. display 是什么时候创建的（在它之前还是之后）

**判据**：一个库导入了某初始化函数 ≠ 这个初始化在目标进程里生效。**必须查该进程内所有模块的导入表 + 调用时序。**

> **⚠️ 2026-10-26 09:20 修正（报告 61）**：本节"**差异被抹平**"的说法**过于绝对**，需收窄。
> 本轮在 525 里找到实际代码：它先 `dlsym(NULL, "_XInitDisplayLock_fn")`（libX11 导出的 `.bss` 全局函数指针），
> 再 `call XInitThreads@plt`，然后 **读该指针判空**，非空则 **`call *%rax` 给 dpy 加锁**（`0x80aba`–`0x80ad7`）。
> **515 完全不做这套检测。**
> 所以准确表述是：**"两版都跑在线程安全模式"成立**（Qt 早调了 `XInitThreads`），
> 但 **"差异被完全抹平"不成立** —— 525 会**检查并据此分支**，515 不检查。
> **然而**：这段在 **display 初始化**里，每次开 display 走一次，**不在每帧路径** → **仍解释不了 20.4ms**。
> **教训**：`dlsym` 的符号不在动态符号表，**只做 `nm -D` 必然漏掉 `_XInitDisplayLock_fn`**。

---

## 5. 服务端 GLX：也等价，且 DRI3/DRI2 假设作废

`libglxserver_nvidia.so`（12.7MB，X server 侧的 GLX 模块）是"primus 之下"的下一站。

| 项 | 结果 |
|---|---|
| 导入符号 | **67 条，两版逐条相同（diff 为空）** |
| 导出符号 | 3 个（X server 模块标准入口），无法做逐函数锚点 |
| `DRI3` / `DRI2` / `present` / `Present` / `vblank` / `shm` / `SHM` 字符串 | **两版全部 0 命中** |
| `swap` / `pbuffer` 字符串 | 各 3 条，两版相同 |

**`DRI3`/`DRI2`/`present` 零命中**，说明 NVIDIA 的 GLX **根本不走标准 DRI 缓冲管理**——客户端库自己用 `socket`/`sendmsg`/`recvmsg` + `_XSend`/`_XReply` 说 X 协议，配 `_nv0XXglcore` 私有接口。

> **⚠️ 2026-10-26 09:20 更正（报告 61）**：本节结论**只对服务端成立，对客户端不成立**。
> 本轮对**客户端** `libGLX_nvidia` 做 `strings` diff 发现：它通过 **`dlsym` 加载了整套 `libxcb-dri3` / `libxcb-present` / `libxcb-sync`**（`xcb_present_pixmap`、`xcb_dri3_pixmap_from_buffer_checked`、`xcb_sync_await_fence`、`xcb_send_fd` 等，两版**逐条相同**）。
> 因为全是 `dlsym` 字符串，**`nm -D` 里一个 xcb 符号都没有** → 本报告的 L1 符号层在结构上看不见这层。
> **正确分层**：服务端 GLX 模块不走 DRI3/Present；**客户端走 DRI3 直连 + Present 提交**，绕过服务端 GLX 的缓冲管理。
> **对结论的影响：无**（两版都有且相同，仍不是版本差异）。**对未来查证的影响：有**（present 入口在客户端 `xcb_present_pixmap`，不在服务端）。

→ **报告 59 §4.2 写的"primus 转发可能走 DRI2 的阻塞式缓冲管理"这条假设，作废。**

服务端 GLX 也调 `_nv0XXglcore`（`_nv000/001/002/005/011/012/013/014/017/018/019/022/025/026glcore`），说明 **X server 进程里跑着 glcore 的第二个实例**。这是后续如果要查服务端的一条入口。

---

## 6. ★ 顺手把 515 也拟合成二元式 —— 唯一分界浮出来了

报告 56 只对 525 做了三点拟合。报告 29 里其实有 515 的两个点，**拟合出来对比，结论比预期更干净**：

| | 固定项 `a` | 斜率 `b` | 等效带宽 | 点数 |
|---|---|---|---|---|
| **515** | **1.29 ms** | **1.227 ms/MiB** | 0.85 GB/s | 2（无残差可验） |
| **525** | **20.40 ms** | **1.329 ms/MiB** | 0.79 GB/s | 3（残差 ≤0.08ms） |
| **差** | **×15.8（+19.10ms）** | **+8%** | — | — |

原始点（报告 29 的 D，即 `T_land`）：

| 分辨率 | MiB | 515 | 525 |
|---|---|---|---|
| 1920×1080 | 7.91 | **11.00 ms** | **30.9 ms** |
| 960×540 | 1.98 | — | 23.1 ms |
| 640×360 | 0.88 | **2.37 ms** | 21.5 ms |

### 6.1 这条修正了报告 56 的优化顺序

报告 56 写的是"**先打 20.4ms 固定项（66%），再打带宽（4×）**"。

但 515 的斜率是 **1.227 ms/MiB = 0.85 GB/s**，只有 PCIe Gen1 x16 线速（3.15 GB/s）的 **27%**。

→ **515 也是"线速的 1/4"。带宽效率两版一样差，它不是分界点。**
→ **把带宽优化到线速（0.333 ms/MiB），525 的 1080p T_land 也只是 20.40+2.63=23.0ms，仍是 43fps。**
→ **唯一值得打的是那 19.10ms。**

### 6.2 把固定项拉回 515 水平会怎样

```
T_land(1080p) = 1.29 + 1.329 × 7.91 = 11.81 ms  →  fps 上限 84.7
```

实测 515 是 108fps（9.26ms/帧）。**11.81ms 的 T_land 配上"回读与渲染部分重叠"（报告 29 §6.3 观察到的 515 行为），落在 108fps 完全说得通。**

→ **整条 3 倍差距 = 那个 19.10ms 的纯延迟，加上"525 里 D≈1/fps 完全串行化、515 里 D 与帧时间脱钩"这个结构差异。**

### 6.3 19.10ms 已经排除了哪些来源

| 可能来源 | 状态 | 依据 |
|---|---|---|
| primus 用户态（上传/上屏/主线程swap/读FRONT-BACK） | ❌ 排除 | 报告 58/59，4 可动点 16 组合全无效 |
| 客户端 GLX 转发/回读逻辑 | ❌ 排除 | **本报告 §3** |
| Xlib 线程安全模式（XInitThreads） | ❌ 排除 | **本报告 §4** |
| X server 侧 GLX 的 DRI 缓冲管理 | ❌ 排除 | **本报告 §5**（不走 DRI2/DRI3） |
| primus autosleep | ❌ 排除 | 报告 25：`PRIMUS_SLEEP=0` 只快 2ms，帧率反降 |
| glcore 的 cond/futex 等待机制 | ❌ 排除 | 报告 44~55（12 轮，含 20µs 自旋那条线程） |
| **GPU 侧 copy 的实际执行延迟** | **◑ 唯一剩余** | 由排除法留下 |
| **内核驱动（nvidia.ko）提交/调度** | **◑ 唯一剩余** | 同上 |

---

## 7. 结论

| 问题 | 答案 |
|---|---|
| 515/525 的 GLX 转发路径有差异吗？ | **没有**（符号层 1 个导入差异、调用层逐个相同、语义层四核心函数等价） |
| 那唯一多出来的 `XInitThreads` 呢？ | **被 Qt 抹平**，两版实际都跑线程安全模式 |
| 回读路径（`glReadPixels`/`glMapBuffer`）经过 GLX 吗？ | 经过，但两版行为一致 |
| 3 倍差距在哪？ | **一个 19.10ms 的纯延迟（固定项）**，与数据量、与 GLX、与 primus 用户态都无关 |
| 带宽是问题吗？ | **不是分界**（两版同为线速的 1/4） |

**一句话**：**客户端 GLX 这条线关掉了；服务端 GLX 也没走标准 DRI 通道。追了 6 轮的"primus 之下"，现在只剩 GPU 提交层和内核驱动。**

---

## 8. 下一步（两个选项，判据明确）

### 选项 1：先验"19.10ms 是否与 GPU 忙闲有关"（零成本，用已有数据）

报告 41 做过压力复现：加负载后 D 2.66→30.72ms，但 `glFinish` 后恒为 2.48ms → "涨的全是等前面提交的计算"。

**若这条对 525 也成立，那 19.10ms 就是"等同期提交的渲染完成"。** 但报告已经测过 GPU 功耗只有 48~68W ≈ 空转基线 50W → **GPU 并不忙，这是矛盾点。**

**做法**：从已有的 `n60`/`n62` trace 里，取 `RP→MB` 窗口，看窗口内是否有其它 GPU 提交（swap / upload / 渲染）。**零成本，纯数据整理。**

- 有 → 19.10ms 是排队，打点是"减少同期提交"
- 没有 → 19.10ms 是驱动/内核的固定延迟

### 选项 2：直接换 515 的内核模块 + 服务端 GLX，保留 525 的客户端（需重启 X，有风险）

这是唯一能**直接二分"内核 vs 用户态"**的实验。但 `libglxserver_nvidia` 与 `nvidia.ko` 有 ABI 耦合，混装大概率不兼容。

**我的建议：先做选项 1**，因为它零成本且能给出"排队 vs 固定延迟"的判定，而这两者的修法完全不同。

---

## 9. 产物

```
analysis/n64/imp-515.txt, imp-525.txt          客户端导入符号（122/123）
analysis/n64/exp-515.txt, exp-525.txt          导出符号+地址（2674/2673）
analysis/n64/n64_cmp.py                        指令级 difflib（含寄存器归一化）—— **L0，不可用，留作反例**
analysis/n64/n64_cmp2.py                       边界改为"到下一个导出符号"+ nop/ret 归一化
analysis/n64/n64_fp.py                         语义指纹 v1（内部调用未归一化 → 80 个假阳性）
analysis/n64/n64_fp2.py                        语义指纹 v2（内部调用归一化 → 17 个真差异）
analysis/n64/simp-515.txt, simp-525.txt        服务端导入符号（67，完全相同）
analysis/n64/srv-515.str, srv-525.str          服务端字符串指纹
```

### 9.1 方法论（三条，已入技能库）

1. **指令级 difflib 在 0.7~0.9 区间没有信息量**——两个编译器版本编同一函数天然落这里。判"逻辑是否相同"要用对寄存器分配/指令选择不敏感的指纹（调用序列 + 条件跳转数 + 内存偏移集合）。
2. **语义指纹必须先把"内部调用"归一化**。`call 7e1c0 <...+0x4520>` 两版地址不同，不归一化会把 80 个函数全判成"不同"。
3. **一个库导入了某初始化函数 ≠ 它在目标进程里生效**。`XInitThreads` 必须在 `XOpenDisplay` 之前才让 `dpy->lock` 非 NULL —— 必须查进程内所有模块的导入表 + 调用时序，才能判"加锁有没有真的发生"。
