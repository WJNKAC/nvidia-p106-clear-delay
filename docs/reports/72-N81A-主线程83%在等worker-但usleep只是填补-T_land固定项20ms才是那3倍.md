# 72 · N81-A / N81-B：主线程 83% 在等 worker；但 `usleep` 只是填补，真正的量是 `T_land` 固定项 19ms

日期：2026-10-26 11:05
驱动：525.116.03（P106-100，1920×1080，primus）
本轮两个任务：**N81-A** 分解主线程三段；**N81-B** 查 515 独有的 flags gate 是否延续

---

> ## ⚠️ 本报告有两处已被 **报告 73（N82）** 更正（2026-10-26 11:20）
>
> **更正 1（分段表错误）**：§1「readback worker 的一帧」表里，`glReadPixels = 0.146ms` **是错的**
> （那个数字来自另一次采集，误抄），且 `p_rp→p_mb = 30.191ms` 被误标成了 `glReadPixels`——
> 它实际跨了 **`glReadPixels` + `sem_post` + `usleep` + `sem_wait`** 四件事。
>
> 正确的 Heaven HIGH 分解（全套 10 探针，报告 73 §4.1）：
> `glWaitSync 0.007 / glReadPixels 0.105 / usleep 31.396 / glMapBuffer 3.545`（合计 35.05ms）
>
> **更正 2（"0 次也是 38ms"的理由）**：§4 的结论**碰巧成立但样本有问题**——
> 同一配置两次采 `glMapBuffer` 次数 **342 vs 263**（Heaven 在循环场景，两次不是同一段）。
> 正确说法：存在**两类帧**（有轮询 17~20 次 / 整帧不轮询），**两类时长都是 37~38ms**，
> 所以"轮询次数不是成因"仍成立，但依据是"两类帧时长相同"，不是"0 次档"。
>
> **仍成立**：主线程 83% 在等 worker、swap 内部 0.042ms、`PRIMUS_SLEEP=0` 使 `map` 吸收睡眠、
> `usleep` 是填补物、`T_land` 截距 20.39 vs 1.29 的量级。
>
> **但要改一处解释**：`usleep` 不是"填补"，而是**在给 GPU 让时间**（报告 73 §3.4）。
> 关掉后 `map` 涨到 29.6~37.8ms，是因为 worker 立刻要数据、GPU 还没处理完。

---

## 0. 三句话

1. **N81-A**：主线程一帧 33.5ms 里，**等 readback worker = 27.8ms（83%）**，应用渲染 = 5.5ms，swap 内部 = 0.04ms。worker 那 33.5ms 里 `usleep` 30.2ms + `glMapBuffer` 3.3ms。
2. **但 `usleep` 不是原因**：`PRIMUS_SLEEP=0` 让 `usleep` 归零后，`glMapBuffer` 从 3.3ms 涨到 **29.6~37.8ms**，帧周期只从 33.5 降到 **29.75ms**。⇒ **`usleep` 是在填补一段本来就存在、并且长度固定的等待**。
3. **N81-B 假设否定**：那个 flags gate 在 **515 / 525 / 535 都有**（都是 bit 1），580 换成 bit 30。它**不是** 515 独有。报告 71 §3.2 的说法作废。

---

## 1. N81-A：线程角色（实证，不靠推理）

9 个 uprobe，10s，525 默认配置。事件按 tid 分组的**事件集合**直接给出角色，不需要先验假设：

| tid | 身份判据 | 事件集 |
|---|---|---|
| **68331 = PID** | 有 `p_sw` | `p_sw` 299、`p_post` 299、`p_wdone` 299、`i_swap` 299 |
| 68379 | 有 `p_ws`/`p_mb` | `p_ws` 299、`p_rp` 299、`p_mb` 299、`p_mbd` 299 |
| 68378 | 只有 `i_swap` | `i_swap` 299 |

- **主线程 = `tid == pid`**（这条纪律在报告 71 已立，本轮再次验证）。
- `i_swap` 在主线程与 display worker 上**各有 299 次**：说明 `libGLX.so.0:glXSwapBuffers` 是 glvnd 的厂商无关入口，主线程（提交 NVIDIA 侧）和 display worker（上屏 Intel 侧）都会经过它。**不能把它当成 display worker 专属探针**。
- `n_texsub`（`libGLX_nvidia:glTexSubImage2D`）= **0 次**：display worker 的上传不走这个导出入口（可能走 PBO/texImage 或另一个指针）。**这一路本轮没抓到，是缺口**。

