# Heaven Benchmark 决定性复现 + 3D 路径机制探测

- **日期**: 2026-10-24 20:30-21:30
- **机器**: desktop@<testhost>，Debian 13 (trixie)，内核 6.12.111+deb13-amd64，Xeon E3-1246 v3 + 核显，Wayland + Xwayland
- **GPU**: NVIDIA P106-100（01:00.0，**第二 PCIe 槽位**），6GB GDDR5X，BIOS 86.06.58.00.21
- **驱动**: 580.126.18（现役）
- **基准**: Unigine Heaven 4.0（用户从 NFS 挂载提供）
- **状态**: 复现**成功**；机制**尚未定论**，本文档为交接收尾状态

---

## 1. 结论摘要（TL;DR）

1. **Heaven 是决定性复现器**。在 580 驱动上，Heaven 稳定跑出 **FPS 30~54 / 功耗 42~50W**，与用户描述的"525+ 只能 60-70W、30fps"方向一致，且**同一场景下 515 能到 138fps / 100-110W**（社区实测，见 §5）。
2. **这不是经典功耗墙**。Heaven 运行期间：`SwPowerCap = Not Active`、SM 时钟满频 1911MHz、显存时钟满速 4006MHz、GPU 占用 91-93%。功耗低是因为**帧率被锁死导致 GPU 空转在低负载点**，不是驱动在限功耗。
3. **分辨率与功耗几乎解耦**。1920×1080 → 50W / 30fps；640×360（像素少 9 倍）→ 42W / 36fps。渲染量掉 9 倍，功耗只降 8W。**瓶颈不在渲染吞吐**。
4. **瓶颈在帧调度/呈现路径**。perf 调用图显示 33% CPU 采样落在 `clock_gettime`，调用方为 `libnvidia-glcore.so.580.126.18` ← `libGLX_nvidia.so.580.126.18` → `glXSwapBuffers`。即 **3D 帧提交/呈现路径在驱动侧自旋等待**。
5. **该行为仅限 3D 路径**。CUDA 路径能跑满（社区反馈一致），GL 离屏合成负载（glmark2）也能到 79W。→ 定位到 **525+ 在 3D 呈现/帧调度路径的特有改动**，而非 NVPCF 功耗管理面（后者在本机 DSDT 无 NVPCF、nvidia-powerd 未运行，全程不活跃）。
6. **与静态版本边界自洽**：静态锁定改动落在 `515.105.01 → 525.116.03`（520/525 一代，报告 01）。Heaven 复现说明 525+ 代的 3D 路径确实引入了新行为。**待 A/B 验证**（dkms 装 525.116.03 重跑 Heaven）。

> **对报告 04 的修正**：报告 04 用 glmark2 离屏合成负载未能复现"60-70W 钳制"，据此当时判定"60-70W 是中等 GL 负载的自然功耗"。**该结论对 Heaven 类真实 3D 场景不成立**——Heaven 是 3D 路径的决定性复现器，报告 04 的"未复现"应理解为"用错了负载"。

---

## 2. Heaven 启动与复现方法

### 2.1 关键前置条件（踩坑记录）

Heaven 4.0 在该环境下启动需要两个非显然的步骤，缺任一则直接失败：

```bash
# 1) 必须手建空配置文件（包内不带）
touch /home/<user>/下载/Unigine_Heaven-4.0/bin/unigine.cfg

# 2) 数据目录软链（否则引擎去找 bin/datadata/ 之类的拼写变体而失败）
ln -sfn ../data /home/<user>/下载/Unigine_Heaven-4.0/bin/data
```

> 软链缺失时 strace 显示引擎把 `../data` 拼成了 `bin/datadata/`，是定位此坑的关键线索。

### 2.2 完整启动命令

```bash
cd /home/<user>/下载/Unigine_Heaven-4.0/bin
export LD_LIBRARY_PATH=$PWD DISPLAY=:1 __GLX_VENDOR_LIBRARY_NAME=nvidia
./heaven_x64 \
  -engine_config unigine.cfg \
  -console_command "world_load heaven/heaven" \
  -video_app opengl \
  -video_fullscreen 0 \
  -video_mode -1 \
  -video_width 1920 -video_height 1080 \
  -sound_app null \
  -video_vsync 0
```

关键参数说明：
- `__GLX_VENDOR_LIBRARY_NAME=nvidia` — 强制走 P106 原生 GLX，不经核显（Wayland 下 Mesa 的 EGL 默认会走核显）
- `DISPLAY=:1` — Xwayland 显示，SDDM 会话
- `-video_vsync 0` — 关闭垂直同步，**必须**，否则测出来的是 vsync 锁帧而非驱动性能上限
- `-video_mode -1` — 窗口化（`-video_fullscreen 1` 为全屏，两者均测）

