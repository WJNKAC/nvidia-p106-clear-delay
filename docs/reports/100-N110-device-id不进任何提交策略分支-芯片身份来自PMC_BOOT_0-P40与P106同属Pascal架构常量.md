# 报告 100 / N110：device id 不进任何"提交策略"分支 —— 芯片身份来自 `NV_PMC_BOOT_0` 寄存器，且 P40 与 P106 共享同一个 Pascal 架构常量

- 时间：2026-10-26
- 驱动：525.116.03（`nv-kernel.o_binary` 63.1 MB，`.text` 0xb4d904）
- 源码依据：`okm/open-gpu-kernel-modules-525.60.11/`（与 525.116.03 同分支）
- 任务来源：用户指令「先静态查 `nvidia.ko` 里 `0x1c07`（P106）和 `0x1b38`（P40）的配置分支差异，找 device id → 芯片型号 → 提交策略的映射；如果找到，看 P40 的配置是不是异步批次；如果找不到，直接用 QEMU `x-pci-device-id=0x1b38` 验证」

**一句话结论**：
**这张映射表不存在，而且按 NVIDIA 的架构它在原理上不可能以"device id → 提交路径"的形式存在。**
芯片身份由 **BAR0 的 `NV_PMC_BOOT_0`（偏移 0x0）寄存器**读出，不是 PCI 配置空间的 device id；
vGPU 与否由 **`NV_PMC_BOOT_1`（偏移 0x4）bit 17:16** 决定，也不是 device id。
所以 **QEMU `x-pci-device-id=0x1b38` 改不到这两处任何一处**，该实验会得到一个干净的"无变化"，但它否掉的是 device id 假设，不能否掉"vGPU 路径更快"。

---

## 1. device id 在加载路径上只有一个用途：白名单

`kernel-open/nvidia/nv-pci.c`（525.116.03 版，与 OKM 525.60.11 逐行一致）：

```c
// line 232
if (!rm_is_supported_pci_device(
            (pci_dev->class >> 16) & 0xFF,
            (pci_dev->class >> 8) & 0xFF,
            pci_dev->vendor,
            pci_dev->device,
            pci_dev->subsystem_vendor,
            pci_dev->subsystem_device,
            NV_FALSE /* print_legacy_warning */))
{
    nv_printf(NV_DBG_ERRORS, "NVRM: ignoring the legacy GPU ...");
    goto failed;
}
```

这是 nv_pci_probe 里**唯一**一次把 `pci_dev->device` 往下传的地方。它的语义是"这张卡在不在支持名单里"，返回 bool，**不选择任何后续路径**。

这条源码结论正好解释了 N110 前三步的反常结果：

| 观察 | 解释 |
|---|---|
| P106(`0x1c07`) 在 `.text` 里 **0 处真立即数比较** | 因为它只被当作参数传进白名单函数，不参与任何 `cmp` |
| P40(`0x1b38`) 只有 **1 处**（`file=0xccb50`） | 那处在 vGPU 类型路径上（报告 99 的 `GRID P40-2B` 相关），不是芯片配置选择 |
| `.rodata` 里两张 1429 / 1339 项的表是 `subsys_vendor/subsys_device/vendor` 三元组 | 那就是 `rm_is_supported_pci_device()` 的数据源，不是 chip config 表 |

**⇒ "device id → 芯片型号" 映射表：不存在。**（不是"没找到"）

---

## 2. 芯片身份来自 `NV_PMC_BOOT_0` 寄存器（偏移 0x0）

`src/common/inc/swref/published/nv_ref.h:106-127`：

```c
/* dev_master.ref */
#define NV_PMC_BOOT_0                                    0x00000000 /* R--4R */
#define NV_PMC_BOOT_0_MINOR_REVISION                            3:0
#define NV_PMC_BOOT_0_MAJOR_REVISION                            7:4
#define NV_PMC_BOOT_0_IMPLEMENTATION                          23:20
#define NV_PMC_BOOT_0_ARCHITECTURE                            28:24
#define NV_PMC_BOOT_0_ARCHITECTURE_TU100                 0x00000016
#define NV_PMC_BOOT_0_ARCHITECTURE_GA100                 0x00000017
...
```

驱动侧读取点（同一份 OKM）：

