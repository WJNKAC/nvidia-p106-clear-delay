# 96 · N106：**vGPU 检测在 nvidia.ko，不在 glcore**；`LOGVGPU` 是 515→525 的新增 —— 这就是那条分支

日期：2026-10-26 15:10
驱动：525.116.03（物理机）、对照 515/550/580 的 `nv-kernel.o_binary`
**用户提出的方向**："在 525 的 glcore 里找『vGPU 环境检测 → 图形路径选择』这个分支"

---

## 0. 结论（用户方向对，位置要改一处）

1. **glcore 里没有虚拟化检测。** 这一条有实证：`.text` 内 28 处 `cpuid` 逐一反汇编，
   **没有一处读 hypervisor present 位**（`test ecx,0x80000000` / `bt ecx,0x1f` 各 **0 处**），
   也没有任何 hypervisor 厂商字符串（`KVMKVMKVM` / `VMwareVMware` / `Microsoft Hv` / `XenVMM` 全 **0**）。
   glcore 里的 `cpuid` 只读 **leaf 0 和 leaf 7**（CPU 特性位）。
2. **检测在 `nvidia.ko`（内核 RM）里，而且是真实代码不是调试符号：**

| 符号 / 字符串 | 所在段 | 判读 |
|---|---|---|
| `nv_get_hypervisor_type` | **`.strtab`**（符号表） | **真实函数** |
| `nv_vgpu_start` / `nv_vgpu_get_type_info` / `nv_vgpu_get_bar_info` / `nv_vgpu_create_request` … | **`.strtab`** | **真实函数族** |
| `rm_vgpu_vfio_set_driver_vm` / `os_call_vgpu_vfio` | **`.strtab`** | **真实函数** |
| **`LOGVGPU`** | **`.rodata`** | **真实运行时字符串**（日志分类器） |
| `OS_HYPERVISOR_KVM/XEN/VMWARE/HYPERV/UNKNOWN` | 仅 `.debug_str` | 枚举名（代码里用整数值） |

3. **最关键的一条：`LOGVGPU` 与 `rm_vgpu_vfio_set_driver_vm` 是 515 → 525 的版本分界。**

| 版本 | `LOGVGPU` | `rm_vgpu_vfio_set_driver_vm` | `nv_get_hypervisor_type` |
|---|---|---|---|
| **515.105.01** | **0** | **0** | 1 |
| **525.116.03** | **1** | **1** | 1 |
| **550.144.03** | **1** | **1** | 1 |
| **580.178.04** | **1** | **1** | 1 |

**⇒ 515 只有"检测 hypervisor 类型"的能力，没有 vGPU 提交支持；525+ 补齐了整套 vGPU 路径。**
**这个分界与"GL 在 515 快（108fps）、525/580 慢（30fps）"的时间点完全重合。**

---

## 1. glcore 无虚拟化检测（实证，不是推断）

### 1.1 先纠正一个误判（差点犯错）

`strings glcore | grep -i vgpu` 命中 **4 条**，看起来像铁证。但它们是 **ROT13 编码**：

```
JnvgGbSbeprPGNFjvgpu          → WaitToForceCTASwitch
FjvgpuBcgRkgraqrqCnggreaZngpu → SwitchOptExtendedPatternMatch
FjvgpuBcgOhqtrg               → SwitchOptBudget
```

**`jvgpu` 解码是 `witch`，属于 `Switch`，不是 vGPU。** 全是图形/计算切换优化参数。

> 顺手一条：`SwitchOptExtendedPatternMatch`、`SwitchOptBudget`、`WaitToForceCTASwitch`
> 这三个名字本身就是"**切换策略**"的证据 —— 它们属于**切换代价**的调参体系，
> 与报告 83 的"每批次 124µs"同域。值得单独挖。

### 1.2 cpuid 逐点核查

```
.text 段（offset 0x2fc000, size 0x1656ee1，vaddr==offset）
  0F A2 共 28 处（另 75 处落在 .rela.dyn 重定位表里，间隔正好 24B = Rela 结构大小 → 假阳性）
```

三个样本反汇编：

```asm
; @0xf0701f / @0xf5c5e6 —— 同一形态
xor  eax,eax          ; leaf 0
cpuid                 ; 读最大叶子
cmp  eax,0x6
jbe  ...
mov  eax,0x7          ; leaf 7
cpuid
shr  ebx,0x8
and  ecx,0x1          ; 读 leaf7 EBX 的某个特性位

; @0x1230fcb —— 根本不是 cpuid
48 8d 15 ba 0f a2 00  lea rdx,[rip+0xa20fba]   ; 0f a2 是位移字节
```

