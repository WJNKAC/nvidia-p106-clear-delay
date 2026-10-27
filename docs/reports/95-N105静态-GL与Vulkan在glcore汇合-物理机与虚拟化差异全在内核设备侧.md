# 95 · N105 静态：GL 与 Vulkan 在 glcore 汇合；物理机 vs 虚拟化 的差异全在内核/设备侧

日期：2026-10-26 14:50
驱动：525.116.03（物理机直装，`systemd-detect-virt = none`）
目的：用户要求 —— **先静态看清 vGPU 与物理机在 VK/GL 链路上的区别**

---

## 0. 一句话

**用户态没有区别，一点都没有。** GL 和 Vulkan 两条链路**在 `libnvidia-glcore` 就汇合了**
（Vulkan 的 ICD 就是 `libGLX_nvidia.so.0`，而它 `NEEDED libnvidia-glcore`），
之后共用同一个 `nvidia.ko`。⇒ **换 API 不可能绕开任何东西**（这从库依赖层面钉死了报告 93 的实测）。

**所以"物理机 vs 虚拟化"的差异只可能出现在内核与设备侧**：中断投递、BAR/DMA 的页表属性、
PCIe 配置空间暴露给 guest 的样子。下面逐项列出，并标出本轮已排除 / 待查的。

---

## 1. GL 链路（物理机，静态实证）

```
应用
 └─ libGL.so.1.7.0              ← glvnd 厂商无关层
     └─ libGLdispatch.so.0.0.0  ← 按 vendor 分发（应用拿到的指针=这里的 stub）
         └─ libGLX_nvidia.so.525.116.03
             ├─ readelf -d: NEEDED libnvidia-glcore.so.525.116.03   ★
             ├─ nm -D -u: U _nv000glcore ... _nv026glcore（16 个）  ★
             └─ libnvidia-glcore.so.525.116.03
                 ├─ 统一命令提交器  +0x1238920（sfence@+0xa3, wait_ms@+0x1b1）
                 └─ ioctl /dev/nvidiactl, /dev/nvidia0
                     └─ nvidia.ko  →  PCIe →  GPU
```

**关键点（报告 71/75 已定）**：静态导出符号有 2673 个，但驱动实际服务的是
`libGLX_nvidia` 的内部 thunk（`jmp [表+0xf10]` 等），**应用拿到的指针只是 glvnd stub**。

---

## 2. Vulkan 链路（物理机，静态实证）

```
应用
 └─ libvulkan.so.1 (loader)
     └─ /etc/vulkan/icd.d/nvidia_icd.json
         { "library_path": "libGLX_nvidia.so.0", "api_version": "1.3.224" }   ★
         └─ libGLX_nvidia.so.525.116.03
             ├─ 导出 vk_icdGetInstanceProcAddr / vk_icdGetPhysicalDeviceProcAddr
             │        / vk_icdNegotiateLoaderICDInterfaceVersion
             ├─ 字符串里含 vkCreateDevice / vkGetDeviceQueue / vkQueueSubmit
             │        / vkQueuePresentKHR / vkAcquireNextImageKHR / vkGetDeviceProcAddr
             ├─ readelf -d: NEEDED libnvidia-glcore.so.525.116.03   ★
             └─ libnvidia-glcore.so.525.116.03   ← 和 GL 是同一个文件
                 └─ ioctl /dev/nvidia* → nvidia.ko → PCIe → GPU
```

### 2.1 这就是"Vulkan 也慢"的静态解释

```
nm -D --defined-only libGLX_nvidia.so.525.116.03 | grep " T vk_"
  vk_icdGetInstanceProcAddr
  vk_icdGetPhysicalDeviceProcAddr
  vk_icdNegotiateLoaderICDInterfaceVersion

readelf -d libGLX_nvidia.so.525.116.03 | grep NEEDED
  libnvidia-glsi.so.525.116.03
  libnvidia-tls.so.525.116.03
  libnvidia-glcore.so.525.116.03     ★★
  libX11 / libXext / libc / libdl
```

**Vulkan ICD == GL 用的同一个 `libGLX_nvidia`；两者都 NEEDED 同一个 `libnvidia-glcore`。**
⇒ 报告 93 实测的"Vulkan 也只 64fps"**不是巧合**，是库结构决定的必然。

**⇒ 任何"换个 API/换个后端试试"的想法，在这张图上就可以否掉，不用上机。**

---

## 3. 虚拟化（VFIO 直通 / vGPU）下的链路差异

**用户态完全相同**（同样的 NVIDIA 驱动包，同样的 `.so`，同样的 ICD json）。
差异全部在下面这一层：

