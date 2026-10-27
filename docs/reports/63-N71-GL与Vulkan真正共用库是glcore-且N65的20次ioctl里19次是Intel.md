# 63 · N71：GL/Vulkan 真正的共用库是 **`libnvidia-glcore`**，不是 `libGLX_nvidia` —— 并推翻 N65 的一处隐含解读

> 2026-10-26 09:55
> 用户指令：先零成本确认"Vulkan 和 GL 共用哪个用户态库、它在 515/525 有没有差异"，避免又一轮"跑完发现排除错对象"。
>
> **三条结论**：
> 1. **ICD 确实指向 `libGLX_nvidia.so.0`**（用户猜对），**但 Vulkan 实现不在里面**——它是通过 GL 扩展 `glGetVkProcAddrNV` 从 **`libnvidia-glcore.so`** 取的。
> 2. **`libnvidia-glcore` 才是 GL 与 Vulkan 真正的共用库**（36.8MB → 44.3MB）。此前报告 60/61 逐字节对比的 `libGLX_nvidia` 只有 1.2MB，**从未查过真正的实现体**。
> 3. **★ N65 的"每帧 20 次 ioctl"里，19 次是 Intel DRM，只有 1 次是 NVIDIA。** 这条解读此前被默认成"NVIDIA 提交 20 次"，错了。

---

## 1. 第一问：ICD 指向谁

```
/etc/vulkan/icd.d/nvidia_icd.json
  "library_path": "libGLX_nvidia.so.0"
  "api_version" : "1.3.224"
```

**用户判断正确**。但顺着看下去，情况不是直觉那样。

### 1.1 `libGLX_nvidia` 里的 Vulkan 入口只有 3 个，且全是转发

| 符号 | 515 | 525 |
|---|---|---|
| `vk_icdGetInstanceProcAddr` | `0xa5ed0` | `0x9dce0` |
| `vk_icdGetPhysicalDeviceProcAddr` | `0xa5f30` | `0x9dd40` |
| `vk_icdNegotiateLoaderICDInterfaceVersion` | `0xa5f90` | `0x9dda0` |

`vk_icdGetInstanceProcAddr` 的实现（525 `0x9dce0`）核心是**一次间接跳转**：

```asm
9dced: cmpl   $0x1,0x87264(%rip)     # 初始化过没
9dcf6: call   ...                    # 没初始化 → 去初始化
9dd0a: call   *0x872e0(%rip)         # ★ 0x124ff0 = 运行时填入的函数指针
```

### 1.2 那个指针是从哪来的 —— 关键的一跳

初始化函数（`0x9cd50` 附近）做的事情，按顺序：

```asm
9cd60: call   XOpenDisplay@plt            # ① 先开一个 X display
9cd8e: call   <glXChooseFBConfig 内部>     # ② 选 FBConfig
9ceb1: lea    0x46ddf(%rip),%rdi  # "ex7991765ed"
9ceb8: call   glXGetProcAddressARB        # ③ 取 GL 扩展函数
9ced4: call   *%rax                       # ④ 调它（名字见下）
9ced6: mov    %rax,0x124ff0               # ⑤ 存成 vk_icdGetInstanceProcAddr
```

那几个字符串实际内容是：

| 地址 | 字符串 |
|---|---|
| `0xe3c6b` | **`glGetVkProcAddrNV`** |
| `0xe3c7d` | `vk_icdGetInstanceProcAddr` |
| `0xe3ca8` | `vk_icdGetPhysicalDeviceProcAddr` |
| `0xe3cc8` | `vk_icdNegotiateLoaderICDInterfaceVersion` |
| `0xe3bd7` | `vkGetInstanceProcAddr` |

**→ Vulkan 入口是"先建 GL 上下文、再用 GL 的 `glGetVkProcAddrNV` 取出来"的。**

**结论**：`libGLX_nvidia` 只是 Vulkan loader 的**过路点**，它自己不含 Vulkan 实现。
真正的实现必须由 GL 侧提供 → **在 `libnvidia-glcore` 里**。

### 1.3 验证：glcore 里确实有 Vulkan 入口