### 主线程的一帧（事件流实测）

```
p_wdone ──5.5ms──> p_sw ─0.04ms─> [i_swap] ─> p_post ──27.8ms──> p_wdone
        应用渲染           swap 内部              等 readback worker
```

| 段 | 中位 | 占帧 |
|---|---|---|
| `p_sw→p_post`（swap 内部：XFlush + 查表 + 提交） | **0.042 ms** | 0.1% |
| **`p_post→p_wdone`（等 readback worker）** | **27.834 ms** | **83.1%** |
| `p_wdone→下一帧 p_sw`（应用渲染下一帧） | 5.553 ms | 16.6% |
| **帧周期** | **33.517 ms** | 29.8 fps |

### readback worker 的一帧

| 段 | 中位 | 占轮 |
|---|---|---|
| `glMapBuffer` | 3.317 ms | 9.9% |
| `glWaitSync` | 0.007 ms | 0.02% |
| `glReadPixels` | 0.146 ms | 0.4% |
| **`p_rp→p_mb`（= `sem_post(relsem)` + `usleep` + `sem_wait(acqsem)`）** | **30.191 ms** | **90%** |
| 整轮 `p_mb→p_mb` | 33.573 ms | — |

> ⚠️ 名字纠正：`p_rp→p_mb` 这一段**不是** `glReadPixels`，而是 readPixels 提交之后到下一次 map 之间。
> 上一轮（报告 71）把它标成 `usleep` 段是对的，但这里要写明它**还包含**两次 sem 操作。本条是踩过的坑，见 §6.2。

### display worker

`i_swap` 间隔中位 **33.563ms**，与帧周期同值 ⇒ 每帧上屏一次，**不是瓶颈**。

---

## 2. `PRIMUS_SLEEP=0`：决定性对照

同一 Heaven 场景，只改一个环境变量，同样的探针：

| 量 | `PRIMUS_SLEEP=90`（默认） | `PRIMUS_SLEEP=0` |
|---|---|---|
| 帧周期 | 33.517 ms（29.8 fps） | **29.753 ms（33.6 fps）** |
| `glMapBuffer` | **3.317 ms** | **29.600 ms** |
| `glReadPixels` | 0.146 ms | 0.146 ms |
| 主线程等 worker | 27.834 ms | 23.926 ms |
| 应用渲染 | 5.553 ms | 5.417 ms |

**`map` 吸收了几乎全部被移走的睡眠**（3.3 → 29.6ms，+26.3ms；睡眠减少 30.2ms）。

这就是技能库里那条式子的实测再现：

```
map = max(0, T_land − sleep)
T_land = sleep + map = 30.19 + 3.32 ≈ 33.5ms   (SLEEP=90)
T_land = 0     + 29.60 ≈ 29.6ms                 (SLEEP=0)
```

**两条推论（都很硬）：**

1. **`usleep` 是果不是因。** 它在填补一段时间，关掉它帧周期只改善 11%。
   ⇒ 报告 59（N62）"关掉 autosleep fps 不变"**本轮独立复现**（33.5→29.75ms，不是 3 倍）。
2. **`glMapBuffer` 是余额，不是开销。** 它自己不干活（报告 71 已证 11 条指令），它等的是"回读数据真正落地"。
   ⇒ **任何"map 多久"的横向比较必须换算成 `T_land`**，否则同一个系统换个配置就能得出相反结论（本轮 3.3ms vs 29.6ms 是同一件事）。

---

## 3. `T_land` 拆开：固定项 20.4ms 才是那 3 倍

报告 56 拟合过 `T_land = 截距 + 斜率 × MiB`：

| | 截距（与像素无关） | 斜率 | 1080p（8.29MiB）T_land |
|---|---|---|---|
| **525.116.03** | **20.39 ms** | 1.335 ms/MiB | **31.5 ms** |
| **515.105.01** | **1.29 ms** | 1.227 ms/MiB | **11.5 ms** |
| 差 | **−19.1 ms** | −0.108 | **−20.0 ms** |

**两版斜率几乎相同（1.335 vs 1.227，差 8%），截距差 16 倍。**
⇒ **3 倍差距不是带宽，是那 20ms 的固定延迟。**

本轮实测与之吻合：1080p、SLEEP=0 时 `map = T_land = 29.6~37.8ms`（不同采样窗口），与 31.5ms 同量。

**固定项与像素量无关**（640×360 像素是 1080p 的 1/9，固定项不变）⇒ 它**不是**填充/带宽，只能是"每帧一次、与分辨率无关"的开销或延迟。

