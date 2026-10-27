# 报告 156 — zink 实测推翻"同一堵墙"：legacy 提速 6.9 倍；但 Heaven 的限制确实是另一个问题

> 日期：2026-10-27 22:20
> 实验机：`<user>@<testhost>`（<testhost>，P106-100，525.116.03 + Mesa 25.0.7 zink）
> 触发：用户提出"zink 在 525 上 32fps，说明 Vulkan 提交也撞同一堵墙"
> 前置：报告 154（legacy 退化）、155（反压是 spin-wait）

---

## 0. 结论：一半推翻、一半确认

| 用户的判断 | 实测 |
|---|---|
| "zink 也撞同一堵墙" | **推翻。** 对 legacy 负载，zink 快 **6.9 倍**（127.9 → 18.59 ms/帧，含回读，画面对） |
| "525 的底层限制不限于 GL legacy 路径" | **确认。** Heaven 根本不走 legacy（0 次），却仍然受限 ⇒ 确实存在**另一个**问题 |

**两件事必须分开：**
- **legacy 问题**：zink 能解决（已验证 6.9×）
- **Heaven 问题**：与 legacy 无关，zink 帮不上（报告 101 实测只 +8%）⇒ **仍未定位**

---

## 1. zink 环境（先确认它真的生效了）

这里有个坑：**设了 `__GLX_VENDOR_LIBRARY_NAME=nvidia` 时 zink 不会生效**，
因为该变量强制走 NVIDIA 的 GLX 供应商库，绕过 Mesa 加载器。

我第一次就是这么测的，`GL_RENDERER` 仍报 `NVIDIA P106-100/PCIe/SSE2`，
**看起来有结果，实际是原生 GL**。必须检查 renderer 串。

正确的环境：

```bash
unset __GLX_VENDOR_LIBRARY_NAME __NV_PRIME_RENDER_OFFLOAD
export MESA_LOADER_DRIVER_OVERRIDE=zink
export VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json
```

验证生效（关键！）：

```
GL_VENDOR   = Mesa
GL_RENDERER = zink Vulkan 1.3(NVIDIA P106-100 (NVIDIA_PROPRIETARY))
GL_VERSION  = 4.6 (Compatibility Profile) Mesa 25.0.7-2+deb13u1
```

`nvidia_icd.json` 位于 **`/etc/vulkan/icd.d/`**（不是 `/usr/share/vulkan/icd.d/`，
那里只有 Mesa 自家的软件/开源驱动）：

```json
{ "ICD": { "library_path": "libGLX_nvidia.so.0", "api_version": "1.3.224" } }
```

**注意**：`library_path` 是 `libGLX_nvidia.so.0` —— 它 `NEEDED libnvidia-glcore`。
**所以 zink 并没有绕开 glcore，它只是换了入口**（报告 101 已指出）。

---

## 2. 决定性对照：n83b M3（legacy）在 zink 下快 6.9 倍

### 2.1 完整路径（含 `glReadPixels` + `glMapBuffer`，外部墙钟双点相减）

| 配置 | 帧时间 | chk |
|---|---|---|
| 原生 NVIDIA GL | **127.90 ms** | 1200（正确） |
| **zink（GL→Vulkan→同一 glcore）** | **18.59 ms** | **1200（一致）** |

**6.9 倍，且画面校验和完全相同。**

### 2.2 跳过回读（`-R`，纯 CPU+提交路径）

| 配置 | 帧时间 |
|---|---|
| 原生 GL | 120.3 ms |
| **zink** | **2.6 ms** |
| 比值 | **46 倍** |

### 2.3 三个模式在 zink 下被拉平

| 模式 | 原生 GL | zink | chk |
|---|---|---|---|
| M3（legacy 状态） | 128.7 ms | 19.14 ms | 1200 ✓ |
| M2（uniform） | ~9 ms | 20.50 ms | 1200 ✓ |
| M1（纯 draw） | ~9 ms | 19.99 ms | 1200 ✓ |