```
src/nvidia/arch/nvalloc/unix/src/osinit.c:364    pmc_boot_0 = NV_PRIV_REG_RD32(nv->regs->map_u, NV_PMC_BOOT_0);
src/nvidia/arch/nvalloc/unix/src/osinit.c:1161   pmc_boot_0 = NV_PRIV_REG_RD32(pNv->regs->map_u, NV_PMC_BOOT_0);
src/nvidia/arch/nvalloc/unix/src/osapi.c:308     NvU32 pmcBoot0 = NV_PRIV_REG_RD32(nv->regs->map_u, NV_PMC_BOOT_0);
src/nvidia/arch/nvalloc/unix/src/osapi.c:3188    pmc_boot_0 = NV_PRIV_REG_RD32(reg_mapping, NV_PMC_BOOT_0);
```

其中 `osinit.c:1161` 把它存进 `nvp->pmc_boot_0`，之后只用于"GPU 掉总线"判定（`osHandleGpuLost`：值变了 ⇒ 认为卡掉了）。**真正的芯片识别在 RM 核心里，用同一个寄存器**：arch = bit 28:24、impl = bit 23:20，合成索引去查 HAL 表。

这就是为什么整套 `.text` 里搜不到 device id 比较 —— **驱动根本不用 device id 认芯片**。

---

## 3. vGPU 环境检测有一个专用寄存器位：`NV_PMC_BOOT_1` bit 17:16

这是本轮**最重要的新发现**，`nv_ref.h:132-137`：

```c
#define NV_PMC_BOOT_1                                    0x00000004 /* R--4R */
#define NV_PMC_BOOT_1_VGPU8                                     8:8
#define NV_PMC_BOOT_1_VGPU8_REAL                         0x00000000
#define NV_PMC_BOOT_1_VGPU8_VIRTUAL                      0x00000001
#define NV_PMC_BOOT_1_VGPU16                                  16:16
#define NV_PMC_BOOT_1_VGPU                                    17:16 /* C--VF */
#define NV_PMC_BOOT_1_VGPU_REAL                          0x00000000
#define NV_PMC_BOOT_1_VGPU_PV                            0x00000001   // 半虚拟化（vGPU guest）
#define NV_PMC_BOOT_1_VGPU_VF                            0x00000002   // SR-IOV VF
```

**这条直接改写报告 96/97 的结论链**：

| 旧说法（报告 96/97） | 现在 |
|---|---|
| "vGPU 检测在 `nvidia.ko`，走 DMI（`dmi_get_system_info`），不引用 `x86_hyper_type`" | DMI 只是 Linux 胶水层的辅助（`nv-hypervisor.h` 的 `HYPERVISOR_TYPE`，仅用于 `nv_treat_missing_irq_as_error()` 和 ACPI PR3）。**vGPU 与否的权威判据是 BAR0 的 `PMC_BOOT_1[17:16]`** |
| kprobe 打不进 `_nv027892rm`，"让驱动以为在 KVM 里"做不通 | 做不通的真正原因不是 `notrace`，是**改错了对象**：`x86_hyper_type` 不参与图形路径选择；要改的是 BAR0 里那两位 |

**推论（关键）**：`PMC_BOOT_1` 是 **BAR0 里的寄存器**。物理直通时它由真硅片回答，永远是 `REAL=0`。只有 BAR0 被宿主 RM 模拟（真 vGPU）时才会返回 `PV=1`/`VF=2`。
⇒ **"让驱动以为自己在 vGPU 里"的唯一途径是虚拟化 BAR0，即真上 vGPU；任何 PCI 配置空间层面的伪装（device id / subsystem id）都无效。**

---

## 4. 标准驱动在结构上进不去 vGPU 模式

`kernel-open/nvidia/nv-pci.c:186-227`：

```c
#ifdef NV_PCI_SRIOV_SUPPORT
    if (pci_dev->is_virtfn)
    {
#if defined(NV_VGPU_KVM_BUILD)
        ...
        if (nvidia_vgpu_vfio_probe(pci_dev) != NV_OK) { ... goto failed; }
        nv_kmem_cache_free_stack(sp);
        return 0;
#else
        nv_printf(NV_DBG_ERRORS, "NVRM: Ignoring probe for VF %04x:%02x:%02x.%x ", ...);
        goto failed;
#endif /* NV_VGPU_KVM_BUILD */
    }
#endif /* NV_PCI_SRIOV_SUPPORT */
```

