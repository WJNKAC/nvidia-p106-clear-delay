# N188 — Vulkan 的 ~137 µs 是**每 render pass 的固定开销**，与工作量无关

**日期**：2026-10-31
**驱动**：580.178.04 / kernel 6.12.113 / P104-100 / PCIe Gen1 x4
**状态**：只用户态清除延迟（内核白名单未打）
**问题**：N187 找到「Vulkan 3D 提交 ~139 µs」，但没说清成本单位是什么、注入点在哪。本报告继续拆。

---

## 0. 结论

### ① 成本单位是 **render pass**，不是提交

| 配置 | 总时间（每帧） | per_pass |
|---|---|---|
| 100 次提交 × 1 pass（各自等 fence） | 13.947 ms | 139.5 µs |
| **1 次提交 × 100 pass** | **13.966 ms** | 139.7 µs |
| 10 次提交 × 10 pass | 13.328 ms | 133.3 µs |
| 空提交 × 100（无 render pass） | 0.750 ms | 7.5 µs |

**三者完全一样。** 提交次数从 100 降到 1，总时间一分没少。
⇒ **N187 里我说的「成本按提交次数算」是错的，应为「按 render pass 数算」。**

### ② 它是固定开销，不是带宽/算力 —— 用带宽反算可以一眼看出

render pass 只做 clear（0 个 draw）：

| 分辨率 | 像素 | 数据量 | per_pass | 反算带宽 |
|---|---|---|---|---|
| 256² | 65 K | 262 KB | 137 µs | **1.9 GB/s** |
| 4096² | 16.7 M | 67 MB | 562 µs | 158 GB/s |

P104 板载带宽约 192 GB/s。**同一个 clear 操作，小尺寸时带宽利用率只有 1.2%。**
256² 那 262 KB 绝不可能需要 137 µs ⇒ 那 137 µs 是**与数据量无关的固定开销**。

正确分解：**固定 ≈ 137 µs + 真实带宽成本**（4096² 时后者 ≈ 425 µs）。

### ③ 已排除的三个可能来源

| 假设 | 怎么排除的 | 结果 |
|---|---|---|
| 是 glcore 里那条 `method 0x3998` | glcore 补丁 `eb` 在位（已验证 21.8µs/batch 生效），Vulkan 仍 139 µs | ❌ 无关 |
| 是 eglcore 里同构的那份 `je` | 把 `0xb7bcd1` 的 `74` 改 `eb`，**139 µs → 138.9 µs，零变化** | ❌ 无关 |
| 是同一 image 反复写造成的 WAW 同步 | 改成 8 个 framebuffer 轮换，**仍 139.5 µs** | ❌ 无关 |

### ④ 与 GL 侧机制不同

strace 对比（同一探针、同一负载）：

| 侧 | 每批成本 | futex 调用 | 延迟花在哪 |
|---|---|---|---|
| **GL 原厂** | 149.0 µs | 79 次 / 0.13 ms | **不在 syscall 里**（syscall 总计仅占 3%） |
| **GL 补丁** | 22.9 µs | 79 次 / 0.01 ms | — |
| **Vulkan render pass** | 139 µs | 509 次 / **1.03 s（69.3%）** | **futex 等待** |

GL 的延迟在库内部（CPU 自旋等 GPU），Vulkan 的延迟是**线程间 futex 等待**。
⇒ **不是同一处代码**。这解释了为什么改哪一份 glcore/eglcore 的 `je` 都没用。

---

## 1. 静态排查记录

在全部 NVIDIA 用户态库里搜那条命令的编码（`movabs $0xf020010e66` / 立即数 `66 0e 01 20`）：

| 库 | movabs | 立即数命中 | 备注 |
|---|---|---|---|
| `libnvidia-eglcore.so` | **2** | 2 | **Vulkan/EGL 的实现都在这**（perf 显示 96 %） |
| `libnvidia-glcore.so` | 1 | 2 | GL 实现（已打补丁的那处） |
| `libnvidia-vksc-core.so` | 0 | 2 | Vulkan SC |
| 其余 20+ 个库 | 0 | 0 | — |

**eglcore 里 `0xb7bcd1` 那处和 glcore 的 `0xb51c82` 结构完全同构：**

```
glcore @0xb51c82（已打 eb）              eglcore @0xb7bcd1（原厂 je）
  mov    0x1240(%r14),%rax                mov    0x1240(%rbx),%rax
  cmpb   $0x0,0x20b(%rax)                 cmpb   $0x0,0x20b(%rax)     ← 同一字段
  jmp    0xb51cb0                         je     0xb7bcff
  mov    0x80(%r14),%rax                  mov    0x80(%rbx),%rax      ← 同一字段
  movabs $0xf020010e66,%rcx               movabs $0xf020010e66,%rcx    ← 同一立即数
  mov    %rcx,(%rax)                      mov    %rcx,(%rax)
```

