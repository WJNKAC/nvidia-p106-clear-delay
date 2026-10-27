# 18 · glcore swap 链静态 diff（515.105.01 vs 525.116.03）：命中 present barrier / QuadroSync 新增模块，未命中任何等待循环

> 2026-10-25 13:10
> 目的：解释报告 17 留下的唯一机制问题——**525 为何每帧多吞一个 vblank**（空场景 P8 下亦然）
> 方法：按预注册方案执行 5 步静态 diff，全程零上机
> **结论：热点路径（drain / 门控 / GLX 槽）两版逐行等价，无新增等待循环。525 唯一的结构性新增是 present barrier 与 QuadroSync 两套模块，且二者在代码里都是配置期（非每帧）。**
> **即预注册判读表中「以上全无显著差异」命中 → 回归在更深层（RM 交互 / present barrier 协议运行时激活），下一步只剩动态分流。**

---

## 0. 预注册判读表：逐行结案

| 预注册判据 | 结果 | 证据 |
|---|---|---|
| LBR 链某层 525 新增等待/同步语义 | **未命中** | drain 函数逻辑逐行等价（§2.1）；hop4 增长 471B 但无新增循环（§2.2） |
| 槽实现（0x598–0x5b8）结构变化 | **未命中（GLX 侧）** | 两版 `glXSwapBuffers` 槽调用序列与次数完全相同（§3.1） |
| `glpmWaitForIdleNV` 被默认路径触达 | **不适用** | 该符号在 515/525 二进制中**都不存在**（§4.1） |
| 525 相对 515 新增第 7 调用点在提交路径 | **未命中** | drain 外部调用点 **6 vs 6**（§3.2），与报告 12 一致 |
| `clock_gettime` 密度 525 明显高于 515 | **静态不可答** | 两版均 0 处直接 call，全走函数表间接调用（§4.2） |
| 命中 580 同类「自旋 + deadline」结构 | **命中但不分叉** | 该结构即 drain，**两版都在**（§2.1），非 525 新增 |
| **以上全无显著差异** | **✅ 命中本行** | 回归在更深层：present barrier 运行时激活 / RM 交互 |

---

## 1. 第 0 步：符号 / 字符串 diff

### 1.1 动态符号表：两版都是 28~30 项，无可比信息

```
515.105.01: 30 个动态导出（全部形如 _nv0NNglcore）
525.116.03: 28 个
```

glcore 不导出 GL 函数名——GLX 层通过**序址**间接调用（这正是"五个接口槽"存在的原因）。
所以任何"按名字找 swap"的路径都不成立，必须走序址或指令指纹。

### 1.2 字符串 diff（22677 vs 22401 条唯一串）：525 新增两套完整模块

**525 新增的 present barrier 全套（515 一条都没有）：**

```
VK_NV_present_barrier
Failed to allocate present barrier for swapchain
Failed to allocate present barrier for system
Failed to activate present barrier on GPU
Failed to activate present barrier on system
Failed to clear present barrier
Failed to configure present barrier displays
Failed to prepare disabled present barrier
Failed to query present barrier display
Failed to set present barrier attributes
Failed to match GPU %d (uuid %s) DPY %s with present barrier regkeys
Swapchain failed to join present barrier
waitingAtBarrier=%d, barSel=%d, redCntResult=%d
```

**525 新增的 QuadroSync 全套（帧锁 / genlock 多 GPU 同步）：**

```
QuadroSyncServerDpy        QuadroSyncClientDpys        QuadroSyncPolarity
QuadroSyncInterval         QuadroSyncDelay             QuadroSyncHouseInputs
QuadroSyncHouseOutput
+ 6 条 "Failed to set/parse QuadroSync* regkey" 错误串
+ "Failed to set GPU %d (uuid %s) for QuadroSyncHouse{Output,Inputs}"
```

**对全部关键词做原始计数，两版几乎相同** → 不是零散改动，是**结构化新增模块**：

| 关键词 | 515 | 525 |
|---|---|---|
| vblank | 2 | 2 |
| swap | 41 | 43 |
| vsync | 2 | 2 |

> 525 唯一"丢失"的相关串：`Deferred-barrier latency hiding: Num %d, Min %d, Max, %d, Avg %.3f`（515）
> → 525 改成 `Deferred-barrier latency hiding: Num `（截断/重构）。此为 515 唯一 QuadroSync 邻近串 `Quadro`。