| 库 | 大小 | Vulkan 入口 |
|---|---|---|
| `libnvidia-glcore.so.515.105.01` | **36,771,312** | `vk_optimusGetInstanceProcAddr` @ `0x11c8d80`<br>`vk_optimusGetDeviceProcAddr` @ `0x11c93e0` |
| `libnvidia-glcore.so.525.116.03` | **44,282,616** | `vk_optimusGetInstanceProcAddr` @ `0x179d270`<br>`vk_optimusGetDeviceProcAddr` @ `0x179d8e0` |
| `libnvidia-glvkspirv.so` | 16.7MB → 25.1MB | **0 个** Vulkan 入口（只有 shader 编译器） |

**`libnvidia-glcore` 从 36.8MB 涨到 44.3MB（+20.4%）**，而 `libGLX_nvidia` 是**缩小**的（1,289,552 → 1,191,352）。

**这是本案第一次把排查对象放到正确的位置上。**

---

## 2. 第二问：两版的 ioctl 提交路径同构吗

### 2.1 静态：ioctl 调用点 7 → 8，多出来那个是死代码

| | 515 | 525 |
|---|---|---|
| `ioctl@plt` 调用点 | **7** | **8** |
| 其中在 `_nv011glcore` | 4 | 4 |
| 其中在 `_nv012glcore` | **0** | **1** ← 新增 |
| 其中在 glcore 深处 | 3 | 3 |

**新增的那一个**（525 `0x14a98ea`）是一个**遍历 fd 数组的循环**：

```asm
14a98d0: mov    0x0(%r13),%edi       # fd = arr[i]
14a98d4: cmp    $0xffffffff,%edi     # fd == -1 ?
14a98d7: je     ...                  # 跳过
14a98e0: mov    $0x80104518,%esi     # ★ cmd = 0x80104518
14a98ea: call   ioctl@plt
14a990a: add    $0x4,%r13            # 下一个 fd
14a9911: jne    14a98d0              # 循环
```

**但这个 cmd 在实测中从未出现**：

| 检查 | 结果 |
|---|---|
| `0x80104518` 在 525 glcore 出现次数 | **1**（就是这处） |
| `0x80104518` 在 515 glcore 出现次数 | **0** |
| `0x80104518` 在 n27a.trace 命中 | **0** |
| `0x80104518` 在 n27d.trace 命中 | **0** |

→ **静态多一个，动态 0 次。这是"静态差异 ≠ 动态热点"的干净例子。**

### 2.2 静态：四个真实执行的 ioctl 点，cmd 序列逐条相同

| 位置 | 515 cmd | 525 cmd | 一致 |
|---|---|---|---|
| `_nv011glcore` #1 | `c030644a` | `c030644a` | ✅ |
| `_nv011glcore` #2 | `c00c642d` | `c00c642d` | ✅ |
| `_nv011glcore` #3 | `40086409` | `40086409` | ✅ |
| `_nv011glcore` #4 | `c0206442` | `c0206442` | ✅ |
| 深处 #3 | `c01046d3` | `c01046d3` | ✅ |

### 2.3 指令级：#3 点几乎逐条相同

**515 `0xec6505`** vs **525 `0x147c693`**：

```asm
515                             525
lea  0x14(%rsp),%rdx            lea  0x14(%rsp),%rdx
mov  $0x40086409,%esi           mov  $0x40086409,%esi      ← cmd 同
mov  %r15d,%edi                 mov  %ebp,%edi             ← 仅寄存器不同
movl $0x0,0x18(%rsp)            movl $0x0,0x18(%rsp)
mov  %eax,0x14(%rsp)            mov  %eax,0x14(%rsp)
xor  %eax,%eax                  xor  %eax,%eax
call ioctl@plt                  call ioctl@plt
```

**逐条相同，只有寄存器分配差异（`%r15d` → `%ebp`）。**

### 2.4 唯一的实质差异：`#1` 点的一个立即数

```asm
515 #1 (0xec647f)                525 #1 (0x147c608)
movl $0x1,0x68(%rsp)    ← 1     movl $0x0,0x98(%rsp)   ← 0  ★
mov  %rcx,0xa0(%rsp)            mov  %rcx,0xa0(%rsp)
movl $0x30,0x9c(%rsp)           movl $0x30,0x9c(%rsp)
mov  %rbx,0x90(%rsp)            mov  %r12,0x90(%rsp)
call ioctl@plt                  call ioctl@plt
```