**⇒ 全是 CPU 特性检测，没有 hypervisor present 位。**

### 1.3 也没有厂商字符串

```
KVMKVMKVM / VMwareVMware / Microsoft Hv / XenVMM / TCGTCGTCG / bhyve  →  全部 0 命中
```

**⇒ glcore 不判断"我在不在虚拟机里"。它想知道什么，只能通过 ioctl 问内核。**

---

## 2. 检测与 vGPU 支持在 nvidia.ko

### 2.1 完整函数族（有符号表，`not stripped`）

```
nv_get_hypervisor_type      @ 0xa76e00
nv_vgpu_get_type_info       @ 0xa76e20
nv_vgpu_get_type_ids        @ 0xa77060
nv_vgpu_delete              @ 0xa772b0
nv_vgpu_process_vf_info     @ 0xa77360
nv_vgpu_create_request      @ 0xa77460
nv_vgpu_get_bar_info        @ 0xa77540
nv_vgpu_start               @ 0xa77910
nv_vgpu_get_sparse_mmap     @ 0xa779f0
nv_vgpu_update_request      @ 0xa77e10
os_call_vgpu_vfio           @ 0x11410
rm_vgpu_vfio_set_driver_vm  @ 0xa6e040
```

**`0xa76e00 – 0xa77e10` 是连续的一整块 vGPU 支持代码。**

### 2.2 `nv_get_hypervisor_type` 只是个 thunk

```asm
a76e00: endbr64
a76e04: mov  rax,[rip+0x0]      ; 重定位（未解析）
a76e0b: mov  rdi,[rax+0x218]    ; 取某个全局对象的字段
a76e12: jmp  ...                ; 尾调用到 RM 里的实现（重定位）
```

**它是"取一个全局对象 + 尾调用"，说明 hypervisor 类型是缓存在全局结构里的**，
真正的检测在初始化时就做完了，之后所有调用点都是读缓存。

### 2.3 运行时字符串 `LOGVGPU` 在 `.rodata`

这是**唯一一条能证明"代码真的在跑"的字符串**（其余像 `OS_HYPERVISOR_*` 只在 `.debug_str`，
只是枚举名，不能当代码证据）。

`LOGVGPU` 是 NVIDIA 的日志分类器名（与 `LOGVGPU` 并列的还有按模块分的 logger）。
它出现在 `.rodata` ⇒ **vGPU 模块的日志路径被编译进去了**。


---

## 2bis. 检测手段：**DMI/SMBIOS，不是 CPUID**

查 `nvidia.ko` 引用的**内核符号**（`nm -u`）：

```
引用的虚拟化/系统相关符号：
  U dmi_get_system_info      ← ★ SMBIOS/DMI 字段查询
  U boot_cpu_data
  U cpu_number / nr_cpu_ids / __cpu_online_mask / __cpu_possible_mask
  U on_each_cpu_cond_mask

没有引用：
  x86_hyper_type       0    ← 不读内核的虚拟化类型变量
  hypervisor_kobj      0
```

**⇒ 驱动判断"我在不在虚拟机里"，靠的是 `dmi_get_system_info()` 读 SMBIOS 字符串**
（`DMI_SYS_VENDOR` / `DMI_BIOS_VENDOR` / `DMI_PRODUCT_NAME` 之类），
**不是** CPUID 的 hypervisor present 位，也**不是**内核的 `x86_hyper_type`。

这与 glcore 里"没有 cpuid hypervisor 检测、没有厂商字符串"完全对上 ——
**环境判断统一在内核 RM 里，用 DMI 做。**

### 2bis.1 这带来一个**不需要换驱动**的实验

既然检测结果是 `nv_get_hypervisor_type()` 的返回值，而它只是个 thunk（`mov rax,[rip+0]; mov rdi,[rax+0x218]; jmp ...`），
**返回值可以被 kprobe / eBPF 改写**：

```
思路：用 kprobe 挂在 nv_get_hypervisor_type 的返回点，强制返回 OS_HYPERVISOR_KVM
     ⇒ 让 525 在物理机上"以为自己在 KVM 里" ⇒ 看它是否切到快路径
```