### 1.3 修正报告 06 的一条表述

报告 06 写"525 代 glcore **新增** `glpmSampleNV` / `glpmTriggerNV` / `glpmWaitForIdleNV` 三入口"。

**实测：这三个名字在两版二进制里都不存在。**

```bash
$ strings -a libnvidia-glcore.so.525.116.03 | grep -ci glpm
0
$ strings -a libnvidia-glcore.so.515.105.01 | grep -ci glpm
0
```

它们来自 **okm 源码树（525.60.11）的源码级符号名**，不是 525.116.03 的二进制符号。
525.116.03 二进制里确有 present barrier 机制（`VK_NV_present_barrier` + 12 条错误串），
但**不能**用 `glpmWaitForIdleNV` 去 xref——**该符号不可 xref**。
（`analysis/glcore/str-525.txt` 里的 `glpmWaitForIdleNV` 也来自源码树，不是二进制 strings，此前被误当作二进制证据。）

---

## 2. 第 1 步：LBR 链函数级 diff

用指令指纹 `imul $0xfa` + `ctx+0x22e0c` 定位（580 链的 drain 层）：

| 版本 | 指纹地址 | 所在函数 | 大小 |
|---|---|---|---|
| 515.105.01 | `0xfbe66c` | `0xfbe5b0-0xfbe6bf` | 271 B |
| 525.116.03 | `0x1577b5a` | `0x1577a90-0x1577bc0` | 304 B |
| 580.178.04 | `0xbf2fe1` | （ctx 字段变 0x2093c） | — |

### 2.1 drain（deadline 装配）函数：**逻辑逐行等价**

对照 `drain515.func.asm` / `drain525.func.asm`：

| 语义 | 515 | 525 |
|---|---|---|
| 门 1：ctx 非空 + `testb $0x2, ctx+0x54d` | ✔ `0xfbe5bd` | ✔ 同，字段 → `0x5ad` |
| 门 2：`testb $0x7, global` | ✔ `0xfbe5e1` | ✔ 同 |
| 门 3：`testl $0x88000, (obj)+0xc` | ✔ `0xfbe5f2` | ✔ 同 |
| 门 4：`cmpb $0x0, ctx+0x6f88` | ✔ `0xfbe5fb` | ✔ 同，→ `0x6fc0` |
| 门 5：`ctx+0x22e0c` (N 乘数) 非 0 | ✔ `0xfbe604` | ✔ 同，→ `0x22edc` |
| 读时钟（间接 `call *0x280(_nv014glcore)`） | ✔ `0xfbe615` | ✔ `call *0x290` |
| ns→µs 魔数除 `0x20c49ba5e353f7cf` >> 58 | ✔ | ✔ **同一条魔数** |
| deadline = now + 250×(N−1) µs | ✔ `imul $0xfa` / `sub $0xfa` | ✔ 同 |
| 重试计数上限 3（`cmp $0x3,%al; ja`） | ✔ | ✔ 同 |
| 两个 deadline 写点 | ✔ `0xfbe67e` / `0xfbe6a9` | ✔ 同 |

**ctx 字段偏移整体漂移（结构体重排），但指令序列与语义一一对应。**
→ **报告 13 的"515 deadline 几乎从不 arm"不是 drain 代码差异造成的**，差异必在门控字段被谁置位的上游。

### 2.2 drain-caller 配对：6 对 6，两对增长，但都不是等待循环

按（门控调用次数，是否调 drain，大小）签名配对，15 个函数 1:1 对上：

| 515 | 525 | 大小差 |
|---|---|---|
| `0xf1ea00` | `0x14d5dc0` | **+471** |
| `0xea2510` | `0x1456940` | **+156** |
| `0xfb5d30` | `0x156e2a0` | +24 |
| `0xf6cfd0` | `0x1523530` | +17 |
| `0xfb18d0` | `0x1569e10` | +8 |
| `0xedff20` | `0x1496ae0` | +1 |
| 其余 9 个 | 其余 9 个 | −16 ~ 68 |

**对增长最大的两对做控制流统计（`analysis/probes/cfgstat.py`）：**

