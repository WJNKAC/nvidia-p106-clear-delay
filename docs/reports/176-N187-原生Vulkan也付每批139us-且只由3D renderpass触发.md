# N187 — 原生 Vulkan 也付「每批 ~139 µs」，且只由 3D（render pass）触发

**日期**：2026-10-31
**驱动**：580.178.04 / kernel 6.12.113+deb13-amd64 / P104-100 / PCIe Gen1 x4
**状态**：只用户态清除延迟（内核白名单**未打**）
**问题**：接着 N186 —— Vulkan 路径那 ~142 µs 到底是什么？是提交机制、是真实渲染，还是同一个受限 SKU 延迟？

---

## 0. 结论

**既不是提交机制，也不是真实渲染负载 —— 是「每次 3D 提交」的固定开销，且只有 render pass 会触发。**

| 提交内容 | per_submit | 说明 |
|---|---|---|
| 空命令缓冲（无 render pass） | **7.8 µs** | 提交基础设施本身很便宜 |
| 纯 `vkCmdCopyBuffer` | **20.0 µs** | 走拷贝引擎，便宜 |
| **render pass + clear（0 个 draw）** | **136.9 µs** | ★ 只要进 render pass 就贵 |
| render pass + 1 个 draw | 139.0 µs | 加一个 draw 只多 2 µs |

**同样的总工作量（100 个 draw @256²），换成不同提交次数：**

| 拆分方式 | total_p50 | per_submit |
|---|---|---|
| 100 次提交 × 1 draw | **13.975 ms** | 139.8 µs |
| 10 次提交 × 10 draw | 1.481 ms | 148.1 µs |
| 1 次提交 × 100 draw | **0.309 ms** | 309.5 µs |

⇒ **成本完全按「提交次数」算，按「draw 数」几乎不算**（每个 draw 仅 ~1–2 µs）。
100 次提交 vs 1 次提交，同样的活，**差 45 倍**。

---

## 1. 为什么这个结果重要

N186 里 zink 测到 142 µs/batch，当时无法排除「zink 自身开销」。
现在原生 Vulkan（不经过 Mesa、不经过 glcore 的 GL 代码）测到 **139 µs**，
和 zink 的 142 µs、原生 GL 未打补丁的 133 µs **三者几乎完全一致**：

| 路径 | 每批成本 | glcore 补丁影响 |
|---|---|---|
| native GL，原厂 | 133 µs | — |
| native GL，补丁 | **21.8 µs** | ✅ 有效 |
| zink（Vulkan） | 141 µs | ❌ 无效 |
| **原生 Vulkan 3D** | **139 µs** | ❌（不经过该代码） |
| 原生 Vulkan **copy** | **20 µs** | 本来就没有 |
| 原生 Vulkan **空提交** | **7.8 µs** | 本来就没有 |

注意「21.8 µs（已修好的 GL）」和「20 µs（Vulkan copy）」和「7.8 µs（空提交）」
是同一量级 —— 这是**没有延迟时**的正常水平。
而「133 µs（未修的 GL）」「139 µs（Vulkan 3D）」「141 µs（zink）」也是同一量级。

---

## 2. 分辨率实验：固定部分 + 真实工作量

render pass 只做 clear（0 个 draw）：

| 分辨率 | per_submit |
|---|---|
| 256² | 136.9 µs |
| 4096² | 561.7 µs |

4096² 比 256² 多 256 倍像素，只多了 425 µs。
按 P104 板载显存带宽（约 192 GB/s）算，4096²×4B = 67 MB → 约 350 µs，
和「425 µs 增量」对得上。

⇒ **可以分解为：固定 ~137 µs + 显存带宽决定的 clear 时间。**
那 137 µs 与分辨率无关、与 draw 数无关，是纯粹的「进一次 render pass 的代价」。

**这也解释了之前 N186 里一个看似矛盾的点**：Vulkan 的 142 µs 不是「算得慢」，
因为把分辨率从 256² 提到 4096²（像素量 ×256），固定那部分一分钱没少。

---

## 3. 机制推断：延迟挂在 3D 引擎上，不在拷贝引擎上

那条受限 SKU 的命令是 **往 gr3d 写 method `0x3998`** —— `gr3d` 就是 3D 引擎。

| 提交走哪个引擎 | 有延迟？ |
|---|---|
| 3D / graphics（render pass + draw） | **有，~137 µs** |
| copy（`vkCmdCopyBuffer`） | 无，20 µs |
| 空提交（不碰任何引擎） | 无，7.8 µs |