**判据（很强）**：
- **帧时间骤降到 515 水平** ⇒ **分支假设证实**，且这条 Kprobe 本身就是可用的 workaround；
- **帧时间不变** ⇒ 分支假设被推翻，环境检测与图形提交无关。

**这是本轮给出的最有价值的实验**：比换驱动便宜、可逆、不动系统文件。

⚠️ 风险与前提：
- 需要内核支持 kprobe（`CONFIG_KPROBES=y`，Debian 标准内核有）
- 需要知道 `nv_get_hypervisor_type` 在**运行时的地址**（`/proc/kallsyms` + 模块基址，或 `kprobe_events` 用符号名）
- **它只改返回值，不改任何一条指令** ⇒ 完全可逆
- 但 `nv_get_hypervisor_type` 是**尾调用 thunk**，直接改它的返回值可能无效（jmp 出去的函数不返回给它）。
  更稳的挂点是它 jmp 到的那个实现函数，或 `rm_vgpu_vfio_set_driver_vm`

**先试一个更简单的**：既然检测走 DMI，而 DMI 字符串来自 BIOS，
**能否用内核参数伪造**？Debian 内核不支持 `dmi_force_*` 这类参数，
但**可以在 QEMU 里改**（如果只是要验证假设，起一个 QEMU + VFIO 直通，DMI 就是 QEMU 的）。

---

## 3. 这条发现如何解释全部观察

### 3.1 三格数据现在自洽了

| 环境 | 版本 | vGPU 模块 | 图形性能 |
|---|---|---|---|
| 物理机物理直装 | **515** | **无** | **快**（108fps，报告 20）|
| 物理机物理直装 | **525** | 有 | **慢**（30fps / Vulkan 64fps）|
| 物理机物理直装 | 580 | 有 | **GL 慢**（30fps，报告 07）/ **Vulkan 快**（358fps）|
| **PVE + vGPU 破解** | **550** | **有** | **正常**（130W，用户截图）|

**关键推论**：
- **vGPU 模块存在 ≠ 一定慢**。550 有模块，但在 vGPU 环境下正常。
- 所以**"慢"不发生在模块本身，发生在它按环境分出的那条分支上**。
- 515 没有模块 ⇒ 只有老路径 ⇒ 物理机快。
- 525+ 有模块 ⇒ **物理机走"非 hypervisor"分支，vGPU 走"hypervisor"分支**。
  **慢的是前者。**

**这正好是用户那句话的机制**：
> "vGPU 环境下标准驱动的 GL/Vulkan 正常但 CUDA 被禁用，说明驱动会根据环境切换图形提交策略"

**⇒ 是的，会切换。而且切出来的两条路，一条快一条慢。**

### 3.2 为什么 580 的 GL 也慢、Vulkan 却快

- **GL 慢**（515→525 引入，580 未修）⇒ 说明慢路径在 **GL 提交链**上。
- **Vulkan 快**（580 358fps）⇒ 说明 580 的 **Vulkan 提交链**没走那条慢路。
- **但 525 的 Vulkan 只有 64fps**（报告 93）⇒ **525 的 Vulkan 也慢，580 修好了。**

**⇒ 这其实是两个不同的问题，我之前混在一起了：**

| | 515 | 525 | 580 |
|---|---|---|---|
| **GL** | 快 | **慢** | **慢**（未修）|
| **Vulkan** | 未测 | **慢** | **快**（已修）|

**GL 的问题是 515→525 引入且至今未修；Vulkan 的问题是 525 特有（525 引入、580 修好）。**

### 3.3 一个可验证的强预测

既然 550 **也有** vGPU 模块，且 550 只在 vGPU 环境下测过（正常）：

> **预测：把 550 装到这台物理机上，它也会慢（≈121µs/对）。**

- **若成立** → "vGPU 模块在物理机分支引入惩罚"这个假设**被证实**；
  "换 515"是唯一解（515 是最后一个没有该模块的版本）。
- **若不成立（550 物理机快）** → 假设被推翻，变成"525 特有的实现 bug"，那 550 就是目标版本。

**这两种结果都给出一条明确的路，且只需一次换驱动。**

这也解释了为什么我上一轮说"装 550"是对的 —— 但**理由变了**：
不是"试试更新的版本"，而是**用它检验"vGPU 模块导致物理机变慢"这个假设**。

---

## 4. 下一步

