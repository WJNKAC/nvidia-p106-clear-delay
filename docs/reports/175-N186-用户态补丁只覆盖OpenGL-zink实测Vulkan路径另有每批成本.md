# N186 — 用户态补丁只覆盖 OpenGL：zink 实测证明 Vulkan 路径另有每批成本

**日期**：2026-10-31
**驱动**：580.178.04 / kernel 6.12.113+deb13-amd64 / P104-100
**状态**：只用户态清除延迟（内核白名单**未打**，PCIe 未解锁 Gen2 = LnkSta 2.5GT/s x4）
**问题**：用户态那 1 字节补丁，对走 Vulkan 的程序有没有用？

---

## 0. 结论

**没有用。** zink（GL→Vulkan）实测：改了 glcore 之后每批成本**从 143.5 µs 变成 142.0 µs，
差 1%，等于没变**。

| 路径 | glcore 原厂 `je` | glcore 补丁 `eb` | 补丁效果 |
|---|---|---|---|
| **native OpenGL** | 147.1 µs/batch | **21.8 µs/batch** | ✅ 6.7× |
| **zink（Vulkan）** | 143.5 µs/batch | **142.0 µs/batch** | ❌ 无变化 |

而「zink 的 142 µs」和「native 未打补丁的 147 µs」是同一量级 ——
**⇒ Vulkan 路径确实存在每批固定成本，但它不在 glcore 里被跳过的那段代码上。**

这正是 10-30 七日世界那一幕的机制解释：游戏走 DXVK（Vulkan），
glcore 补丁在位仍然只有 19 fps；只有打上**内核白名单**才到 60 fps。
本实验说明那不是巧合，是必然。

---

## 1. 为什么要用 zink

要判断 Vulkan 路径是否受限，需要一个「同一个负载，既能走 GL 又能走 Vulkan」的桥梁。
zink 正好是 Mesa 的 GL-on-Vulkan：GL 调用由 **Mesa** 处理，转成 Vulkan 命令交给
NVIDIA 的 Vulkan 驱动。

关键性质：**zink 完全绕开 `libnvidia-glcore.so` 里的 GL 代码**
（那段 pushbuffer 构建逻辑根本不会执行），但底层仍然用 NVIDIA 的 Vulkan ICD。
于是「改 glcore 补丁前后 zink 有没有变化」就成了干净的判据。

> ⚠ 第一个坑：zink 默认会挑 **Intel 核显**，不是 P104。
> `glxinfo -B` 显示 `zink Vulkan 1.2(Intel HD Graphics P4600)` ——
> 这时测到的数字跟 P104 毫无关系。必须显式指定：
> `MESA_VK_DEVICE_SELECT=10de:1b87`

---

## 2. 方法

自写探针 `batchprobe.c`（见 §8）：每帧循环 N 次 `{ glDrawArrays; glFlush; }`，
人为制造 N 个批边界，`glFinish` 后计时取 p50。可选 `-s` 让**每个 draw 后都 `glFinish`**，
强制每 draw 一个真实同步点。

- `per_batch = p50 / N`；若每批有固定成本 T，则帧时间 ≈ N·T ⇒ 斜率即 T
- 用 pbuffer，不开可见窗口
- 三种运行配置：
  - native：`__GLX_VENDOR_LIBRARY_NAME=nvidia` + `__NV_PRIME_RENDER_OFFLOAD=1`
  - zink：`MESA_LOADER_DRIVER_OVERRIDE=zink` + `MESA_VK_DEVICE_SELECT=10de:1b87`
  - Intel 对照：zink + `MESA_VK_DEVICE_SELECT=8086:041a`

**阳性对照**：临时把 `libnvidia-glcore.so` 换回原厂（`0xb51c82` = `0x74 je`），
验证探针真能测出那条延迟；测完立即恢复（脚本带 `trap`，无论怎么退出都还原）。

---

## 3. 数据

### 3.1 native OpenGL（`-s`，每 draw 同步）

