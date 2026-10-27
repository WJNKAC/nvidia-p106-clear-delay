# 42 · pbuffer 路径不能复现 24.8ms；但真进程里 `glMapBuffer` 本身是 30.3ms

> 2026-10-26 01:10
> 用户指令：先零成本读 primus 源码确认回读目标，再用 rb_lat2 pbspin 确认"pbuffer+swap 能否复现 24.8ms"；
> 能复现才做 N21，不能复现说明 24.8ms 不在 pbuffer 路径上，要往 primus 的同步逻辑查。
>
> **三条结论，第二条带一个必须立刻记录的旧结论作废：**
> 1. **源码确认**：worker 读 `GL_FRONT`，pbuffer 是双缓冲，主线程 swap 把 BACK 换到 FRONT；另有一个**独立的显示 worker 用 Intel GPU 上屏**。
> 2. **`rb_lat2` 的 pbuffer 模式是坏的**（`glXCreatePbuffer(dpy,cf[0],NULL)` 不给尺寸）→ `gpu_copy=0.122ms`（68GB/s，超 PCIe 上限 20 倍）。**报告 26 §5 的"pbuffer D≈0"是这个假象，作废。**
> 3. **严格复刻（rb_lat6）后：pbuffer+swap+读 FRONT = 2.602ms，不能复现 24.8ms。** 但实测真进程里 **`glMapBuffer` 本身是 30.289ms**（`PRIMUS_SLEEP=0`）——
>    **所以 24.8ms 不在"pbuffer 路径"，而在"真进程让 map 变慢"的某个东西上**，且**不在显示交接**（`st` 探针 0 命中）。

---

## 1. 源码确认（零成本，先把目标钉死）

`primus_libglfork.cpp`：

```c
// glXSwapBuffers（主线程）
primus.afns.glXSwapBuffers(primus.adpy, di.pbuffer);            // ← 交换 pbuffer
di.sync = primus.afns.glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
sem_post(&di.r.acqsem);
sem_wait(&di.r.relsem);

// readback_work
primus.afns.glReadBuffer(GL_FRONT);                             // ★ line 512，只设一次
for (;;) {
  sem_wait(&di.r.acqsem);                                       // 领活
  glWaitSync(di.sync, ...);
  glReadPixels(0,0,w,h,GL_BGRA,GL_UNSIGNED_BYTE,NULL);           // → PBO，异步
  if (!primus.sync) sem_post(&di.r.relsem);                      // 主线程在此被放开
  usleep(sleep_usec);
  pixeldata = glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);    // ← 等拷贝落地
  if (!primus.sync && sem_timedwait(&di.d.relsem, &tp)) ...       // ← 等显示 worker
  di.pixeldata = pixeldata; sem_post(&di.d.acqsem);
  glUnmapBuffer(...);
}

// display_work（★ 在 ddpy = 显示 GPU 上跑）
sem_wait(&di.d.acqsem);
glTexSubImage2D(..., di.pixeldata);      // ← 用 map 返回的主机指针上传
if (!primus.sync) sem_post(&di.d.relsem); // 放开 readback worker
glDrawArrays(...); glXSwapBuffers(ddpy, di.window);   // ← 真正上屏
```

**三个要点**：

| 要点 | 含义 |
|---|---|
| `glReadBuffer(GL_FRONT)` + 双缓冲 pbuffer + 主线程 swap | 读的是**刚被换到 front 的那一帧**；下一次 swap 会把它换成 back |
| pbuffer 尺寸来自 `create_pbuffer()`：`{W,H, GLX_PRESERVED_CONTENTS,True, None}`，**不含 GLX_DOUBLEBUFFER** | 双缓冲与否**继承 FBConfig**（`match_fbconfig` 从应用的 visual 复制，通常带 DOUBLEBUFFER） |
| 有**第二个 worker** 用显示 GPU（Intel）上屏，通过 `di.d.relsem/acqsem` 与回读 worker 握手 | 这一段在报告 26 的 17 探针里**没有单独计时**（它在 map 之后） |

