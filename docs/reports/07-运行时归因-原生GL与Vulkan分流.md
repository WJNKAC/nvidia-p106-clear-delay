# 原生 GL 与 Vulkan 分流 — 3D 受限路径的运行时归因

> **⚠️ 数字口径更正（2026-10-24 23:45，见报告 08）**
> 本文的**机制方向**（Vulkan 358 fps vs GL 30 fps、成本 ∝ 每帧调用数、GL 提交侧串行化）成立，但以下数字已作废：
> ① "240 次交接/帧 × 105 µs" → 实为 **119 次 futex / 40 次 `FUTEX_WAIT` 每帧，中位 357 µs**；
> ② "45 次 clock_nanosleep/帧" → **全部来自应用 `libUnigine` 的 `usleep(1000)`，驱动稳态不调用 nanosleep**；
> ③ §8 表中 525 的 `imul $0xfa` / `/1000` 两行 → **作废**（当时 525 的 asm 取自 32 位库）。
> 报告 08 另给出两个否定结果：`pthread_cond_*` 三代皆无；gate bit 翻转（gdb 实做）对帧率/CPU 均无影响。


- **日期**: 2026-10-24 夜（接报告 06）
- **机器**: desktop@<testhost>，Xeon E3-1246 v3 4C/8T，Debian 13，内核 6.12.111+deb13
- **GPU**: NVIDIA P106-100（01:00.0，**无显示输出**），经 PRIME offload 回传核显扫描
- **驱动**: 580.126.18（glcore sha256 `3d5ca897…9da7`，已拉回 `analysis/glcore/`）
- **会话**: KWin Wayland + Xwayland 24.1.6，`DISPLAY=:1`
- **上机纪律**: 全程零系统改动、零驱动更换、零重启；每次运行 ≤30 s，共 6 组

---

> ## ⛔ 「问题只在 GL 用户态前端」已被 **报告 93（N102）** 推翻（2026-10-26 14:30）
>
> **在 525.116.03 下，同一个 `vkmark` 在 P106 上只跑 66 fps / 15.15 ms**，
> 而本文的 358 fps / 2.79 ms 是 **580.126.18** 下测的 —— **差 5.4 倍，Vulkan 同样被钳制**。
> 同机核显跑同一 `clear` 场景是 **4747 fps / 0.211 ms**，排除合成器/vsync/显示管线。
>
> ⇒ 本文 ①「Vulkan 358 fps 正常」只对 **580** 成立；③「成本 ∝ 每帧 GL 调用数」在 525 下
> 只解释了 GL 侧的一部分，**Vulkan 上还存在一个与调用数无关的「每帧 ≈13.3 ms 固定等待」**（报告 93）。
> **仍然成立的部分**：PRIME/DMA-BUF/PCIe/供电/功耗墙的排除；FIFO 精确 16.667ms；LBR 调用链。

---

## 1. 结论摘要（TL;DR）

1. **决定性分流**：同一块卡、同一个 Xwayland、同一条 DMA-BUF 回传、同一套内核提交、同一个 PCIe Gen1 槽位——
   **原生 Vulkan 在 1920×1080 跑 358 fps / 37 W**，而**原生 GL（Heaven）在 1920×1080 只跑 30 fps / 48 W**。
   → PRIME 跨 GPU present、DMA-BUF、PCIe 带宽、内核提交、供电、功耗墙**全部出局**，问题只在 **GL 用户态前端**。
2. **`FIFO` 模式精确 60.0 fps / 16.667 ms**，一分不差 → 显示管线本身完全健康（这不是"半速刷新"）。
3. **帧时间与像素几乎无关**：`T ≈ 26.1 ms + 3.5 ns/像素`；320×200 仍有 26.3 ms，
   而 glxgears 在 300×300 能跑 960 fps（1.04 ms/帧）。→ 成本与**每帧 GL 调用密度**成正比，与光栅化量无关。
4. **LBR 调用链**（Intel 硬件分支记录，不依赖帧指针）证明 **73% 采样**落在同一条链上：
   `glXSwapBuffers → glcore 提交路径 → 队列排空轮询(glcore+0xb0fd70) → clock_gettime`。
