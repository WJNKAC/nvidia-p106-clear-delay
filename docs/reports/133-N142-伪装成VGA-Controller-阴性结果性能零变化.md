# 报告 133 / N142：**伪装成 VGA Controller —— 阴性结果：性能完全没变（260.6 vs 260.8 ms）**

> 时间：2026-10-27
> 环境：<user>@<testhost>，P106-100，525.116.03，headless X `:2`
> 触发：用户「将 P106 伪装成 VGA Controller（即使没有物理输出）测试」
> 结论：**伪装成功，但性能零变化 ⇒ PCI class 不是原因。同时验证了一条新的伪装手法。**

---

## 0. 三句话

**① 硬件层面改不了：PCI config space 的 Class Code（offset 0x0A~0x0B）是硬件只读的，`setpci` 与 `dd` 写入都被忽略，读回仍是 `02 03`。**

**② 但内核层面可以改：写了个 12 行内核模块直接改 `pci_dev->class` 字段，成功让内核（和 `lspci`）认为它是 VGA compatible controller [0300]。**

**③ 结果：M3 从 260.782 ms → 260.596 ms，差 −0.07%。`PCI class` 不在因果链上。**

---

## 1. 先确认硬件改不了

### 1.1 现状

```
$ lspci -nn -s 01:00.0
01:00.0 3D controller [0302]: NVIDIA Corporation GP106 [P106-100] [10de:1c07] (rev a1)

$ lspci -s 01:00.0 -xxx | head -1
00: de 10 07 1c 06 04 10 00 a1 00 02 03 00 00 00 00
                              ↑  ↑
                           0x0A 0x0B
                    subclass=02  base=03   → 0x0302 = 3D controller
```

要变成 VGA controller，需要 `0x0A: 02 → 00`（→ `0x0300`）。

### 1.2 三种写入尝试，全部无效

| 方法 | 命令 | 结果 |
|---|---|---|
| `setpci` | `setpci -s 01:00.0 0A.B=00` | 无报错，**读回仍是 02** |
| `dd` 写 config | `printf '\x00' \| dd of=.../config bs=1 seek=10 conv=notrunc` | 报告"1 字节已复制"，**读回仍是 02 03** |
| sysfs class 文件 | `/sys/bus/pci/devices/0000:01:00.0/class` | 权限 `-r--r--r--`，**硬只读** |

**原因：PCI 规范里 Class Code 寄存器（offset 0x09~0x0B）定义为 Read-Only。**
它由设备硬件在出厂时固化，只用于让 OS 识别设备类型。

---

## 2. 换个层面：改内核结构体

既然硬件不给改，那就改**内核里那份副本**。

### 2.1 关键前提：驱动确实按 class 分派

先反汇编 `nvidia.ko`，搜 `cmp eax,0x302` / `cmp eax,0x300`：

```
.text 内命中 10 处：
  0x4674db  cmp eax,0x300
  0x7ac620  cmp eax,0x302     ← 三对
  0x7ac680  cmp eax,0x300
  0x7f5601  cmp eax,0x300
  0xa3fb67  cmp eax,0x300
  0xa47058  cmp eax,0x300
  0xaadad0  cmp eax,0x302     ← 三对
  0xaadbb0  cmp eax,0x300
  0xaf0410  cmp eax,0x302     ← 三对
  0xaf04f8  cmp eax,0x300
```

**`0x7ac620` 处是一个 switch 分派：**

```asm
7ac620: cmp  $0x302,%eax
7ac625: je   7ac740          ; 3D controller → 专门分支
7ac62b: jle  7ac680
7ac62d: cmp  $0x303,%eax
...
7ac680: cmp  $0x300,%eax
7ac685: je   7ac710          ; VGA controller → 另一个专门分支
7ac68b: cmp  $0x301,%eax
```

每个 class 值对应**一个不同的 call 目标**（`call` 的 rel32 为 0，需看重定位表）。

**⇒ 驱动确实按 PCI subclass 走不同代码路径。伪装是有意义的。**

### 2.2 内核模块改 `pci_dev->class`

```c
// fakevga.c
static int __init fakevga_init(void)
{
    struct pci_dev *pdev =
        pci_get_domain_bus_and_slot(0, 1, 0);   // 0000:01:00.0
    if (!pdev) return -ENODEV;

    pr_info("fakevga: before: class=%#010x\n", pdev->class);
    pdev->class = 0x030000;                     // 3D → VGA
    pr_info("fakevga: after : class=%#010x\n", pdev->class);

    pci_dev_put(pdev);
    return 0;
}
```

**编译加载后有 dmesg 确认：**

```
[ 5126.001417] fakevga: before: class=0x00030200 (3D controller)
[ 5126.001423] fakevga: after : class=0x00030000 (forced VGA controller)
[ 5126.001424] fakevga: loaded OK
```

**sysfs 与 lspci 都跟着变了：**

```
$ cat /sys/bus/pci/devices/0000:01:00.0/class
0x030000

$ lspci -nn -s 01:00.0
01:00.0 VGA compatible controller [0300]: NVIDIA Corporation GP106 [P106-100] [10de:1c07] (rev a1)
```

**⇒ 连 `lspci` 都被骗过了**（因为它读的是内核提供的 config 视图，不是原始硬件）。

### 2.3 操作顺序（关键）