这个分界和「命令写给 gr3d」完全吻合。
⇒ **Vulkan 的 3D 提交路径上存在同一性质的每批开销，但它不在 glcore 里被
`0xb51c82` 跳过的那段 GL 代码中**，因此用户态补丁碰不到它。

至于它是在 RM 内部、还是在驱动另一个发命令的地方，**本报告不做断言**
（没抓到 Vulkan 的 pushbuffer，见 §5）。

---

## 4. GPU 侧签名（如实记录，但不构成判据）

原生 Vulkan 3D 持续负载（n=100, 256²）：

```
99 %, 71.11 W, 1911 MHz, 5005 MHz, P0
99 %, 70.49 W, 1911 MHz, 5005 MHz, P0
（六次采样一致）
```

纯 copy 提交同样 99 % / 71 W，但 per_submit 只有 20 µs。

⇒ **「99 % 占用 + 70 W」在本机不能区分「有延迟」和「没延迟」**，
因为两种负载都是这个数字。**所以不能用功耗单独当判据**——
上一份报告里我提过「100% 占用只 60 W」的签名，这里必须修正：
那个签名只在**游戏**场景成立，不是普遍指标。

---

## 5. 局限（很重要）

1. **因果链还差最后一环。** 现在只能说「Vulkan 3D 提交也有 ~139 µs，且与 GL 未修时的
   ~133 µs 同量级、同分界（3D 有 / copy 无）」。要坐实「是同一个受限 SKU 机制」，
   还需要**打上内核白名单再测一遍**：若 Vulkan 3D 的 139 µs 随之消失，即闭环。
   本报告**尚未做**这一步（需要重编内核模块并重载驱动，会中断图形会话）。
2. **Intel 对照拿不到。** 本想用核显的独立 Vulkan 实现做对照，但本机 Mesa 25.0.7 的
   ANV 报 `MESA-INTEL: warning: Haswell Vulkan support is incomplete`，
   枚举出 0 个设备 ⇒ 无法提供「非受限 GPU 的 render pass 开销」基线。
3. 探针是最小合成负载（全屏三角形 / clear），不是真实游戏。
4. 未抓 Vulkan pushbuffer，**无法直接验证 `method 0x3998` 有没有出现在 Vulkan 提交里**。

---

## 6. 实践含义（与已知事实对齐）

1. **走 Vulkan 3D 的程序必须打内核白名单。** 用户态 glcore 补丁对它们无效。
   这正是 10-30 七日世界（DXVK）打 glcore 补丁仍 19 fps、打内核补丁才 60 fps 的原因。
2. **纯计算 / 拷贝型负载不受影响**（copy 提交 20 µs，和没打补丁的 GL 修复后同量级）。
   所以「矿卡跑 CUDA 拷贝正常、一跑 3D 就慢」这个现象也能解释。
3. **批次数比像素数更重要。** 4096² 一个 render pass 才 562 µs，
   而 100 个 256² 的 render pass 要 13.9 ms —— 合批（减少提交次数）是唯一有效的软件侧缓解。

---

## 7. 复现

探针（本次新增，已收进 `tools/static/`）：

| 文件 | 作用 |
|---|---|
| `batchprobe.c` | GL 探针（native / zink 通用），`-s` 每 draw 同步 |
| `vkprobe.c` | Vulkan 探针（**copy** 引擎），`-n` 提交数 / `-d` copy 数 / `-s` |
| `vkdraw.c` | Vulkan 探针（**3D render pass**），`-n` / `-d` / `-r` 分辨率 / `-e` 空提交 / `-s` |
| `tri.vert` / `tri.frag` | 最简着色器，`glslangValidator -V` 编译 |

```bash
# 3D 提交：n 个 render pass
VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json ./vkdraw -n 100 -d 1 -f 40 -w 10 -r 256

# 空提交（判据：应远小于 render pass）
./vkdraw -n 100 -d 1 -f 40 -w 10 -e

# 纯 copy 提交
./vkprobe -n 100 -d 1 -f 40 -w 10
```

`VKPROBE_VID` / `VKPROBE_DID` 可指定设备（默认 0x10de / 0x1b87 = P104-100）。

---

## 8. 下一步（需要中断图形会话）

```bash
# 1) 打内核白名单（此刻未打）
cd <gen2 仓库>/drivers/580.178.04
sudo ./00-whitelist.sh apply
cd /usr/src/nvidia-580.178.04 && sudo make -j$(nproc)
sudo <gen2 仓库>/drivers/580.178.04/sync-dkms.sh apply   # 防 DKMS 覆盖
# 2) 重载 nvidia（或重启）
# 3) 重跑 vkdraw：若 139 µs → ~20 µs，闭环
```