---

## 2. 先纠一个旧结论：`rb_lat2` 的 pbuffer 模式是坏的

按用户建议直接跑 `rb_lat2 pbspin`：

| 模式 | D_p50 | gpu_copy | 带宽 |
|---|---|---|---|
| fbo | 2.488 ms | 2.497 ms | 3.32 GB/s ✅ |
| pb | **0.000 ms** | **0.122 ms** | **68 GB/s** ❌ |
| pbspin | **0.000 ms** | **0.122 ms** | **68 GB/s** ❌ |
| pbspin_fence | **0.000 ms** | **0.122 ms** | **68 GB/s** ❌ |

**68 GB/s 是 PCIe Gen1 x16 上限（3.15 GB/s）的 21 倍——物理不可能。**

根因在 `rb_lat2.c:54`：

```c
GLXPbuffer pb = glXCreatePbuffer(dpy, cf[0], NULL);   // ← 没给 W/H
```

按 GLX 规则，不给 `GLX_PBUFFER_WIDTH/HEIGHT` 时由实现自选尺寸。实测 `glReadPixels(0,0,1920,1080)`
大部分落在越界区域，读回来的是空的 → 拷贝瞬间"完成"。

> ⚠️ **报告 26 §5 的"pbuffer 模式 D=0.000"就是这个假象**，当时被当作"pbuffer 路径快"的证据。
> 这条现在作废。技能库里"pbuffer 回读 D≈0，很快 → 自检是否随像素量缩放"那条判据，
> 正是当时为了防这个坑写的——但没人回头用它复核那个数。

---

## 3. 严格复刻：pbuffer+swap+FRONT = 2.602ms（不能复现）

新写 `rb_lat6.c`，把四点做对：
1. **显式指定** `GLX_PBUFFER_WIDTH/HEIGHT`，并用 `glXQueryDrawable` **回读实际尺寸**做自检；
2. **显式要求** `GLX_DOUBLEBUFFER`，并 `glXGetFBConfigAttrib` 确认实得值；
3. **阳性对照**：全屏 clear 成已知色，检查 map 出来的首像素（初版把 clear 也 scissor 掉了，导致 0%，已修）；
4. `glFinish` 在 readPixels 前，把 GPU 计算剥掉（报告 41 的教训）。

实测（1920×1080，双缓冲，FINISH=1，6s/组）：

| dbuf | mode | D_p50 | gpu_copy | 带宽 | 阳性对照 |
|---|---|---|---|---|---|
| 1 | back（不 swap） | 2.487 ms | 2.490 ms | 3.33 GB/s | — |
| 1 | front（不 swap） | 2.487 ms | 2.491 ms | 3.33 GB/s | — |
| 1 | swap_back | 2.602 ms | 2.608 ms | 3.19 GB/s | — |
| **1** | **swap_front（= primus）** | **2.602 ms** | **2.607 ms** | **3.19 GB/s** | **100.0%** |
| 0 | front（单缓冲） | 2.602 ms | 2.609 ms | 3.19 GB/s | — |
| 0 | swap_front | 2.602 ms | 2.607 ms | 3.19 GB/s | — |
| 1 | swap_front + 400 quad | 2.602 ms | 2.607 ms | 3.19 GB/s | — |

（修复阳性对照后的单独复跑：`D=2.602 / gpu=2.607`，**阳性对照 100.0%（1496/1496）**，两者独立互证）

**结论：pbuffer + swap + 读 FRONT + 双缓冲 + 并发负载，全部落在 2.60ms，就是 PCIe 物理下限（2.63ms）。**

- 与 FBO（2.487）只差 0.115ms → **swap 与 FRONT 读的代价 ≈ 0.1ms，不是 22ms**。
- 单缓冲/双缓冲无差别。
- 加 400 quad 负载也无差别（与报告 41 一致）。