5. **否定结果**：`glcore+0xb0fd70` 这个轮询函数在 **515.105.01 里就存在，且与 580 结构逐行对应**
   （连上下文偏移 `0x22e0c` 都一致，只有取时间 vtable 槽 `0x280→0x290`、几个字段偏移变化）。
   → **不是 525 新增代码**；"515 快"只能来自**触发频率**或**放行条件**的差异。
6. **syscall 侧**：`ioctl` 仅 **1761 次**（排除 per-draw 内核提交）；`futex` **87,637 次占 83% 系统调用时间**、
   平均阻塞 **105 µs**、其中 **5,117 次 EAGAIN**；`clock_nanosleep` **20,717 次**。
   → 开销在**进程内的线程交接/带超时轮询睡眠**，不在内核提交。
7. **`__GL_YIELD=USLEEP`：CPU 130% → 32%，帧率 30 → 31 fps 不变**。
   → 自旋是纯浪费；驱动程序**真在等**某个需要 ~26 ms 才成立的条件。
8. **修正报告 04/05 的关键误读**：glmark2 的 79 W 是走 **zink（GL→Vulkan）**跑出来的，
   本来就绕开了原生 GL 前端。所谓"合成 GL 负载正常、Heaven 异常"**不是矛盾，是两条不同的路径**。

> **一句话**：不是功耗被限，是**原生 GL 每帧被一段"等队列排空 + 时间戳轮询"串行化**，
> 每帧多付约 26 ms，GPU 大部分时间在等米下锅，所以功耗低、利用率高、帧率 30。

---

## 2. 决定性分流实验：原生 Vulkan vs 原生 GL

### 2.1 前置更正

此前（本轮开头）我据 `/usr/share/vulkan/icd.d/` 无 nvidia 条目，曾判断"P106 没有 Vulkan"。**该判断错误**：
NVIDIA ICD 装在 **`/etc/vulkan/icd.d/nvidia_icd.json`**（140 B，2026-02-24，与驱动同日），Vulkan loader 会搜索该路径。
`libnvidia-glvkspirv.so.580.126.18`（9.5 MB）在位，`VK_LAYER_NV_optimus` 生效。

### 2.2 实测

命令（`VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json`，`--winsys xcb`，1920×1080）：

| 路径 | 呈现模式 | FPS | 帧时间 | 功耗 | 利用率 | SM 时钟 |
|---|---|---|---|---|---|---|
| **Vulkan** | `IMMEDIATE` | **358** | 2.79 ms | 37.0–37.5 W | 99 % | 1911 MHz |
| **Vulkan** | `FIFO` | **60.0** | **16.667 ms** | ~37 W | 99 % | 1911 MHz |
| **GL**（Heaven） | vsync off | **30–31** | 32.3–33.3 ms | 46–51 W | 89–93 % | 1911 MHz |

`vkmark -L` 确认设备为 `0x10DE / 0x1C07 / NVIDIA P106-100`，不是核显、不是 lavapipe。

### 2.3 判读

同一硬件、同一时刻、同一个 `DISPLAY=:1`：

- **PRIME 跨 GPU present 不是瓶颈**——若是，Vulkan 不可能 358 fps、更不可能在 FIFO 下精确 16.667 ms。
- **PCIe Gen1 x16 不是瓶颈**——Vulkan 1080p 的帧回传量更大（2.79 ms/帧）。
- **内核提交路径不是瓶颈**——两者共用 `nvidia.ko` 的同一套 channel/ioctl。
- **供电/功耗墙不是瓶颈**——Vulkan 只吃 37 W 就跑 358 fps，GL 吃 48 W 只有 30 fps。

→ 唯一剩下的舞台：**`libnvidia-glcore.so` 这条 GL 用户态前端**。

> 附带结论：**"P106 无显示输出"这个硬件特征不是原因**。Vulkan 同样要经 DMA-BUF 交给核显扫描，它没问题。

---

## 3. 地板测定：成本 ∝ 每帧 GL 调用密度，∝̸ 像素

