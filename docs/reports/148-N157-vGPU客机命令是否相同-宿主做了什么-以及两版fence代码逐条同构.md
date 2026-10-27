# 报告 148 — vGPU 客机发的命令一样吗？宿主做了什么？以及由此得到的真正差异定位

> 日期：2026-10-27 14:00
> 方式：**纯静态**（实验机 `<testhost>` 仍未恢复，见报告 147 §9）
> 材料：515.105.01 / 525.116.03 完整驱动包 + open-gpu-kernel-modules-525.60.11

---

## 0. 三个结论先摆出来

1. **命令流一样。** 推到 pushbuffer 的 class/method 字节流，客机和裸机是同一份——
   因为生成它的用户态代码（glcore）里**一行 vGPU 逻辑都没有**。
2. **宿主确实做了事**，而且是关键的四件事：寄存器访问截获、版本握手、
   vGPU Manager 调度、以及（525 新增的）GSP 化卸载。
3. **但顺带挖到一个更要紧的事实**：515 和 525 读 fence 的代码**逐条同构**，
   连 min 循环和双分支都一样，只有结构体偏移不同。
   ⇒ **差异不在代码，在运行时状态。** 这一条比 vGPU 本身更接近根因。

---

## 1. Q1：客机发出的命令一样吗——一样

### 1.1 证据 A：glcore 里没有任何 vGPU 代码

严格区分大小写扫两版 `libnvidia-glcore`：

| 串 | 515 | 525 |
|---|---|---|
| `VGPU` | **0** | **0** |
| `vGPU` | **0** | **0** |
| `vgpu` | 3 | 4 |

那 3~4 个小写命中是假的，是 **ROT13 混淆串里的子串**。逐个解码：

```
JnvgGbSbeprPGNFjvgpu            -> WaitToForceCTASwitch
FjvgpuBcgRkgraqrqCnggreaZngpu   -> SwitchOptExtendedPatternMatch
FjvgpuBcgOhqtrg                 -> SwitchOptBudget
```

`grep -i vgpu` 匹配到的是 `CTASwitch` 里的 **witch**。
这三个是**着色器编译器的开关名**（CTAS = Cooperative Thread Array Switch），与 vGPU 无关。

> NVIDIA 把内部选项名做 ROT13 后放进二进制，这是个容易误判的坑。
> 以后查这类关键词，必须**先用 `grep`（区分大小写）排除 ROT13 假阳性**，再解码确认。

**结论：用户态 glcore 完全不知道 vGPU 的存在，所以它生成的命令不可能因 vGPU 而变。**

### 1.2 证据 B：宿主 vGPU 能力是编译期开关，不是运行时分支

`kernel-open/nvidia/os-interface.c`：

```c
NvBool NV_API_CALL os_is_vgx_hyper(void)
{
#if defined(NV_VGX_HYPER)
    return NV_TRUE;
#else
    return NV_FALSE;
#endif
}
```

**是 `#if`，不是运行时判断。** 也就是说：

- 宿主 vGPU 能力由**单独编译的 host 驱动**提供（`NV_VGX_HYPER` / `NV_GRID_BUILD`）
- 我们手上这个标准 `.run` 是"裸机 + 客机"双身份，靠运行时区分
- 区分依据是硬件位：`NV_PMC_BOOT_1`（寄存器 `0x4`）的 **bit 17:16**

```c
#define NV_PMC_BOOT_1_VGPU         17:16
#define NV_PMC_BOOT_1_VGPU_REAL    0x00000000   // 裸机
#define NV_PMC_BOOT_1_VGPU_PV      0x00000001   // 半虚拟化
#define NV_PMC_BOOT_1_VGPU_VF      0x00000002   // SR-IOV 虚拟功能
```

**客机是靠读这个硬件位知道自己身份的**，不是靠配置文件、也不是靠 glcore。

### 1.3 证据 C：物理 GPU 直读客机的 pushbuffer

vGPU 的通道是这样建的：客机在自己的内存里写 pushbuffer →
宿主 vGPU Manager 把这段客户物理地址经 IOMMU 映射给物理 GPU →
**物理 GPU 直接 DMA 读它**。

所以 GPU 真正执行的命令字节，就是客机写的那一份。
这也解释了为什么 `sub4=0x902D`（3D 引擎）在两版的 subchannel 表里完全一样
（报告 145）—— 硬件接口是同一套，vGPU 只是换了"谁把命令递过去"。

---

## 2. Q2：宿主做了什么——四件事，都有二进制证据