**zink 下三个模式都在 19~20 ms —— legacy 的额外惩罚完全消失。**
剩下的 19~20 ms 是 zink 自身的翻译开销 + 回读等待（`map_p50` = 13~15 ms）。

---

## 3. 机制证明：zink 让 legacy 状态机从 15 万次/秒变成 0

| 配置 | `0x15c2e50`（glcore legacy 状态机）调用频率 |
|---|---|
| 原生 GL，M3 | **151,250 次/秒** |
| **zink，M3** | **0 次/秒** |

**这是一条完整的因果链：**

```
原生 GL：glColor3f/glTranslatef/... → glcore legacy 状态机（每次 draw 触发 18 次）
        → 151,250 次/秒 → 120~128 ms/帧

zink  ：glColor3f/glTranslatef/... → Mesa 翻译成 Vulkan 命令
        → glcore 的 Vulkan 路径（不走 legacy 状态机）
        → 0 次/秒 → 2.6~18.6 ms/帧
```

**⇒ legacy 的 120 µs 惩罚完全在 glcore 的 GL legacy 前端。底层提交路径（Vulkan 模式）
没有任何问题 —— 否则 zink 不可能快 6.9 倍。**

**这直接否掉了"Vulkan 提交通不过"的假设。**

---

## 4. 但用户的后半句是对的：Heaven 确实撞的是另一堵墙

### 4.1 Heaven 完全不走 legacy

在 Heaven 运行中（800×450，功耗 57.97 W）测 glcore 的 legacy 状态机：

```
0x15c2e50 命中 0 次 / 3.00s = 0.0 次/秒
```

**3 秒内 0 次。Heaven 根本不用 legacy 固定管线。**

顺带测的其他几个"病点"也全是 0：

| 函数 | Heaven 调用频率 |
|---|---|
| `0x15761b0`（fence 退避） | 0 |
| `0x1578280`（等空间） | 0 |
| `0x1572e60`（取 rp） | 0 |
| `0x15c2e50`（legacy 状态机） | 0 |
| `0x1577a90`（时间检查） | 0 |

### 4.2 Heaven 的线程几乎不碰 glcore

逐线程 RIP 采样（`tsample`，150 样本/线程）：

```
[tid 2809670] 样本 150  glcore 内 2%
```

**最热的线程只有 2% 在 glcore 里。** 其余时间在 libc（syscall 返回点）、
futex 等系统调用上（`/proc/task/*/syscall` 采样：futex 3327 次、epoll_wait 800、nanosleep 399）。

**⇒ Heaven 在 525 上的时间不在 glcore 代码里，也不在 legacy 路径上。**
这与"zink 对 Heaven 只 +8%"完全一致：zink 修的是 legacy，而 Heaven 没有 legacy 可修。

### 4.3 所以之前用 n83b M3 解释 Heaven 是错的

报告 105 曾写"Heaven 的 30fps = 268 对 × 118.5 µs + 3.3 ms = 35.1 ms ⇒ 28.5fps"。

**那个 118.5 µs 来自 n83a/n83b 的 legacy 场景，而 Heaven 不用 legacy。**
这条推算不成立，需要从 Heaven 的解释里移除。

---

## 5. 用户观察到的"抽搐"是个重要线索，且改变了数字的含义

用户指出：**Heaven 在 525 上高帧率时会"抽搐"，不抽搐时回到 30fps 左右。**

这个观察的价值：

> **如果偶尔能冲到高帧率，就说明 30fps 不是硬上限，而是被某个周期性事件压住的。**

这与"每帧固定 122 µs 惩罚"的模型**不相容** —— 固定惩罚不会产生"偶尔很快"。

### 5.1 本轮实测到一个相关数字

原生 GL、**800×450** 下用 `glXSwapBuffers` 断点计数，第一个 1 秒窗口：

```
62 次/秒  （= 62 fps）
```

**⇒ 800×450 下 Heaven 是 62fps，不是 30。**
历史上那些"30fps"是 **1920×1080** 的数字（报告 101 的 zink 对比也是 1080p）。