---

## 4. 排除 `wait_ms` 轮询（本轮的自我纠错）

我一开始看到 SLEEP=0 的一帧里 `glMapBuffer` 37.81ms、`wait_ms` 中位 19 次、上限 20 次，
而 `19 × 2ms = 38ms` —— 几乎完美。差点就写成"map 的 38ms = 19 次 2ms 轮询"。

**逐帧拟合直接否掉：**

| `wait_ms` 次数 | 帧数 | map 中位 |
|---|---|---|
| **0** | 68 | **38.28 ms** |
| 17 | 3 | 37.08 ms |
| 18 | 12 | 36.92 ms |
| 19 | 62 | 37.22 ms |
| 20 | 117 | 37.86 ms |

```
拟合: map = 38.18 − 0.026 × (wait_ms 次数)
残差中位 0.07ms
```

**0 次也是 38ms，20 次也是 38ms。** ⇒ `wait_ms` 是 map 等待期间**顺带发生的轮询**，不是等待的成因。
`19×2ms≈38ms` 是巧合（因为轮询周期 2ms 与固定等待 38ms 恰好同量级）。

> **教训**：整数倍吻合看起来像机制，但必须做**对照点**（这里是"0 次"这一档）。
> 只统计"有 N 次的那批"永远发现不了"没有 N 次也一样慢"。
> **任何"X 次 × Y 周期 = 总时长"的假说，必须包含 X=0 的样本。**

---

## 5. 环境事实：GPU 100% 忙，但 PCIe 只有 Gen1

Heaven 运行中采样 10 次（SLEEP=0，1080p）：

```
utilization.gpu  100 %   ← 十次全 100
utilization.mem   9~14 %
clocks.sm        1898 MHz  (P0 最高性能态)
clocks.mem       4006 MHz
pstate           P0
temperature      48 °C
power            55~68 W
```
限流项全部 `Not Active`（SW Power Cap / HW Slowdown / Thermal / Sync Boost）。

**PCIe 链路**

```
GPU Link Info → PCIe Generation
   Max              : 1
   Current          : 1
   Device Current   : 1
   Device Max       : 1     ← ★ 卡自己只报 Gen1
   Host Max         : 3     ← 主板上限是 Gen3
```

- **卡端最高只支持 Gen1**（`Device Max = 1`）。Gen1 ×16 ≈ 4 GB/s 原始、≈3.2 GB/s 有效。
- 实测斜率 1.335 ms/MiB ⇒ 0.75 GB/s，只有 Gen1 理论值的 **23%** —— 回读路径本身效率不高。
- 但这**解释不了版本差异**：两版跑在同一条链路上。

**GPU 100% 忙 + 显存带宽只用 13%** ⇒ GPU 是**计算/命令处理饱和**，不是带宽饱和。
与"固定项 20ms 与分辨率无关"一致：那 20ms 是 GPU 处理**命令流**的时间，不是填像素的时间。

⚠️ 但 `utilization.gpu` 只表示"有 kernel 在跑"，**不能区分"在干有效工作"与"在空转等 fence"**。
这一条本轮**没有**解决，是下一步的靶点。

---

## 6. N81-B：flags gate 不是 515 独有（报告 71 更正）

### 6.1 事实

提交器里那道 gate 的判据是"全局 flags 的 bit 1 置位 → 强制走 `esi=7` 的 flush 路径"。
它在各版本**都存在**，只是寻址形式与偏移在变：

| 版本 | 指令 | 位置 | 语义 |
|---|---|---|---|
| **515.105.01** | `mov eax,[_nv026glcore+0x8f0+0xb48]` ; `and eax,2` ; 之后 `test eax,eax; jne`（`0xc836ae`） | `0xc835e5` | dword, bit 1 |
| **525.116.03** | `lea rax,[_nv026glcore+0x8f0]` ; `test BYTE PTR [rax+0xc08],0x2` ; `je/jmp` | `0x1238b31` | byte, bit 1 |
| **535.129.03** | `test BYTE PTR [rip+0x190001f],0x2`（`_nv021glcore+0x2888`） | `0x12e5982` | byte, bit 1 |
| **580.178.04** | `mov eax,[_nv015glcore+0x1cd0]` ; `test eax,0x40000000` | `0x81254e` | dword, **bit 30** |

**结论：这不是版本分界点。** 515→535 都是 bit 1，只在 580 改成 bit 30（同一决策点，位号重排）。

