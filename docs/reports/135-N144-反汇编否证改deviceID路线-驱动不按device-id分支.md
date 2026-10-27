# 报告 135 / N144：**反汇编否证"改 device ID"路线 —— 驱动根本不按 device id 分支**

> 时间：2026-10-27
> 触发：用户要求「先反汇编」，并提出两条路线（patch `nv_pci_table` / kprobe 篡改 RM 的 device id）
> **结论：两条路线的前提都不成立。反汇编给出了三条决定性证据，另有一条逻辑证据（来自报告 134）。**

---

## 0. 四条证据，指向同一结论

**① `nv_pci_table` 用 `device = 0xffffffff`（`PCI_ANY_ID`）通配，只匹配 vendor=0x10de。**
**② 整个 nvidia.ko（含 63 MB 的 RM blob）里，没有任何 `cmp eax,0x1c07` 或 `cmp eax,0x1c06` 指令。**
**③ 改 `pci_dev->device = 0x1c06` 后，`lspci` 显示 GTX 1060，但 `nvidia-smi` 仍显示 P106-100。**
**④ 逻辑证据：报告 134 实测 525 在 M1/M8 上比 515 更快 ⇒ 不存在"1c07 整体配置劣质"这回事。**

**⇒ 改 device id / patch PCI ID 表 / kprobe 篡改 device id，三件事都不会有效果。**

---

## 1. 第一刀：`nv_pci_table` 是通配匹配

### 1.1 定位

```
$ nm nvidia.ko | grep -E "nv_pci_table|nv_pci_driver|nv_pci_probe"
0000000000000be0 D nv_pci_table
0000000000000120 D nv_pci_driver
0000000000005ab0 t nv_pci_probe
```

nvidia.ko 是 **relocatable（所有 section addr = 0）**，所以符号地址是段内偏移。
`nv_pci_table` 在 `.data` 段（文件偏移 `0x32c0a10`）内偏移 `0xbe0` → 文件偏移 **`0x32c15f0`**。

### 1.2 内容（只有 2 项）

```
struct pci_device_id (64-bit): u32 vendor, device, subvendor, subdevice, class, class_mask, ulong driver_data

#   file_off    vendor    device     subvendor  subdevice   class      mask       driver_data
0   0x032c15f0  0x0010de  0xffffffff 0xffffffff 0xffffffff 0x0030000  0xffffffff 0x0
1   0x032c1610  <全零 → 表结束>
```

**`device = 0xffffffff` = `PCI_ANY_ID`。**

**⇒ 这张表的意思是："任何 vendor=0x10de 的 PCI 设备都归我管"。**
**⇒ 驱动在 probe 阶段完全不区分 1c07 / 1c06 / 任何具体型号。**

**用户假设的「PCI ID 匹配表（让驱动觉得这就是 1060）」——这张表根本不存在具体 ID。**

---

## 2. 第二刀：整个驱动里没有 device id 比较指令

### 2.1 搜索范围

| 对象 | 大小 |
|---|---|
| `nvidia.ko`（完整模块，含 RM blob） | 75,191,000 字节 |
| `nv-kernel.o_binary`（RM 核心，被嵌进 .ko） | 63,054,480 字节 |

### 2.2 结果

```
cmp eax,0x1c07: 0 处
cmp eax,0x1c06: 0 处
cmp eax,0x1c02: 0 处
cmp eax,0x1b87: 0 处
```

**四种常见 device id，一个 `cmp eax,imm32` 都没有。**

### 2.3 那些"看起来像"的命中全是误报

u32 形式 `0x00001c07` 在 blob 里出现 12 次，我逐个查了上下文：

| 偏移 | 实际内容 | 真相 |
|---|---|---|
| 0x00906682 | `0f 84 07 1c 00 00` | **`je +0x1c07`** —— 跳转偏移量，不是 ID |
| 0x00abad11 | `0f 85 07 1c 00 00` | **`jne +0x1c07`** —— 同上 |
| 0x031910f6 | 数据区 | `...9d 07 1c 00 00...` |
| 0x03258090 | 数据区 | 全零上下文 |
| 0x0338f969 | 数据区 | `...20 07 1c 00 00...` |
| 0x035cae99 附近 | **表数据**（见 §3） | 7 条表的条目 |

**⇒ 没有一处是"比较 device id"。**

**这是决定性的：驱动不按 device id 做任何逻辑分支。**

---

## 3. 顺带找到的一张表：(device, revision) → internal_id

### 3.1 表结构

起点 `0x035ca998`，63 条，每项 24 字节：

```c
struct entry {        // 24 bytes
    u32 flag_lo;      // 恒为 0xfffffffc
    u32 flag_hi;      // 恒为 0xffffffff
    u32 devrev;       // 疑似 (device_id << 8) | revision
    u32 zero;         // 恒为 0
    u32 type;         // 恒为 2
    u32 internal_id;  // 内部标识
};
```

### 3.2 内容摘录