| # | 宿主行为 | 证据（来自 `nv-kernel.o` 字符串 / OKM 源码） |
|---|---|---|
| 1 | **寄存器访问被截获** | `RMVGPUGpInBAR0`（"GP in BAR0"）。客机 BAR0 是模拟的，每次读写触发 VM exit |
| 2 | **宿主/客机版本独立握手** | `Incompatible Guest/Host drivers: Host VGX version is older than the minimum version supported by the Guest. Disabling vGPU.` 还有 `RMSetVGPUVersionMax/Min` |
| 3 | **vGPU Manager 独立调度** | `VGPU_CONFIG`、`nv_vgpu_create_request`、`nv_vgpu_update_request`、`nv_vgpu_get_bar_info`、`nv_vgpu_get_sparse_mmap`、`os_call_vgpu_vfio` |
| 4 | **中断注入** | `os_inject_vgx_msi()` / `osVgpuInjectInterrupt()`——**这两处在开源部分可见**（`os-hypervisor.c:65`），走 `xen_ioemu_inject_msi` 或 VFIO |

### 2.1 525 新增：vGPU 卸载到 GSP

525 的 `nv-kernel.o` 里多出这些字符串（515 没有）：

```
LOGVGPU
RMSetVgpuGspPluginOffloadMode
VGPU_GSP_PLUGIN_TRIGGERED
VGPU%d
.fwlogging_vgpu
```

**525 把一部分 vGPU 工作交给了 GSP 固件。** 这与报告 96 的结论（`LOGVGPU` 是
515→525 的新增）一致，也说明 525 这一代的 vGPU 架构确实改过。

> 注意：这不等于"525 在裸机上也走 vGPU 路径"。vGPU 相关代码只在
> `PMC_BOOT_1.VGPU != REAL` 时才被激活。但它提示了一个方向：
> **525 把更多逻辑下放到了固件侧，而固件侧的时序不可观测**——
> 这与我们"122 µs 是固定开销、CPU 侧改不动"的观测是相容的。

### 2.2 为什么 vGPU 里"不能 poll 寄存器"——这是架构硬约束

第 1 条（寄存器截获）有个直接推论：

> 在 vGPU 里，**每一次 BAR0 读都是一次 VM exit**（微秒级）。
> 如果一个驱动每提交一次就去 poll 一次 fence 寄存器，guest 会被自己的轮询打死。

所以 vGPU 架构**必须**让 fence 从"寄存器"搬到"内存"：
GPU 把进度通过 DMA 写进客机内存，客机读内存即可，零陷入。

**而驱动里确实就是这么做的**（见 §3）——它有一整套"缓存 fence"路径。
**这就是 vGPU 与裸机在软件上的真正分岔点。**

---

## 3. 本轮真正的收获：两版读 fence 的代码逐条同构

我本来想找"525 是不是多读了引擎 fence"，结果反过来了——**两版一模一样。**

### 3.1 515 的 fence 读取函数 `0xfb19d0`（144 B）

```asm
fb19db  eax = [ctx+0x1fc]           ; ← 缓存标志（515 的偏移）
fb19e3  jne  fb1a50                 ; 非 0 → 走缓存
fb19e5  r14d = [ctx+0x22e5c]        ; ← 引擎个数 N（515 的偏移）
fb19ec  esi = 0x800000ba
fb19f1  call 0xfbc490               ; BAR 读
fb19fb  call 0xfc7f20               ; 读后维护
fb1a00  cmp r14d,1 ; jle fb1a3c     ; N<=1 → 直接返回
fb1a06  sub r14d,0x7fffff46         ; ← 同一个"减补数"技巧
fb1a0d  ebx = 0x800000bb
loop:   call 0xfbc490 ; ebx++ ; cmp ebp,r12d ; cmovns ebp,r12d   ; 取 min
fb1a3c  [ctx+0x10c8] = rbp          ; 缓存下来（515 的偏移）
fb1a50  rax=[ctx+0x3a0]; ebp=[rax+0x48]   ; 缓存路径（515 的偏移）
```

### 3.2 525 的同一函数 `0x1569f10`（160 B）

报告 146 已完整列出。逐条对齐：

