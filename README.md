# nvidia-p106-clear-delay

> **🤖 AI 参与声明**
> 本项目的分析、实验设计、工具编写与文档整理均有 AI 参与。
> 但**所有偏移地址、字节值、性能数字都来自真机实测**，没有一个是推测填充的。
> 推导过程与踩坑记录（包括几次判断错误和撤回）完整保留在 `docs/reports/` 里，可自行核对。

**清除 NVIDIA 驱动给 P106 矿卡加的「每批次延迟」，帧率提升约 14 倍，恢复到满速。**

---

## Principle (English, brief)

NVIDIA's kernel module ships a **restricted-SKU device table**. P106-100 (`10DE:1C07`)
is listed in it. Once the card is identified as restricted, the user-space GL driver
writes an 8-byte command at **every batch boundary**:

```
0x20010E66  0x000000F0      # write gr3d method 0x3998 = 240
```

The GPU then idles for roughly **122 µs per batch**, costing about **14× frame rate**. At 400 batches per frame that is
~48.8 ms of pure waiting — the GPU shows 100 % utilization but draws only ~60 W.

Two independent fixes both work and do **not** stack:

1. **Kernel side (root cause)** — remove the card's entry from the table; the command is
   never generated at all.
2. **User space (symptom)** — flip the conditional jump that guards the emit
   (`74`/`75` → `eb`, 1–2 bytes); the command is generated but never written.

Rendering output is bit-identical either way (verified by pixel checksum).

---

## 这是什么（中文）

上面那段是英文原理速览，下面开始都用中文。

NVIDIA 从 520 系列驱动起，在 GPU 内核模块里加了一张**受限设备表**，把 P104 / P106 / CMP
这些矿卡的 device id 列了进去。卡一旦被判定为"受限"，用户态 GL 驱动就会在**每次批提交的
边界**往里塞一条 8 字节命令：

```
method 0x3998 = 0xF0 (240)
```

意思是"向 gr3d 的 0x3998 写 240"，GPU 收到就白等约 **122 µs**。

一帧 400 批，就是 400 × 122 µs ≈ **48.8 ms** 的纯等待 —— 所以你会看到
**GPU 占用 100%、功耗却只有 60 W 左右上不去**，帧率只有正常水平的 1/14。

最反直觉的一点：**耗时只跟批次数有关，跟分辨率和像素量无关。**
512×512 的小窗口和 1920×1080 一样慢，因为 GPU 根本没在画，它在等。

**普通 GTX/RTX 卡不在那张表里，所以完全不受影响** ——
这也是为什么网上同类报告很少，只有矿卡用户会遇到。

**515 及更早的驱动里整套机制都不存在**，所以那个版本一切正常。

---

## 本项目做的事情

| | |
|---|---|
| **定位** | 内核侧找到那张「受限设备表」；用户态找到发射那条命令的两处代码 |
| **打补丁** | 内核：把矿卡条目从表里抹掉（10 字节）<br>用户态：把条件跳转改成无条件跳转（1~2 字节） |
| **验证** | 三层验证（磁盘 / 进程内存 / 行为）+ 像素校验确认画面不变 |
| **固化** | 写进文件，驱动每次加载自动生效，无需 `LD_PRELOAD` |

**关键点：这条命令在旧驱动里根本不存在。** 515.105.01 压根没有这套机制 ——
所以这不是"延迟参数调太大"，而是"新增了一条只对矿卡生效的命令"。

---

## 效果（实测）

测试程序 `n83b -M 7 -c 400 -n 400`：每帧 400 个 draw、400 个批次，带回读与像素校验。

| 驱动 | 原版 `map_p50` | 补丁后 `map_p50` | 提升 | 画面校验 `chk` |
|---|---|---|---|---|
| 525.116.03 | 52.276 ms | **3.448 ms** | **15.2×** | 5150（不变） |
| 580.178.04 | 52.195 ms | **3.434 ms** | **15.2×** | 5150（不变） |
| 515.105.01 | ≈3.4 ms | —（不需要补丁） | — | 5150 |

`fps` 从 **18.96 → 259.79**。

**跨内核跨驱动对照**：525 补丁后 3.448 ms（kernel 6.1）与 580 补丁后 3.434 ms（kernel 6.12）
相差 0.4% —— 差了一个大版本内核加一个大版本驱动，补丁后性能几乎完全一致。
**⇒ 这条延迟命令就是全部原因。**