**所以分辨率和阈值强相关** —— 这一点与"固定每帧开销"一致，
但与"周期性抽搐"的描述需要进一步用**帧时间直方图**确认（下一轮）。

### 5.2 一个测量事故（要记录）

用 `bc2`（给**所有线程**同时下断点）时，第二秒窗口里 Heaven 被触发
`追踪或断点陷阱` 直接崩溃。

**⇒ 多线程程序不要同时给所有线程下执行断点。**
`bpcount` 只挑 CPU 时间最大的**单线程**，是更安全的做法。

---

## 6. 本轮净结论

**已确立（新）：**

1. **zink 对 legacy 负载有效：127.9 → 18.59 ms（6.9×），`chk` 一致**
2. **机制闭合：legacy 状态机 151,250 次/秒 → 0 次/秒**
3. **Vulkan 提交路径没问题** —— 否则不可能快 6.9 倍
4. **Heaven 完全不用 legacy**（0 次），也不用 fence 等待系列函数（全 0）
5. **Heaven 的线程只有 2% 在 glcore 里**，时间主要在 syscall/futex
6. **Heaven 在 800×450 是 62fps**；"30fps"是 1080p 的数字

**已撤回：**

- ~~"Heaven 的 30fps 可以用 n83b 的 118.5 µs/对解释"~~（报告 105 的推算）
  原因：Heaven 不走 legacy，那个数字来自 legacy 负载

**仍未定位（诚实标注）：**

- **Heaven 在 525 上的限制究竟是什​么。** 已知它不在 glcore 计算、不在 legacy、
  不在 fence 等待。下一步应该做**帧时间直方图**（验证"抽搐"是否双峰），
  以及在 1080p 下重新采集（当前 800×450 = 62fps 已经不算慢）。

---

## 7. 建议的下一步（按信息量排序）

1. **帧时间直方图**（最低成本、直击"抽搐"）
   在 1080p 下记录每一帧的时间，看是否双峰：
   - 双峰 ⇒ 存在周期性阻塞事件，应去查它在哪（不是固定每帧开销）
   - 单峰但均值高 ⇒ 是稳定的每帧开销，与 legacy 无关的另一条路径

2. **`perf`/采样对比 515 vs 525 在 Heaven 下的热点**（需切驱动两次）
   515 上 Heaven 正常，两版的差异点会直接指出来。

3. **不要再用 n83b 的 legacy 数字去解释 Heaven**（已犯过一次）

---

## 8. 本轮工具

| 工具 | 用途 | 注意 |
|---|---|---|
| `analysis/onheap/tsample.c` | **逐线程** RIP 采样，找哪个线程在 glcore 里 | 只读 RIP，安全 |
| `analysis/onheap/bc2.c` | 任意库 + 任意偏移的断点计数 | **给所有线程下断点会崩目标进程**，慎用 |
| `analysis/onheap/bpcount.c` | 单线程（最忙）断点计数 | 修正了装载基址必须取 `offset==0` 的 bug |
| `analysis/onheap/rpsample2.c` | 进程级 RIP 采样 | **只采到主线程**，多线程程序会误导 |

### 8.1 本轮修的三个工具 bug

1. **`bpcount` 取基址没有校验 `offset==0`** —— 对某些程序会选中错误的映射，
   导致断点地址错、计数恒为 0。已修（和 `fd2`/`callers2` 对齐）。
2. **`rpsample2` 只采主线程** —— 对多线程程序（Heaven 有 24 个线程）会得出
   "72% 在 syscall"这种误导性结论，因为主线程在做事件循环而不是渲染。
   需要用 `tsample` 逐线程采。
3. **zink 不生效时没有报错** —— 必须检查 `GL_RENDERER` 是否含 `zink`。
   我第一次测出"zink 120.4 ms"，实际是原生 GL 的 120.3 ms（几乎一样），
   差点得出"zink 无效"的错误结论。