字段偏移 `0x1240` / `0x20b` / `0x80` / `0x88` 全一致，只是基址寄存器 r14→rbx。
**改成 `eb` 后 Vulkan 的 139 µs 一点没变** ⇒ 这段不是 Vulkan 3D 提交走的路径
（更可能是 EGL/GLES 那条）。

第二处 `0xd05160` 是**另一种检查**（pushbuffer 空间：`cmp 0x60,0x68`），不是标志位。

perf 热点全部落在**虚函数派发桩**上（`d7d110: mov 0xf0(%rdi),%rdi; mov (%rdi),%rax; jmp *0x18(%rax)`），
说明间接调用极密；间接跳转上 IP 归因会 skid，所以拿不到更深的位置。

---

## 2. 那个「14 倍」的旁证（仅记录，不作结论）

本项目历史数据：受限 SKU 未修时 `52.202 ms`，修复后 `3.668 ms` —— **14.2 倍**。

本报告：100 个 render pass 需要 13.9 ms；按正常独立显卡「空 render pass 提交 ≈ 3~5 µs」
估算应为 0.3~0.5 ms。**慢了约 28~46 倍**，比 14 倍还大。

⚠ 这个对比**不严谨**（没有同机对照卡，正常值靠经验），**不能当证据**，只作为排序参考。

---

## 3. 仍未解决

1. **这 ~137 µs 是不是受限 SKU？** 唯一严格的办法：**打内核白名单做 A/B**。
   若 render pass 从 137 µs 掉到十几 µs，即闭环。**本报告未做**（需重编内核模块 + 重载驱动，
   会中断图形会话）。
2. **没有同机对照 GPU。** 本机 Intel 核显的 Vulkan 不可用（Mesa 25.0.7 报
   `Haswell Vulkan support is incomplete`，枚举 0 设备），lavapipe（软件 Vulkan）同样枚举不到设备。
   ⇒ 「正常 Vulkan 的 render pass 应该多快」只能靠经验值，这是本报告最大的短板。
3. **注入点未定位。** 只知道不在 glcore 补丁点、不在 eglcore 同构处；futex 等待说明在等某个线程，
   但等的是谁没抓到。

---

## 4. 实践结论（这部分可靠）

**减少 render pass 次数是最有效的软件侧缓解**，与 GL 侧「减少 batch 数」同构：

| 做法 | 100 个 draw 的耗时 |
|---|---|
| 100 个 render pass 各 1 draw | 13.9 ms |
| 10 个 render pass 各 10 draw | 1.48 ms |
| **1 个 render pass 装 100 draw** | **0.31 ms** |

**差 45 倍。** 对游戏而言：减少不必要的 render pass 切换（阴影图、后处理通道合并、
避免每帧重建 framebuffer）比降分辨率有效得多 —— 分辨率只在 4096² 这种极端尺寸才开始明显。

---

> **✅ 已完成（报告 178 / N189）**：三个方向都推完了。
> **空 render pass（`LOAD/STORE=DONT_CARE`、0 draw、不 clear）在 P104 上要 154.2 µs，
> 在 lavapipe（纯 CPU 软件 Vulkan）上只要 10.0 µs —— 15 倍差。**
> 且 P104 的「空操作」与「真的 clear 256²」（153.2 µs）几乎一样 ⇒
> 证明这 ~144 µs 与工作量无关，是 P104 特有的固定成本，与 GL 侧 125 µs 同量级。
> LD_PRELOAD 劫持被判不可行（等的是真实事件，poll 本身只要 2.8 µs）。

## 5. 下一步（需要中断图形会话，待确认）

```bash
# A/B：打内核白名单，重测 render pass 成本
cd <gen2 仓库>/drivers/580.178.04
sudo ./00-whitelist.sh apply
cd /usr/src/nvidia-580.178.04 && sudo make -j$(nproc)
sudo <gen2 仓库>/drivers/580.178.04/sync-dkms.sh adopt
# 重载 nvidia 或重启，然后：
VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json ./vkdraw -n 100 -d 1 -f 40 -w 10 -r 256
# 期望：per_pass 137 µs → 十几 µs
```

## 6. 复现

探针已收进 `tools/static/`（`vkdraw.c` 现支持 `-n` 提交数 / `-p` 每次提交内 render pass 数 /
`-d` 每 pass 内 draw 数 / `-r` 分辨率 / `-e` 空提交 / `-s` 逐次等 fence）。

```bash
# 成本单位（pass vs submit）
./vkdraw -n 100 -d 1 -p 1 -r 256      # 100 提交 × 1 pass
./vkdraw -n 1   -d 1 -p 100 -r 256    # 1 提交 × 100 pass  → 两者应相同
# 固定 vs 带宽
./vkdraw -n 100 -d 0 -r 256           # 137 µs
./vkdraw -n 100 -d 0 -r 4096          # 562 µs
```