> **更新**：后来发现这条命令的**开关在内核侧**（受限设备白名单）。
> 详见下方「更深一层」一节 —— 只打内核补丁同样能到 3.668 ms，且命令**根本不生成**。

**稳定性**：Heaven 4.0 满负载连续 5 分 28 秒，
GPU 100% 占用 / 117–119 W / 54 °C / 零 Xid / 零错误。

---

## ★ 更深一层：这不是「用户态 bug」，根源在内核的设备白名单

后续实验（报告 174）发现：**同样的性能问题，只打内核补丁也能完全解决，而且更彻底。**

NVIDIA 内核模块 `nv-kernel.o_binary` 里有一张**受限设备表**，P1XX / CMP 系列矿卡
（含 P106-100，device `0x1C07`）列在其中。当内核把该卡判为"受限 SKU"，
用户态 glcore 就会置位一个策略位，从而**每批次发射那条 240 单位的延迟命令**。

把这张表里的矿卡条目抹掉（改成无效 device id）后：

| 配置 | `map_p50` | `fps` | `method 0x3998` |
|---|---|---|---|
| 都无（基线） | 52.202 ms | 18.98 | 40+ 条 |
| **只打内核补丁** | **3.668 ms** | **243.78** | **0 条（根本不生成）** |
| 只打用户态补丁 | 3.434 ms | 259.79 | 0 条 |
| 两个都打 | 3.667 ms | 246.64 | 0 条 |

**⇒ 两个补丁互不叠加。内核补丁是治本，本仓库的用户态补丁是治标（下游拦截）。**

**这同时解释了一个关键现象**：普通 GTX/RTX 卡不在那张表里，
策略位永远不置位，那条延迟命令**对它们根本不生效** ——
所以这是一个**针对矿卡的定向限制**，而不是普遍的性能回归。

> 内核方案的做法与出处：参见报告 174，以及 <https://github.com/dartraiden/NVIDIA-patcher/issues/3>。
> **内核补丁改动更大、影响面更广（改设备身份），本仓库仍以用户态补丁为主**，
> 因为它只需要 1~2 个字节，且能用像素校验值 `chk` 证明画面未变。

---

## 目录结构

```
.
├── README.md                  ← 本文件
├── DISCLAIMER.md              免责与法律说明（务必先读）
├── docs/
│   ├── INDEX.md               研究报告索引（175 份，按主题分类）
│   └── reports/               脱敏后的完整研究记录
├── patches/
│   ├── README.md              补丁总览与通用约束
│   ├── patch_glcore.py        通用补丁工具（check / apply / revert / verify）
│   ├── 525.116.03/            patch.json + apply.sh + revert.sh + 详细说明
│   ├── 580.178.04/
│   └── kernel/                内核侧方案（改受限设备表，治本，风险更大）
└── tools/
    ├── desensitize.py         报告脱敏工具（规则外置）
    ├── rules.example.json     脱敏规则示例（假数据）
    ├── static/                xcall.py —— .so 静态分析（函数/立即数/调用者/反汇编）
    ├── runtime/               运行时探针（硬件断点、代码补丁、pushbuffer 改写）
    └── pushbuffer/            pushbuffer 命令流分析（method 直方图、实参、ctx 扫描）
```

### 没有包含什么（以及为什么）

| 缺失 | 原因 | 怎么获得 |
|---|---|---|
| `libnvidia-glcore.so.*` 原始文件 | 体积大（35–44 MB/个）、属 NVIDIA 版权 | 从你自己的驱动安装包解包 |
| `before/` `after/` 二进制 | 同上 | 用 `patches/*/apply.sh` 从原版生成 |
| 驱动安装包 `.run` | 数百 MB，属 NVIDIA 版权 | 从 NVIDIA 官网下载 |
| perf / trace 原始数据 | 数 GB，无长期价值 | 按 `docs/reports/` 里的方法复现 |

**补丁只需要两个数字：偏移和期望原值。** 不需要分发任何二进制。

---

## 快速开始

### 1. 先确认你的驱动有没有这个问题

**这一步不能跳过。** 有些版本可能已经修了。