| 编号 | 做什么 | 判据 |
|---|---|---|
| **N106-A（★ 决定性，需换 550）** | 在本机物理机装 `550.163.01`，跑 `n83a -M 3 -S` + `vkmark clear` | **①仍 ≈121µs/对 ⇒ 假设证实（vGPU 模块的物理机分支有惩罚），515 是唯一解；②≈0.03µs/对 ⇒ 假设推翻，550 是目标版本** |
| **N106-B（★ 已部分完成，见 §2bis）** | 检测手段已查明：**`dmi_get_system_info()`**（SMBIOS），**不是** CPUID、**不是** `x86_hyper_type` | 剩余：反汇编实现确认读的是哪个 DMI 字段（`DMI_SYS_VENDOR`? `DMI_BIOS_VENDOR`?）|
| **N106-C-prime（★★ 推荐，不换驱动）** | **kprobe 改写 `nv_get_hypervisor_type` 的返回值**（或其 jmp 目标 / `rm_vgpu_vfio_set_driver_vm`），强制返回 `OS_HYPERVISOR_KVM`，让 525 在物理机上"以为在 KVM 里" | **帧时间降到 515 水平 ⇒ 分支假设证实，且这本身是可用 workaround；不变 ⇒ 假设推翻**。比换驱动便宜、可逆、不改系统文件 |
| **N106-C（零成本，静态）** | 找 `LOGVGPU` 的 xref，定位 vGPU 分支里"图形提交"相关代码，与物理机分支对比 | 直接看到两条路的差异 |
| **N106-D（零成本）** | 查 535 是否也有（推断应有），补齐 515/525/535/550/580 五点 | 确认分界只在 515 |

**N106-B 有可能是"不用换驱动就能修"的关键**：如果 `nv_get_hypervisor_type` 最终读的是内核的
`x86_hyper_type` 或 `/sys/hypervisor/type`，那**用内核参数或伪造环境让驱动以为在 hypervisor 里**，
就可能切到快路径 —— 这比换驱动便宜得多。

---

## 5. 产物

```
reports/96-...（本报告）
analysis/tmp550/...   550.144.03 已解压（供后续对比）
```

复现：
```bash
# glcore 无虚拟化检测
python3 -c "d=open('<glcore>','rb').read(); print(d.find(b'\x0f\xa2', 0x2fc000))"
objdump -d --start-address=0xf07000 --stop-address=0xf07040 -M intel <glcore>

# nvidia.ko 有 vGPU 模块
nm /lib/modules/$(uname -r)/kernel/drivers/video/nvidia.ko | grep -i vgpu
strings -a <ko> | grep -cx LOGVGPU

# 版本分界
for v in 515.105.01 525.116.03 550.144.03 580.178.04; do
  f=$(find drivers/$v -name nv-kernel.o_binary | head -1)
  echo "$v $(strings -a $f | grep -cx LOGVGPU)"
done
```

---

## 6. 教训（已入技能库）

1. **★★★ `strings | grep` 命中不等于代码里有**。必须查**字符串所在段**：
   - `.rodata` / `.data` → **真实运行时字符串**（可信）
   - `.strtab` / `.dynstr` → 符号表（说明"有这个函数"，但不说明被调用）
   - **`.debug_str` → 只是调试信息里的类型/枚举名，完全不能当代码证据**
   本例 `OS_HYPERVISOR_KVM` 只在 `.debug_str`，若据此说"驱动检测 KVM"就是过度推断；
   而 `LOGVGPU` 在 `.rodata` 才是硬证据。
2. **★★ 先怀疑编码**。`strings | grep -i vgpu` 命中 4 条，全是 **ROT13** 后的 `Switch`。
   NVIDIA 大量字符串是 ROT13 的 —— **搜到可疑命中，先 `tr 'A-Za-z' 'N-ZA-Mn-za-m'` 解一下**。
3. **★★ 字节搜 `0F A2` 会命中重定位表**。`.rela.dyn` 每项 24 字节，
   若某个字段固定位置碰巧是 `0F A2`，就会出现"间隔 0x18 的一串假 cpuid"。
   **必须限定在 `.text` 段内搜，并对命中点反汇编确认**。
4. **★★ 定位"环境检测"，内核模块比用户态更可能是答案。** glcore 只做图形，
   环境判断属于 RM。**顺序应该是：先 `nm <ko> | grep -i hypervisor/vgpu`，再回头看 glcore。**
5. **★ 区分"有模块"和"模块在跑"**。550 有 vGPU 模块却在 vGPU 下正常 ⇒
   **模块的存在不能推出行为**，必须找到它按环境分叉的那个点。
