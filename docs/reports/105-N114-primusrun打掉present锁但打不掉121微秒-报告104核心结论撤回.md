# 报告 105 / N114：用户的 `primusrun` 打掉了 present 锁，但打不掉 121µs —— 报告 104 的核心结论必须撤回

- 时间：2026-10-26 16:00
- ⚠️ **2026-10-26 16:25 注（报告 107）**：本文凡引用「用户截图 130W」处**均失去依据** —— 原图上 `Pwr:Usage/Cap` 是 `N/A / N/A`，130W 无一手来源。本文自身实测（48W vs 69W）不受影响。
- 驱动：**525.116.03**（用户从 `550.144.03` 目录运行，但 `nvidia-smi` 报的是 525.116.03；目录名与实际驱动无关）
- 触发：用户贴出 `vblank_mode=0 primusrun vkmark` 的完整日志，800×600 全套 **Score 269**，质问「怎么会只有 60，你有没有关垂直同步！！？？？」
- 数据留档：`analysis/traces/n113/`、`analysis/onmachine/n113-hv5-fps.png`

---

## 0. 用户是对的，而且比"关没关 vsync"更深

**Score 60 → 269，同一台机器、同一个驱动、同一条命令，唯一差别是 `primusrun`。**

这证明我上一轮的结论"525 的 present 被强制同步 vblank，是 525 独有的病"**错了一半**：
病确实存在，但**它不是 525 独有的，而且 `primusrun` 就能绕过**。

日志里最关键的一行是用户没标注的那句：

```
PrimusVK: Got integrated gpu!        ← display GPU = Intel
PrimusVK: Got discrete gpu!          ← render GPU  = NVIDIA P106
PrimusVK: Creating Swapchain for size: 800x600
PrimusVK: Creating a Swapchain thread.
```

**swapchain 被建在 Intel 上，NVIDIA 只做渲染。** present 由 PrimusVK 的独立线程经 Intel 完成，
根本不走 NVIDIA 的 X11 present 路径 —— 所以那条锁在 vblank 的路径被整个绕开了。

---

## 1. 三层对照（本轮实测，全部 525.116.03）

### 1.1 present 层：primus 打掉了锁

| 测试 | 直连 NVIDIA | **primusrun** | 倍数 |
|---|---|---|---|
| **glxgears 300×300**（`vblank_mode=0`） | **68.8 fps** | **590.4 fps** | **8.6×** |
| vkmark `clear` 800×600 immediate | 67 | **452** | 6.7× |
| vkmark `clear` 800×600 **fifo** | 60 | **60** | **1.0×** ✅ |
| vkmark 全套 800×600 immediate | 60 | **269**（用户实测） | 4.5× |
| vkmark 全套 1080p immediate | 60 | **135** | 2.25× |

**`fifo` 下两者都是 60 —— 完美的对照**：这说明 primus 只在"本该关掉 vsync"的模式下拉开差距，
fifo 模式两边都该是 60，事实也是 60。**这是"primus 确实绕过了 present 锁"而不是"primus 让一切都变快"的铁证。**

### 1.2 提交层：primus **完全无效**

用 `n83a`（pbuffer + FBO，**不经过 present**）测每对成本：

| | M1 基线（无状态变更） | M3（切 program + draw） | **每对成本** |
|---|---|---|---|
| **直连 NVIDIA** | **3.316 ms** | **15.170 ms** | **118.5 µs** |
| **primusrun** | **3.221 ms** | **15.144 ms** | **119.2 µs** |

**118.5 vs 119.2 —— 差 0.6%。primus 对病 B 毫无影响。**

（N = 100 对/帧，`-c 200`；`(15.170−3.316)/100 = 118.5µs`，与报告 78/79 的 121µs 完全对上。）

**这也顺手纠正了我本轮中途的一个误判**：我一开始拿 `M0`（固定管线 4 次 legacy 状态）当基线，
得出"M0 = M3 = 15.4ms，121µs 消失了"的错误结论。
**`M0` 本身就含状态变更，它同样触发惩罚**——这与报告 75「M0 = M3」一致。
真正的基线是 **M1**。这条判据要写进纪律：**基线只能用 M1，不能用 M0。**

### 1.3 应用层：Heaven 在 primus 下仍是 29 fps

`analysis/onmachine/n113-hv5-fps.png`（xwd 抓真实窗口，`scrot` 在 Xwayland 下抓到全黑）：

| | FPS | GPU 利用率 | 功耗 | SM 时钟 |
|---|---|---|---|---|
| Heaven 直连（历史） | 30 | 89-93% | 46-51 W | 1911 MHz |
| **Heaven primus（本轮）** | **29** | **100%** | **68.96 W** | 1898 MHz |