```bash
# 查你的 glcore 里有没有那条命令的立即数
python3 -c "
import sys
p = '/usr/lib/x86_64-linux-gnu/libnvidia-glcore.so.<你的版本>'
d = open(p,'rb').read()
pat = bytes.fromhex('660e0120f0000000')   # 0x20010E66 + 实参 0xF0
print('命中', d.count(pat), '处')
"
```

- **0 处** → 没有这个问题，不需要补丁
- **1~2 处** → 有，可以继续

同时**实测一次性能**，确认它真的是你的瓶颈（见 `docs/INDEX.md` 里的测量方法）。

### 2. 用现成补丁

```bash
cd patches/580.178.04        # 换成你的版本

# 只看状态，不改动
sudo ../patch_glcore.py check \
     /usr/lib/x86_64-linux-gnu/libnvidia-glcore.so.580.178.04 --spec patch.json

# 应用（自动备份为 .orig）
sudo ./apply.sh

# 回滚
sudo ./revert.sh
```

**生效时机**：改的是磁盘文件，**已在运行的程序不受影响**，需重启程序或重新登录。

### 3. 你的版本没有现成补丁

按 `docs/reports/` 里的方法自己定位，核心判据只有三步：

```
① 搜立即数 66 0e 01 20 f0 00 00 00
   = movabs（高32位实参 0xF0 + 低32位命令头 0x20010E66）→ 得到发射点

② 发射点前应能看到 "cmp byte [reg+偏移], 0" 的结构
   偏移值会变（525 是 0xc18，580 是 0x20b），不要写死

③ 紧随其后的条件跳转 74 xx (je) / 75 xx (jne)
   → 改成 eb xx (jmp)，只改首字节
```

**74/75/eb 都是 2 字节 rel8 短跳转，长度一致，可原地替换。**
换成 `e9`（rel32，5 字节）会破坏后续所有偏移，**绝对不要**。

---

## 重要限制

| 项 | 说明 |
|---|---|
| **驱动升级即失效** | 偏移随版本变化，必须重新定位 |
| **重装同版本驱动会覆盖** | 安装器会写回原始文件（这也是最可靠的恢复手段） |
| **包校验会报** | `dpkg -V` / rpm 校验会显示该文件被修改，属预期 |
| **只验证过特定负载** | 主要在 `n83b` 和 Heaven 上验证；其他应用可能有未覆盖的代码路径 |
| **不是厂商修复** | 这是逆向研究的产物，随时可能被后续版本改变行为 |

**生产环境请优先考虑：**
1. 使用没有该问题的驱动版本（本项目实测 **515.105.01 没有此问题**）
2. 在应用层减少批次数（合批），把每帧批次数压进性能预算

---

## 补丁在做什么（一句话）

把

```asm
cmp  byte [obj + 0x20b], 0
je   skip                ; 标志位为 0 → 跳过发射
movabs rcx, 0xF020010E66 ; 实参 0xF0 + 命令头 0x20010E66
mov  [pushbuf], rcx
```

改成

```asm
cmp  byte [obj + 0x20b], 0
jmp  skip                ; ← 只改这一字节：74 → eb，无条件跳过
```

**为什么不改那个 `0xF0`（治标）**：把 240 改成 16 只能恢复一半多（52.3 → 7.0 ms），
而"根本不发"能到 3.4 ms，**完全追平老驱动**。
成本不是"延迟太长"，而是"这条路径本身不该走"。

---

## 文档入口

| 想了解 | 看这里 |
|---|---|
| 完整研究报告（174 份） | [`docs/INDEX.md`](docs/INDEX.md) |
| 根因是怎么定位的 | `docs/reports/` 中编号 170 的报告 |
| 补丁怎么固化的 | `docs/reports/` 中编号 171 的报告 |
| 580 上的实测 | `docs/reports/` 中编号 173 的报告 |
| 补丁的通用约束 | [`patches/README.md`](patches/README.md) |
| 法律与免责 | [`DISCLAIMER.md`](DISCLAIMER.md) |

---

## 环境说明

本仓库内所有文档、日志、脚本**已脱敏**：IP、主机名、用户名、个人路径
均替换为 `<testhost>` / `<user>` / `<workdir>` 等占位符。
脱敏工具与规则示例见 `tools/desensitize.py` 和 `tools/rules.example.json`。

如果你要提交自己的记录，请先跑一遍脱敏 —— **规则文件本身包含真实信息，不要提交 `rules.json`。**
