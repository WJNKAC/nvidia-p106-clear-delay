# 报告 121 / N130：**`RMInvalidateAllTLBsBug3896322WAR` —— 不是它。用 580 一个反例就能否掉**

> 时间：2026-10-26 22:0x
> 环境：<user>@<testhost>（`<testhost>`），P106-100 / Gen1 x16 / headless X
> 触发：用户「RMInvalidateAllTLBsBug3896322WAR 是这个问题吗？」
> 结论：**不是。而且这条线索比"不成立"更有价值 —— 它把 MMU/TLB 这整个方向一起否掉了。**

---

## 0. 三句话

**① 这个字符串确实只在 525 里存在（515 没有、580 也没有），它是 RM 的 registry 键，"开"和"关"实测差 0.1% —— 127.119 ms vs 127.246 ms。**

**② 更硬的是反例：580 里既没有这个键、也没有它对应的那段 MMU 代码，但 580 在你机器上一样慢。同一现象、同一量级，不可能由只在一个版本里存在的东西解释。**

**③ 顺带把 MMU/TLB 整个方向也否掉了：M6（切到同一个 program，不需要任何新页表映射）比 M3（切不同 program）贵 2.1 倍。任何页表/TLB 维护假说都预测"同一对象最便宜"。**

---

## 1. 字符串本身：确实存在，而且只有 525 有

```
$ strings -a nv-kernel.o_binary | grep -i "3896322\|InvalidateAllTLBs"
515.105.01 : （无）
525.116.03 : RMInvalidateAllTLBsBug3896322WAR
580.178.04 : （无）
```

在 525 的 blob 里只有一处，文件偏移 `0x3184238`（`.rodata`，段内偏移 `0x2572058`）。

**唯一引用它的是 `_nv023892rm`（0x497900，1138 字节），在 `+0x30b`。**

### 1.1 它被当作参数传出去，不是分支条件

```
497c05:  mov    %r12,%rdx            ; rdx = &local
497c08:  mov    $0x0,%rsi            ; ← 0x497c0b 就是这 4 字节立即数（重定位）
                                     ;    rsi = "RMInvalidateAllTLBsBug3896322WAR"
497c0f:  mov    %rbx,%rdi            ; rdi = pGpu
497c12:  call   _nv037055rm          ; (gpu, "NAME", &out)
497c17:  or     0xc(%rbp),%eax
497c1a:  je     497ab6
497c20:  movb   $0x1,0x74d(%rbx)     ; pGpu->[0x74d] = 1   ← 置 WAR 标志
```

> ⚠️ 这里踩了和报告 115 里 `VGPU%d` 一样的坑的**反面**：`0x497c0b` 落在 `mov $imm32,%rsi` 的立即数中间，
> 我第一眼以为"引用位置不在指令边界"，其实是因为重定位作用在立即数字段上。**这不是异常，是正常形态。**

### 1.2 它是 683 个 registry 键之一

`_nv037055rm`（0x5faa0，146 字节）是参数校验 wrapper：
`test %rsi,%rsi; je fail` / `test %rdx,%rdx; je fail`，失败返回 `0x1f` = `NV_ERR_INVALID_ARGUMENT`。

**它被 902 个调用点、683 个不同键名使用**，其中包括：

```
RmDisableMmuInvalidate                      ← 直接的 MMU 开关
RMBug3007008EmulateVfMmuTlbInvalidate       ← TLB 相关
RMInvalidateAllTLBsBug3896322WAR            ← 你问的这个
RMForcePstateBug3811584War
RMBar1RestoreSize / RMPowerFeature2 / RMNvLinkverboseControlMask …
```

**⇒ 这是 RM 的 registry 读取器。`RMInvalidateAllTLBsBug3896322WAR` 是一个可配置键，不是硬编码逻辑。**

### 1.3 默认值：开启

```
读到 1       → 返回 NV_OK(0)，local=1  → or ≠ 0 → flag=1
读到 0       → 返回 NV_OK(0)，local=0  → or = 0 → flag 保持 0
读不到       → 返回错误码(≠0)         → or ≠ 0 → flag=1   ← 默认开启
```

### 1.4 这个标志控制的是 MMU 页表**寄存器选择**

两个读点：

| 函数 | 位置 | 行为 |
|---|---|---|
| `_nv030941rm` | `0x469a8f` `cmpb $0x0,0x74d(%r12)` | flag≠0 → `ecx = (ecx<<7) + 0x1f206c` 后写寄存器 |
| `_nv031060rm` | `0x469b73` `cmpb $0x0,0x74d(%rdi)` | flag≠0 → 走 `0x30a0/0x30a4` 系列；flag=0 → 走 `0x1f205c` 系列 |

`_nv031060rm` 里有明确的**页表编程特征**：

```
469b5f:  shr    $0xc,%r8          ; >>12 = 4KB 页对齐
469b65:  shl    $0x4,%r8d         ; <<4  = PTE 索引步长
469b69:  and    $0x2,%eax         ; 取有效位
469b84:  lea    0x30a0(%rax),%ecx ; MMU 寄存器窗口
469ba8:  add    $0x30a4,%ecx
```

**所以你的直觉方向是对的 —— 这确实是 MMU 页表/TLB 的地盘。**下面说为什么它不是答案。

---

## 2. 动态实测：开和关，一模一样

方法：`NVreg_RegistryDwords` 是**模块参数**，必须重载模块才生效（不需要整机重启，因为无 X 占用）。

```sh
sudo modprobe -r nvidia_drm nvidia_modeset nvidia
sudo modprobe nvidia NVreg_RegistryDwords="RMInvalidateAllTLBsBug3896322WAR=0"
# 起 headless X，跑 n83a -M 7 -n 1000 -c 1000
```