对应 `cmd = c030644a`（`size = 0x30` = 48 字节的结构体）。
**同一个字段，515 写 1、525 写 0**；偏移也从 `0x68` 变成 `0x98`（结构体布局变了）。

**这是全库 ioctl 路径上唯一值得追的实质差异。**它可能是某个"行为开关"，但目前**无法从静态判断这个字段的语义**（候选：等待模式、缓存策略、同步标志）。

---

## 3. ★★★ 推翻 N65 的一处隐含解读

### 3.1 解码 N65 那 20 次 ioctl 的归属

我把实测的 cmd 按 Linux ioctl 编码（`dir|size|type|nr`）解出来：

| cmd | nr | **type** | size | 每帧次数 | **归属** |
|---|---|---|---|---|---|
| `c00c6466` | 0x66 | **0x64 `'d'`** | 12 | **8** | **DRM（Intel）** |
| `c0086457` | 0x57 | **0x64 `'d'`** | 8 | **6** | **DRM（Intel）** |
| `c0106461` | 0x61 | **0x64 `'d'`** | 16 | **2** | **DRM（Intel）** |
| `40406469` | 0x69 | **0x64 `'d'`** | 64 | **1** | **DRM（Intel）** |
| `c00864bf` | 0xbf | **0x64 `'d'`** | 8 | **1** | **DRM = `SYNCOBJ_CREATE`** |
| `c00864c0` | 0xc0 | **0x64 `'d'`** | 8 | **1** | **DRM = `SYNCOBJ_DESTROY`** |
| **`c020462a`** | 0x2a | **0x46 `'F'`** | 32 | **1** | **NVIDIA** |

**总计：19 次 Intel DRM + 1 次 NVIDIA。**

### 3.2 这条改变了什么

| 此前（报告 62）的表述 | 实际 |
|---|---|
| "渲染线程每帧 20 次 ioctl 提交" | **只有 1 次是 NVIDIA 的；19 次是在跟 Intel 的 DRM 说话** |
| "窗口内 0 次 GPU 提交 → GPU 空着" | **更强成立**：NVIDIA 侧每帧只有 1 次，确实几乎没提交 |
| "CPU 在生成 GL 命令" | 仍成立，但要补一句：**同时还每帧做 19 次 Intel DRM 调用** |

**两条附带发现**：

1. **`SYNCOBJ_CREATE` + `SYNCOBJ_DESTROY` 各 1 次/帧** —— 每帧创建并销毁一个 DRM 同步对象。这是"每帧分配临时同步原语"的模式，本身有开销。
2. **`c00c6466` 8 次/帧、`c0086457` 6 次/帧** —— 这两个占了 14/20，是绝对主力，且都是 `dir=3`（`_IOWR`，读写往返 = **会阻塞**）。

### 3.3 为什么这解释了之前所有"排除都没用"

| 已排除 | 为什么排除无效 |
|---|---|
| primus 用户态 4 个可动点（报告 58/59） | 改的是 NVIDIA 侧数据搬运 |
| GLX 层全等价（报告 60/61） | 那 1.2MB 库本来就不是 Vulkan 实现 |
| zink 换后端无效（报告 70） | **zink 也要经 Intel 上屏，19 次 DRM 调用照旧** |
| glcore 等待机制 12 轮（报告 44~55） | 查的是 NVIDIA 侧 CPU 空转 |

**→ 真正的每帧重活有 19/20 落在 Intel DRM 侧，而所有排除实验都在 NVIDIA 侧。**

---

## 4. 第三问：共享内存 / 映射函数

| PLT 调用点 | 515 | 525 | 差 |
|---|---|---|---|
| `mmap` | 6 | 6 | **0** |
| `munmap` | 9 | 11 | +2 |
| **`memmove`** | **45** | **101** | **+56（+124%）** |
| `memcpy` / `memset` | 0 / 0 | 0 / 0 | 0（编译器内联了） |
| `poll` | 7 | 9 | +2 |
| `usleep` / `nanosleep` | 1 / 4 | 1 / 4 | **0** |
| `sched_yield` | 2 | 2 | **0** |
| PLT 调用点总数 | 2268 | 2480 | +212（+9.3%） |

### 4.1 `memmove` +124% 怎么读

增量分布：