| 分辨率 | 像素数 | FPS（截图角标） | 帧时间 | 功耗 |
|---|---|---|---|---|
| 320×200 | 64 K | **38** | 26.3 ms | 41.4 W |
| 640×360 | 230 K | **36** | 27.8 ms | 43.3 W |
| 1920×1080 | 2073 K | **30–31** | 32.3–33.3 ms | 46–51 W |

线性拟合（点 1↔点 3）：`ΔT = 7.0 ms / Δpx = 2009 K` → `3.5 ns/px`，即 **1080p 的纯渲染量只值 7 ms**，
剩下 **26.1 ms 是与像素无关的固定部分**。

对照：`glxgears` 300×300 = **960 fps（1.04 ms/帧）**，640×360 = 828 fps，1280×720 = 486 fps，1920×1080 = **294 fps**。

**两者都是原生 GL、同一个 Xwayland、同一条 present 路径。** 差别只有一个：glxgears 每帧个位数 draw call，
Heaven 每帧上千次状态/绘制调用。所以那 26 ms 是：

```
26 ms ÷ 每帧调用数 = 每次调用的附加成本
若每帧 ~1000 次调用 → ~26 µs/次
```

**每次 GL 调用多付 20–30 µs** —— 这就是 30 fps 的全部来源。

### 3.1 与"vsync 无关"的交叉验证

| 组 | 配置 | FPS |
|---|---|---|
| hvA/hvB | 1080p，`__GL_SYNC_TO_VBLANK=0` | 30 / 31 |
| hvE | 1080p，`__GL_SYNC_TO_VBLANK=1` | 31 |
| hvC | 1080p，`__GL_MaxFramesAllowed=4` | 30 |

vsync 开/关、帧队列深度都**不影响**帧率 → 那 26 ms 在 present **之前**，不是等 vblank、不是帧队列背压。

---

## 4. CPU 侧：主线程被一个核吃满，但那是自旋

### 4.1 线程级分解（1080p，6 s 窗口）

| 线程 | 占单核 |
|---|---|
| **主线程** | **100.2 %** |
| 驱动工作线程 | 16.5 % |
| 第三个线程 | 7.5 % |
| 其余 16 个 | 各 ≤1.3 % |
| **合计** | **131 %**（19 线程） |

6 s 墙钟消耗 7.86 s CPU；按 30 fps ≈ 180 帧 → **主线程约 33 ms CPU/帧，恰好等于整帧时间**。

### 4.2 但"CPU 忙"是假象：USLEEP 一改就露馅

| 组 | 帧率 | CPU | 功耗 | 利用率 |
|---|---|---|---|---|
| base | **30** | **130 %** | 49.6 W | 89 % |
| `__GL_THREADED_OPTIMIZATIONS=0` | 28 | 112 % | 47.4 W | 79 % |
| **`__GL_YIELD=USLEEP`** | **31** | **32 %** | 48.1 W | 93 % |
| `__GL_THREADED_OPTIMIZATIONS=0` + `__GL_YIELD=NOTHING` | 28 | 110 % | 50.9 W | 84 % |

**CPU 从 130% 降到 32%（四分之一），帧率一点没变。** 结论：

- 主线程那 100% 里，绝大部分是**自旋等待**，不是工作量；
- 把自旋换成睡眠后，暴露出真实构成：**每帧 ≈ 10 ms CPU + ≈ 22 ms 阻塞**；
- 所以"CPU-bound"的说法要修正为"**等待主导、自旋浪费**"。

---

## 5. 系统调用侧：不是 per-draw 内核提交，是进程内交接

`strace -f -c` 包住 Heaven，约 30 s 窗口：

| syscall | 次数 | 占 syscall 时间 | 平均每次 | 备注 |
|---|---|---|---|---|
| **futex** | **87,637** | **83.1 %**（9.22 s） | **105 µs** | 5,117 次出错 |
| clock_nanosleep | 20,717 | 15.2 %（1.69 s） | 81 µs | 定时睡眠 |
| **ioctl** | **1,761** | 1.0 % | 62 µs | ← 关键 |
| recvmsg | 23,559 | 0.3 % | 1 µs | 21,439 次 EAGAIN |
| poll / ppoll | 4,462 / 608 | — | — | 轮询 |
| mmap / mprotect | 362 / 2,788 | — | — | — |