| # | 环节 | 物理机（本机） | VFIO 直通 / vGPU | 对本问题的意义 |
|---|---|---|---|---|
| 1 | `nvidia.ko` 绑定的设备 | 物理 `0000:01:00.0` | QEMU 造的虚拟 PCI 设备（config space 直通） | 配置空间里的能力字段可能被改写 |
| 2 | **PCIe 链路能力** | 卡自报 `LnkCap: 2.5GT/s`（Gen1），上游 root port `8GT/s` + `Target Link Speed: 8GT/s`，**实际跑 Gen1** | **QEMU 的 root port 通常报 Gen3 x16** ⇒ **guest 内驱动以为自己是 Gen3** | ⚠️ 见 §4.1 |
| 3 | **MSI 中断投递** | 物理 MSI → APIC 直接投递 | guest MSI → VM exit → QEMU → KVM `irqfd` → host MSI → APIC | ⚠️ 见 §4.2 |
| 4 | BAR 访问 | 直接 MMIO | 经 EPT 二级页表转换 | 多一层 walk，但命中 TLB 后≈0 |
| 5 | DMA | IOMMU 可选（本机无 DMAR） | IOMMU **强制**（VFIO 依赖） | 每次 DMA 多一次翻译 |
| 6 | 中断亲和 | 单向量 IRQ 36 全压 **CPU 3** | guest 内 vCPU 可分散，QEMU 侧可绑核 | 单核串行点 |

---

## 4. 本轮静态查出的两个异常

### 4.1 PCIe 链路被压在 Gen1，而上游明明支持 Gen3

```
GPU 01:00.0      LnkCap:  Port #0, Speed 2.5GT/s, Width x16, ASPM L0s L1
                 LnkCtl:  ASPM Disabled
                 LnkSta:  Speed 2.5GT/s, Width x16

Root port 00:01.0 LnkCap:  Port #2, Speed 8GT/s, Width x16
                  LnkCtl2: Target Link Speed: 8GT/s          ← 上游想跑 Gen3
                  LnkCap2: Supported Link Speeds: 2.5-8GT/s
                  LnkSta:  Speed 2.5GT/s, Width x16           ← 实际只有 Gen1
```

**GPU 自身的 `LnkCap` 就只报 2.5GT/s。** 而 P106-100 是 GP106，硬件本应支持 PCIe 3.0 x16。

两种可能：
- **VBIOS 被改过**（矿卡常见，报告 02 已经提取过 `86.06.58.00.21`，但功耗表区非明文）
- **BIOS 把该插槽限制在 Gen1**（或链路训练失败后锁定在 Gen1）

**影响范围**：只影响**斜率项**（8.29MiB 回读 ⇒ 2.6ms，等效 0.75 GB/s ≈ Gen1 线速的 23%）。
**不影响固定项**：Vulkan `clear` 几乎不回读，仍是 15.6ms（报告 93）。**所以这不是主因。**

**但对 vGPU 有意义**：QEMU 的 root port 报 Gen3，guest 内驱动会按 Gen3 做内部时序假设，
而实际物理链路只有 Gen1 ⇒ **驱动与硬件对"一次回传要多久"的预期不一致**。
这可以解释"行为不同"，但**不能直接解释"虚拟化更快"**。

### 4.2 只有单个 MSI 向量，没有 MSI-X，且全压在一个 CPU 核

```
lspci -vv -s 01:00.0:
  Capabilities: [68] MSI: Enable+ Count=1/1 Maskable- 64bit+
  （整份能力列表里没有 MSI-X capability）
  Interrupt: pin A routed to IRQ 36

/proc/interrupts:
  36:  0 0 0 16949 0 0 0 0   IR-PCI-MSI 524288-edge  nvidia     ← 16949 次全在 CPU 3
```

**这块 P106 只有 1 个 MSI 向量**（低端/矿卡常见简化）。所有完成通知共用这一条线、一个核。

**这为什么可能让虚拟化反而更快**：VFIO 下 guest 的 MSI 由 QEMU 接管，
QEMU 可以把多条 eventfd 分散到不同 host 核，且 **irqfd 可以避开某些物理机的 MSI 路由限制**。
**这是目前唯一一个"虚拟化可能更优"的静态候选**，但**未验证**。

---

## 5. 已排除（本轮静态，均为实测值）

