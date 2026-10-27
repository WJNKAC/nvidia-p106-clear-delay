# 97 · N107：kprobe 实验 —— **技术上做不通，而且做完发现假设本身站不住**

日期：2026-10-26 15:25
驱动：525.116.03（物理机直装，内核 6.1.0-50-amd64）
目标（用户指定）：用 kprobe 让 525 在物理机上"以为自己在 KVM 里"，看是否切到快路径

---

## 0. 结论：三层都堵死了，而且第三层推翻了我自己的假设

| 层 | 结果 |
|---|---|
| **① 挂 `_nv027892rm`（真正的检测实现）** | ❌ **内核拒绝**：`trace_kprobe: Could not probe notrace function _nv027892rm` |
| **② 改缓存字段 `[rdi+0x4c]`** | ❌ 需要先拿到 `rdi`，而拿 `rdi` 又必须挂在①上 |
| **③ 假装验证假设** | ❌ **`_nv027892rm` 在全模块里调用者 = 0** ⇒ **这个检测函数根本没被驱动自己调用** |

**第三层是致命的**：即使 kprobe 能挂上，改了也没用 —— **没有任何代码在问它**。

---

## 1. 已完全查清的静态事实（这些是真实收获）

### 1.1 `nv_get_hypervisor_type` 的完整调用链

```asm
; nv_get_hypervisor_type  @ 0xa76e00（导出包装，thunk）
a76e00: endbr64
a76e04: mov  rax,[rip+0x0]        ; 重定位 → _nv022325rm（某个全局）
a76e0b: mov  rdi,[rax+0x218]      ; 取全局上下文对象
a76e12: jmp  _nv027892rm          ; 尾调用（重定位 0xa76e13 → _nv027892rm）

; _nv027892rm  @ 0xcb990（真正的实现）
cb990: endbr64
cb994: mov  eax,0x4               ; 默认 4
cb999: test rdi,rdi
cb99c: je   cb9a1
cb99e: mov  eax,DWORD PTR [rdi+0x4c]   ; ★ 读缓存字段 [ctx + 0x4c]
cb9a1: ret
```

**含义（很明确）**：hypervisor 类型是**缓存在全局对象的 `+0x4c` 字段里**，
`nv_get_hypervisor_type()` 只是"读缓存"，不做检测。检测发生在初始化时。

### 1.2 枚举值（DWARF `DW_AT_const_value`，权威）

| 枚举 | 值 |
|---|---|
| `OS_HYPERVISOR_XEN` | 0（推断，未直接读到）|
| `OS_HYPERVISOR_VMWARE` | 1（推断）|
| **`OS_HYPERVISOR_HYPERV`** | **2** |
| **`OS_HYPERVISOR_KVM`** | **3** |
| **`OS_HYPERVISOR_UNKNOWN`** | **4**（= `_nv027892rm` 的默认返回值）|

**⇒ 当前物理机上，这个字段应该是 4（UNKNOWN）。要让驱动"以为在 KVM 里"，需要把它改成 3。**

### 1.3 检测手段确实是 DMI，但**只在加载时做一次**

`dmi_get_system_info` 可以被 kprobe 挂上（**实测注册成功**），但：

```
事件 n107/dmi2/enable = 1
跑完整个 GL 基准（n83a）后：trace 命中 = 0 行
```

**⇒ 驱动启动后不再查 DMI。** 环境判断在 `nvidia.ko` 初始化阶段就做完了，之后全是读缓存。

**这条解释了为什么"事后改环境"没用**：`modprobe` 那一刻就把结论写进 `ctx+0x4c` 了。

### 1.4 `hypervisor_id` 字段偏移 268（0x10C）—— 与 `+0x4c` **不是同一个字段**

DWARF 里确有 `hypervisor_id` 成员（`DW_AT_data_member_location: 268`），
但它所在的 struct 后缀是 `acpi_gbl_FADT` 附近的调试记录（`decl_file 247 / line 255`），
**与 `_nv027892rm` 读的 `[rdi+0x4c]` 不是同一个**。

⇒ **「改 `hypervisor_id`」这条路是错的**，会改到无关结构体。这一条我差点写错，及时核对了偏移。