两道门：
1. `pci_dev->is_virtfn` —— 必须是 SR-IOV 虚拟功能。**VFIO 直通不是 VF**（透传的是 PF 本身），这个条件为假。
2. `NV_VGPU_KVM_BUILD` —— 编译期开关。实测 525.116.03 的 `kernel-open/` 里 **没有** `nv-vgpu-vfio-interface.c`（`find -iname "*vgpu*"` 只命中 `common/inc/nvgputypes.h`），即标准包**不可能**定义这个宏。

**⇒ 用户现在装的 525 是标准版，无论 device id 是什么，都进不了 vGPU 代码路径。**

---

## 5. vGPU 类型表是"用户态提交的配置清单"，不是代码分支

这也补齐了报告 99 的机制侧：

```c
// src/common/sdk/nvidia/inc/ctrl/ctrla081.h
#define NVA081_CTRL_CMD_VGPU_CONFIG_SET_INFO  (0xa0810101)

typedef struct NVA081_CTRL_VGPU_INFO {
    NvU32 vgpuType;
    NvU8  vgpuName[NVA081_VGPU_STRING_BUFFER_SIZE];      // "GRID P40-2B"
    ...
    NvU64 vdevId;          // 虚拟 device id（= vGPU type id）
    NvU64 pdevId;          // 物理 device id
    NvU64 profileSize;     // 显存切分大小
    NvU64 bar1Length;
    NvU32 vgpuExtraParams[NVA081_EXTRA_PARAMETERS_SIZE];  // ← 每型可带额外参数
    ...
} NVA081_CTRL_VGPU_INFO;
```

以及一条**明确不校验**的内部通道（`ctrl2080vgpumgrinternal.h`）：

```
NV2080_CTRL_CMD_VGPU_MGR_INTERNAL_PGPU_ADD_VGPU_TYPE (0x20804003)
  "Unlike NVA081_CTRL_CMD_VGPU_CONFIG_SET_INFO, it does no validation
   and is only to be used internally."
```

**这就是 vGPU_unlock 的真实着力点**：它改的是**交给内核的 vGPU 配置清单**（往 `vgpuConfig` 里塞消费卡的 `pdevId` 和自造的 vGPU 类型），而不是改任何 `cmp`/`jcc`。
所以"N109 只找到 P40 的 1 处比较、P106 零"是**预期形态**，不是遗漏。

---

## 6. 对"P40 是异步批次 / P106 是同步"这个假说的结构性否证

`src/common/inc/swref/published/nv_arch.h:44-52`：

```c
#define GPU_ARCHITECTURE_MAXWELL         GPU_ARCHITECTURE(_CLASSIC, 0x0110)
#define GPU_ARCHITECTURE_PASCAL          GPU_ARCHITECTURE(_CLASSIC, 0x0130)
#define GPU_ARCHITECTURE_VOLTA           GPU_ARCHITECTURE(_CLASSIC, 0x0140)
...
```

**P40 = GP102，P106 = GP106，两者 arch 常量都是 `0x0130`（Pascal）。**

RM 的 HAL 分派粒度是 **arch**（`kernl_gp100.o` 这一级），同一 arch 下的各 chip 只共享同一套 HAL，差异落在 HAL 实例里的少量标量参数（FB 容量、PGRAPH 上下文大小、engine 条数、bar1 大小）。
**Pascal 的命令提交器、pushbuffer 格式、`SEMAPHORE_*` 语义、`kchannel` 调度器 —— 对 GP102 和 GP106 是同一份代码。**

⇒ **"121µs 来自芯片型号不同 / 来自 P40 与 P106 走了不同提交分支"在架构层就不成立。** 这一条不需要上机，靠源码常量即可判决。

至于 GL 侧，报告 95 已经证明 GL 与 Vulkan 在 `libnvidia-glcore` 汇合，而那是**纯用户态、与芯片型号无关**的同一份二进制。

---

## 7. 判决：QEMU `x-pci-device-id=0x1b38` 会得到什么

按上面四条的机制，`x-pci-device-id` 只改 PCI 配置空间里的 `device` 字段（guest 侧 `pci_dev->device`）。它**改不到**：