### 5.1 两条硬结论

1. **`ioctl` 只有 1,761 次** → **不存在"每次 draw 都陷入内核提交"**。
   内核提交路径与 Vulkan 用的是同一套，Vulkan 358 fps 正常，故此处同样正常。
2. **`futex` 87,637 次、单次平均阻塞 105 µs、5,117 次 EAGAIN** → 这是**进程内线程交接**的特征：
   - 105 µs 远高于无竞争 futex 的 1–5 µs，说明每次都在等对面线程真正产出；
   - EAGAIN 说明大量是 **`FUTEX_WAIT` 带超时**，也就是**把 futex 当睡眠原语用的轮询**；
   - 伴随 20,717 次 `clock_nanosleep`，与反汇编里 `usleep(250)`/`usleep(1000)` 的退避阶梯吻合。

按 ~360 帧折算：**每帧约 240 次交接 × 105 µs ≈ 25 ms** —— 与第 3 节测到的 26 ms 固定成本**数量级与数值都对齐**。

---

## 6. LBR 调用链：73% 采样落在同一条链上

perf 的帧指针回溯在 glcore 上是坏的（无帧指针），普通 `-g` 会产出 `5355f48949544155` 这类垃圾帧。
改用 **Intel LBR**（`--call-graph lbr`），拿到精确的"调用者→被调用者"配对，减基址 `0x7f9717600000` 后：

```
libGLX_nvidia 0x…c6bc
└─ libGLX_nvidia 0x…6abb
   └─ libGLX_nvidia 0x…8918              ← 从 glXSwapBuffers 进入
      └─ glcore +0xab1af1
         └─ glcore +0xa82440
            └─ glcore +0xb1029a
               └─ glcore +0xae5c59
                  └─ glcore +0xb0fd70          ← 73% 采样聚集
                     └─ glcore +0xb0fe08       ← call *0x290(%rax) 取时间
                        └─ glcore +0xa1ad2c
                           └─ clock_gettime    ← 15.6% 直接落在 vDSO
```

- `+0xb0fe08` 与我此前独立反汇编出的 `call *0x290(%rax)` **精确重合**，两条独立路径互相验证，链可信。
- `+0xa1ad2c`/`+0xa1abc0` 是取时间的内部实现（515 里对应 vtable 槽 `+0x280`）。
- 链的顶端三层全在 **`libGLX_nvidia`**，即整个热点是**由 swap 调用驱动的**，与报告 06"GLX 层两代一致"不冲突——
  GLX 只是入口，逻辑在 glcore。

### 6.1 方法论教训（写给下一位）

- **不要用单条指令的高占比当热点**：本轮开头 `0xa0ffc0` 占 35.6%，但它是 `munmap` 包装的**函数入口**，
  左右邻接指令零采样 —— 典型的 precise/非 precise 采样 skid 归因假象。用 LBR 或函数级（`.eh_frame` FDE）聚合才可靠。
- glcore 剥了符号但 **`.eh_frame` 保留 46,990 个 FDE**（`readelf --debug-dump=frames`），
  每个都带 `pc=start..end`，可直接重建函数边界表，做函数级归因。

---

## 7. 反汇编：那个"等队列排空"的东西长什么样

### 7.1 外层判定（`glcore+0xae5c30`，8 行）

```asm
ae5c30: test %rdi,%rdi          ; 无上下文
        je   → return 0
ae5c35: testb $0x1, global(0x235dec0)   ; 全局开关：置位则直接跳过等待
        jne  → return 0
ae5c3e: testb $0x2, 0x565(%rdi)         ; 队列特性位：未置位则不等待
        je   → return 0
ae5c59: call 0xb0fd70                   ; ← 排空轮询
ae5c5e: test %eax,%eax
        je   → return 0                 ; 已排空
ae5c6b: jmp  0xb0ff30                   ; 仍有在飞任务 → 继续
```