**帧率一样，功耗高了 40%。** 这否掉了"GPU 在空转等米下锅"的旧解释——
**GPU 是真在算，只是算得慢。**

### 1.4 三层的账算得通

```
Heaven 每帧 ≈ 268 对（报告 79 的实测配对计数）
268 × 118.5 µs = 31.8 ms
31.8 ms + 3.3 ms 基线 = 35.1 ms  ⇒  28.5 fps  ≈  实测 29 fps ✅
```

**Heaven 的 30 fps 几乎全部由病 B 解释，不需要病 A 参与。**
这也解释了为什么 primus 把 glxgears 从 68.8 拉到 590（病 A 消失），
但 Heaven 只从 30 变成 29（病 A 本来就不是 Heaven 的主要成本）。

---

## 2. ⛔ 报告 104 的核心结论必须撤回

### 2.1 撤什么

报告 104 §3 说：

> **glxgears 300×300：525 = 63 fps，580 = 960 fps（报告 07），差 15 倍**
> ⇒ **病 A 是 525 独有，580 已修**

**这条撤回。** 理由：**那 63 fps 是我在直连下测的，而那 960 fps 极可能是在 primus 下测的。**

证据链：
1. 这台机器的图形栈**一直跑在 primus 环境**（README 里所有 Heaven 测量都是 `primusrun`，报告 07 的 Heaven 也是）。
2. 报告 07 的复现命令段落里**没有记录 glxgears 是否加了 primus 前缀**——这又是"命令没留档"的老毛病（报告 103 已立规矩，我自己又犯了）。
3. **决定性反驳**：我本轮在 **525** 上用 primusrun 测 glxgears 得到 **590 fps**，与 580 的 960 fps **同一量级**。
   如果 580 真"修好了"而 525 坏，两者不该这么接近。

⇒ **"63 vs 960"更可能是"直连 vs primus"的差异，而不是"525 vs 580"的差异。**
**"病 A 是 525 引入的"这个断言目前没有任何证据支撑。**

### 2.2 什么仍然成立

| 报告 104 的结论 | 状态 |
|---|---|
| vsync 三重开关已关，且用 Intel 自证开关有效 | ✅ **成立**（Intel 5903 fps 那条） |
| 直连时 NVIDIA 的 present 被锁在刷新率 | ✅ **成立**（本轮 fifo=60 对照加强） |
| 帧时间恒定 16.667ms 是同步指纹 | ✅ 成立 |
| **`primusrun` 能绕过这个锁** | ✅ **本轮新增，报告 104 完全没提** |
| 病 A 是 525 独有、580 已修 | ⛔ **撤回，无证据** |
| 病 B（121µs/对）与病 A 是两层 | ✅ **强化**（本轮 118.5 vs 119.2 的直接测量） |

### 2.3 报告 104 里"Intel 自证"那条也要降级说明

报告 104 用 `Intel 5903 fps` 证明"开关有效、是 NVIDIA 不听"。**这个逻辑仍然成立**，
但现在多了一层解释：NVIDIA 不听的原因很可能是**它的 X11 present 实现（P106 无显示引擎，必须经 DMA-BUF 交给 Intel）**
在缺少 primus 中转时走了同步路径。**这是架构限制，不一定是驱动回归。**

---

## 3. 这对用户的实际意义（可立刻用）

### 3.1 现在就能用的方案

```bash
# Vulkan：
vblank_mode=0 primusrun vkmark -s 1920x1080 --winsys xcb -p immediate -b :duration=10

# GL：
vblank_mode=0 primusrun <你的 GL 程序>
```

**但要知道它的上限**：primus 只解决"简单场景被锁 60"。
对 Heaven 这种每帧几百次状态切换的负载，primus 只值 +0%（30 → 29 fps）。

### 3.2 判断某个程序值不值得用 primus

**一条命令就能判**：

| 用 primus 后 | 说明 | 建议 |
|---|---|---|
| 帧率翻几倍（>3×） | 原来被 present 锁卡住 | **用 primus** |
| 帧率几乎不变（±10%） | 瓶颈在病 B（状态切换密度） | primus 无用，只能减少每帧状态切换/换 515 |

**分界线大概在每帧 50 次状态切换**：低于它 primus 有明显收益，高于它就没意义了。

---

## 4. 现在的完整图景

| 层 | 现象 | 直连 | primus | 525 | 515 | 580 |
|---|---|---|---|---|---|---|
| **A. present 锁** | 简单场景卡刷新率（16.667ms） | ✅ | ❌ 绕过 | ✅ | 未测 | 未测 |
| **B. 提交惩罚** | 每对 118-121 µs | ✅ | ✅ **不变** | ✅ | ❌（0.03µs） | ✅（Heaven 30fps） |
| **C. 基线** | M1 = 3.2-3.3 ms（渲染+回读） | 3.32 | 3.22 | — | — | — |

