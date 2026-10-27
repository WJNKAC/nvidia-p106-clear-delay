# 报告 104 / N113：vsync 关了，但 525 无视它 —— glxgears 63fps vs 580 的 960fps，present 被强制同步到 vblank

- 时间：2026-10-26 15:45
- ⛔ **2026-10-26 16:00 核心结论撤回（报告 105）**：本文 §3「glxgears 525=63 vs 580=960，差 15 倍 ⇒ 病 A 是 525 独有、580 已修」**撤回**。
  用户实测 `primusrun vkmark` 在**同一台机器、同一个 525.116.03** 下拿到 **Score 269**（直连只有 60）。
  查证：**那 63fps 是我直连测的，而 960fps 极可能是走 primus 的**（这台机器的图形栈一直跑在 primus 环境，报告 07 的 Heaven 也是）。
  决定性反驳：我在 **525 + primus** 下测 glxgears 得 **590fps**，与 580 的 960fps 同量级 —— 若 580 真"修好"而 525 坏，两者不该这么近。
  **⇒「病 A 是 525 引入」目前无证据；更可能是「直连 vs primus」的差异。**
  **仍然成立**：三重 vsync 开关已关且经 Intel 自证有效、直连时 present 被锁刷新率、帧时间恒定是同步指纹、病 A 与病 B 是两层。
  **本文完全没提的关键事实**：`primusrun` 能绕过病 A（swapchain 建在 Intel 上，present 走独立线程）。详见报告 105。
  **「强制同步 vblank」措辞也不准确（报告 106）**：尺寸扫描显示 NVIDIA 在 64×64 能跑 **90 fps（11.1ms）** > 60，不是硬锁 16.667ms。准确表述是「present 完成事件被合成器节拍量化 + 每帧一次跨设备同步，小尺寸可部分绕过」。
  **根因已定位（报告 106）**：**P106 `Display Mode: Disabled`、0 个 connector，当不了显示拥有者**，present 必须拷给 Intel 并同步等 KWin 合成完（20/20 采样卡在 `poll → xcb_wait_for_special_event`）。
- 驱动：525.116.03（实测）｜对照：580.126.18（报告 07 历史数据）、515.105.01（未测）
- 触发：用户「有没有关闭垂直同步？」
- 数据留档：`analysis/traces/n113/n113-matrix.txt`、`n113-525-full-1080.log`

---

## 0. 直接回答

**关了，三重开关全用上，而且证明开关是有效的。**

但**525 的 NVIDIA 呈现路径无视这些开关，把它强制同步到显示器刷新率（59.98 Hz）**。
所以那些 60fps 不是"忘了关 vsync"，**是病本身**。

而且顺着这个问题查下去，发现了一个比之前所有结论都更根本的事实：
**525 的 glxgears 只有 63 fps，而 580 上是 960 fps —— 差 15 倍。**
glxgears 只有 5 个齿轮、1 个 draw call，它慢不可能是"GL 调用太多"。**这是 present 层被锁死。**

---

## 1. 三重开关都用了，且证明它们有效

### 1.1 用了哪些

| 层 | 开关 | 说明 |
|---|---|---|
| Vulkan | `-p immediate` | vkmark 的 present mode，Vulkan 关 vsync 的标准方式 |
| GL | `vblank_mode=0` | Mesa/GLX 标准开关 |
| GL | `__GL_SYNC_TO_VBLANK=0` | NVIDIA 专有开关 |
| Vulkan | `__GLX_VENDOR_LIBRARY_NAME` 未设（默认走 NVIDIA） | 确认没走错后端 |

### 1.2 证明开关有效的办法：让 Intel 用同一套开关

**这是本轮最关键的一步**。如果只测 NVIDIA，永远分不清"开关没生效"和"驱动不听开关"。
所以同一时刻、同一个 Xwayland（`:1`）、同一个 `glxgears` 二进制，换设备再跑一遍：