| glcore | n=100 | n=200 | n=400 | per_batch |
|---|---|---|---|---|
| **补丁 `eb`** | 2.152 ms | 4.026 ms | 8.727 ms | **21.5 / 20.1 / 21.8 µs** |
| **原厂 `je`** | 14.549 ms | 29.822 ms | 58.607 ms | **145.5 / 149.1 / 146.5 µs** |

⇒ 补丁把每批成本从 ~147 µs 压到 ~21 µs，**约 6.7 倍**。
（21 µs 里还含 `glFinish` 自身的往返开销，所以不是「零成本」）

### 3.2 native OpenGL（不加 `-s`）

| glcore | n=100 | n=400 | 现象 |
|---|---|---|---|
| **补丁 `eb`** | 0.673 ms | 0.661 ms | **总时间恒定**，不随 N 增长 |
| **原厂 `je`** | 13.304 ms | 53.160 ms | 线性增长，**≈133 µs/batch** |

补丁在位时 `glFlush` 不足以把 batch 拆开（驱动会合并），所以总时间恒定；
原厂时每批都要付 133 µs，于是自然线性增长。
**133 µs 与项目历史记录的 130 µs 一致** ⇒ 探针口径可信。

### 3.3 zink（GL→Vulkan，P104）

| glcore | n=100 | n=200 | n=400 | n=800 | per_batch |
|---|---|---|---|---|---|
| **补丁 `eb`** | 13.841 ms | 28.283 ms | 56.548 ms | 112.517 ms | **138.4 / 141.4 / 141.4 / 140.6 µs** |
| **原厂 `je`** | 3.578 ms* | 27.764 ms | 57.477 ms | — | 35.8* / **138.8 / 143.7 µs** |

\* n=100 那个点偏低（shader/pipeline 缓存刚建好、批还没真正拆开），n≥200 才稳定。
对比 n=200 / n=400：**补丁前后 141.4 vs 138.8、141.4 vs 143.7 —— 差异在噪声内。**

### 3.4 Intel 核显 zink（对照组）

| n=100 | n=400 | n=800 |
|---|---|---|
| 55.4 µs | 69.8 µs | 77.1 µs |

**P104 的 zink ≈ 142 µs，Intel 核显 zink ≈ 65 µs，差约 77 µs。**
两者的 zink/Mesa 是同一份、CPU 是同一颗 ⇒ 那 77 µs 的差额落在
**NVIDIA Vulkan 驱动 + GPU 侧**，不是 zink 的转换开销。

### 3.5 GPU 侧指标（n=400 运行中采样）

| 组 | GPU 利用率 | 功耗 | SM/MEM 时钟 |
|---|---|---|---|
| native + `eb` | 63 % | 68.7 W | 1911 / 5005 |
| native + `je` | 94–95 % | 70.0 W | 1911 / 5005 |
| zink + `je` | 84–92 % | 70.3 W | 1911 / 5005 |
| zink + `eb` | 93–96 % | 70.4 W | 1911 / 5005 |

有意思的是**四组功耗都只有 ~70 W**（这卡满载能到 110–150 W），
GPU 利用率却接近满 —— 和 README 里「GPU 显示 100 % 占用、功耗只有 60 W」的描述一致。
说明这批时间 GPU 是「被占着但没在出活」，不是真的在算。

---

## 4. 分析

### 4.1 为什么改 glcore 对 zink 毫无影响

调用链不同：

```
native OpenGL :  程序 → libGLX_nvidia → libnvidia-glcore.so（含被跳过的发射逻辑）→ GPU
zink          :  程序 → Mesa(libGL) → zink → Vulkan → libGLX_nvidia(Vulkan 入口) → GPU
```

`0xb51c82` 那个 `jmp` 跳的是 **glcore 内部 GL pushbuffer 的构建分支**。
zink 从不调用那条路径，所以改它等于没改 —— 142 µs 纹丝不动。

### 4.2 那 Vulkan 的 142 µs 是什么

现在只能说清它的性质，还说不清它的具体位置：