| 函数 | 指令 | call(间接) | 回边 | **pause** | **rdtsc** | 睡眠类调用 |
|---|---|---|---|---|---|---|
| 515 `0xf1ea00` | 790 | 45 (21) | 58 | **0** | **0** | **0** |
| 525 `0x14d5dc0` | 926 | 59 (35) | 75 | **0** | **0** | **0** |
| 515 `0xea2510` | 350 | 20 (10) | 20 | **0** | **0** | **0** |
| 525 `0x1456940` | 399 | 24 (14) | 21 | **0** | **0** | **0** |

- 回边增长（58→75、20→21）与指令增长（+136、+49）**成比例**，且绝大多数是跳转表分发的短跳（跨度 16~120 字节），不是循环体。
- 两版 **`pause` 指令 0 条、`rdtsc` 0 条、无 nanosleep/usleep/sched_yield/futex/pthread_cond 调用**。
- 内部调用目标集合也一致：515 = 24 次调用 / 10 个目标，525 = 24 次 / 10 个目标，多重度一一对应。

→ **这两个增长函数是命令派发器，不是新增等待路径。**
（诚实标注：把它们认定成"LBR hop4"只有大小相近这一条弱证据——580 hop4 是 3197B，515 候选 3291B。反编译缺失，无法确证身份。）

---

## 3. 第 2 步：接口槽与调用点

### 3.1 GLX→glcore 的五个槽：两版完全相同

从 `analysis/glx/swap-{515,525}.txt`（`glXSwapBuffers` 及其两个内部 helper 的完整反汇编）统计：

| 槽 | 515 | 525 |
|---|---|---|
| `call *0x598(%reg)` | 2 | 2 |
| `call *0x5a0(%reg)` | 2 | 2 |
| `call *0x5a8(%reg)` | 0 | 0 |
| `call *0x5b0(%reg)` | 1 | 1 |
| `call *0x5b8(%reg)` | 1 | 1 |

→ **交换界面的调用形状完全没有变化**，印证报告 06"GLX 包装层两代逐行等价"。
差异只能在槽的实现体（glcore 内部）里。

### 3.2 第 7 调用点假设：**不存在**

drain 函数的调用点（排除函数内自跳转后）：

```
515: 6 个外部调用点（1+1+1+1+2 = 6，来自 6 个函数）
525: 6 个外部调用点（同上，同分布）
```

与报告 12 记录的"515=6 / 525=6 / 535=20 / 580=7"完全一致。
→ 525 **没有**相对 515 新增的谓词调用点；方案里"525 新增第 7 处调用点、落在提交路径"的假设**不成立**
（第 7 处是 **580** 的特征，580=7）。

### 3.3 门控置位函数：126 B vs 126 B，完全同构

```
515: 0xfbcd50-0xfbcdce (126B)   20 个调用点   12 个调用者函数
525: 0x15762d0-0x157634e (126B)  20 个调用点   12 个调用者函数
调用者多重度分布：3, 3, 2, 1×9  —— 两版完全相同
```

→ 门控的"谁打开它"拓扑没变，只是 ctx 偏移漂移（`0x6f88`→`0x6fc0`）。

---

## 4. 第 3/4 步：符号 xref 与补充线索

### 4.1 present barrier 管理函数 = 配置期，不是每帧

字符串 xref 定位到 `0x17c4e40`（1007 字节，525 独有函数，515 无对应物）：

```
0x17c50b7 引用 "Swapchain failed to join present barrier"
0x17c51ba 引用 "Failed to clear present barrier"
0x17c520e 引用 "Failed to activate present barrier on GPU"
```

读函数体（`pbar525.func.asm`）：
- 遍历 `this+0x90` 的**显示器链表**，对每个 display 调 `0x17c3e60 / 0x17c3ee0 / 0x17c3f90 / 0x17c4420 / 0x17c4910` 配置 barrier；
- 失败时走 `0x17c5120` 的析构链表清理；
- **无轮询、无读时钟、无睡眠、无 pause**；
- 唯一外部调用者 `0x17c52f0`。

→ **它是交换链创建/销毁期的配置代码，不在每帧 swap 路径上。**
把它当"每帧多等一拍"的直接原因不成立；但它**有可能**在运行时改变 present 模式（见 §6）。

### 4.2 `clock_gettime` 密度：静态不可答

```bash
$ grep -cE 'call.*clock_gettime' full-515.105.01.asm   → 0
$ grep -cE 'call.*clock_gettime' full-525.116.03.asm   → 0
```

