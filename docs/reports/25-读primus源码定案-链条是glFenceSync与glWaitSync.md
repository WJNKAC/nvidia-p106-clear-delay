# 25 · 读 primus 源码定案：链条是 glFenceSync→glWaitSync，睡眠是果不是因

> # ⚠️ 本文的核心机制判决已被报告 26 推翻（2026-10-25 18:50，已独立复核）
> **实测：`glWaitSync` 中位 5µs（n=521，max 39µs），不是 25ms。**
> 我按原始 trace 独立重算（不依赖报告 26 的分析器），逐项吻合：
> `glWaitSync` 0.005ms / `glReadPixels` 0.119ms / `usleep` 26.441ms / `glMapBuffer` 2.864ms /
> `sem_wait(relsem)` 22.009ms / **D = 29.369ms** / **主线程 msw_out − rp_out = 0.057ms（57µs）**。
>
> ## 错在哪（三处，按严重程度）
>
> **① 数量级上的自相矛盾没抓住（最要命）。**
> 我自己测出主线程 `sem_wait` 中位 **26.92ms**，又断言"这段时间在 `glWaitSync`"。
> 但 worker 本轮 `glWaitSync + glReadPixels` 合计只有 **0.124ms**——**26.9ms 不可能装进 0.12ms 的段里，差 200 倍**。
> 这个矛盾在本文 §3 里就摆着，我却用"Hmm 睡眠是果不是因"盖过去了。**任何时刻，先做量纲检查。**
>
> **② 把异步流水线的"相位"搞错了。**
> 我以为主线程等的是 worker **本轮**的 `glWaitSync+RX`；实际上 worker 是循环的，且
> **主线程相位比 worker 落后一轮**：主线程 `msw_in`（进 sem_wait）时，worker 已经在跑**下一轮**的
> `usleep` 中段（约 4.6ms 处）。所以 `sem_wait` 的长度 = **worker 一轮的剩余时间** ≈ 剩余 usleep + map + 尾 + ws + rp。
> **源码顺序 ≠ 执行序**（报告 26 §1：`sem_post(relsem)` 编到 0x18a10 出站块，地址上在 mb_out 之后、执行序在 rp 之后）。
>
> **③ 有可直接测的量却用推理。**
> `glWaitSync` 的耗时只需 `ws_in`/`ws_out` 两个探针就能夹住。我给 glcore 的 `wait_ms`、给 `sem_wait`、
> 给 `usleep` 都挂了探针，**唯独没给最关键的 `glWaitSync` 挂**，改用"glReadPixels 是异步的，所以时间全在 glWaitSync"来推断。
>
> ## 具体作废条目
>
> | 本文位置 | 原结论 | 状态 |
> |---|---|---|
> | §3 全节 | "那 25ms 基本全在 `glWaitSync`" | **❌ 推翻**（实测 5µs） |
> | §3.1 链条图 | `glWaitSync 无限等 → 驱动 wait_ms(100ms) → 25ms` | **❌ 推翻**（`wait_ms` 观测仍有效，但不是 26ms 所在） |
> | §5 | "525 的回归 = 与数据量无关的**固定** ~26ms **fence 完成延迟**" | **❌ 改判**：真凶是 D（回读完成延迟）≈29.4ms，范围 24.06~33.08ms，**不是固定值** |
> | §5 反推 | "`map_time` 从 1.1ms 涨到 3.1ms，被 autosleep 放大 9 倍 → 28ms 睡眠" | **◑ 数字巧合、因果错**：实测 map 2.86ms 与反推 3.13ms 接近，但**因果方向反了**——D 是独立量，sleep 是 D 的跟随者 |
> | §5.2 | 三峰 4.17ms 台阶 | **◑ 降级**：可能在 D 的分布里自然解释（报告 26 §9） |
>
> ## 仍然有效的部分
>
> - §1 源码链条（`glFenceSync` → `sem_post(acqsem)` → `sem_wait(relsem)`）✔
> - §1.1 配置默认值 `PRIMUS_SYNC=0` / `PRIMUS_SLEEP=90` ✔
> - §1.2 autosleep 稳态解 `s=9m` ✔（报告 26 用 IIR 帧级残差 13.1µs 坐实）
> - §2.1 `usleep` 实测 28.17ms、§2.2 "关掉只快 2ms" ✔（报告 26 §3 给出机制解释）
> - §4 相位均匀与 `sync=0` 的自洽 ✔
> - §5.1 分辨率扫描数据（26.9/26.0/27.2ms）✔，但解读从"固定延迟"改为"**非带宽**"（报告 26 更强：min 0.014ms）
>
> ## 补充作废（2026-10-25 19:35，报告 27 §2）
>
> **§5.1 那三档分辨率其实全是 1920×1080。** `-video_width/-video_height` 被 `-video_mode 6` 覆盖，
> heaven 日志是 `Set 1920x1080 windowed video mode`。所以：
> - 26.92 / 25.98 / 27.24ms 是**同一配置的三次重复**，不是三个分辨率
> - "与像素量无关"这个结论**从未被真正测过**
>
> **真测之后（报告 27 §3）**：1080p D=31.09ms、640×360 D=21.27ms（像素 1/9，D 只降 31%），
> 拟合 `map_time = 2.05ms 固定 + 0.51ms/Mpx`。**不是"无关"，而是"有一个占 2/3 的固定项"。**
>
> 2026-10-25 16:40
> 方法：按计划第 0 步（零成本并行）读 primus 开源 + 实测验证，**不换驱动、不改配置**。
> **六条结论：**
>
> 1. **完整链条从源码拿到**：主线程在 `sem_wait(&di.r.relsem)` 阻塞，等 readback worker；该 worker 在 `glWaitSync(fence, GL_TIMEOUT_IGNORED)` 等 GPU 完成。
> 2. **`PRIMUS_SLEEP=90` 让 readback worker 每帧睡 28.17ms**（实测，来自 `libGL.so.1+0x18895`），稳态放大 **9× map_time**。
> 3. **但关掉它只快 2ms**：`sem_wait` 中位 **26.92ms → 24.77ms**，fps 34.7 → 30.7（**反而略降**）。
>    → **睡眠是果不是因，真正的阻塞是 `glWaitSync` 的 ~25ms。**
> 4. **主线程用 `sem_wait` 而非 cond**：信号量在 glibc 里就是 futex 实现，所以报告 22 测到的"裸 futex 无限等待"就是 `sem_wait`。**heap 地址 = primus 的 `DrawableInfo` 对象**。
> 5. **那 ~26ms 是与像素量无关的固定延迟**：525 上 1080p/720p/540p 的 `sem_wait` 分别是 26.92 / 25.98 / 27.24ms，
>    像素降到 1/4 阻塞不变 → **排除 PBO/PCIe 带宽**，指向"驱动察觉 fence 完成的时机"。
> 6. **`PRIMUS_SLEEP=0` 不是可用优化**（实测更慢），别写进"规避方案"。