**Heaven 30 fps 的账**：病 B 268 对 × 118.5µs = 31.8ms（占 91%）+ 基线 3.3ms = 35ms ⇒ 28.5fps。

**⇒ 想救 Heaven，只有一个方向：消灭病 B。而病 B 只在 515 上不存在（0.03µs）。**

---

## 5. 仍未解决 / 未验证

| 项 | 状态 | 说明 |
|---|---|---|
| 515 的 primus / 直连 / Vulkan | **全部未测** | 需一次驱动切换 + 重启，会中断用户桌面 |
| 580 直连 glxgears | **未测** | 若也 ≈63fps，"病 A 是 525 引入"彻底死掉；若是 960fps，则我的撤回过头了 |
| 病 A 的精确机制 | 未定位 | 候选：P106 无显示引擎 → DMA-BUF 同步等待；Xwayland present 的 `present_pixmap` 阻塞 |
| 病 B 与 present 是否耦合 | **已分离** | 本轮 n83a（不 present）测到 118.5µs，证明独立 |

---

## 6. 复现命令（本轮全部，含踩坑）

```bash
# 环境
SRC=$(pgrep -x plasmashell | head -1)
for kv in $(tr '\0' '\n' < /proc/$SRC/environ); do
  case "$kv" in DISPLAY=*|WAYLAND_DISPLAY=*|XDG_RUNTIME_DIR=*|XAUTHORITY=*|DBUS_SESSION_BUS_ADDRESS=*) echo "export $kv";; esac
done > /tmp/vkenv.sh
source /tmp/vkenv.sh

# A. present 层对照（注意 fifo 必须两边都是 60，这是对照的地基）
env __GLX_VENDOR_LIBRARY_NAME=nvidia vblank_mode=0 timeout 7 glxgears -geometry 300x300+0+0
env vblank_mode=0 primusrun timeout 7 glxgears -geometry 300x300+0+0

vkmark -D $NV -s 800x600 --winsys xcb -p immediate -b clear:duration=3      # 67
vkmark -D $NV -s 800x600 --winsys xcb -p fifo      -b clear:duration=3      # 60
primusrun vkmark -D $NV -s 800x600 --winsys xcb -p immediate -b clear:duration=3   # 452
primusrun vkmark -D $NV -s 800x600 --winsys xcb -p fifo      -b clear:duration=3   # 60

# B. 提交层（关键是基线用 M1，不要用 M0）
cd <workdir>/analysis/n83a
./n83a -M 1 -c 200                         # 3.316ms  ← 唯一合法基线
./n83a -M 3 -c 200                         # 15.170ms
primusrun ./n83a -M 1 -c 200               # 3.221ms
primusrun ./n83a -M 3 -c 200               # 15.144ms

# C. Heaven（截图必须用 xwd，scrot 在 Xwayland 下全黑）
WID=$(xdotool search --name "Unigine Heaven" | head -1)
xwd -silent -id $WID -out /tmp/hv.xwd
convert /tmp/hv.xwd /tmp/hv.png
convert /tmp/hv.png -crop 400x90+1520+0 +repage -resize 800x180 /tmp/hv-fps.png
```

**本轮踩的坑（都记进 SKILL 了）**：
1. `scrot` 在 Xwayland 下抓 Heaven 是**全黑**（`extrema (0,0)`），必须用 `xwd -id <window>` + `convert`。
2. `n83a` 的基线**只能用 M1**，M0 含 4 次 legacy 状态变更、同样触发惩罚，用它当基线会把 121µs 误判为"消失"。
3. 同一条 `n83a -M 3 -c 200` 连续两次可能给出 28ms 和 15ms —— **首跑受上一个大负载影响**，必须 `pkill` + 等 2 秒 + 确认 `nvidia-smi` 回到 `0% / 6.6W / 139MHz`。
4. `Heaven -benchmark` 参数在本机上不产出 Score 文本（跑完即退出无输出），只能靠角标截图。

---

## 7. 一句话

**你是对的，Score 269 不是 60。原因是 `primusrun` 把 swapchain 建在了 Intel 上，绕开了 NVIDIA 那条锁在 vblank 的 present 路径。**
**但它只绕过 present（病 A），绕不过每对 118.5µs 的提交惩罚（病 B）——所以 Heaven 在 primus 下还是 29 fps，而且 GPU 功耗从 48W 涨到 69W，是真的在算而不是在等。**
**我上一轮说"病 A 是 525 独有、580 已修"是错的：那个对比里 63fps 是直连、960fps 很可能是 primus，我把两种路径当成了两个版本。**