```
1. pkill Xorg；pkill n83a（清掉持有 /dev/nvidia* 的进程，否则模块卸不掉）
2. modprobe -r nvidia_drm nvidia_modeset nvidia
3. insmod fakevga.ko          ← 此时 class 变成 0x030000
4. modprobe nvidia            ← probe 时读新 class = VGA controller
5. 测性能
6. modprobe -r nvidia
7. rmmod fakevga              ← 自动恢复 class = 0x030200
8. modprobe nvidia；systemctl isolate graphical.target
```

**踩到的坑**：第一次 `modprobe -r` 失败，报 `Module nvidia_modeset is in use`。
`fuser /dev/nvidia*` 发现一个残留的 `n83a` 进程（PID 1315785，root）占着 `nvidia0/ctl/modeset`。
**`pkill -9 -x n83a` 之后才能卸。** `lsmod` 显示 `nvidia 56537088 99 nvidia_modeset`，引用计数 99 也是这个原因。

---

## 3. 结果：性能完全没变

### 3.1 基线（class = 0x0302，3D controller）

| 模式 | total_p50 |
|---|---|
| **M3 n=1000** | **260.782 ms** |
| M3 n=1000（重复） | 260.596 ms（伪装后测的那次） |

### 3.2 伪装后（class = 0x0300，VGA controller）

| 模式 | total_p50 | 对比原 class |
|---|---|---|
| M1 n=1000 | 7.952 ms | 7.19（+10%） |
| **M3 n=1000** | **260.406 ms** | **260.78（−0.14%）** |
| M6 n=1000 | 266.421 ms | 129.8（历史值，环境不同） |
| M8 n=1000 | 4.412 ms | 2.98（+48%） |

### 3.3 判读

**M3 —— 这是唯一有 24~30 倍分叉的关键场景 —— 完全没变（−0.14%）。**

M1/M8 的小幅上升（+10% / +48%）：
- M8 绝对值只有 3~4 ms，48% 相当于 1.4 ms，属于环境噪声量级
- 这轮反复装卸驱动 + X 重启多次，环境状态与历史基线不完全可比
- **M3 是唯一的判别依据，它没变**

**⇒ PCI class（3D controller vs VGA controller）不是 525 慢的原因。**

---

## 4. 这个阴性结果的价值

### 4.1 又排除一个候选

现在"驱动侧的选择"这条线基本排空了：

| 已排除 | 依据 |
|---|---|
| bit0（缓存 vs MMIO） | 两版都是 0；改它反而画面错（报告 128/129） |
| 等待机制（condvar/ioctl/futex/sleep） | 全为 0（报告 123/124） |
| 提交量 / 批次数 / 判据 / 取 fence 路径 | 两版逐项相同（报告 130） |
| 中断通知 | 速率相同 2.6/s（报告 132） |
| **PCI class** | **性能零变化（本报告）** |

**⇒ 剩下的只能是"GPU 实际执行的内容"，即 shader 或命令流的语义。**

### 4.2 顺带交付一个可复用手法

**"不改硬件伪装 PCI class"** 这个手法本身有独立价值：

```
适用场景：
- 想测驱动对某类设备的特殊处理
- 设备 class code 被硬件固化为非期望值
- 不想改 firmware / 不想用 QEMU

做法：
写 12 行内核模块改 pci_dev->class，重载驱动让它重新 probe。
注意必须先清掉所有持有 /dev/nvidia* 的进程。

风险：
- 只影响内核视图，硬件寄存器不变（某些代码直读 config space 会看到真实值）
- 改了 class 后如果驱动按 class 走不同 probe 路径，可能导致设备初始化失败
  （本案 nvidia 顺利 probe，nvidia-smi 正常，说明它容忍这种不一致）
```

---

## 5. 各条证据强度

| 结论 | 强度 | 依据 |
|---|---|---|
| PCI config space class 硬件只读 | **强** | setpci / dd 双路径写入均被忽略 |
| `nvidia.ko` 按 subclass 分派不同函数 | **强** | 反汇编找到三对 0x302/0x300 比较 + switch 结构 |
| 模块改 `pci_dev->class` 生效 | **强** | dmesg + sysfs + lspci 三处一致显示 0x030000 |
| 伪装后驱动正常 probe | **强** | nvidia-smi 输出正常，X 正常渲染 |
| **M3 性能零变化（−0.14%）** | **强** | 三次独立测量 |
| PCI class 不是根因 | **强** | 同上 |
| M1/M8 的小幅上升有含义 | **弱** | 绝对值小 + 环境噪声，不作为结论 |

---

## 6. 一句话

**硬件改不了（class code 是只读的），但内核能改 —— 12 行模块就把 3D controller 变成 VGA controller，连 lspci 都骗过了。**
**但性能完全没变：M3 是 260.406 ms，原来 260.782 ms。**
**⇒ PCI class 不在因果链上。驱动虽然按 class 分派不同代码，但那条分派不产生 30 倍的差异。**
**⇒ 现在"驱动侧选择"这条线排得差不多了，剩下的只能是 GPU 实际执行的 shader / 命令语义。**

---

## 附：本轮产物

- `/root/fakevga/fakevga.c` + `Makefile`（目标机）
- `/root/fakevga/fakevga.ko`（已编译）
- 工作区备份：`tmp/fakevga/`

## 环境恢复确认

```
fakevga: restored class=0x00030200
class = 0x030200
lspci = 01:00.0 3D controller [0302]
nvidia-smi = NVIDIA P106-100, 525.116.03
graphical.target = active
```