- **不是 zink 的 CPU 开销** —— Intel 对照只有 65 µs
- **不是 glcore 里那段代码** —— 改它前后差 1 %
- **量级与 GL 侧的每批成本相同**（142 vs 147 µs）
- **GPU 被占满但功耗只有 ~70 W**

⇒ 高度怀疑同一个「受限 SKU」机制在 **Vulkan/RM 内部还有一条独立实现**，
而不是复用 glcore 里那段。这也和内核白名单能一次治好两条路径的事实吻合
（内核侧置位的是策略位，两个用户态后端都会读）。

**这仍是推断，尚未直接证据**（没抓到 Vulkan 提交里的 `method 0x3998`）。
要坐实得对 Vulkan 提交做 pushbuffer 抓取，属于下一步工作。

---

## 5. 与 10-30 七日世界实测的印证

| 事实 | 本实验的解释 |
|---|---|
| 七日世界走 DXVK（Vulkan），glcore 补丁在位仍 19 fps | zink 对照说明 glcore 补丁对 Vulkan 无效 |
| 打上内核白名单后升到 60 fps | 内核是上游，两条用户态路径都受影响 |
| 用户态 glcore 补丁在 Heaven（OpenGL）上一直有效 | native GL 路径确实被覆盖 |

**⇒ 两者不矛盾：补丁有效，只是覆盖面只到 OpenGL。**

---

## 6. 局限（别过度外推）

1. **zink 是 GL 经 Mesa 转 Vulkan**，不是原生 Vulkan 程序。
   它的 142 µs 里混有 zink 自己的提交开销（约 65 µs），
   所以**不能直接说「Vulkan 也有 122 µs 延迟」**，只能说「多了约 77 µs 且与 glcore 无关」。
2. 测的是**合成负载**（每帧 N 个全屏三角形 + flush），不是真实游戏。
3. 只测了 P104-100 / 580.178.04 一套组合。
4. 未抓 Vulkan pushbuffer，**机制位置未确认**。
5. 全程 PCIe 是 **Gen1**（LnkCap 只有 2.5GT/s），未混入 Gen2 变量。

---

## 7. 实践建议

| 程序走什么 | 该打哪个补丁 |
|---|---|
| OpenGL（Heaven、原生 GL 应用、GL 游戏） | 用户态 glcore 补丁够用 |
| **Vulkan**（DXVK、vkd3d-proton、原生 VK 程序） | **必须内核白名单**，用户态补丁无效 |
| 不确定 | 直接打内核白名单，两条都覆盖 |

这也解释了为什么 10-30 那次七日世界必须走内核补丁才见效。

---

## 8. 复现

探针源码（`batchprobe.c`）与运行脚本在本次实验的临时目录，
核心逻辑 40 行，可随时重写。运行方式：

```bash
# native OpenGL
DISPLAY=:0 XAUTHORITY=<xauth> \
  __GLX_VENDOR_LIBRARY_NAME=nvidia __NV_PRIME_RENDER_OFFLOAD=1 \
  ./batchprobe -n 400 -f 20 -w 5 -s

# zink on P104（★ 必须显式选卡，否则会跑到 Intel 核显）
DISPLAY=:0 XAUTHORITY=<xauth> \
  MESA_LOADER_DRIVER_OVERRIDE=zink MESA_VK_DEVICE_SELECT=10de:1b87 \
  ./batchprobe -n 400 -f 20 -w 10

# Intel 对照
MESA_LOADER_DRIVER_OVERRIDE=zink MESA_VK_DEVICE_SELECT=8086:041a ./batchprobe ...
```

切换 glcore 状态做对照时**务必带 `trap` 恢复**，否则忘恢复就掉回 19 fps：

```bash
G=/usr/lib/x86_64-linux-gnu/libnvidia-glcore.so.580.178.04
cp -a "$G" /tmp/glcore-patched.bak
trap 'cp -a /tmp/glcore-patched.bak "$G"' EXIT
cp -f "$G.show-orig" "$G"      # 换原厂
# ... 测 ...
# 退出时自动恢复
```