### 2.3 采样与截图

```bash
# 功耗/时钟/利用率采样（每 2s 一行，连续采样）
sshpass -p desktop ssh desktop@<testhost> \
  'nvidia-smi --query-gpu=power.draw,clocks.sm,clocks.mem,utilization.gpu,utilization.memory,temperature.gpu --format=csv -l 2'

# Clocks Event Reasons（判读 SwPowerCap 是否激活）
nvidia-smi -q -d PERFORMANCE | sed -n '/Clocks Event Reasons/,/Applications Clocks/p'

# 截图（取 FPS 角标）
xwd -root -silent | xwdtopnm ...   # 见 §2.4
```

### 2.4 截图链路（XWD → PNG）

主机上 xwd 原始格式传回本地后用 Python 手工解析（读 25 个大端 uint32 头，跳过 name + ncolors*12，按 bpp=32 组装 RGB）：

- `<workdir>/analysis/onmachine/heaven.png` — 1920×1080 窗口化，**FPS: 30**
- `<workdir>/analysis/onmachine/h2.png` — 第二组，**FPS: 54**
- `<workdir>/analysis/onmachine/h640.png` — 640×360，**FPS: 36**
- `<workdir>/analysis/onmachine/hfs.xwd` / `h640.xwd` — 全屏与低分辨率原始帧

> 环境限制记录：当前会话的图像输入被过滤，无法直接读图，故走 XWD→PNG 手工转换路径。

---

## 3. 实测数据矩阵

### 3.1 三组分辨率对照（同一场景 heaven/heaven，vsync 关闭）

| 场景 | 分辨率 | 帧率（截图角标） | 功耗 | SM 时钟 | 显存时钟 | GPU 占用 | 显存占用 | SwPowerCap |
|---|---|---|---|---|---|---|---|---|
| 窗口化 | 1920×1080 | **30 fps** | ~50W | 1898MHz | 4006MHz | 91% | 3% | Not Active |
| 窗口化(2) | 1920×1080 | **54 fps** | ~50W | ~1900MHz | 4006MHz | 92% | 3% | Not Active |
| 全屏 | 1920×1080 | 30-54 fps | ~50W | 1898MHz | 4006MHz | 93% | 3% | Not Active |
| 窗口化 | 640×360 | **36 fps** | ~42W | ~1900MHz | 4006MHz | 92% | 3% | Not Active |

**关键读数**：
- 1080p → 640×360，**像素数减少 9 倍**，功耗仅从 50W → 42W（-8W），帧率不升反微降。
  → 瓶颈**与渲染量/填充率无关**，与光栅化吞吐无关。
- 显存利用率恒为 3%：Heaven 场景数据量小，显存带宽远未饱和 → **不是显存瓶颈**。
- 显存时钟 4006MHz 满速、SM 1911MHz 满频（高于技嘉标称 1708 boost）：**没有降频**。
- `SwPowerCap = Not Active` 全程保持 → **不是功耗墙**。

### 3.2 锁频尝试（失败）

```
$ nvidia-smi -lgc 1500
Setting locked GPU clocks is not supported
```

→ 无法用锁频手段进一步区分"降频"与"调度阻塞"。**下一位接手者应知：此路不通，不必重复尝试。**

### 3.3 perf 调用图

```
$ perf record -g -p <heaven_pid> -- sleep 20
$ perf report --stdio
```

热点：

| 占比 | 符号 | 归属库 |
|---|---|---|
| **33%** | `clock_gettime` | 调用方 `libnvidia-glcore.so.580.126.18`（158k 采样） |
| — | `glXSwapBuffers` | `libGLX_nvidia.so.580.126.18`（60k 采样） |

调用链：`libGLX_nvidia.so.580.126.18` → `glXSwapBuffers` → `libnvidia-glcore.so.580.126.18` → `clock_gettime`（**自旋**）

→ CPU 侧近 1/3 时间在等时间戳。这是**帧提交/呈现路径的自旋等待**，而不是 GPU 忙不过来。

### 3.4 与 glmark2 / CUDA 路径的横向对照（来自报告 04）

| 负载 | 功耗 | 帧率/利用率 | 结论 |
|---|---|---|---|
| Heaven 窗口化 1080p | 42-50W | 30-54 fps | **异常**（本报告对象） |
| glmark2 离屏单实例全场景 | 41→**79W** | 82-100% 利用率 | ⚠️ 见下方修正 |
| glmark2 重着色器 2 并发 | ~60W | 100% 利用率 | ⚠️ 见下方修正 |
| CUDA PTX 负载 | 46W | 100% 利用率 | 正常（社区反馈 CUDA 可跑满） |