---

## 1. 源码：`glXSwapBuffers` 的完整链条

`primus/libglfork.cpp`（1033 行，已存 `analysis/src/`）：

```c
void glXSwapBuffers(Display *dpy, GLXDrawable drawable) {
  XFlush(dpy);
  primus.afns.glXSwapBuffers(primus.adpy, di.pbuffer);   // ① 交换 NVIDIA pbuffer
  ...
  // Readback thread needs a sync object to avoid reading an incomplete frame
  di.sync = primus.afns.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);   // ② 建 fence
  sem_post(&di.r.acqsem);   // ③ 叫醒 readback worker
  sem_wait(&di.r.relsem);   // ④ ★★★ 主线程阻塞在这里 ★★★
  primus.afns.glDeleteSync(di.sync);
  ...
}
```

readback worker（`readback_work`）：

```c
for (;;) {
  unsigned sleep_usec = 0;
  ...
  primus.afns.glWaitSync(di.sync, 0, GL_TIMEOUT_IGNORED);   // ★ 无限等 GPU fence
  primus.afns.glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
  if (!primus.sync)
    sem_post(&di.r.relsem);        // 解锁主线程（usleep 之前！）
  usleep(sleep_usec);              // 自适应睡眠
  ...
  map_time = ...glMapBuffer(...)   // PBO 回读完
  sleep_usec = (map_time * 1e6 + sleep_usec) * primus.autosleep / 100;
  ...
  sem_timedwait(&di.d.relsem, &tp);   // 等 display worker 释放 PBO（1s 超时）
  sem_post(&di.d.acqsem);             // 叫 display worker 上屏
}
```

### 1.1 关键配置项（源码 + 打包默认值都确认了）

```c
// PrimusInfo
int sync;       // 0: no sync, 1: D lags one frame, 2: fully synced
int autosleep;  // sleep ratio in readback thread, percent
...
sync(atoi(getconf(PRIMUS_SYNC))),        // 默认 "0"
autosleep(atoi(getconf(PRIMUS_SLEEP))),  // 默认 "90"
```

Makefile：`PRIMUS_SYNC ?= 0`、**`PRIMUS_SLEEP ?= 90`**。

**`sync=0` 是关键**：主线程在 `glReadPixels` **发出后**就被解锁，不等 display 上屏。
所以主线程不可能是"等 vblank"——这与报告 24 的相位均匀完全一致。

### 1.2 `autosleep=90` 是一个 9 倍放大器

稳态解：`s = (m + s) × 0.9` → **`s = 9m`**（s = sleep_usec，m = map_time）。