| 元素 | 515 | 525 |
|---|---|---|
| 缓存标志 | `[ctx+0x1fc]` | `[ctx+0x21c]` |
| 引擎个数 | `[ctx+0x22e5c]` | `[ctx+0x22f2c]` |
| 缓存值来源 | `[[ctx+0x3a0]+0x48]` | `[[ctx+0x3c0]+0x48]` |
| rp 缓存写入 | `[ctx+0x10c8]` | `[ctx+0x1128]` |
| BAR 读原语 | `0xfbc490` | `0x1575990` |
| 读后维护 | `0xfc7f20` | `0x1581460` |
| 减补数技巧 | `sub r14d,0x7fffff46` ✓ | 同 ✓ |
| min 循环起点 | `0x800000bb` ✓ | 同 ✓ |
| 调用点数量 | **1**（`0xfbae7c`） | **1**（`0x1572e80`） |

**结构完全同构，只有偏移不同。**

### 3.3 设置"缓存标志"的写入点也一一对应

| 版本 | 写入点 | 值 |
|---|---|---|
| 515 | `0xf105f8` / `0xf1114c` / `0xfb2289` | 1 |
| 525 | `0x14c73e4` / `0x14c8239` / `0x156a7db` | 1 |

525 的 `0x156a7db` 所在函数（`0x156a760`）是个**复位/换段**动作：

```asm
for (i = 0; i < [ctx+0x22f2c]; i++)          ; 清空每个引擎的 0x470 字节状态
    memset([ctx + i*8 + 0x14f0], 0, 0x470);
[ctx+0x21c] = 1                              ; ← 打开"缓存 fence"模式
[ctx+0x1240] = [ctx+0x1130] - 1              ; ← 预置"缓存 rp" = (基准-1)
... 写入 [ctx+0x220]->+0x40/+0x44/+0x58/+0x5c
call 0x156a2a0
```

而 `0x1572e60` 的快路径正好返回 `[ctx+0x1240]`——**两者是配套的**：
复位时把缓存 rp 预置成 `基准-1`，之后一直用缓存值。

### 3.4 结论

> **515 与 525 在这个关键机制上没有任何代码差异。**
> 差异只能是：**运行时 `[ctx+0x1fc/0x21c]` 的取值，或 `[[ctx+0x3a0/0x3c0]+0x48]`
> 这个缓存值的内容/更新时机。**

这与前面所有测量完全自洽：同样的函数、同样的调用结构（报告 145 的 subchannel 表
逐项相同）、同样 13 个"等待函数"调用点（报告 145 §2）。

---

## 4. 由此得到的两个可验证假设（都比"伪造 fence"靠谱）

### 假设 A：陈旧缓存（stale cached fence）—— 我认为这是目前最强的

如果 525 在运行时 `[ctx+0x21c] != 0`（走缓存），而缓存值 `[[ctx+0x3c0]+0x48]`
只在某些时机（ISR / 提交后 / 复位时）被刷新，那么：

```
驱动读缓存 → 缓存是旧值 → 认为"没有空间" → 退避/重试 → 再次读缓存（还是旧值）
     ... 直到某个时机缓存被刷新 ... → 才认为"有空间"
```

**这能解释我们观测到的全部现象：**

| 观测 | 陈旧缓存如何解释 |
|---|---|
| GPU 功耗只有 +1.1 W（报告 147） | **GPU 根本没在算**，它没收到活；是 CPU 误判没空间 |
| 每对固定 122 µs | 缓存的刷新周期 / 退避粒度 |
| 与分辨率、对象类型无关 | 与缓存内容无关，只与"刷新得多慢"有关 |
| 515 免疫 | 515 可能 `[ctx+0x1fc] == 0`（走真 MMIO，永远读到最新） |
| M8 快、M9/M10 快 | 只有"绑定紧跟 draw"才需要每对查一次空间 |
| CPU 侧改不动（报告 147 五次全败） | 因为病根是"读了旧值"，不是"读得慢" |

**关键的修法（与上次相反）**：把 `[ctx+0x21c]` 强制改成 **0**，让它走真 MMIO 读硬件。
如果假设成立，M3 会立刻变快。

### 假设 B：真 MMIO 每次都慢（就是"读得太慢"）

如果 525 已经走 MMIO（`[ctx+0x21c] == 0`），那 122 µs 就是单次 BAR 读的代价。
但这条与"GPU 功耗 +1.1 W"有张力——读寄存器不该让 GPU 停止工作。
**所以我不看好 B，但不能排除。**

### 4.1 实验设计（不需要伪造任何值，因此不会死锁）

**第 0 步（最关键，先做）**：读运行时实际值，判断 A 还是 B。

用现成的 `analysis/onheap/ctxread2.c`（ptrace 硬件断点，零字节修改）读三样：

