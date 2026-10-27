# N170 — 官方调优 key 全线无效；且"79.5% 在 sched_yield"是等待姿态，不是调度开销

**日期**：2026-10-27
**驱动**：525.116.03（内核 + 用户态一致）
**起因**：用户问「网上泄露的 NVIDIA 源码有助于分析吗？」→ 转去查**官方**暴露的调优旋钮，作为泄露源码的更优替代

---

## 0. 一句话结论

**NVIDIA 官方自己公布了 24 个 GL registry key（`nvidia-application-profiles-<ver>-key-documentation`），其中 `GLYield` 精确对应我们测到的 `sched_yield` 现象——它确实生效（16,523 次 yield → 0），但性能一分没变。**
**⇒ 惩罚是硬编码在代码路径里的，不是任何可配置项。这一轮同时纠正了"79.5% 在 sched_yield = 调度开销"的误读。**

---

## 1. 意外发现：NVIDIA 主动公布了全部可调 key

`/usr/share/nvidia/` 下有个**官方文档**（不是逆向出来的）：

```
nvidia-application-profiles-525.116.03-key-documentation   7321 字节
nvidia-application-profiles-525.116.03-rc                  10480 字节 / 221 行
```

前者是 **24 个 registry key 的完整说明**（key 名 + 类型 + 语义），后者是针对具体程序的 profile 列表。
**每个 key 都能用环境变量 `__GL_<KEY>` 覆盖。**

这比逆向猜测高效得多 —— 相当于 NVIDIA 把"旋钮清单"和"旋钮含义"直接给你了。

### 24 个 key 全表

| key | 类型 | 本案相关性 |
|---|---|---|
| **`GLYield`** | string | **★★★★★ 直接对应 79.5% 的 sched_yield** |
| `GLThreadedOptimizations` | boolean | ★★★ 多线程提交 |
| `GLWriteTextSection` | boolean | ★★ JIT/patch 相关 |
| `GLSingleThreaded` | boolean | ★★ |
| `GLShaderDiskCache` / `Path` | bool / string | ★ 着色器缓存（已确认本案不受影响） |
| `GLSyncToVblank` / `GLSyncDisplayDevice` | bool / string | ★★ vsync（已测过无效） |
| `GLFSAAMode` / `GLLogMaxAniso` | int | ✗ |
| `GLNoDsoFinalizer` | bool | ✗ |
| `GLSortFbconfigs` / `GLAllowUnofficialProtocol` / `ForceGLXTrueColorVisual` | bool/int | ✗ GLX 层 |
| `GLExtensionStringVersion` | string | ✗ |
| `GLAllowFXAAUsage` / `GLGSYNCAllowed` / `GLSharpen*` | — | ✗ |
| `GLIgnoreGLSLExtReqs` | bool | ✗ |
| `GLShowGraphicsOSD` | bool | ✗ |
| `GLSELinuxBooleans` | string | ✗ |
| `GLDoom3` | bool | ✗ |

---

## 2. `GLYield`：看起来是完美命中，实测零效果

**官方原文**：

```
GLYield (string)
This controls how the NVIDIA graphics driver will perform a yield.
  "USLEEP"        OpenGL will call usleep(0) to yield
  "NOTHING"       OpenGL will never yield
  <any other>     OpenGL will call sched_yield() to yield   ← 默认
```

**为什么它看起来是完美嫌疑**：我们此前测到 **79.5% 的墙钟时间在 `sched_yield` 里**
（报告 155/156，`rpsample2` 采样 + `0x15761b0` 自计时器双重印证）。
一个控制"sched_yield 怎么调"的官方旋钮，正好对上。

### 2.1 生效性验证（这一步不能省）

只看性能没变化是不够的 —— 必须先确认 key 真的被读进去了，否则排除不成立。
用 `strace -c` 数 `sched_yield` 系统调用：

| GLYield | sched_yield 调用次数 | total_p50 |
|---|---|---|
| 默认 | **16,523** | 152.190 ms |
| `NOTHING` | **0**（strace 里整行消失） | 152.149 ms |

**⇒ key 100% 生效了：16,523 次 yield 被彻底消掉。但性能一分没变。**

### 2.2 完整三档 + 其他 key

| 设置 | total_p50 | fps |
|---|---|---|
| 基线（默认 sched_yield） | 152.255 ms | 6.57 |
| `__GL_YIELD=NOTHING` | 152.149 ms | 6.57 |
| `__GL_YIELD=USLEEP` | 152.209 ms | 6.57 |
| `__GL_THREADED_OPTIMIZATIONS=0` | 152.216 ms | 6.57 |
| `__GL_THREADED_OPTIMIZATIONS=1` | 152.176 ms | 6.57 |
| `__GL_WRITE_TEXT_SECTION=0` | 152.262 ms | 6.57 |
| `__GL_WRITE_TEXT_SECTION=1` | 152.128 ms | 6.57 |
| `__GL_SINGLE_THREADED=1` | 151.761 ms | 6.59 |

**全部在 ±0.3% 内波动 = 噪声。没有一个 key 有用。**

---

## 3. ★ 纠正一个被误读的结论

之前的表述是「**79.5% 的墙钟在 `sched_yield` 里**」，很容易被读成「**调度开销占了 79.5%**」。
**本轮实验证明后者是错的。**

推理：

- 如果 79.5% 是**调度开销**（让出 CPU → 等被重新调度 → 切回来），那把 yield 换成纯自旋（`NOTHING`）应当**大幅变快**，因为省掉了每一次的调度往返。
- 实测：yield 从 16,523 次降到 0，**耗时完全不变**。
- ⇒ **时间不在"让出 CPU"上，而是在"等某个条件成立"上。`sched_yield` 只是等待时的姿态（spin posture），不是开销本身。**