---

## 2. 为什么 kprobe 挂不上：`notrace` 标记

```
$ echo 'p:n107/hv _nv027892rm' >> kprobe_events
$ dmesg | tail
trace_kprobe: Could not probe notrace function _nv027892rm
```

逐个测了四个符号：

| 符号 | kprobe | 说明 |
|---|---|---|
| `_nv027892rm` | ❌ `notrace` | 真正的检测实现 |
| `nv_get_hypervisor_type` | ❌ `notrace` | 导出包装 |
| `nv_vgpu_start` | ❌ `notrace` | vGPU 启动 |
| `rm_vgpu_vfio_set_driver_vm` | ❌ `notrace` | vGPU/VFIO 设置 |
| **`os_call_vgpu_vfio`** | ✅ **可挂** | **但调用者 = 0（死路径）** |
| **`dmi_get_system_info`** | ✅ **可挂** | **但运行期不被调用** |

**⇒ NVIDIA 给整个 vGPU / 环境检测函数族都加了 `notrace`。**
`notrace` 是内核的显式标记（`__attribute__((no_instrument_function))` 或 `NOTRACE`），
**目的就是禁止 ftrace/kprobe 追踪** —— NVIDIA 明确不让这些函数被动态插桩。

这不是"我手法不对"，是**厂商主动设的墙**。

---

## 3. 假设本身要重新审（这是本轮最重要的结论）

我上一轮（报告 96）提出的假设是：

> "525+ 引入 vGPU 提交支持 ⇒ 物理机走非 hypervisor 分支 ⇒ 慢的是这条分支"

**本轮的三条证据削弱了它：**

| 证据 | 影响 |
|---|---|
| **`_nv027892rm` 调用者 = 0** | **驱动自己不查 hypervisor 类型** ⇒ "按 hypervisor 分支"这个模型**缺乏调用点支撑** |
| `dmi_get_system_info` 运行期 0 命中 | 环境判断只在加载时，且**结论存在 `ctx+0x4c`，但没人去读它** |
| `os_call_vgpu_vfio` 调用者 = 0 | vGPU/VFIO 路径在**标准驱动里是死代码**（只在真正的 vGPU 驱动里被接上）|

**⇒ 更可能的解释是：**
- `nv_vgpu_*` / `rm_vgpu_vfio_set_driver_vm` 是**为 vGPU 驱动版本共用的一份代码**，
  在标准驱动里**从未被调用**（符号存在、代码在、但是死的）。
- 那么 **515→525 的 `LOGVGPU` 新增，只是"把 vGPU 代码打进标准驱动"**，
  **与物理机变慢没有因果关系**。

**⇒ 我上一轮那条"版本分界 = 病因"的推论，很可能也是错的。**
时间点重合（515 快 / 525+ 慢，且 vGPU 代码 525 才进）**只是相关，不是因果**。

---

## 4. 那么还剩什么（诚实清单）

**已被彻底排除的（累计，均为实测）**：

功耗墙、GSP、ASPM、PCIe 带宽、BAR memtype、拓扑、合成器/vsync、GL 前端、Vulkan 前端、
内核提交次数、调度延迟、`glMapBuffer` 内部、`wait_ms` 轮询、flags gate、固定延迟、
20/40ms 硬编码超时、vGPU 提交路径被调用。

**仍然成立的观测**：

1. **现象**：本机 MSI + 525，GPU 在"每批次"处空转（37~64W），`每对 121µs`。**多次实测，可信。**
2. **对照**：同机 515 是 0.03µs/对（4000 倍）。**可信。**
3. **另一对照**：同一台机、2 年前 PVE + 550 + vGPU 破解，130W 正常。**来自截图，可信。**
4. **580 的 GL 仍慢、Vulkan 快**。**可信（报告 07 + 报告 93）。**

**未解释的**：**为什么本机物理机 + 525 会慢，而 PVE + 550 不慢。**

**这条"为什么"现在是唯一的开放问题，而它需要的是"环境差异"的直接证据，不是驱动内部代码。**

---

## 5. 下一步（方向要改）

我原来的静态路线（在驱动里找分支）**已经走到尽头**：函数是 `notrace`、调用者是 0、
检测只在加载时做。**继续在驱动里挖，收益极低。**