→ **按用户预案：24.8ms 不在 pbuffer 回读路径上，转查 primus 同步逻辑。**

---

## 4. ★ 但真进程的账本指向了一个更直接的地方：`map` 自己就是 30.3ms

用已有的 `primus_probe_offsets.md` 偏移（11 个探针，注册校验 11/11），在真实 Heaven 里跑两组：

### 4.1 默认（autosleep=90）

| 段 | n | p50 | p90 |
|---|---|---|---|
| rp `glReadPixels` | 263 | 0.110 ms | 0.137 |
| us `usleep` | 262 | **27.754 ms** | 28.876 |
| mb `glMapBuffer` | 263 | **3.037 ms** | 3.280 |
| msw 主线程 `sem_wait` | 262 | 25.092 ms | 27.067 |
| **一轮（acq→下一个 acq）** | 262 | **31.00 ms** | → 32.3 fps |

`us + mb = 27.754 + 3.037 = 30.79ms ≈ 31.00ms` ✅ **守恒律在真进程里也成立**

### 4.2 PRIMUS_SLEEP=0（关掉 autosleep）

| 段 | n | p50 | p90 |
|---|---|---|---|
| rp `glReadPixels` | 269 | 0.078 ms | 0.124 |
| us `usleep` | 269 | **0.059 ms**（已关） | 0.075 |
| **mb `glMapBuffer`** | 268 | **30.289 ms** | 31.675 |
| msw 主线程 `sem_wait` | 268 | 25.189 ms | 27.174 |
| **一轮** | 268 | **30.48 ms** | → 32.8 fps |

`us + mb = 0.059 + 30.289 = 30.35ms ≈ 30.48ms` ✅ **同样守恒**

### 4.3 两张表合起来说明什么

**真进程的 `T_land` ≈ 30.4ms**（`PRIMUS_SLEEP=0` 时 map 直接等于它）。
autosleep 只是把这个 30.4ms 在 "usleep" 和 "map" 之间分配——**总量不变**，这正是报告 26 的结论，
也再次确认报告 39/40 的守恒律。

**而隔离环境的 `T_land` = 2.48~2.60ms。同一张卡、同样 8.29MB、同一个 `glMapBuffer`、同样 pbuffer+swap。**

→ **`T_land` 差 12 倍，差值不在 GL 调用路径上（N23 已排除 pbuffer/swap/FRONT），
也不在 GPU 负载上（N18/N19/N20 已排除），更不在显示交接（见 §5）。**

---

## 5. 显示交接这一段没测到（诚实标注）

源码里 `sem_timedwait(&di.d.relsem)` 是"等显示 worker"，报告 26 的 17 探针里它在 map **之后**，
所以从未被计入 D。我用偏移表里的 `st_in=0x18755` / `st_out=0x1875a` 挂了探针，
**注册成功（11/11）但两组采集都是 0 命中**。

两种可能：
1. **偏移 `0x18755` 不是那条 `sem_timedwait`**（偏移表这一行当年没验证过命中数）；
2. 该分支没走到（但 `primus.sync=0` 是默认值，按理必然走到）。

→ **这一段的偏移需要重新反汇编核对**，暂不下结论。

**但它不影响主结论**：`mb`（map）在 `st` **之前**，且 `us+mb ≈ 一轮总时长` 已经闭合到 98%。
显示交接若真有 20ms，会打破这个闭合（`us+mb` 会远小于一轮）。
**所以 30.4ms 确实花在 `map` 自己身上，不是显示交接。**

---

## 6. 剩下的唯一差异：`map` 为什么只在真进程里慢 12 倍

隔离环境和真进程之间，**已经排除**的差异：

| 差异 | 排除方式 |
|---|---|
| FBO vs pbuffer | N23：pbuffer 2.60ms |
| 读 BACK vs FRONT | N23：两者都是 2.487/2.602 |
| swap 与否 | N23：2.487 → 2.602（+0.115ms） |
| 单/双缓冲 | N23：无差别 |
| 并发 GPU 负载（0~28ms） | N18/N19/N20：map 恒 2.48ms |
| 显示 worker 交接 | §5：`us+mb` 闭合，说明不在那里 |