即：**readback worker 每帧睡 map_time 的 9 倍。**

---

## 2. 实测：睡眠确实是 28ms，但它不是原因

### 2.1 直接读 `usleep` 的参数

给 libc `usleep`（`0x1100b0`）挂 uprobe，读 `%di`：

| usleep 参数 | 占比 | 调用者（返回地址归属） |
|---|---|---|
| 1000 | 87.7% | `libUnigine_x64.so +0x26a7fd`（引擎 1ms 轮询，无关） |
| 10000 | 9.4% | `libUnigine_x64.so +0x15abcc`（引擎 10ms，无关） |
| **27304 ~ 31464** | 每帧 1 次 | **`libGL.so.1 +0x18895`** ← **primus 的自适应睡眠** |

按线程分：tid 15349 的 158 次 usleep **中位 28170us、最大 29743us**，且 **158 次 = 158 次 swap**（每帧一次）。
→ **`sleep_usec ≈ 28.2ms`，反推 `map_time ≈ 28.2/9 = 3.13ms`。**

### 2.2 但关掉它只快 2ms

| PRIMUS_SLEEP | 主线程 `sem_wait` 中位 | p95 | fps |
|---|---|---|---|
| **90（默认）** | **26.92 ms** | 30.72 | 34.7 |
| **0（关闭）** | **24.77 ms** | 28.92 | **30.7** |

**睡眠从 28ms 降到 0，主线程阻塞只从 26.9ms 降到 24.8ms（−2ms），帧率反而略降。**

→ **结论：`sleep_usec` 是"果"不是"因"。** 它按 `map_time` 比例增长，本质是 primus 在"PBO 回读有多慢"上做退避；
而真正卡住主线程的是 `glWaitSync`。

> 帧率反而降，是因为关掉睡眠后 readback 线程变忙，与主线程抢 CPU。**`PRIMUS_SLEEP=0` 不能作为规避方案。**

---

## 3. 那 25ms 到底在哪：`glWaitSync`

主线程阻塞 = 从 `sem_post(r.acqsem)` 到 `sem_post(r.relsem)`，中间 readback worker 只做两件事：

```c
glWaitSync(di.sync, 0, GL_TIMEOUT_IGNORED);   // 等 fence
glReadPixels(...);                             // 异步，立即返回
```

`glReadPixels` 绑定了 PBO，是异步的；所以 **24.77ms 基本全在 `glWaitSync`**。

`glWaitSync` 是"等 GPU 执行到 fence 点"，**`GL_TIMEOUT_IGNORED` = 无限等**，没有超时兜底。

### 3.1 与驱动侧观测的闭合

报告 23 测到：glcore worker 在 `wait_ms(cond, 100ms)` 上等，超时 100ms **从未到期**，是被 `notify`（`0x146c680`）唤醒的。
那正是驱动内部**追踪 fence 完成**的机制。

两条观测合起来：

```
glWaitSync 无限等
   ↓ 向驱动查询/等待 fence 状态
驱动 worker 在 arena cond 上 wait_ms(100ms)  ← 被 notify 唤醒
   ↓ 驱动靠**轮询 host 内存标记**发现完成（IRQ 仅 2/s，报告 23）
fence 完成 → glWaitSync 返回 → sem_post(relsem) → 主线程继续
```

**所以那 25ms = "从 GPU 真正完成，到驱动察觉"的延迟 + "驱动察觉，到 glWaitSync 返回"的延迟。**

这与报告 10 在 580 上的 E2 结果同构：**GPU 2.64ms 完成，驱动 30.24ms 才收到**。

---

## 4. 与报告 24 的相位均匀完全自洽

`sync=0` 的语义是"主线程不等上屏"，所以：

- 主线程解锁时刻 = readback worker 走完 `glWaitSync` 的时刻 = **GPU fence 完成的时刻**
- GPU fence 完成与显示刷新**无关**（GPU 自己跑自己的）
- 因此主线程唤醒时刻的 vblank 相位**必然均匀** ✓

报告 24 的相位检验结果（标准差 28.1% ≈ 均匀 28.9%）不再是"意外"，而是**源码层面的必然**。

---

## 5. 为什么 515 快、525 慢：`map_time`/fence 延迟变了

同一份 primus 二进制（`autosleep=90` 固定），唯一能变的是 **`map_time`**（进而 `sleep_usec`）与 **fence 延迟**：

| | 515 | 525 |
|---|---|---|
| 主线程 `sem_wait` | ~10.8ms（报告 22） | **24.8~26.9ms** |
| 反推 map_time | ~1.1ms | **~3.1ms** |
| sleep_usec | ~10ms | **~28ms** |
| GPU 功耗 | 116W | 48~64W |