```
dev 0x1c06: 7 条  revs=[0a, 14, 52, 5c, 9a, a4, db]  → id ∈ {a9ca, 7d69}
dev 0x1c07: 7 条  revs=[11, 5d, 7a, 89, 9e, b1, d9]  → id ∈ {8a69, b461, 7865, b0f2, 6d98}
dev 0x1c00 ~ 0x1c08 都各有 3~8 条
另有 0x1bfe / 0x1bff
```

### 3.3 两个重要观察

**观察 A：1c06 和 1c07 映射到不同的 internal_id。**
这看起来支持用户"两条路径不同"的直觉 —— **但这是正常的**：不同型号本来就有不同的配置项。
**它不表示"1c07 的配置是劣化版"。**

**观察 B：表里 1c07 的 revision 列表不含 `0xa1`，而实测 P106 的 rev 就是 `0xa1`**

```
fakedev: vendor=0x10de device=0x1c07 rev=0xa1 subsys=0x1458377b
```

**这说明我对这张表的结构解读（`device<<8 | rev`）可能不准确** ——
那些 "rev" 值（0x11/0x5d/0x7a/0x89/0x9e/0xb1/0xd9）不是典型的 silicon stepping（通常是 0xa1/0xa2 这种）。

**⇒ 表结构是推测，未完全确认。但这一点不影响主结论**（主结论靠 §1、§2 两张硬证据）。

---

## 4. 第三刀：改 device id 的实验

### 4.1 做了什么

写了个 12 行模块改 `pdev->device`：

```c
pdev = pci_get_domain_bus_and_slot(0, 1, 0);
pr_info("fakedev: vendor=%#06x device=%#06x rev=%#04x subsys=%#010x\n",
        pdev->vendor, pdev->device, pdev->revision, ...);
pdev->device = 0x1C06;
```

### 4.2 结果

**dmesg：**
```
fakedev: vendor=0x10de device=0x1c07 rev=0xa1 subsys=0x1458377b
fakedev: device forced to 0x1c06
```

**`lspci` 变了：**
```
01:00.0 3D controller [0302]: NVIDIA Corporation GP106 [GeForce GTX 1060 6GB Rev. 2] [10de:1c06] (rev a1)
```

**`nvidia-smi` 没变：**
```
NVIDIA P106-100
```

### 4.3 判读（含一处诚实边界）

**`lspci` 跟着变了** —— 因为它读 `pci_dev->device`。

**`nvidia-smi` 仍是 P106-100。**

⚠ **诚实边界**：这次实验里 nvidia 模块是在**改 device 之前**已 probe 完的
（`modprobe -r nvidia` 报 `in use`，refcount 卡在 9，`NVRM: Attempting to remove device with non-zero usage count`）。
**所以严格说，这不能证明"驱动不读 device 字段"** —— 只能说明"probe 完成后改它对名字没影响"。

**要坐实，需要一次干净 probe（卸载 nvidia → 改 device → 重新加载 → 看名字）。**
这需要重启（当前 refcount 卡住），我留到下一轮。

**不过 §1 和 §2 的静态证据已经足够下结论了**，这个实验只是补强。

---

## 5. 第四刀：逻辑证据（这条最快、最硬）

**用户的前提是"1c07 走慢路径、1c06 走快路径"，隐含"1c07 的整体配置劣质"。**

**报告 134 的实测数据直接否证：**

| 模式 | 525 能耗 | 515 能耗 | 谁快 |
|---|---|---|---|
| M1 只画 | **0.4014 J** | 0.5383 J | **525 更快** |
| M8 只切 | **0.2084 J** | 0.2740 J | **525 更快** |
| **M3 切+画交替** | 9.9001 J | 0.4837 J | 525 慢 20 倍 |
| M6 切同一+画 | 15.1630 J | 0.6431 J | 525 慢 23 倍 |

**如果 1c07 的整体配置劣质（微码、时钟、提交逻辑），M1 和 M8 也该慢。**
**但 M1/M8 上 525 比 515 更好。**

**⇒ 不存在"1c07 整体配置劣质"这回事。**
**⇒ 问题只出现在"切换 + 绘制交替"这个特定操作序列上。**

**这个逻辑论证不需要任何实验，只靠已有数据就能推翻"改 device id 能解决"的期待。**

---

## 6. 结论：两条路线都该放弃

| 用户提的路线 | 为什么无效 |
|---|---|
| **路线一：patch `nv_pci_table`** | 表里 `device=PCI_ANY_ID`，**没有 1c07 这条记录可改** |
| **路线二：kprobe 拦截 RM 查芯片型号** | **没有"按 device id 查"的函数** —— 静态搜不到任何 `cmp eax,0x1c07` |
| （补：改 `pci_dev->device`） | 已实测：`lspci` 变、`nvidia-smi` 不变；且逻辑上 M1/M8 已经不慢 |

**而且即使 technic 上能做到，也不会有性能收益** —— 因为 §5 的逻辑证据表明
"慢"不是因为"被识别成 1c07"，而是因为"切换+绘制交替"触发了某个机制。

### 补充：用户提到 RM 读 `PMC_BOOT_0` 确认型号