| 候选 | 实测 | 判读 |
|---|---|---|
| **ASPM L1 唤醒延迟** | GPU 与 root port **两端都 `ASPM Disabled`** | ❌ 排除。L1 退出延迟（`<16us`/`<8us`）本是最像"每批次 124µs"的候选 |
| **BAR0 映射成 UC 导致门铃慢** | PAT: `[f6000000-f7000000] uncached-minus`（寄存器区，**正常**）；门铃页 `e0080000/e0090000/e00a0000/e00b0000` 均为 **write-combining**（**正常**） | ❌ 排除 |
| **BAR 未映射到用户态** | 进程 maps 里无 `e0000000`/`f6000000` 段 ⇒ 驱动走 ioctl `remap_pfn_range` 给匿名 vma，非直接 mmap 物理 BAR | 说明：查 maps 找不到 BAR 段是**正常的**，不代表没映射 |
| 功耗墙 / vBIOS TDP | `power.limit=120W`（min60/max140），`SwPowerCap` 未激活 | ❌ 排除 |
| GSP 固件 | `GSP Firmware Version: N/A` | ❌ 排除 |
| 拓扑（挂在 PCH 后） | `[0000:00]-+-01.0-[01]----00.0` = **CPU 直连 PEG** | ❌ 排除 |
| 内核里有 VFIO 在跑 | `lsmod`: 只有 `kvm/kvm_intel/irqbypass`，**无 vfio** | 本机现在是纯物理机 |

---

## 6. 必须问清的一个歧义（影响后续所有静态分析的方向）

截图里 **`Compute M: N/A`**，而本机物理机是 **`Compute M: Default`**。

`Compute M: N/A` 在 NVIDIA 语境里通常意味着 **不是普通物理 GPU 直连**（vGPU / 虚拟化 / 某些特殊 SKU）。

但 **P106-100 官方不支持 NVIDIA vGPU**（vGPU 需要 Tesla/GRID 系列 + license）。
社区有 `vgpu_unlock` 之类的破解，可以让 GeForce/矿卡跑 vGPU。

所以"2 年前 PVE 上那套"到底是：

| 可能 | guest 内看到的设备 | `Compute M` 预期 | 静态分析方向 |
|---|---|---|---|
| **A. VFIO 直通** | 真实 P106（config space 直通） | 应显示 `Default`（和物理机一样） | 差异只在 §3 表里的 2/3/4/5 |
| **B. 破解 vGPU** | NVIDIA vGPU 虚拟设备 | **`N/A`**（与截图一致） | 渲染后端在 host 的 vGPU manager，**guest 驱动是精简版**，用户态库不同 |

**如果是 B，"用户态没有区别"这个结论就不成立了** —— vGPU guest 里跑的是 `nvidia-vgpu` 驱动，
库路径可能不同（有 `libnvidia-vgpu.so` 之类）。

**这条必须你确认**，否则后面的静态对比会跑错方向。

---

## 7. 下一步（静态侧还能做什么）

| 编号 | 做什么 | 成本 |
|---|---|---|
| **N105-A** | **对比 `550.163.01`（本机曾装）与 `525.116.03` 的 `nvidia.ko` + `glcore` 在"中断/同步/PCIe"相关字符串与符号上的差异** —— 若能看出 550 走了不同的完成通知机制，就直接对上 | 零成本（两个版本的 .run 都在 `drivers/`） |
| **N105-B** | 查本机 BIOS 里 PCIe 相关设置（Above 4G Decoding、PCIe Max Link Speed、SR-IOV）。Gen1 是否被 BIOS 锁 | 需重启进 BIOS |
| **N105-C** | 若你确认是 vGPU（B），则对比 `nvidia-vgpu` 驱动与普通驱动的用户态库清单 | 零成本 |

**建议先答 §6 那个问题。** 它是岔路口：VFIO 和 vGPU 的静态对比内容完全不同。

---

## 8. 教训（已入技能库）

1. **★★ 静态查"两个 API 有没有共用底层"，只需看三条**：
   `readelf -d <ICD 库> | grep NEEDED`、`nm -D -u <ICD 库> | grep glcore`、ICD json 的 `library_path`。
   **本例三条全部指向同一个 `libnvidia-glcore`** —— 一分钱不花就能否掉"换 API"这条路。
   **在花时间做 A/B 实测之前，先花两分钟看库依赖。**
2. **★ 查 BAR 映射别只看进程 maps**。NVIDIA 驱动把 BAR 页通过 ioctl `remap_pfn_range`
   映射到**匿名 vma**，maps 里看不到 `e0000000` 段是正常的。
   要查真实属性得看 `/sys/kernel/debug/x86/pat_memtype_list`。
3. **★ `LnkCap` vs `LnkSta` vs 上游 `LnkCtl2` 要三个一起看**：
   本例 GPU 自报 Gen1、上游 `Target=Gen3`、实际 Gen1 ⇒ **链路被限制**，仅看 `LnkSta` 会漏掉这个矛盾。
4. **★ 单 MSI 向量是低端卡的硬件属性，不是配置错误**。它会在"每批次都要等完成通知"的场景里
   形成串行点 —— 这可能正是"虚拟化反而更快"的候选解释（irqfd 可绕开）。