| 落点 | 515 | 525 |
|---|---|---|
| 未命名区域（导出符号之后） | 7 | **39** |
| `vk_optimusGetDeviceProcAddr` 之后 | 5 | **21** |
| `_nv023glcore` | 22 | 28 |
| `_nv009glcore`（新版才出现） | 0 | 2 |

**这些增量几乎全在 Vulkan 相关区域**（`vk_optimus*` 附近 + 未命名段）。
`glcore` 体积涨 20.4%、PLT 总点涨 9.3%，而 `memmove` 涨 124% —— **超出代码增长率，但落在新增的 Vulkan 代码里，不是 GL 提交路径。**

**→ 与"GL 每帧性能"无直接关系，但说明 525 的 glcore 里塞进了更多 Vulkan 相关工作。**⚠️ 调用点数量 ≠ 调用次数，这一条不能当性能证据用。

### 4.2 `mmap` 完全一致 = 映射路径没变

6 vs 6，且 `usleep`/`nanosleep`/`sched_yield` 全都一致 → **内存映射与睡眠原语层面无差异。**

---

## 5. 结论

| # | 问题 | 答案 |
|---|---|---|
| 1 | ICD 指向 `libGLX_nvidia`？ | **是**，但那里只有 3 个转发入口 |
| 2 | 真正的 Vulkan 实现在哪 | **`libnvidia-glcore.so`**（经 `glGetVkProcAddrNV` → `vk_optimusGetInstanceProcAddr`） |
| 3 | GL/Vulkan 共用库 | **`libnvidia-glcore`**（36.8→44.3MB），**不是** `libGLX_nvidia` |
| 4 | 两版 ioctl 提交路径同构？ | **是**。4 个执行点 cmd 逐条相同、指令级几乎逐条相同 |
| 5 | 525 多的那 1 个 ioctl | **死代码**（cmd `0x80104518`，实测 0 次） |
| 6 | 唯一实质差异 | `#1` 点 `movl $0x1 → $0x0`（cmd `c030644a` 的结构体某字段） |
| 7 | **N65 的 20 次 ioctl 是谁的** | **19 次 Intel DRM + 1 次 NVIDIA** ← **推翻此前解读** |

### 5.1 对"共用层"这句话的修正

用户原本的假设是"Vulkan 和 GL 共用某个底层，那个底层有问题"。**这个方向仍然成立，但对象要换两次**：

```
❌ libGLX_nvidia（1.2MB，报告 60/61 查过，等价）
❌ libnvidia-glcore 的 ioctl 提交路径（本轮查过，同构）
✅ 剩下两个候选：
   ① libnvidia-glcore 的 #1 ioctl 那个字段（$0x1 → $0x0）
   ② Intel DRM 侧那 19 次/帧调用（本轮才第一次看见）
```

### 5.2 为什么 ② 值得优先

因为它同时满足：

- **量级对**：20 次里 19 次，占绝对多数
- **会阻塞**：`c00c6466`/`c0086457`/`c0106461` 都是 `dir=3`（`_IOWR`，等待返回）
- **与分辨率无关**：buffer 管理是每帧固定次数 → 正好对应"20.4ms 固定项"
- **解释所有既往排除**：primus / GLX / zink 都绕不过 Intel 上屏

### 5.3 ★★★ 但 ② 也被自己的数据否掉了 —— ioctl 只占 0.7%

我把 N65 trace 里每个 ioctl 的**进入/返回配对**，逐个算耗时：

| cmd | 真实身份 | 次数 | 中位 | p90 | **每帧合计** | 占比 |
|---|---|---|---|---|---|---|
| `c020462a` | **NVIDIA** | 266 | **45 µs** | 63 µs | **0.12 ms** | 56.0% |
| `40406469` | `GEM_EXECBUFFER2` | 266 | 50 µs | 67 µs | 0.05 ms | 23.9% |
| `c00c6466` | `GEM_MADVISE` | 2132 | **1 µs** | 5 µs | 0.02 ms | 7.1% |
| `c0086457` | `GEM_BUSY` | 1601 | **1 µs** | 4 µs | 0.01 ms | 5.3% |
| `c0106461` | `GEM_SET_TILING` | 536 | 1 µs | 2 µs | 0.00 ms | 1.2% |
| `c00864bf` | `SYNCOBJ_CREATE` | 266 | 2 µs | 3 µs | 0.00 ms | 0.8% |
| `c00864c0` | `SYNCOBJ_DESTROY` | 266 | 1 µs | 1 µs | 0.00 ms | 0.7% |