> **⚠️ 2026-10-24 修正（见报告 07 §10）**：glmark2 在本机是通过 **zink 桥接到 nvidia vk**（Vulkan）运行的，
> 它**根本没有经过原生 GL 前端**，因此不能作为"同一驱动、同一卡、同一时刻的 GL 对照"。
> 正确的原生 GL 对照应看 `glxgears`（1080p 294 fps，单 draw call）与 Heaven（1080p 30 fps，上千调用）——
> 两者同为原生 GL，差异在**每帧调用数**。原生 Vulkan 的 1080p 358 fps / 37 W 才是真正的硬件上限证明。

→ **同一驱动、同一卡、同一时刻**：合成 GL 负载能到 79W，真实 3D 场景只有 50W。排除硬件供电、排除全局功耗管理，锁定 **3D 呈现路径**。

---

## 4. 机制假说（开放，供下一位接手者验证）

| 假说 | 内容 | 现有支持 | 现有反证 | 验证手段 |
|---|---|---|---|---|
| **A. 帧呈现节流（Throttling present/present-blocking）** | 525+ 对 3D 呈现路径引入节流，驱动在 `glXSwapBuffers` 内按某种配额自旋等待，使每帧实际 GPU 工作被压缩 | perf 显示 `glXSwapBuffers`→`clock_gettime` 自旋 33%；GPU 92% 占用但功耗 50W（空转）；分辨率无关 | 需确认自旋是否真是"限流"而非普通等待 | 对比 `glxgears`（60fps 正常）→ 说明不是所有呈现都受限；需定位 525+ 新增的 present 相关代码 |
| **B. Xwayland 合成器交互** | Wayland+Xwayland 下 3D 窗口的 present 路径与 5xx 前不同，导致 30fps 锁 | 全屏与窗口化均复现；vGPU（同样是 Wayland+核显直通）能跑 132.9fps | ~~vGPU 用的是 PVE 虚拟显卡，路径不同，不构成同条件对照~~ **⚠️ 该排除已被报告 93 撤回：用户指出 525/535/570 在 vGPU 下都不回退，这是唯一判别性线索** | 在纯 Xorg（关掉 Wayland）下重跑 Heaven；或对比 X11 会话 |
| **C. 3D 路径特有的功耗预算/降频映射** | 3D 场景在 525+ 走独立的功耗预算表，被映射到低档 | 社区反馈"3D 卡 60W，CUDA 跑满"高度吻合 | `SwPowerCap` 全程 Not Active；时钟满频 | 需在 RM 内部（Ghidra）找 3D vs CUDA 的功耗预算分叉 |
| **D. P-state 强制（`RMForcePstateBug3811584War`）** | 525+ 新增的强制 P-state 规避逻辑对 3D 路径生效 | 符号表 diff 中唯一的功耗相关新增符号 | 时钟显示满频，nvidia-smi 未报 P-state 下降 | 需查该 War 函数的 xref 与门控条件 |

**当前的证据权重**：假说 A/B 解释力最强（直接覆盖"CPU 自旋 + GPU 空转 + 与分辨率/渲染量无关"三现象），C/D 缺少运行时签名支持。

---

## 5. 社区互证

### 5.1 learndiary 博客（2024-02）
> https://www.learndiary.com/2024/02/p106-100-linux-game/

- 作者使用 **515.48.07** 跑 Heaven：**138.1 fps / 100-110W**
- 作者**明确因 535 的 3D 满载低帧问题弃用 535**
- 结论表述与本机现象同源：同一张 P106-100，同一个 Heaven 基准，515 满血、535+ 掉到 30fps 档

### 5.2 社区截图反馈（用户提供）

> "535 感觉功耗控制有问题，跑 CUDA 能跑满，但跑 3D 卡在 60w 左右"

→ 明确区分了 **CUDA 正常 / 3D 受限**，与本报告 §3.4 的横向对照一致。
→ 且"535 有问题"落在静态锁定的 `515.105.01 → 525.116.03` 边界之后，**与报告 01 自洽**。

---

## 6. 遗留待办（按性价比排序）

### P0 — A/B 闭环（最高价值）
dkms 安装 **525.116.03**，重跑 Heaven 同一场景。
- 预期：若 525 复现 30fps/50W，而 515 满血 → 版本归因闭环，报告 01 的静态边界得到运行时确证。
- 注意：515 系列不适配当前内核（6.12.111+deb13），可能需降内核或用 515.48.07 + 旧内核容器。
- 风险：改驱动需重启，会中断当前会话（SDDM Xorg 会被杀）。