两版都是 0 处直接调用——驱动经 `_nv014glcore` 函数表（drain 里是 `call *0x280` / `*0x290`）间接取时。
静态无法统计"每帧取时次数"。**这条判据只能用动态探针**（报告 09/10/13 的数据仍是唯一来源）。

### 4.3 580 同类"自旋 + deadline"结构：两版都在，不是 525 新增

即 §2.1 的 drain 函数。结论是**负面但重要**：515 里就有完整机制，所以"525 引入了这套等待"的叙事被彻底否掉。

### 4.4 RM 侧小项

| 项 | 515.105.01 | 525.116.03 | 580.178.04 |
|---|---|---|---|
| `RMForcePstateBug3811584War`（nv-kernel.o + firmware 全树） | **0** | **2**（另有 gsp_tu10x/gsp_ad10x） | **0** |

> **修正 README / 报告 01**：原文写"RM 核心 46.6→63.1MB，580 代新增 `RMForcePstateBug3811584War`"。
> **580.178.04 全树 0 次命中**（含 75MB `gsp_ga10x.bin` 与 30MB `gsp_tu10x.bin`）。
> 该工作标记是 **525 分支窗口独有**，不是 580 的特征。这让"525 = 特殊一代"更强，而不是"525 起一直延续"。

| `pthread_mutex_*` 未定义引用（glcore 动态符号） | 515=0 | 525=0 | 580=**3** |
|---|---|---|---|
（535.129.03 目录为空/坏符号链接，无法查证"535 已有"的说法。）

### 4.5 尝试过但走不通的路径（存档，避免下一位重走）

| 尝试 | 结果 |
|---|---|
| 用 `nm -D` 找 swap 相关新符号 | glcore 只导出 28~30 个 `_nv0NNglcore`，无 GL 函数名 |
| 用 `glXGetProcAddress` 的**名字串**（`glReadPixels`/`glXSwapBuffers`/`glFinish`）定位实现 | 两版都 **0** 个精确匹配——GLX 按序址分发，不用名字查表 |
| 用 `.data.rel.ro` 的连续 RELATIVE 重定位找"函数指针表"再取表[179] | 找到 1761 项的表（**两版都是 1761 项**），但槽 179 指向的 `0x3fdcec`/`0x48ac4f` 传的是**数据指针**（读出来是 `-@` 与二进制垃圾），不是名字串 → **该表不是 GL 分发表**，作废 |
| Ghidra 反编译 glcore 的两个候选函数 | 现有 Ghidra 工程**只有 libGLX_nvidia.so**（4293 文件全是 GLX）；glcore 从未导入。磁盘 92% 满（余 11G），44MB 二进制全量分析风险高，本轮未做 |

---

## 5. 结案：静态能答的与不能答的

**能答（已答）：**

1. 525 相对 515 在 glcore 里**新增了两套呈现同步模块**：`VK_NV_present_barrier` 全套 + QuadroSync 全套（含 7 个 regkey + 6 条解析失败串）。515 一条都没有。
2. 用户最关心的等待逻辑（drain / 门控 / deadline 装配）**两版逐行等价**，250µs 退避、魔数除、3 次重试、两个写点全部相同。
3. GLX→glcore 交换界面**零变化**（槽调用次数与序列完全相同）。
4. 525 **没有**新增谓词调用点（6 vs 6）。
5. 525 的热点函数里**没有**任何新增的自旋/睡眠/读时钟循环（`pause`/`rdtsc`/sleep 全 0）。
6. `glpmWaitForIdleNV` 等三名**不是二进制符号**，报告 06 的表述需降级为"源码级"。
7. `RMForcePstateBug3811584War` 是 **525 独有**，580.178.04 没有（README 需修正）。

**不能答（本轮判死刑）：**

> **静态 diff 已经排除了"swap 链里多了一段等待代码"这一整类解释。**
> 剩下的可能只有两类，**都是运行时才显现的**：
>
> (a) **present barrier / QuadroSync 在运行时被激活**，改变了 present 模式或加了一个提交屏障
>     （代码是配置期的，但配置的结果影响每帧）；
> (b) **驱动与 RM 的交互**（ioctl 往返、提交节流、vblank 归属）——在用户态库里看不到。

---

## 6. 新假说：一个机制同时解释"30fps 地板"与"抽搐"