> **泛化：profiler 显示"时间在 `sched_yield`"时，不能直接判成"调度开销大"。
> 验证方法只有一个：把 yield 关掉（`__GL_YIELD=NOTHING` 或纯自旋），看时间是否转移。
> 时间不转移 ⇒ 是真实的等待，yield 只是姿态。
> 时间转移 ⇒ 才是调度开销。**
>
> 这条同样适用于内核侧的 `cond_resched` / `cpu_relax` / `pause` 类采样：
> **落在自旋原语上的样本，说明"在等"，不说明"在花"。**

这条也顺便解释了为什么报告 155 里"删掉 38.5% CPU 的函数 `0x15c2790` 后帧时间毫无变化"——
**高 CPU 占比 ≠ 时间瓶颈**，两者在这个案子里反复被证伪。

---

## 4. 回到用户的问题：泄露源码有助于分析吗

先确认事实：**这台机器上没有泄露源码**（只有官方 `open-gpu-kernel-modules` 的 515.48.07 / 520.56.06 / 525.60.11 三个版本）。

### 4.1 泄露内容的实际范围 vs 本案卡点

2022 年 LAPSUS$ 泄露的那批（约 40GB）主要覆盖 **Windows 驱动**（`Display.Driver` 一类，即 `nvlddmkm` 相关）
以及代码签名证书。**Linux 用户态 OpenGL 库（`libnvidia-glcore.so` / `glsi` / `GLX_nvidia`）从来没有泄露过。**

而本案的卡点**恰恰全在 Linux 用户态 glcore**：

| 层次 | 定位结论 | 泄露源码能否覆盖 |
|---|---|---|
| 内核模块 | **已排除（只占 0.37%，报告 160）** | 不需要 —— 有官方 OKM，且比泄露更全 |
| X / GLX 协议层 | 已排除（GLX 交互正常） | — |
| **用户态 glcore GL legacy 前端** | **← 100% 的惩罚在这里** | **覆盖不到（未泄露）** |

**⇒ 交集很小。**

### 4.2 唯一可能有用的一处，以及它为什么仍然不够

NVIDIA 的 OpenGL 实现是**跨平台共享同一份核心代码**的：
Windows 的 `nvoglv64.dll` 与 Linux 的 `libnvidia-glcore.so` 在结构上高度同源
（同样的职责划分、同样的状态机、同样的 fence 逻辑）。

所以**如果**泄露包里含 `nvoglv64` 的源码，理论上能看到：
- legacy 固定管线状态机的设计意图（对应我们的 `0x15c2e50`，151,250 次/秒）
- 「为什么 legacy 状态一旦存在，每个 `glDrawArrays` 都要重建约 18 次」

**但它回答不了这个案子真正的问题。** 原因是：

> **我们关心的是"515 和 525 差在哪"，而泄露源码只是某一个版本的静态快照。**
> 单版本源码无法表达"差分"。而 515 与 525 的二进制**都在我们手上**，
> 直接对两版做二进制 diff，才是回答差分问题的唯一正确方法 —— 泄露源码做不到这件事。

而且已知 515/525 的 fence 代码**结构完全同构**（报告 148：`0x1569f10` vs `0xfb19d0`，
连 `sub r14d,0x7fffff46` ≡ `add r14d,0x800000ba` 这个手法都一样，只有结构体偏移变了）。
如果泄露源码是 5xx 早期的 Windows 版，它大概率也只反映"**两版都有**的那套逻辑"。

### 4.3 结论

| 用途 | 泄露源码的价值 |
|---|---|
| 理解 GL legacy 状态机的设计意图 | 有（若含 Windows GL 前端） |
| **定位"515→525"的具体回归** | **几乎没有**（单版本快照，无法差分） |
| 替代官方 OKM 研究内核 | 负价值（OKM 更全、可编译、有 3 个版本可 diff） |
| 替代官方 key 文档找旋钮 | 负价值（文档已完整列出 24 个 key） |

**综合：投入产出比低。** 本案的正确路径依次是：
1. **两版二进制 diff**（回答"差在哪"）—— 已有工具与结论基础
2. **官方 OKM 三版 diff**（内核层，已做完，同构）
3. **官方 key 文档**（配置层，本轮做完，全部无效）
4. 泄露源码（语义层）—— 只在前三步都无解时才有意义

---

## 5. 本轮附带的方法论产出

1. **`nvidia-application-profiles-<ver>-key-documentation` 是官方旋钮清单**，比逆向猜高效；
   先试官方 key，再考虑改二进制。
2. **验证一个 key 是否生效，要用可观测的系统调用计数**（如 `strace -c -e trace=sched_yield`），
   不能只看性能没变化就下结论 —— 可能是 key 根本没被读。
3. **`sched_yield` 样本 = 等待姿态，不是开销**（见 §3）。
4. 24 个官方 key 里与本案相关的 6 个全部无效 ⇒ **惩罚是代码路径里的硬编码行为，不可配置**。

---

## 6. 遗留

- Heaven 的帧时间直方图仍未取到（`glXSwapBuffers` 钩子拦不到，Unigine 走 `dlopen`+glvnd dispatch，见报告 160 §7）。
- 「每个 `glDrawArrays` 触发约 18 次状态重建」的**具体判据与可绕过点**仍是最大盲点。
  这是唯一一处"若能看到源码会显著省时间"的地方 —— 但前提是**能看到 525 这一版的源码**，而泄露包给不了。