| 编号 | 做什么 | 为什么现在该做这个 |
|---|---|---|
| **N107-A（★ 推荐，零成本，只需你回答）** | **说明 2 年前那套 PVE 的具体配置**：<br>① P106 是 **VFIO 直通** 还是 **vGPU 破解**？<br>② 用了 `vgpu_unlock` 吗？guest 里跑的是 `nvidia-vgpu` 驱动还是标准驱动？<br>③ guest 里**有没有第二个虚拟显卡**（QXL / virtio-gpu / SPICE）？<br>④ PVE 版本、内核版本？ | **这是唯一已知"正常"的参照系。** 我一直在猜它的配置，猜错方向就全错。你的截图 `Compute M: N/A` 与物理机 `Default` 不同，这条必须你确认 |
| **N107-B（零成本）** | 若①是 VFIO 直通：**在 QEMU 里复现一次**（VFIO + P106），跑 `n83a -M 3 -S` | 若复现也快 ⇒ **"在 VM 里"这个状态本身改变了行为**，与 DMI 无关；若复现也慢 ⇒ 差异在 PVE 的其他配置（内核/CPU pinning/中断）|
| **N107-C（零成本）** | 反过来：**在这台物理机上装 PVE**（装回 2 年前的环境），验证是否恢复 | 最贵但最确定。**既然 2 年前那套是正常的，它就是已知可用配置** |
| ~~N106-C-prime~~ | kprobe 改返回值 | **已证不可行**（`notrace` + 零调用者）|
| ~~N106-B~~ | 找 DMI 字段判断 | 已查清：**只在加载时判断，运行期不查** |

---

## 6. 产物

```
analysis/probes/n107_kprobe.sh   # kprobe 实验脚本（保留，记录了 notrace 事实）
```

复现关键结论：
```bash
# 1) notrace 墙
echo 'p:t/x _nv027892rm' >> /sys/kernel/tracing/kprobe_events
dmesg | tail -1     # → trace_kprobe: Could not probe notrace function _nv027892rm

# 2) 零调用者
objdump -d /lib/modules/$(uname -r)/kernel/drivers/video/nvidia.ko | grep -c 'call.*<_nv027892rm>'
# → 0

# 3) dmi 只在加载时
#   挂上 dmi_get_system_info，跑 GL 基准，命中 = 0
```

---

## 7. 教训（已入技能库）

1. **★★★ 做 kprobe 之前先查 `notrace`**。`dmesg` 会明确报
   `Could not probe notrace function <name>` —— 这是厂商主动禁止插桩，不是配置问题。
   **闭源驱动（NVIDIA/AMD）对同步、环境检测、许可相关函数普遍加 `notrace`。**
   判断顺序：`kprobe_events` 试注册 → 失败立即看 `dmesg` → 不要反复换写法。
2. **★★★ 改一个"检测函数"之前，先数它的调用者。**
   `objdump -d <ko> | grep -c "call.*<func>"` 一条命令。
   本例结果 **0** —— 如果先做这一步，整个 kprobe 实验就不必开始了。
   **`nm` 里有符号 ≠ 有调用者。**（导出符号更可能只给外部模块用。）
3. **★★ "只在初始化时做一次"的检测，事后 hook 无效。**
   判据：挂上入口探针，**跑完整负载后命中 0** ⇒ 它不在热路径上。
   这类检测的结论被缓存进某个字段，此时只能改字段，不能改函数。
4. **★★ 时间点重合 ≠ 因果。** 515→525 同时发生了"GL 变慢"和"vGPU 代码被编入"，
   但本轮证明 vGPU 代码**零调用** ⇒ 两者只是相关。
   **要证因果，必须证明"那段代码在慢的路径上被执行"**，而不是"它和变慢同时出现"。
5. **★★ 静态挖到"函数存在、代码完整、但没人调用"时，就该停下来换方向。**
   本轮在驱动内部花了大量时间，最后发现关键函数是死代码。
   **判据：一旦发现"符号存在但调用者为 0"，就说明这条线只能解释"代码被编进来了"，
   不能解释"行为变了"。**