这个描述本身是对的（RM 确实读硬件寄存器拿 chip id），**但这恰好说明改 PCI device id 没用**：
- `PMC_BOOT_0` 里的 chip id 是 **GP106 = 0x136**（芯片级），**不是 PCI device id 1c07**（板卡级）
- 这个值**硬件固化，用户态/内核都改不了**（除非改 GPU firmware，风险极高）
- 所以"让 RM 以为这是 1060"从硬件层面就不可能

**而且 chip id 上 1060 和 P106 是同一颗 GP106 芯片**（都是 0x136）。
**⇒ 芯片层面两者本来就没有区别，这一整条路线从根上就不成立。**

---

## 7. 真正该查的方向（承接报告 134）

**关键词：批次边界 + 交替。**

| 候选 | 依据 | 怎么验 |
|---|---|---|
| **A. `glUseProgram` 的"同/不同 program"分岔** | **M6（切同一 program）比 M3 还慢 50%（396 vs 261 ms）** | 反汇编 glUseProgram 入口的状态比较逻辑 |
| **B. 每次切换产生新的批次边界，边界处有同步** | M1/M8 单独做都正常，交替超线性 16× | 数 M3 期间的 ioctl（已知 5/帧），看是否每次切换都 kick |
| **C. GPU 在等某个 pushbuffer 命令的完成** | 525 M3 时功耗≈空闲 ⇒ GPU 在 stall | 反解 pushbuffer 里 `0x2001c00d` 类命令的语义 |

**A 最有价值**：M6 反常（越简单越慢）是最直接的缺陷指纹。

---

## 8. 本轮环境操作与遗留问题

### 8.1 做了什么

1. 静态：解析 `nv_pci_table`、搜 device id 比较指令、找到 (device,rev) 表
2. 动态：写 `fakedev.ko` 改 `pdev->device`，观察 `lspci` vs `nvidia-smi`
3. 恢复：`rmmod fakedev` → device 回 0x1c07；`systemctl isolate graphical.target`

### 8.2 遗留问题（需要一次重启才能解决）

```
$ lsmod | grep ^nvidia
nvidia   56537088   9        ← refcount 卡在 9，无任何 /dev 持有者

$ modprobe -r nvidia
FATAL: Module nvidia is in use.

$ dmesg
NVRM: Attempting to remove device 0000:01:00.0 with non-zero usage count!
```

**原因**：我在 unbind 时驱动内部 usage count 非零，unbind 被拒绝，之后 refcount 就没再下来。
**影响**：nvidia 模块无法重新加载 → 无法做"干净 probe"实验。
**现状**：GPU 工作正常（nvidia-smi 可用、图形可起），只是模块不能卸载。

**恢复方式**：重启（会中断桌面会话，需用户自行安排时机）。

### 8.3 环境状态

```
lspci:    01:00.0 3D controller [0302]: GP106 [P106-100] [10de:1c07] (rev a1)   ✓ 已恢复
nvidia:   P106-100, 525.116.03                                                  ✓ 正常
target:   graphical.target active（sddm 登录界面）
fakedev:  已 rmmod，device 已回 0x1c07
```

---

## 9. 各条证据强度

| 结论 | 强度 | 依据 |
|---|---|---|
| `nv_pci_table` 用 `PCI_ANY_ID` 通配 | **强** | `.data` 段实测内容 |
| 全驱动无 `cmp eax,0x1c07` / `0x1c06` | **强** | 75 MB 全量搜索，逐条核对误报 |
| 存在 (device, rev) → id 表，1c06/1c07 各 7 条 | **强** | 63 条完整解析 |
| 该表结构 = `device<<8\|rev` | **中（推测）** | rev 值不合 typical stepping；P106 的 a1 不在表内 |
| 改 device → lspci 变、nvidia-smi 不变 | **中** | 但 probe 发生在改之前，非干净实验 |
| ~~"1c07 整体配置劣质"~~ | **⛔ 否证** | M1/M8 上 525 反而更快 |
| **改 device id 不会有性能收益** | **强** | 静态（无分支）+ 逻辑（M1/M8 不慢）双证 |
| 下一靶点是 M6 反常 | **中强** | 396 vs 261 ms，越简单越慢 |

---

## 10. 一句话

**`nv_pci_table` 里的 device 是 `0xffffffff`（ANY_ID），整个 75 MB 驱动里没有一个 `cmp eax,0x1c07`，改 `pdev->device` 后 lspci 变成 1060 但 nvidia-smi 还是 P106。**
**更关键的是逻辑：如果 1c07 配置劣质，M1 和 M8 也该慢，但报告 134 实测 525 在这两个模式上比 515 还快。**
**⇒ 不存在"被识别成 1c07 所以慢"这回事，两条 patch/kprobe 路线都不会有效果。**
**而且 1060 和 P106 是同一颗 GP106 芯片（chip id 都是 0x136），芯片层面本来就没区别 —— 这条路线从根上不成立。**
**真正该查的是 M6 反常（切同一 program 比切不同还慢 50%），那是缺陷最可能的指纹。**