**还没排除的**：

| 候选 | 为什么可疑 | 怎么测 |
|---|---|---|
| **★ swap 在拷贝飞行期间发生** | N23 是"swap → 读"**串行**（同线程、glFinish）。真进程是 **app 持续 swap**，而 PBO 拷贝从 FRONT 出发、**在飞行中 front 又被换走** → 驱动必须等/重定向 | rb_lat6 加**双线程模式**：线程 A 连续 render+swap，线程 B readback+map。这是唯一没测过的结构差 |
| **两个 context 共享同一 PBO** | primus 的 PBO 在 app context 与 worker context 间共享；N23 只有一个 context | 同上（双线程双 context，share group） |
| **双 PBO 交替 + GL_STREAM_READ** | primus 有两个 PBO，`cbuf^=1` 交替；隔离里只有一个。理论上更宽松，但 orphaning 行为可能不同 | 加"双 PBO 交替"变体 |
| **真实场景的 draw 量/纹理压力** | Heaven 每帧成千上万 draw call；隔离只有 1~1200 个 quad | N18 已把"量"推到 28ms，但**种类**不同（着色/纹理/深度 vs 纯混合填充） |

**最该先做的是第一行**：它把"N23 的串行"变成"真进程的并发 swap"，是唯一还没被覆盖的结构差。

---

## 7. 下一步（N25，零成本）

**给 rb_lat6 加双线程模式**：

```
线程 A（模拟 app）：for(;;) { render(i++); glXSwapBuffers(pb); fence_i = glFenceSync(); post(acq); wait(rel); }
线程 B（模拟 worker）：for(;;) { wait(acq); glWaitSync(fence); glReadPixels→PBO; post(rel); glMapBuffer; ... }
```

判读：

| 结果 | 结论 |
|---|---|
| `map` 跳到 20~30ms | **复现成功** → 病根 = "swap 与飞行中的 FRONT 拷贝冲突"，N21（改 FBO/换读源）有明确落点，且可直接试修 |
| `map` 仍 2.5ms | 与 pbuffer/swap/线程结构都无关 → 只能是"真实 Heaven 的渲染内容"造成的（第 4 行候选），转去查 draw call 种类 |

这比继续在驱动内部挖更省事：它是**最后一个已知的结构差**。

---

## 8. 产物

```
analysis/probes/
├── rb_lat6.c              ★ 严格 pbuffer 探针（尺寸显式+自检、dbuf 可控、阳性对照、RL5_FINISH 同款剥离）
├── n23_pbpath.sh          dbuf × mode × load 矩阵
├── n24_display_handoff.sh 真进程 11 探针（含 st：等显示 worker）
└── n24_analyze.py         分段账 + 守恒闭合检查

analysis/traces/
├── n23/n23a.summary.txt   8 组
└── n24/n24a-{default,nosleep}.trace + n24a.log
```

### 8.1 本轮修掉的三个坑

1. **`rb_lat2` 的 `glXCreatePbuffer(...,NULL)`**：不给尺寸 → 回读越界 → 假快 68GB/s。
   **任何 pbuffer 测试都必须 `glXQueryDrawable` 回读实际尺寸。**
2. **`rb_lat6` 初版把 clear 也 scissor 掉**：探针像素从未被写 → 阳性对照 0%。
   **scissor 只用于负载绘制，clear 必须全屏。**
3. **`gcc` 里 `glGenBuffers` 没有原型**（系统 `gl.h` 太老）：必须走 `glXGetProcAddress` 的函数指针。

### 8.2 一条口径提醒

带宽打印有个单位错误：`8.29MB/2.602ms` 应显示 **3.19 GB/s**，程序里打成了 3186.13 GB/s
（MB→GB 少除 1000）。数字本身没问题（3186 MB/s），只是标签错。