### 7.2 轮询本体（`glcore+0xb0fd70`）

```asm
b0fd82: testb $0x2, 0x565(%rdi)         ; 同上特性位
b0fd8b: mov   0x22e5c(%rdi),%edx        ; 待处理项计数
        ; 扫描 ctx+0x1420 起的指针数组，逐项查 *(item)+0x5e == 0xffff
b0fdd0: testb $0x7, global(0x235fba0)   ; 另一组全局位
b0fdd9: mov   0x11f8(%rbx),%rax
b0fde5: testl $0x88000,0xc(%rax)        ; 队列/通道状态位（0x8000|0x80000）
b0fdee: cmpb  $0x0,0x6e40(%rbx)         ; 节流开关
b0fdf7: mov   0x22e0c(%rbx),%ecx        ; 档位/规模参数
b0fe01: lea   global,%rax
b0fe08: call  *0x290(%rax)              ; ← 取单调时间
b0fe0e: movabs $0x20c49ba5e353f7cf      ; 除法魔数 → /1000（ns → µs）
b0fe39: cmp   %rax,%rdx                 ; 与 ctx+0x6e48 存的"下次允许时刻"比较
b0fe3c: jle   → 放行                   ; 还没到点 → 直接返回
b0fee5: incl  0x6e41(%rbx); cmp $3      ; 重试计数，>3 则放弃
b0fef9: imul  $0xfa,0x22e0c(%rbx),%eax  ; 250 µs × 规模
b0ff03: sub   $0xfa,%eax
b0ff08: add   %rax,%rdx
b0ff0b: mov   %rdx,0x6e48(%rbx)         ; 下次允许时刻 = now + 250µs×(n−1)
```

**形态判读**：查队列 → 读时钟 → 与"下次允许时刻"比较 → 没到点就通过、到点就 `250 µs × n` 退避重试，最多 3 次。
这是**带时间戳门限的轮询退避**，不是限帧器（限帧器不会用"重试 3 次就放弃"）。
它的直接后果是：**每一次这样的等待都要读一次时钟并睡 250 µs 量级**，累计成每帧 20 余 ms。

### 7.3 上层提交路径（`glcore+0xb1029a` 所在函数）

```asm
b1028f: mov  %r13,%rdi
b10292: call 0x1058900                  ; 0x1058xxx 簇，7.5% 采样
b10297: mov  %r13,%rdi
b1029a: call 0xae5c30                   ; ← 排空等待
b1029f: mov  0x22e5c(%r13),%edi
b102a6: test %edi,%edi
b102a8: jg   0xb10140                   ; 仍有待处理项 → 回跳继续
b102ae: mov  0x558(%r13),%edx
b102db: subl $0x1,0x6e30(%r13)          ; 一个"在飞帧计数"
b102e5: movq $0x0,0x6e38(%r13)
b102f0: movb $0x0,0x6e40(%r13)          ; 清节流开关
```

`0x6e30` 递减到 0 就清 `0x6e38`/`0x6e40`（节流状态）——**说明这是"提交并在排空后收敛"的收敛点**，
即每次 kick 都要等 GPU 把队列排到空。**没有跨帧重叠（no pipelining）**：一帧的 GPU 工作结束后才开始下一帧，
于是每帧都要付一次完整往返延迟，这正是"分辨率无关的固定成本"。

---

## 8. 515 vs 580：同函数对照（重要否定结果）

在 515.105.01 的 `glcore` 里找到对应实现（`0xfbe5b0`），结构逐行对应：