**全部 ioctl 合计 = 0.22 ms/帧**，而帧时间是 30.84 ms → **只占 0.7%**。

**→ 19 次 Intel DRM 是真事，但它们加起来 0.22ms。连"Intel 侧是瓶颈"也不成立。**

### 5.4 那 20.4ms 在哪：只能是用户态

把三条已知量摆在一起：

| 项 | 数值 | 来源 |
|---|---|---|
| 帧时间 | **30.84 ms** | N65 |
| 全部 ioctl | **0.22 ms**（0.7%） | 本节 |
| 渲染线程状态 | **整帧占满一个核，`sched_yield` = 0** | 报告 62 |
| 真空期（末次 ioctl → 下次 map） | **20.55 ms**，内含 **ioctl = 0** | 报告 62 |

→ **30ms 里 99% 是纯用户态 CPU 计算，几乎没有 syscall。**

**这是一个极强的约束**：既不是等内核、也不是等 GPU、也不是等锁，**是 CPU 自己在跑代码**。

而 Heaven 1080p 的纯渲染量只有 ~7ms（报告 07 的 `3.5ns/px` 拟合）→ **多出来的 ~23ms 是 driver/wrapper 用户态的开销**。

**这就是 `perf record -t <渲染线程tid>` 必须做的事**：找出这 23ms 落在哪个函数。


---

## 6. 下一步

**静态已经没有更便宜的了**：ioctl 侧占 0.7%、共享内存层无差异、唯一的实质差异是一个语义未知的字段。

| 选项 | 内容 | 代价 | 回答 |
|---|---|---|---|
| **A（唯一有决定性的）** | `perf record -t <渲染线程tid> -g` 跑 10s | 一次 10s 采集 | **那 23ms 用户态落在哪个函数** ← 现在只剩这一个问号 |
| B（零成本） | 追 glcore `#1` 那个字段：`cmd=c030644a`（48B），515 写 `$0x1`@`0x68`、525 写 `$0x0`@`0x98` | 零成本 | 那个开关的语义 |
| C（需换驱动） | 给 515 补同格式 trace，直接数 ioctl 次数与 cmd 分布 | 需换驱动 | 用户原问"515 的 ioctl 多少"，静态侧已答：**同构** |

**建议 A**。因为 B 最多给出"某个字段语义变了"，即使变了也要靠 A 才能证明它值 23ms。

---

## 7. 产物

```
analysis/n64/n71_glcore_ioctl.py      两版 glcore 的 PLT 调用点计数
analysis/n64/n71_ioctl_where.py       每个 ioctl 调用点落在哪个函数
analysis/n64/n71_ioctl_cmd.py         提取每个调用点的 cmd 立即数
analysis/n64/n71_ctx.py               四个 ioctl 点的指令级上下文
analysis/probes/n71_fd.sh / n71_fd2.sh   fd 归属探测（结论：fd 编号每次不同，不可用）
analysis/probes/n70_*.sh              zink 相关（本轮前序）
```

### 7.1 方法论（三条）

1. **"某库导出了某组符号"≠"该组功能实现在这个库里"。**
   本例 `libGLX_nvidia` 导出 3 个 `vk_icd*` 入口，但实现是**先建 GL 上下文、再 `glGetVkProcAddrNV` 取**，真正的代码在 `libnvidia-glcore`。
   **判据**：看入口函数体里是"直接实现"还是"`call *ptr` 转发"；若是转发，追指针的写入点。

2. **ioctl 的归属看 `cmd` 的 type 字节，不要看 fd 编号。**
   `cmd = dir|size|type|nr`，`type = 'd'(0x64)` 是 DRM，`'F'(0x46)` 是 NVIDIA。
   本例 **fd 编号每次运行都不同**（同一编号这次是 pipewire memfd、上次是 GPU），按 fd 归属必然出错。

3. **静态的"调用点数量增加"必须用"实测命中次数"过滤。**
   本例 525 多 1 个 ioctl 调用点，看着像新增同步，实测 **0 次命中** —— 死代码。
   **静态找差异，动态定权重，缺一不可。**