把报告 16/17 的实测放在一起，存在一个统一解释，不需要"每帧多等 30ms"这种常数：

| 路径 | 每帧预算余量 | 525 实测 | 用"每帧多付 δ"解释 |
|---|---|---|---|
| primus（含 1080p readback） | **紧贴 16.67ms 边界** | 中位 33.40ms、86.3% 帧在 30–40ms 档 | 余量 < δ → **每帧都错过 vblank → 稳定 2 vblank = 30fps** |
| vendor（dmabuf 直渲） | 有余量 | 中位 16.97ms、26.4% 帧落 30–40ms | 余量 > δ 但接近 → **部分帧错过 → 抽搐** |
| offload（dmabuf+拷贝） | 余量居中 | 中位 16.16ms、19.6% 落 30–40ms | 同上，拷贝略增成本 → 落后一拍的比例略高 |
| Intel 核显（对照，无跨卡） | 极宽（163ms/帧） | 163.28ms、p95/中位 1.14 | δ 被淹没 → 完全均匀 |

**这个模型的可检验预言**：δ 应当与分辨率/回读量相关。
→ 在 primus 下降分辨率到 1280×720 / 960×540，若中位从 33.3ms 跌回 16.7ms，就是 δ 由回读路径贡献的直接证据。
（这是报告 16 §4 已列出的"最便宜的一刀"，只需一次设置变更，且**在 515 上也能做对照**。）

---

## 7. 产物清单

```
analysis/diffs/glcore-swap/
├── dyn-515.syms / dyn-525.syms           动态符号表（各 28~30 项）
├── full-str-515.txt / -525.txt           全量唯一字符串（22677 / 22401）
├── str-diff-raw.txt                      字符串 diff 原文（18038 行）
├── drain515.func.asm / .callers.txt      drain 函数体 + 调用点（515）
├── drain525.func.asm / .callers.txt      同上（525）
├── gate515_set.func.asm / .callers.txt   门控置位函数（515）
├── gate525_set.func.asm / .callers.txt   同上（525）
├── pbar525.func.asm / .callers.txt       present barrier 管理函数（525 独有）
├── wait515.func.asm / .callers.txt       增长 471B 的派发器（515）
├── wait525.func.asm / .callers.txt       同上（525）
├── swap515.func.asm / .callers.txt       增长 156B 的派发器（515）
└── swap525.func.asm / .callers.txt       同上（525）
```

新入库探针脚本：

```
analysis/probes/extract_func.py    按 .eh_frame FDE 提函数体 + 找调用者并归类
analysis/probes/cfgstat.py         控制流统计（回边/pause/rdtsc/睡眠调用），格式自适应
analysis/probes/pair_callers.py    按签名配对两版调用者函数并比大小
analysis/probes/strref.py          字符串 → vaddr → 代码引用 → 所在函数
analysis/probes/vendortable.py     .data.rel.ro 连续重定位找指针表并 dump 指定槽
```

---

## 8. 下一步（按判别力排序）

| 优先级 | 动作 | 成本 | 能判什么 |
|---|---|---|---|
| **A** | **降分辨率判定**：primus 下 1920×1080 / 1280×720 / 960×540 各 15s 帧间隔 | 3 次 × 15s，零重启 | 若中位 33.3→16.7ms，则 δ 由**回读量**贡献 → 锁定 readback/呈现路径；否则是固定节拍 |
| **B** | 换刷新率：显示器切 50Hz，看地板变 40ms 还是不变 | 一次设置 + 15s | 证明/证伪"半速 vsync 整数倍"模型（报告 16 §3 已预注册） |
| C | 查 present barrier 是否**运行时激活**：`__GL_*`/`VK_*` regkey 或 NVIDIA 日志级别打开后看 `waitingAtBarrier` 是否出现 | 一次运行 | 直接验证 525 特有机制是否真的参与本机 present |
| D | Ghidra 全量反编译 glcore（44MB） | 数小时 + ≥5GB 磁盘（现余 11G） | 才能读 drain 上游门控的语义；磁盘是硬约束 |

> **A 与 B 都不需要装 525**（当前机器是 515），只需在 515 上跑一遍作为对照，
> 但要注意：**A/B 要回答的是 525 的行为**，若只在 515 上做，只能验证"δ 与回读量相关"这个模型的普适性，
> 不能直接测 525 的 δ。这一点必须诚实标注。
