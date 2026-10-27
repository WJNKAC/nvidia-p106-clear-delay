# 报告 144 — 精准狙击成功执行：拿到读写指针、只改活跃区间、并用 Xid 证明改动真的到达 GPU

> 编号：144（对应 N153）
> 日期：2026-10-27
> 平台：P106-100 / 525.116.03 / Heaven 4.0 OpenGL
> 前置：143（盲改 pushbuffer 无效）、142（判据是位标志）、140（指令流对比）
> **本轮第一次做到"改动被 GPU 真的读到"，并用内核 Xid 错误作为铁证。**

---

## 0. 三句话结论

1. **两个技术难题都解决了**：找到 emit 函数（`0x1615190`，`rsi` 就是 pushbuffer 写指针），
   实现"只改 `[rsi-N, rsi)` 活跃区间"的精准狙击。机制完全可行。
2. **第一次拿到"改动生效"的铁证**：填垃圾导致内核报
   **`Xid 31 MMU Fault @ 0x0`**，Heaven 卡死。**这证明那块 pushbuffer 就是 GPU 读的。**
3. **但把 subchannel 改成 6 是无效的**：27,173 处改动 → GPU 报 **`Xid 32`（非法方法）**，
   **fps 119.7 完全不变**。⇒ subchannel 6 上没有绑定 3D class 对象，
   命令被判定非法并丢弃。**515 能用 sub6 是"驱动在 sub6 上绑了对象"，不是"命令头写了 6"。**

---

## 1. 拿到了读写指针

### 1.1 关键突破：`0x1615190` 就是 emit 函数

从报告 142 的反汇编里注意到 `0x1615190` 被 `call` 两次（`0x1615575`、`0x16156c5`）。反汇编它：

```asm
1615190: mov    rax, rcx
1615193: mov    ecx, DWORD PTR [rdi+0x3e8]      ; rdi = ctx（ctx+0x3e8 是状态）
...
16151a4: mov    DWORD PTR [rsi], 0x20028090      ; ★ 直接往 [rsi] 写命令头
16151ae: mov    DWORD PTR [rsi+0x8], eax
16151b1: mov    DWORD PTR [rsi+0x4], edx
16151b4: mov    DWORD PTR [rsi+0xc], 0x20018092
```

**`rsi` 就是 pushbuffer 的写入地址（虚拟地址）。** 不用推算、不用猜。

### 1.2 实测：写指针轨迹

用 ptrace 硬件断点挂 `0x1615190`，采样 `rsi`：

```
rsi 范围: 0x7fb168862d64 ~ 0x7fb168b92044
rdi(ctx): 恒定 0x7fb1b8008ad0
```

连续 emit 能看到步进（`0x2d64 → 0x2d7c → 0x2d94`，步长 0x18/0x1C）。

### 1.3 重要发现：**每帧用不同的 2 MB 块**

连续采样 200 次，`rsi` 的块基址不断变化：

```
0x7fb175701d7c → 0x7fb1660025b4 → 0x7fb1759015b4 → 0x7fb1663015b4 → ...
```

落到 `/proc/pid/maps`：`7f70c3400000-7f70c3600000 rw-s /dev/nvidiactl`，都是 2 MB 块。

**这解释了报告 143 里"盲改无效"的根本原因**：多块轮转下，扫一遍改全部，
改到的多半是**已消费完的旧块**。必须只改当前正在写的这块。

---

## 2. 精准狙击的实现

### 2.1 机制

在 emit 入口（`0x1615190`）用硬件断点停住 → 得到 `rsi` = 当前写指针 →
只扫 `[rsi-WIN, rsi)` 这一小段（刚写入的最新命令）→ 改完 → 继续。

### 2.2 三个实现坑

| 坑 | 现象 | 修法 |
|---|---|---|
| **重复 ATTACH** | 探测命中后我又 ATTACH 一次，线程已在断点处停着 → 状态错乱，报"没有线程执行" | 命中后直接沿用，不再 ATTACH |
| **探测太慢** | 24 线程 × 3 s 超时 = 72 s，外层 timeout 先杀 | **按 `/proc/pid/task/*/stat` 的 CPU 时间排序**，渲染线程第一个就中；单线程超时降到 1 s |
| **非 SIGTRAP 停止** | 线程被别的信号停住时提前 break，误判 miss | 把信号原样转发后继续 CONT |

另外：**第一次没设 alarm 超时**，`waitpid` 在不会执行该函数的线程上无限阻塞，
被外层 timeout 杀掉后**Heaven 被 ptrace 挂住**（`TracerPid` 非 0），需 `kill -9 <tracer>` 恢复。

---

## 3. 三个实验，Xid 作为铁证

### 3.1 实验 A：整段填垃圾 —— **证明这块 buffer 确实是 GPU 读的**

```
操作: 把 [rsi-4KB, rsi) 全部写 0xFFFFFFFF，停 40 次，共 20480 字
结果: Heaven 存活但卡死（状态 S，GPU 0%，fps 测不到）
内核: NVRM: Xid 31, MMU Fault: ENGINE HOST0 HUBCLIENT_HOST_CPU
      faulted @ 0x0_00000000. Fault is of type FAULT_PDE ACCESS_TYPE_READ
      NVRM: Xid 32, Channel ID 00000008 intr 00200000
```

**这是本轮最重要的一条**：之前所有"改 pushbuffer"实验都缺一个判据——
**怎么知道改动真的被 GPU 读了？** 现在有了：**填非法内容 → GPU 报 MMU fault。**