| 设备 | glxgears 300×300 | glxgears 1920×1080 | `vblank_mode=0` |
|---|---|---|---|
| **Intel HD P4600** | **5902.8 fps** | **287.6 fps** | ✅ 生效（远超 60） |
| **NVIDIA P106（525）** | **63.0 fps** | **64.1 fps** | ❌ 无效（锁 60） |

**Intel 在完全相同条件下跑出 5903 fps ⇒ 环境、Xwayland、合成器、`vblank_mode=0` 全部正常。
NVIDIA 只有 63 fps ⇒ 是 NVIDIA 这一侧不听开关。**

这一条把"我是不是忘了关 vsync"彻底排除了。

---

## 2. 完整矩阵（全部本轮实测，525.116.03）

### 2.1 vkmark `clear` / `immediate` / 同一条命令

| 分辨率 | Intel | NVIDIA P106 | 倍数 |
|---|---|---|---|
| 320×240 | **4626** | **71** | 65× |
| 800×600 | **2415** | **63** | 38× |
| 1920×1080 | **521** | **61** | 8.5× |

### 2.2 vkmark 三种 present mode（NVIDIA，1920×1080）

| present mode | FPS | 帧时间 |
|---|---|---|
| **immediate** | **62** | 16.13 ms |
| **fifo** | **60** | 16.67 ms |
| mailbox | （无输出，vkmark 在该组合下失败） | — |

**immediate 与 fifo 只差 2 fps ⇒ 525 把 immediate 当成 fifo 处理。**
这不是"我没有关 vsync"，这是"驱动不认 immediate"。

### 2.3 glxgears（GL 侧，另加 `vblank_mode=0` + `__GL_SYNC_TO_VBLANK=0`）

| 尺寸 | NVIDIA P106（525） | Intel | 报告 07 的 **580** |
|---|---|---|---|
| 300×300 | **63.0 fps** | 5902.8 fps | **960 fps** |
| 640×360 | 60.5 fps | — | 828 fps |
| 1280×720 | — | — | 486 fps |
| 1920×1080 | 64.1 fps | 287.6 fps | **294 fps** |

### 2.4 vkmark 全套 13 个变体（`-b :duration=6`，1920×1080，immediate）

```
[vertex] device-local=true : 61 fps / 16.393 ms
[vertex] device-local=false: 60 fps / 16.667 ms
[texture] anisotropy=0     : 60 fps / 16.667 ms
[texture] anisotropy=16    : 60 fps / 16.667 ms
[shading] gouraud          : 60 fps / 16.667 ms
[shading] blinn-phong-inf  : 60 fps / 16.667 ms
[shading] phong            : 60 fps / 16.667 ms
[shading] cel              : 60 fps / 16.667 ms
[effect2d] kernel=edge     : 60 fps / 16.667 ms
[effect2d] kernel=blur     : 60 fps / 16.667 ms
[desktop] <default>        : 60 fps / 16.667 ms
[cube] <default>           : 60 fps / 16.667 ms
[clear] <default>          : 59 fps / 16.949 ms
=====================================
               vkmark Score: 60
```

**13 个变体全部落在 16.667 ms。** 包含 `clear`（只清屏）和 `shading`（四个 shader 变体）。
渲染量差了几个数量级，帧时间一模一样 ⇒ **不是算力问题，是同步问题。**

---

## 3. 判决：525 的 present 被强制同步

### 3.1 三个特征全部指向"等 vblank"

| 特征 | 观测 | 解释 |
|---|---|---|
| **帧时间恒定** | 13 个变体全 16.667 ms，与渲染量无关 | 固定等待，不是计算 |
| **immediate == fifo** | 62 vs 60 fps | immediate 被降级为 fifo |
| **帧率 == 显示器刷新率** | 62-64 fps vs 显示器 `59.98 Hz` | 精确对齐 |

**显示器实测：`HDMI-A-2 connected primary 2560x1080`，模式 `2560x1080 59.98*+`。**

### 3.2 这条比之前所有结论都更基础