### 6.2 报告 71 §3.2 为什么错

报告 71 只反汇编了 525 提交器的**前 0x1f0 字节**，而 gate 在 **+0x211**（`0x1238b31`），刚好在视野外。
515 的 gate 在 `+0xe5`，一眼就看到了。

> **教训**：做"某版多了/少了某段"的结论前，**必须把两个函数都完整反汇编到 `ret`**，
> 不能用"我看的那一段里没有"代替"整个函数里没有"。
> 事后必须补一句：**我的反汇编窗口覆盖到哪里？**

这条与技能库已有的「下『某版本特有』的结论前必须去另一版同一位置核实」是同一条纪律的加强版：
**不仅要核实同一位置，还要核实搜索窗口的完整性。**

---

## 7. 现在的位置（诚实的收口）

**已经把 3 倍差距压到一句话：**

> 525 上，一帧的"回读落地时间 `T_land`"有一个 **20.4ms 的固定项**（515 只有 1.29ms），
> 与分辨率无关、与调度无关、与 primus 的 `usleep` 无关、与 `glMapBuffer` 内部逻辑无关、
> 与 `wait_ms` 轮询次数无关。

**已排除清单（本轮新增）**

| 已排除 | 依据 |
|---|---|
| 主线程 swap 段内部开销 | 0.042ms，0.1% |
| display worker / 上屏 | `i_swap` 间隔 = 帧周期，同拍 |
| primus autosleep | SLEEP=0 只改善 11%，map 吸收余量 |
| `glMapBuffer` 内部等待逻辑 | 11 条指令，无 fence/sem（报告 71） |
| `wait_ms` 2ms 轮询 | 逐帧拟合斜率 −0.026，0 次档同样 38ms |
| flags gate 版本差异 | 各版本都有（本轮） |
| GPU 降频 / 限流 | P0、1898MHz、48°C、无限流 |

**仍成立且未拆开的一支：**

> 那 20ms 的固定项，是 **GPU 处理该帧命令流的时间**，还是 **回读 fence 的固定延迟**？
> 两者都与像素无关，用分辨率扫描分不开。

---

## 8. 下一步

| 编号 | 做什么 | 判据 |
|---|---|---|
| **N82-A（推荐，一次采集）** | 在 SLEEP=0 下同时测：①`glMapBuffer` 时长；②**该帧 `glXSwapBuffers` 之后到 map 开始之间，GPU 有无 kernel 在跑**（用 `nvidia-smi dmon` 或 driver 侧 `dma_fence_wait_*` 事件）；③把 Heaven 换成 **`glxgears` 级别的最小场景**看 20ms 是否仍在 | ①最小场景仍有 ~20ms → **固定延迟**（fence/timer 语义）；②最小场景掉到 <2ms → **是命令流处理**，回到减 draw call |
| **N82-B（零成本，静态）** | 找那 20ms 的"整数倍"来源：`ctx+0x8e0e00` 附近的常数表（`0x19`=25、`0x14`=20、`0x28`=40）与 `wait_ms` 的**调用点**有几处、各自超时值 | 若有 `20ms`/`40ms` 的硬编码超时，直接对上截距 |
| **N82-C（零成本）** | 复算报告 56 的 515 截距（1.29ms）来源，确认它是在 `sleep=0` 下测的还是换算的 | 若是换算得来，515 的 `T_land` 需要重测才能作为"1.29ms"引用 |
| ~~N80~~ | 插 `glFinish` 分 map | **早已取消** |
| ~~N81-B~~ | flags gate | **已完成：假设否定** |

---

## 9. 产物

```
analysis/n64/n81b_gate.py      # 跨版本定位 glMapBuffer impl + 提交器
analysis/n64/n81b_find.py      # 在提交器里搜 flags gate
analysis/n64/n81a_over.py      # 线程角色（按事件集合）
analysis/n64/n81a_full.py      # 主线程/worker 分段分解（含配对校验）
analysis/n64/n81a_wait.py      # wait_ms 计数 vs map 时长
analysis/probes/n81a.sh        # 9 探针主采集
analysis/probes/n81a_sleep.sh  # PRIMUS_SLEEP 对照
analysis/probes/n81a_wait.sh   # wait_ms 计数
analysis/n81a/trace1.txt          # 默认 SLEEP=90
analysis/n81a/trace2_sleep0.txt   # SLEEP=0
analysis/n81a/trace3_wait.txt     # SLEEP=0 + wait_ms
```