| 检查项 | 515.105.01 (`0xfbe5b0`) | 580.126.18 (`0xb0fd70`) |
|---|---|---|
| 入口 `test %rdi,%rdi` | 有 | 有 |
| 特性位 `testb $0x2` | `0x54d(%rdi)` | `0x565(%rdi)` |
| 队列扫描 `*(item)+0x5e == 0xffff` | 有 | 有（`0x6e`→`0x5e` 偏移不同） |
| 全局位闸门 | `testb $0x7, global(0x251f840)` | `testb $0x7, global(0x235fba0)` |
| 状态位测试 | `testl $0x88000,0xc(%rax)` @`0xfbe5f2` | `testl $0x88000,0xc(%rax)` @`0xb0fde5` |
| 规模参数 | `0x22e0c` | **`0x22e0c`（完全相同）** |
| 取时间 vtable 槽 | `*0x280(%rax)` | `*0x290(%rax)` |
| `/1000` 魔数 | `0x20c49ba5e353f7cf` | 同（525/580 改用了别的实现形式，直接 grep 已不命中） |
| 250 µs 退避 `$0xfa` | 2 处 | 4 处 |
| **函数调用点数量** | **6 处**（`0xea2a00`/`0xedff83`/`0xf1f1bf`/`0xf1f413`/`0xf708b3`/`0xfbe6e2`） | **7 处**（`0x9f9ae3`/`0xa394f3`/`0xa6d728`/`0xa6d975`/`0xaa0753`/`0xabdafb`/`0xae5c59`） |

**结论：这套"等队列排空 + 时间戳退避"在 515 里就已经存在，不是 525 新增；调用点数量也几乎相同（6 vs 7），
不是"新增了调用路径"。**

→ 所以"515 快、525+ 慢"**不可能**由"新增代码"或"新增调用点"解释。只剩两条：

1. **放行条件变了**：`0xae5c30` 的两道门（`global(0x235dec0)` bit0、`ctx+0x565` bit1）在 515 是被置位的
   （→ 直接跳过等待），525+ 变清（→ 每次都要等）。
2. **每次进入的循环次数变了**：函数体没变，但调用它的提交循环（`0xb1029f: jg 0xb10140`）在 580 里
   因为**批处理粒度变小 / 每批都要等排空**，导致同样的 7 个入口被反复进入上千次/帧。

> 量化支持第 2 条：`87,637 futex ÷ 约 360 帧 ≈ 240 次/帧`，而调用点只有 7 个
> → **必然是循环**，不是静态调用密度。所以"批处理粒度"才是真正的变量。

第 1 条成本最低（静态可查：这两个全局位在哪些路径被写）；第 2 条需要对比提交函数的批处理阈值。

---

## 9. 排除清单（本轮新增，均为硬结论）

| 候选原因 | 判定 | 依据 |
|---|---|---|
| 功耗墙 / Power Limit | **排除** | PL = 120 W（min 60 / max 140）；`SwPowerCap` 全程 Not Active |
| 降频 | **排除** | SM 1911 MHz 满频，显存 4006 MHz 满速 |
| GPU 算力不足 | **排除** | Vulkan 1080p 358 fps / 37 W |
| PRIME 跨 GPU present | **排除** | 同上；Vulkan FIFO 精确 60.0 fps |
| DMA-BUF / 显存回传 | **排除** | 同上 |
| PCIe Gen1 x16 带宽 | **排除** | Vulkan 帧回传量更大却更快 |
| 内核提交 / ioctl 开销 | **排除** | ioctl 仅 1,761 次/30 s |
| Xwayland / 合成器 | **排除** | Vulkan 走同一套 WSI 正常；GL 在 vsync 开/关下同为 31 fps |
| vsync / 帧队列深度 | **排除** | `__GL_SYNC_TO_VBLANK` 0/1、`__GL_MaxFramesAllowed=4` 均无影响 |
| 线程优化 / yield 策略 | **不是原因** | `__GL_THREADED_OPTIMIZATIONS=0`、`__GL_YIELD=USLEEP` 均不改帧率（后者只降 CPU） |
| **GL 用户态前端的每调用开销 + 队列排空串行化** | **成立** | 73% LBR 采样在 swap→submit→排空轮询链上；26 ms 固定成本；240 次 futex/帧 × 105 µs |

---

## 10. 修正报告 04/05 的两处误读

1. **glmark2 的 79 W 不能作为"合成 GL 负载正常"的证据**。用户在追问中指出 glmark2 是通过
   **zink 桥接到 nvidia vk 1.4**——也就是说它走的是 **Vulkan 路径**，与原生 GL 前端无关。
   报告 04/05 把它当作"同一驱动、同一卡、同一时刻的 GL 对照"是**错配对照**。
   → 本报告 §2 的原生 Vulkan 数据才是正确的对照，且结论更强（358 fps / 37 W）。