**`map_time`（`glMapBuffer` = PBO 回读完成）从 ~1.1ms 变成 ~3.1ms，被 autosleep 放大 9 倍变成 28ms 的睡眠。**

### 5.1 525 上的分辨率扫描：与像素量无关（本文新增）

| 分辨率 | 像素 | 主线程 `sem_wait` 中位 |
|---|---|---|
| 1920×1080 | 2.07 M | **26.92 ms** |
| 1280×720 | 0.92 M | **25.98 ms** |
| 960×540 | 0.52 M | **27.24 ms** |

**像素量降到 1/4，阻塞时间基本不变（26.9 → 26.0 → 27.2ms，非单调）。**

→ **那 ~26ms 是固定延迟，不是数据搬运。** 排除 PBO 带宽、PCIe 带宽、回读量。

这与报告 21 在 515 上的结论一致（像素降 4 倍 fps 不变），但这次是**在 525 上、直接测阻塞时长**，
比"测 fps"更贴近机制。

**因此 525 的回归是一个固定量的同步延迟（~26ms），与数据量无关。**
这进一步支持"驱动察觉 fence 完成的时机"而非"搬运太慢"。

### 5.2 三峰与 4.17ms 台阶（待解释的指纹）

报告 22 测到帧间隔/周期呈 **1.5× / 1.75× / 2.0×** 三峰 → 25.0 / 29.2 / 33.3ms。
**相邻峰间距 4.17ms = 16.67ms 的 1/4 = 1/240 秒。**

这个 4.17ms 量化台阶与"~26ms 固定延迟 + 量化"的图景一致，
暗示存在一个 **~240Hz 的采样/定时环节**（4×60Hz 或独立的 240Hz 轮询）。
**本文未能定位它**，是下一步的具体靶点。

---

## 6. 这套结论对"规避方案"的意义

| 候选规避 | 实测效果 | 结论 |
|---|---|---|
| `PRIMUS_SLEEP=0` | 26.9→24.8ms，fps 34.7→30.7 | **无效且更差**，不要用 |
| `PRIMUS_SYNC=1/2` | 未测（会让主线程再等上屏，预期更差） | 不建议 |
| 降分辨率 | 515 上无效（报告 21）；**525 上未测** | **值得测**：若 525 的 map_time 与像素量相关，降分辨率会有效 |
| 换回 515 | 已知有效（10.8ms vs 26.9ms） | 现有最佳规避 |

---

## 7. 下一步（按价值排序）

| 优先级 | 动作 | 能答什么 |
|---|---|---|
| **1** | **定位 4.17ms 量化台阶的来源**（240Hz 定时环节）。线索：驱动侧 `wait_ms` 的超时是 100ms、实际被 notify 唤醒；若 notify 的判定本身按 240Hz 节拍跑，就会产生 4.17ms 量化 | 三峰的来源。若能找到 240Hz 的循环，就直接指向回归代码 |
| **2** | uprobe `glMapBuffer` 前后（Mesa libGL 里找符号）直接量 `map_time` | 把 §5 的反推（3.1ms）变成直接测量 |
| 3 | 静态 diff 515/525 的 fence 完成检测路径（报告 19 缩小的范围 + 报告 23 的 `0x146c680`） | 回归的具体判定条件 |
| ~~4~~ | ~~525 降分辨率~~ **已完成（§5.1）**：26.9/26.0/27.2ms，与像素量无关 | — |
| — | ~~读 primus 源码~~ **已完成**（§1） | — |
| — | ~~按 cond 分组 broadcast~~ 已被本文取代：**主线程根本不用 cond**，那个 cond 是 glcore 内部 fence 追踪用的 | — |

---

## 8. 产物

```
analysis/src/
├── primus_libglfork.cpp      primus 主源（1033 行，含 glXSwapBuffers / readback_work / display_work）
└── primus_Makefile           编译期默认值（PRIMUS_SYNC=0, PRIMUS_SLEEP=90）

analysis/traces/t7/
├── t7_usleep.trace           usleep 参数（发现 28.17ms）
├── t7_ucall.trace + .maps    调用者归属（锁定 libGL.so.1+0x18895）
├── t7_sleep0.trace           PRIMUS_SLEEP=0 帧间隔
├── t7_sleep0_both.trace      SLEEP=0 时的 usleep 分布（10ms 来自 Unigine）
├── t7_sem90.trace / t7_sem0.trace   主线程 sem_wait 时长对照（决定性）

analysis/probes/
├── t7_usleep.sh              读 usleep 参数 + 按线程分布
├── t7_both.sh                同时测 usleep + 帧间隔（确认 env 生效）
├── t7_usleep_caller.sh       抓 usleep 调用者返回地址
├── t7_sleep_sweep.sh         PRIMUS_SLEEP 扫描
└── t7_sem.sh                 sem_wait 时长 + 信号量分组
```