| 目标 | 由谁决定 | device id 能改吗 |
|---|---|---|
| 芯片 arch/impl（→ HAL 表） | `PMC_BOOT_0` bit 28:24 / 23:20 | ❌ 寄存器仍答 GP106 |
| 是否 vGPU 环境 | `PMC_BOOT_1` bit 17:16 | ❌ 仍答 `REAL=0` |
| 是否 VF | `pci_dev->is_virtfn`（内核根据 SR-IOV capability 填） | ❌ 透传不是 VF |
| 白名单 | `rm_is_supported_pci_device()` | ✅ 唯一会变的一项 |
| `nvidia-smi` 显示名 | 白名单表里的名字 | ✅ 会变 |

**预期结果**：`n83a -M 3` 的每对成本**仍是 121µs 量级**，`nvidia-smi` 里的卡名可能变成 `GRID P40-2B` 或别的 P40 变体（因为名字确实按 device id 查表）。

**这个实验仍然值得做**，因为：
- 若真有变化 ⇒ 说明存在我们没找到的 device-id 敏感分支（**这将是比 121µs 更大的发现**，说明 RM 里还有一条只看 device id 的旁路）；
- 若无变化（预期） ⇒ **干净地、不可争辩地否掉"device id 假设"**，把剩下的可能性压缩到"环境（BAR0 虚拟化）"和"驱动版本"两项。

**但它不能回答"vGPU 路径是不是更快"。** 不要把它当成复现原场景的实验。

---

## 8. 唯一能改 `PMC_BOOT_1` 的实验（修正方案）

要复现 2 年前 PVE 里那个 130W / 99% util 的场景，必须让 BAR0 被宿主 RM 模拟。步骤与当年逐项同构：

1. 在 P106 所在机器装 **vGPU 宿主驱动**（与截图对齐的版本线：550.90.07 的 `-vgpu-kvm` 包；该包才定义 `NV_VGPU_KVM_BUILD` 并带 `nv-vgpu-vfio-interface.c` / `nvidia-vgpu-vfio.ko`）。
2. 打 **vGPU unlock 配置**：往 vGPU 配置清单里加 P106 的 `pdevId = 0x1c07` 和一个自造 type（`GRID P<卡>-2B` 风格的图形型，`vgpuName` 随便，`cudaEnabled=1` 视需要）。
3. `mdev` 建实例 → 起一台 VM（KVM）→ guest 装对应 **vGPU guest 驱动**（同一 550.90.07）。
4. guest 里跑同一个 `n83a -M 3`（268 对/帧），读 `map_p50` 与 `us/pair`。

**判读表**：

| guest 每对成本 | 结论 |
|---|---|
| ≈ 0.03µs（与 515 同级） | `PMC_BOOT_1=PV` 时 RM 走的是**不付 121µs 的提交路径** ⇒ "vGPU 提交策略"假说成立，接下来静态目标锁定为"读 `BOOT_1` 后分叉的那条边"（在 RM 里搜 `and 0x30000` / `shr 16` / `cmp 1|2`，本轮已列出 21 个候选点，见 §9） |
| ≈ 121µs | vGPU 无关 ⇒ 121µs 是 Pascal 裸机在 525+ 的固有代价，方向回到版本回归（报告 35/37 那条线） |

**注意**：vGPU 把显存切成 2GB，Heaven 的档位必然比裸机低，fps 不能直接比。**只比 `n83a -M 3` 的 µs/对**，那才是同一把尺子。

---

## 9. ~~副产品：21 个 `and 0x00030000` 候选点~~ ⛔ **本节已整体作废（报告 102，2026-10-26 15:30）**

> **作废原因（两条，都是我自己的错）**：
> 1. **掩码撞车**：`0x30000` 在 RM 里是「32 位描述符的 bit 17:16」这个通用位置的常见掩码。
>    判定一个点是不是 `PMC_BOOT_1` 的唯一标准是**数据来源**（是否从寄存器偏移 0x4 读出），不是掩码形状。
>    这 21 处无一处前后 10 条指令内有「以偏移 0x4 读寄存器」。
> 2. **objdump 地址空间用错**：`.o` 里 `nm` 值 = VMA（objdump 要用），文件偏移 = VMA + 0x40。
>    本节当时是用文件偏移直接喂 `objdump --start-address`，反汇编的不是那个位置。
>
> **实测复核结论**：`_nv035724rm` = **NVSwitch** PTE 组装（断言串 `NVSwitch: Assertion failed`，bit 17:16 → PTE bit 38:37）；
> `_nv016286rm` 四处 = **内存描述符 bit 17:16 的无条件读-改-写**，零 `cmp`/`jcc`。
> **没有分叉。** 详见报告 102。