| 项 | 515 偏移 | 525 偏移 |
|---|---|---|
| 缓存标志 | `[ctx+0x1fc]` | `[ctx+0x21c]` |
| 引擎个数 | `[ctx+0x22e5c]` | `[ctx+0x22f2c]` |
| 缓存 fence 值 | `[[ctx+0x3a0]+0x48]` | `[[ctx+0x3c0]+0x48]` |
| 直接返回的 rp | `[ctx+0x1240]`（525） | — |

**判据**：
- 525 的缓存标志 = 1、515 = 0 ⇒ **假设 A 成立**，进第 1 步
- 两版都 = 0 ⇒ 假设 B，转去给 MMIO 读计时

**第 1 步**：用 `patchpb.so` 把 525 的 `[ctx+0x21c]` 强制置 0（`mov dword [rbx+0x21c], 1`
那条指令改成 `mov dword [rbx+0x21c], 0`，地址 `0x156a7db`，字节
`c7 83 1c 02 00 00 01` → `c7 83 1c 02 00 00 00`）。

**这一步是安全的**：用的是驱动自己的真实机制（真 MMIO 读硬件），
不是伪造常量，不会像报告 147 那样死锁。

**第 2 步**：再试反方向（强制置 1），看是否反而变好。

> 两次实验的字节改动都只有 1 个，且都能用 `patchpb.so` 一步回滚（重启即可）。

---

## 5. 关于"逆闭源宿主"这条建议的评价

你的直觉方向是对的，但**目标选错了**——不必逆宿主。

| 目标 | 在哪 | 值不值得逆 |
|---|---|---|
| 宿主 vGPU Manager（`nvidia-vgpu-mgr`） | 单独的 `-vgpu-kvm` 包，**我们没有** | 成本极高，且学不到我们需要的东西 |
| 宿主侧 OS 胶水（中断注入、VFIO） | **开源**（OKM `os-hypervisor.c`） | 已可直接读，不用逆 |
| **客机 RM 里对 `PMC_BOOT_1.VGPU` 的分支** | **`nv-kernel.o`，我们手上就有** | **值得**，而且它是"vGPU 到底改变了哪些行为"的权威答案 |

理由是：我们最终要在**裸机**上改行为。能改的只有 guest 侧的代码。
宿主再怎么逆，也只是知道"它做了什么"，改不了我们这张 P106 的行为。

### 5.1 但有一个前提必须先说清楚

**P106-100 不支持 vGPU。** 报告 99 已确认：标准驱动的 vGPU 型号表里有 `P40`，
**没有 P106**。P106 是矿卡 SKU，没有 vGPU 授权。

所以：
- **真跑 vGPU 不可能**（装 vGPU 宿主驱动会直接拒绝这张卡）
- **但"强制打开 vGPU 会用的那条快路径"完全可行**——因为那条路径就在
  裸机也能执行的代码里（`[ctx+0x21c]` / `[ctx+0x1240]`），只是默认没被启用

**这正是 §4.1 的实验在做的。**

### 5.2 若要把这条路走到底，静态上还能挖

`PMC_BOOT_1`（寄存器 `0x4`）的读取点在 `nv-kernel.o` 里。找到它，就能看到
RM 在 `VGPU != REAL` 时到底分岔到哪些路径——**这是"vGPU 改变了什么"的完整清单**，
可以逐条对照我们需要的快路径。

难点：`nv-kernel.o` 是 stripped 的（41091/46325 个符号，0 个含 `subchannel`），
必须靠 xref 追。可先从 `RMVGPUGpInBAR0` 这个字符串的引用点入手。

---

## 6. 本轮工具

| 文件 | 用途 |
|---|---|
| `wk/scanfield.py` | 扫 `.text` 里所有"内存操作数 disp == X 且带立即数"的指令（字节预筛 + capstone 验证） |
| `wk/ehmulti.py` | 批量查地址所属函数（`.eh_frame_hdr`） |
| `analysis/onheap/ctxread2.c` | ptrace 硬件断点读 glcore 内部字段（零字节修改）——**§4.1 第 0 步就用它** |
| `analysis/shim/patchpb.so` | 运行时改任意字节，`PATCH="0xADDR=hex"` |

### 6.1 本轮踩到的判据坑（已写入 SKILL 67）

用 `grep -i` 查二进制里的技术关键词（vgpu / sm / mmu / gpu 这类短词）**必须防 ROT13 假阳性**：
NVIDIA 把内部选项名做 ROT13 放在二进制里，`CTASwitch` 里就含 `witch`，
会让 `grep -i vgpu` 报出 3~4 个假命中。

正确做法：先 `grep`（区分大小写）拿到精确集，再对可疑串做 ROT13 解码确认。