⇒ **这块 2 MB 的 nvidiactl 映射确实是 GPU 在读的 pushbuffer。**
⇒ 报告 143 里"改 subchannel 没反应是因为改到旧块"这个解释是对的，但"这块不是 GPU 读的"是错的。

（附带代价：Xid 31 让 GPU 通道 8 挂掉，Heaven 卡死，必须重启进程。GPU 本身没坏。）

### 3.2 实验 B：改 `cnt` 字段 +1 —— 无反应

```
操作: 把 [rsi-64KB, rsi) 里所有命令头的 cnt 字段 +1（0x1fff 位域），共 68132 处
结果: Heaven 存活，fps 57，功率 51.9 W，无新 Xid
```

**解读**：cnt 错位后，GPU 会把参数当命令头解析。理论上应该产生垃圾命令。
但实际没崩——可能因为后续内容恰好仍可解析，或者被驱动的容错吞掉了。
（也可能：错位后落到了"合法但无用"的 opcode 上。）

### 3.3 实验 C：改 subchannel 0 → 6 —— **Xid 32，fps 不变**

```
操作: 把 [rsi-64KB, rsi) 里所有 sub=0 的命令改成 sub=6，共 27173 处
结果: Heaven 存活，fps = 359/3s = 119.7（与基线一致），功率 59 W
内核: NVRM: Xid 32, Channel ID 00000008 intr 00800000
      （两条，intr 位与实验 A 的 00200000 不同）
```

**这是决定性结果**：

1. **Xid 32 出现 ⇒ GPU 处理了这些命令**（不是没看到）
2. **但它报错** ⇒ subchannel 6 上没有绑定 3D class 的 object，
   GPU 拿不到方法分发表，判定为非法操作
3. **fps 完全不变** ⇒ 这些命令被**丢弃**，没有产生任何计算

**⇒ "把 subchannel 改成 6 就能分流加速"这个假设被否定。**

---

## 4. 为什么 515 能用 subchannel 6

结合报告 140 的数据：515 的 `sub6:0x0034` 有 6653 条，525 只有 16 条。

**关键区别不是"命令头写了 6"，而是"驱动有没有在 sub6 上绑定对象"。**

NVIDIA 的 subchannel 是**硬件通道**，每个 subchannel 绑定一个 object（对应某个 class 的方法分发表）。
- 驱动初始化时会 `NV_ESC_RM_ALLOC` 分配 object，再绑到某个 subchannel
- 之后写命令到该 subchannel，GPU 才知道用哪个 class 解析

**515 在 sub6 上绑了对象，525 没有（或绑到了别的 subchannel）。
这是驱动初始化的资源分配决策，写死在代码路径里，不是改几个命令字节能变的。**

⇒ 要真正"分流"，必须**同时**：
1. 在 sub6 上分配并绑定合适的 object（需要 `NV_ESC_RM_ALLOC`，走 ioctl）
2. 把对应命令改到 sub6
3. 保证两个 subchannel 的执行顺序/同步正确

**这是驱动级改动，不是内存 patch 能做的。**

---

## 5. 本轮的净结论

| 项目 | 结果 |
|---|---|
| 拿写指针（GP_PUT） | ✅ `0x1615190` 的 `rsi` |
| 只改活跃区间（不盲扫） | ✅ 实现并执行 |
| 证明改动到达 GPU | ✅ **Xid 31 MMU fault** |
| 改 cnt 打乱命令流 | ⚠️ 无反应（容错） |
| **改 subchannel 0→6** | ❌ **Xid 32 + fps 不变（命令被丢弃）** |

**技术链路完全打通了，但结论是否定的**：subchannel 分流不是靠改命令头能实现的。

---

## 6. 这条线到此为止的判断依据

三轮尝试（143 盲改 / 144 精准改 + sub / 144 填垃圾验证）已经把 pushbuffer 这条路走完：

- 改动**能**生效（Xid 证明）
- 但**改变命令的 subchannel 只会让它变成非法命令被丢弃**
- 真正要改的是**驱动的对象/通道分配**，而那在 ioctl 层（`NV_ESC_RM_ALLOC`），
  不是内存 patch

**如果还要继续这条路，唯一可行的形态是：在 emit 函数里，让驱动"以为"某个 subchannel 已就绪**——
这需要伪造 object handle、方法分发表、同步状态，工程量极大且极易让 GPU 进 Xid 死循环。
**性价比不值得。**

---

## 7. 工具清单

| 文件 | 用途 |
|---|---|
| `analysis/onheap/putptr.c` | 硬件断点采样 `rsi`（写指针），验证 emit 函数 |
| `analysis/onheap/hook2.c` | 精准狙击：三种模式 `sub` / `cnt` / `fill`，只改 `[rsi-WIN, rsi)` |
| `analysis/onheap/ctxread2.c` | 硬件断点读任意内部指针（报告 143） |

`hook2` 用法：
```bash
hook2 <pid> <glcore_base> <off> <win_bytes> <max_stops> [newsub] [gap_ms] [sub|cnt|fill]
# 例：只改最近 4KB 的 subchannel
hook2 <pid> 0x7f...0 1615190 4096 40 6 0 sub
```

---

## 8. 环境

- 实验 A 后 GPU 通道 8 因 MMU fault 挂掉，Heaven 卡死；**重启进程后恢复正常**（53.84 W 渲染），GPU 未损坏
- Xid 计数：8 → 10（实验 A 2 条 + 实验 C 2 条，都在 `dmesg` 里可查）
- 当前驱动 525.116.03，无残留 hook 进程