| 配置 | K=100 | K=1000 |
|---|---|---|
| 无参数（基线） | 17.172 ms | 126.988 ms |
| `RMInvalidateAllTLBsBug3896322WAR=1` | 17.135 ms | **127.119 ms** |
| `RMInvalidateAllTLBsBug3896322WAR=0` | 17.170 ms | **127.246 ms** |
| `RmDisableMmuInvalidate=1` | 17.177 ms | **127.123 ms** |

**四种配置的 K=1000 最大差 0.26 ms = 0.2%。开关拨到 0，121 µs/对 一点没少。**

第二个键（`RmDisableMmuInvalidate`，名字直接就是"禁用 MMU 失效"）同样无效。
它在 515 里也存在（闭源 blob 2 处、525 有 4 处），所以它本来就不能解释版本差异，
但至少能验证"121 µs 是不是 MMU invalidate"—— **也不是。**

---

## 3. 决定性反例：580 什么都没有，可它一样慢

这是最省事、也最无法反驳的一条：

| | 515.105.01 | 525.116.03 | 580.178.04 |
|---|---|---|---|
| `RMInvalidateAllTLBsBug3896322WAR` 字符串 | 无 | **有** | 无 |
| 它对应的 MMU 写模式<br>`c1 e1 07` + `81 c1 6c 20 1f 00` | **0 处** | **1 处** | **0 处** |
| 实测 Heaven | 130+ fps | 30 fps | **30 fps** |

**你早就测过 580（还有 535/550/570），全部一样慢。而 580 里这两样东西都不存在。**

⇒ 一个"只在 525 出现、580 里被删掉"的东西，**不可能**是"525 与 580 共有症状"的原因。
**这一条就够了，不需要再讨论它的语义。**

---

## 4. 顺带：MMU/TLB 这整个方向也被 M6 否掉

即使不谈 580，我们自己的数据也不支持"页表/TLB 维护"：

| 模式 | 做什么 | 每对耗时 | 若真是页表/TLB，该是多少 |
|---|---|---|---|
| M3 | 切到**不同** program 再画 | 127.2 ms | 要重建映射 → 贵 |
| M6 | 切到**同一个** program 再画（no-op） | **268.0 ms** | **无需任何新映射 → 该最便宜** |

**M6 比 M3 还贵 2.1 倍。** 任何"每次绑定都要重写页表 / 失效 TLB"的假说都预测反了。

（这个 2 倍本身我仍然没解释，报告 120 已标注；但它足以否掉页表假说。）

---

## 5. 那 3896322 到底是什么

从代码看，它**不改变"要不要做 MMU 操作"，只改变"往哪个寄存器、用什么布局写"**：

- flag=1：`0x1f206c + (idx<<7)` / `0x30a0`+`0x30a4`
- flag=0：`0x1f205c` 系列

也就是说：**这是"换一种写法去写同一件事"的规避方案（Work Around），不是"多做/少做一件事"的开关。**
所以它天生就**不可能**产生 121 µs 量级的差异 —— 两种写法写的是同一个 MMU 状态，
差别只在那条指令序列本身（几十个周期）。

**这就解释了为什么 0/1 测出来一模一样**：不是开关没生效，是这个开关**本来就不控制成本**。

> 诚实边界：我无法从 `/proc/driver/nvidia/registry` 确认键是否真被 RM 消费
> （该文件只输出 `Binary: ""`，看不到键表）。但 §3 的 580 反例不依赖这一点。

---

## 6. 各条证据强度

| 结论 | 强度 | 依据 |
|---|---|---|
| 字符串只在 525 存在（515/580 都无） | **强** | 三个 blob 直接 `strings` |
| 它是 registry 键、由 `_nv037055rm` 读取（683 个键共用） | **强** | 902 个调用点 + 键名列表 |
| 它控制 MMU 寄存器**选择**，不控制"做不做" | **中强** | 两条分支都写 MMU 寄存器，只是地址/布局不同 |
| 开关 0/1 对 121 µs 无影响 | **强** | 四配置实测，差 0.2% |
| `RmDisableMmuInvalidate=1` 也无影响 | **强** | 实测 |
| **580 无此键/无此代码，但一样慢 ⇒ 不是根因** | **强（决定性）** | 跨版本反例 |
| MMU/TLB 维护不是 121 µs 的来源 | **强** | M6 > M3 反证 |
| ~~这就是 525 慢的原因~~ | **⛔ 否证** | 上面全部 |

---

## 7. 一句话

**你这个名字找得很准 —— 它确实只在 525 里，确实是 MMU 页表的规避代码。**
**但它只是"换个寄存器写同一件事"，不是"多做一件事"，所以开关拨到 0，127 ms 一动不动。**
**而且 580 里这两样东西都不存在，580 照样 30 fps —— 一个 580 就把它否掉了。**
**再补一刀：M6（绑同一个对象、零新映射）比 M3 贵 2.1 倍，页表/TLB 假说本身也站不住。**

---

## 附：本轮踩的坑（都记进 SKILL 了）

1. **`/data/用户` 是 NFS 挂载（`<lan-host-b>:/data/用户`），`systemctl isolate multi-user.target` 后掉线**，
   表现为 `<workdir>` 软链断、整个实验室目录"消失"。重挂：
   `sudo mount -t nfs <lan-host-b>:/data/用户 /data/用户`。
   **数据没丢** —— 它在 laptop 上，只是没挂过来。
2. `RegistryDwords` 是**模块参数**，改它必须 `modprobe -r nvidia_drm nvidia_modeset nvidia`
   再 `modprobe nvidia NVreg_RegistryDwords="..."`，**不需要整机重启**（前提是没有 X 持有设备）。
3. 检查完必须把模块**不带参数**重新加载，否则把用户机器留在带 debug 参数的状态。