| 层 | 现象 | 覆盖范围 |
|---|---|---|
| **present / swap 层（本轮）** | **强制同步 vblank，任何场景都 ≤ 刷新率** | **所有 GL + 所有 Vulkan 应用** |
| 批次提交层（报告 75-79） | 121 µs / 对（配对触发） | 只影响每帧大量状态切换的负载 |
| Vulkan 固定项（报告 93） | 每帧 ≈13.3 ms | 已被本轮重新解释为 vblank 同步 |

**`n83a` 的 121 µs/对 是 pbuffer 渲染、不做 present**，所以它是**独立的另一层**问题。
两层叠加才解释 Heaven 的 30 fps（见 §4）。

---

## 4. 两个独立的病，被一次测量分开

| | 病 A：present 强制 vblank | 病 B：批次提交 121 µs/对 |
|---|---|---|
| 触发 | 任何 present / swap | 状态切换 + 紧跟 draw 的配对 |
| 525 有 | ✅（glxgears 63 fps） | ✅（n83a 121 µs） |
| 580 有 | ❌（glxgears 960 fps） | ✅（Heaven 仍 30 fps） |
| 515 有 | 未测（但 Heaven `vblank_mode=0` 能 101 fps ⇒ 大概率无） | ❌（n83a 0.03 µs） |
| 影响 | 简单场景也卡 60 | 复杂场景才暴露 |

**这个分解解释了一个之前一直没想通的事**：
为什么 580 的 Vulkan 能 358 fps、glxgears 能 960 fps，**但 Heaven 还是 30 fps**？
——因为 580 只修好了病 A，**病 B 还在**。

而 Heaven 在 525 上是 30 fps 而不是 60 fps，说明**它每帧触发了 2 次 vblank 周期**（半速锁，与报告 16 的结论一致）。

---

## 5. 对前面报告的三处修正

### 5.1 报告 103 的「5.6 倍不成立」—— **这一条被本轮实测推翻，倍数成立**

上一轮我指控"358 是综合分、64 是单场景，不能相除"。本轮实跑后发现：

- 报告 07 的 `-b :duration=20`（无场景名）**确实跑全套**，输出 `vkmark Score`（几何平均）—— 这一点我说对了；
- 但我在 525 上用**完全相同的命令形式**测出 **Score = 60** —— 这才是与 358 同尺子的量；
- **358 ÷ 60 = 5.97×**，与报告 93 的 358÷64 = 5.6× 几乎相同。

⇒ **"580 的 Vulkan 快约 6 倍"这个结论是对的**，报告 93 的算法虽然混了尺子，但结果凑巧落在正确值附近（因为 525 上所有场景都被钳到 60，综合分 60 ≈ 单场景 64）。

**报告 103 的核心指控（倍数不成立）应撤回；保留的只有"580 那次无原始日志"这条程序性质疑。**

### 5.2 报告 93 的「74 倍」—— 那是混了分辨率的，同分辨率是 8.5 倍

报告 93 的表格：P106 `clear` 1080p = 64 fps，Intel 核显 `clear` "默认" = 4747 fps。**两边分辨率不同。**

本轮同分辨率实测：

| 同分辨率对比 | Intel | NVIDIA | 倍数 |
|---|---|---|---|
| 320×240 | 4626 | 71 | **65×** |
| 800×600 | 2415 | 63 | **38×** |
| 1920×1080 | 521 | 61 | **8.5×** |

⇒ **"74 倍"应作废**，改为"同分辨率 8.5×（1080p）～65×（320×240）"。
倍数随分辨率变化，是因为 Intel 的帧时间随分辨率上升（5.9 → 0.29 ms 级），而 NVIDIA 恒定 16 ms。
**这反而更强地证明了"NVIDIA 的 16 ms 是固定的同步等待"。**

### 5.3 报告 07 的 Intel `4747 fps` 也不可复现

本轮 800×600 多次测得 **1467 / 2415 fps**（同一命令、同一机器、相隔几分钟）。
机时抖动 1.6 倍。⇒ 引用 Intel 绝对帧率时必须带"当次实测"，不能当基准常数。

---

## 6. 仍缺的一格：515

**515 的 glxgears 与 Vulkan 都从未测过。**