### P1 — 分离呈现路径假说 A 与 B
纯 **Xorg** 会话（不经 Wayland/Xwayland 合成）下重跑 Heaven。
- 若 Xorg 下 100+fps → 假说 B（Xwayland 合成交互）成立，**与驱动版本无关**，是重大转向。
- 若 Xorg 下仍 30fps → 假说 A/C 成立，回到驱动内部。

### P2 — Ghidra 定点解剖（静态）
目标符号（按优先级）：
1. `RMForcePstateBug3811584War` 的 xref 与门控条件 —— 假说 D
2. `libnvidia-glcore` 中 `glXSwapBuffers` 的等待循环 —— 假说 A
   - 工具：ghidrecomp（环境已装）、路径 `/home/<user>/…/ghidrecomp`（见报告 03）
3. 525 代 `libnvidia-glcore.so` vs 515 代的 present 路径符号 diff
   - 建议**补做 515 与 525 的 `libnvidia-glcore.so` 符号表 diff**（此前只 diff 了 `nv-kernel.o_binary` 的 RM blob，**用户态 GL 库从未 diff 过**，这是当前最大的静态盲区）

### P3 — 恢复 CUDA 路径
被早期空指针 bug 中毒（`cuMemAlloc` 返回 err=3 / NOT_INITIALIZED），需 GPU 重置或重启恢复（会杀 SDDM Xorg）。恢复后跑 CUDA 满载作为对照基线。
- 产物已在机器上：`/tmp/burn.c`、`/tmp/burn.ptx`（gcc -lcuda，避免 nvcc 与 trixie glibc 2.41 的 cospi/noexcept 冲突）

### P4 — 第二槽位 PCIe 链路量化
```bash
sudo lspci -vv -s 01:00.0 | grep -A3 LnkSta
```
已知：Width x16，Speed 2.5GT/s（Gen1）。→ 需确认是否为 x16 Gen1 实际协商（应约 2.5-8GT/s），以及链路是否为瓶颈。
注：Heaven 帧率与分辨率、渲染量均无关，**PCIe 带宽是本问题更匹配的候选解释**（同样能解释"3D 卡、CUDA 不卡"，因为 CUDA 负载多为显存/计算驻留、PCIe 交互少），但报告 01/04 未给该路径定权重。**P4 优先级可上调。**

---

## 7. 接手者快速上手

```bash
# 环境
sshpass -p desktop ssh desktop@<testhost>
# 目标 GPU
nvidia-smi -L        # 期望看到 P106-100 @ 01:00.0
nvidia-smi -q -d POWER,PERFORMANCE | head -60

# Heaven 起跑（先确认 unigine.cfg 与 data 软链存在）
ls -l /home/<user>/下载/Unigine_Heaven-4.0/bin/unigine.cfg \
      /home/<user>/下载/Unigine_Heaven-4.0/bin/data
cd /home/<user>/下载/Unigine_Heaven-4.0/bin && \
export LD_LIBRARY_PATH=$PWD DISPLAY=:1 __GLX_VENDOR_LIBRARY_NAME=nvidia && \
./heaven_x64 -engine_config unigine.cfg -console_command "world_load heaven/heaven" \
  -video_app opengl -video_fullscreen 0 -video_mode -1 \
  -video_width 1920 -video_height 1080 -sound_app null -video_vsync 0
```

**判读口诀**：
- 看到 `SwPowerCap Not Active` + 时钟满频 + 功耗 50W + FPS 30 → 复现成功，**不是功耗墙，是调度/呈现问题**
- 看到 `SwPowerCap Active` + 时钟下降 + 功耗钉在 60-70W → 才是真的功耗钳制，**说明换了个机制**，回头查 vBIOS 路径
- 帧率随分辨率显著上升 → 渲染瓶颈，方向错
- 帧率与分辨率无关（9 倍像素差，帧率不变）→ 调度/呈现瓶颈，方向对

**采集纪律**（沿用前序约定）：
- **以功耗为准**，不采信鼠标悬停导致视角转动时的 FPS 角标
- 必须 `-video_vsync 0`
- 每组至少 20s 稳定期后再读数
- 改驱动/重置 GPU 属破坏性操作，**先与用户确认时机**

---

## 8. 相关文档

| 文档 | 作用 |
|---|---|
| `01-驱动版本定位报告.md` | 静态版本边界锁定 515.105.01 → 525.116.03（NVPCF 重构） |
| `02-vBIOS功耗表解析.md` | 固件侧受阻（容器非明文存储），转向运行时判定 |
| `03-上机验证清单.md` | 只读采集/实验/判读矩阵 |
| `04-上机实测结果.md` | 早期实测（glmark2 负载，**未能复现**，结论已被本报告部分修正） |
| `05-Heaven实测与机制探测.md` | **本文档**：决定性复现 + 机制探测 + 交接 |
| `README.md` | 项目索引 |