### （以下为原文，仅供追溯）

`n111_boot1.py` 在 `.text` 里扫 `and r32, 0x00030000`（= `PMC_BOOT_1[17:16]` 掩码），排除位移字节后 21 处；`n111b_symmap.py` 映射到 RM 符号（`.o` 里符号值 = 文件偏移 − `.text` 起点 `0x40`，46023 个符号）：

| file off | 符号 | 备注 |
|---|---|---|
| `0x43049` | `_nv026612rm +0x289` | 后续是 `cmp [rsp+...], 0` 形态 |
| `0xdb059` | `_nv013981rm +0xc9` | 后面紧跟 `cmp ...,0x30000` + `je` |
| `0x1bc940` / `0x1bc9b9` / `0x1bcd19` / `0x1bcd89` | `_nv016286rm` +0x3370/33e9/3749/37b9 | **同一函数 4 处**，都写 `[rbx+0x138]`；像是一组 feature 位聚合 |
| `0x1bf176` | `_nv016289rm +0x1636` | 同样写 `[..+0x138]` |
| `0x313577` | `_nv029799rm +0x187` | 与 `0x30000000`/`0x30000000` 位组合，像 capability 合成 |
| `0x3fe10a` | `_nv030738rm +0xa` | 极短函数，`(x==1) ? 0xfe0000 : 0x30000` 形态 |
| `0x457048` / `0x45ca71` | `_nv022259rm +0x108` / `_nv022245rm +0xf1` | 后面接着 `and 0x7f000000` |
| `0x63ad39` / `0x6400f2` / `0x646a71` / `0xb3fc3d` | `_nv029201rm` / `_nv029216rm` / `_nv029204rm` / `_nv036182rm` | 都在判 `0x180000` + `0x200000`，且 `shr 21` 后 `xor 1` —— **某一类布尔属性的取反判定**，四份拷贝 |
| `0xad0148/251/35a/463` | `_nv035819rm +0x378/481/58a/693` | 同一函数 4 处，`cmp ...,5/8/0xb/0xe` —— 枚举分派，**不适合 vGPU 位** |
| `0xb11ed1` / `0xb11ed7` | `_nv035724rm +0x4e1/4e7` | `and ecx/ eax, 0x30000` 后 `shr r12d,16`，再 `shl 21` 写结构体 —— **形态最像"读一个 32 位寄存器、取 17:16、写到对象字段"** |

最可疑的两个：`_nv035724rm+0x4e1`（形态吻合）与 `_nv016286rm`（集中 4 处）。**但本轮无法在不上机的前提下把它们与 `PMC_BOOT_1` 绑定**——缺少"读 0x4 的调用点"这一环。等 §8 的 vGPU 实验拿到"有/无分叉"的结论后再定向收口，比现在盲扫高效。

---

## 10. 本轮新增/修改文件

- `analysis/probes/n111_boot1.py` —— 扫 `and 0x30000 / 0x20000 / 0x10000` 与 `cmp 1/2`
- `analysis/probes/n111b_symmap.py` —— `.text` 文件偏移 → `_nvXXXXXXrm` 符号（46023 符号，二分查找）
- `analysis/probes/n111c_after.py` —— 逐点看后续字节，判是否 `>>16` 后比较
- 本报告；`README.md` 索引同步；`SKILL.md` 新增"寄存器位是虚拟化判据"一节

## 11. 下一步（按优先级）—— ⚠️ 已由报告 102 修订，以报告 102 §5 为准

1. **（用户侧，1 次上机）** 跑 §8 的 vGPU 复现，只取 `n83a -M 3` 的 µs/对。
2. **（可选，零成本）** 若嫌 1 重：先只做 §7 的 `x-pci-device-id=0x1b38`，用它的"无变化"把 device id 假设钉死，避免以后反复回到这条岔路。
3. **（静态，依赖 1）** 若 1 证明 vGPU 更快，用 §9 的 21 个候选点定向找"读 `BOOT_1[17:16]` → 分叉"的那条边。