2. **"贡献只落在 3D 呈现路径"应改为"贡献落在原生 GL 的每调用开销 + 提交串行化"**。
   "呈现"这个词会误导人去查 present/DMA-BUF，而实际瓶颈在 submit 侧的排空等待。

---

## 11. 下一跳（按性价比排序）

| 优先级 | 任务 | 为什么值得做 | 成本 |
|---|---|---|---|
| **P0** | 静态查 `global(0x235dec0)` bit0 与 `ctx+0x565` bit1 的**写入点** | 直接验证 §8 第 1 条：如果 525 改了这两个门的置位条件，问题就闭环了，且**能直接给出绕过方法** | 零上机 |
| **P1** | 用 LBR + 计数确认真实**帧数**，把"240 次 futex/帧"从推算变成实测 | 现在 240 是"87,637 ÷ 约 360 帧"的估算，误差可能 ±1 倍 | 一次 30 s 运行 |
| **P2** | 对比 515 与 580 里 `glcore+0x1058900`（提交/kick 函数，7.5% 采样）的调用点密度 | 验证 §8 第 2 条"触发频率变化" | 零上机，需 Ghidra |
| **P3** | 纯 Xorg 会话复跑 Heaven | 只剩这一条会话级混淆；但 Vulkan 已证明 Xwayland 无罪，**优先级可降** | 需切会话 |
| P4 | dkms 装 525 做版本 A/B | 已被"515/580 同函数"这个否定结果削弱——就算 525 复现，也仍是同一段老代码 | 需重启，不建议 |

---

## 12. 复现命令

```bash
# 0) 通道（沙箱内，需 SSH_ASKPASS）
/home/<user>/WorkBuddy/2026-10-05-17-32-45/tmp/rssh.sh

# 1) 原生 Vulkan 分流（决定性）
export DISPLAY=:1 VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json
vkmark -L                                  # 确认 0x10DE/0x1C07
vkmark -s 1920x1080 --winsys xcb -p immediate -b :duration=20   # 期望 ~350 fps
vkmark -s 1920x1080 --winsys xcb -p fifo      -b :duration=15   # 期望 60.0 fps

# 2) GL 地板
cd /home/<user>/下载/Unigine_Heaven-4.0/bin
export LD_LIBRARY_PATH=$PWD DISPLAY=:1 __GLX_VENDOR_LIBRARY_NAME=nvidia
./heaven_x64 -engine_config unigine.cfg -console_command "world_load heaven/heaven" \
  -video_app opengl -video_fullscreen 0 -video_mode -1 \
  -video_width 320 -video_height 200 -sound_app null -video_vsync 0    # 期望 38 fps

# 3) LBR 调用链（必须用 lbr，不能用默认 -g）
perf record -F 499 --call-graph lbr -p <heaven_pid> -o /tmp/lbr.data -- sleep 6
perf report --stdio -i /tmp/lbr.data --sort dso,sym -g graph,0.5,caller --percent-limit 1.5

# 4) 系统调用构成
strace -f -c -o /tmp/strace_c.txt ./heaven_x64 …（同上参数）; sleep 30; pkill -f heaven_x64

# 5) 函数边界表（glcore 无符号时用）
readelf --debug-dump=frames /usr/lib/x86_64-linux-gnu/libnvidia-glcore.so.580.126.18 \
  | grep "pc=" > fde.txt                  # 46,990 条
```

---

## 13. 相关文档

| 文档 | 作用 |
|---|---|
| `01-驱动版本定位报告.md` | 静态版本边界 515.105.01 → 525.116.03 |
| `05-Heaven实测与机制探测.md` | 决定性复现（部分判读已被本报告修正，见 §10） |
| `06-GLX层与vDSO静态复核.md` | GLX 层两代一致；vDSO 自旋归属证实 |
| `07-运行时归因-原生GL与Vulkan分流.md` | **本文档**：Vulkan/GL 分流 + LBR 调用链 + 排空轮询反汇编 |