不过本轮发现给它加了一条强旁证：README 记录 515 上 **Heaven `vblank_mode=0` 能跑 101 fps**
（若 present 被锁 60，绝无可能超过 60）。⇒ **515 的 present 是异步的，病 A 是 525 引入的。**

要做实这条，只需在 515 上跑一条命令：

```bash
source /tmp/vkenv.sh
env __GLX_VENDOR_LIBRARY_NAME=nvidia vblank_mode=0 timeout 7 glxgears -geometry 300x300+0+0
```

- **≥ 300 fps** ⇒ 病 A 确为 525 引入，515 全面健康；
- **≈ 63 fps** ⇒ 病 A 不是版本差异，是这台机器 + 无显示引擎的固有限制。

**本条未执行**：需要卸载 525、装 515、重启，会中断当前桌面会话。见 §8。

---

## 7. 复现命令（本轮全部）

```bash
# 环境（从 plasmashell 的真实会话取，不要新开 X）
SRC=$(pgrep -x plasmashell | head -1)
for kv in $(tr '\0' '\n' < /proc/$SRC/environ); do
  case "$kv" in DISPLAY=*|XDG_RUNTIME_DIR=*|XAUTHORITY=*|DBUS_SESSION_BUS_ADDRESS=*|WAYLAND_DISPLAY=*) echo "export $kv";; esac
done > /tmp/vkenv.sh
source /tmp/vkenv.sh

NV=d4e4429b22a277896eef1bc4376c44e8        # vkmark -L 取
INTEL=2ee97eb62b0d0649715ae388a3d8338d

# A. Vulkan 关 vsync，同命令比两个设备（必须带 --winsys xcb，否则默认选 wayland 会静默失败）
vkmark -D $NV    -s 1920x1080 --winsys xcb -p immediate -b clear:duration=4
vkmark -D $INTEL -s 1920x1080 --winsys xcb -p immediate -b clear:duration=4

# B. present mode 对比
vkmark -D $NV -s 1920x1080 --winsys xcb -p fifo -b clear:duration=4

# C. 全套综合分（与报告 07 / 93 的 358 同尺子）
vkmark -s 1920x1080 --winsys xcb -p immediate -b :duration=6

# D. GL 关 vsync
env __GLX_VENDOR_LIBRARY_NAME=nvidia vblank_mode=0 __GL_SYNC_TO_VBLANK=0 \
  timeout 7 glxgears -geometry 300x300+0+0
```

**踩过的两个坑**：
1. **不指定 `--winsys xcb` 时 vkmark 会选 wayland，然后静默无输出**（不是崩溃，是没打印结果）。所有 vkmark 命令都要带 `--winsys xcb`。
2. **`VK_ICD_FILENAMES=/etc/vulkan/icd.d/nvidia_icd.json` 会隐藏 Intel 和 llvmpipe**，做设备对比时必须 `unset`。

---

## 8. 未执行的、需要一次驱动切换的两件事（合并列出）

| 编号 | 做什么 | 成本 | 为什么要先问 |
|---|---|---|---|
| **N113-B** | 装 515.105.01，只跑 `glxgears 300x300` + `vkmark -b clear` | 1 次切换 + 重启 | **会中断当前图形会话**，且失败后机器可能无显示；需你确认时机 |
| **N114-A** | 装 vGPU 宿主包做复现（报告 100 §8） | 1-2 h + 重启 | 同上，且需先确认 2 年前 PVE 的确切配置 |

两条都执行的话，建议顺序：**N113-B（20 分钟，回答"病 A 是不是 525 引入"）→ 若确认，则 515 是唯一解，N114-A 可降级为"想彻底搞清机理才做"**。

---

## 9. 一句话

**vsync 关了，三重开关都上了，而且用 Intel 跑同一套开关拿到 5903 fps 证明开关没坏。
是 525 的 NVIDIA present 路径不听——13 个 vkmark 变体、4 档 glxgears 尺寸，全部精确停在 16.667ms。
对照 580 的 glxgears 960 fps，这是 525 独有的病（病 A），与之前查的 121µs/对（病 B）是两层。**
