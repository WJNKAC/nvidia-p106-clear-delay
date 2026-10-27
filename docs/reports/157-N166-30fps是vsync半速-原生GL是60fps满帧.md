> ## ⚠ 报告 165（N174）更正本报告的"满帧"判断
>
> 本报告结论「30fps 是 vsync 半速，原生 GL 是 **60fps 满帧**」中，**「满帧」是错的**。
>
> **60 fps 是 Xwayland 的 present 节流天花板，不是渲染能力上限。**
> 决定性反证（同一份 515 驱动、同一台机器）：
>
> | 启动方式 | 帧率 |
> |---|---|
> | `primusrun`（XPutImage 路径，不走 present） | **100 fps+** |
> | `__GLX_VENDOR_LIBRARY_NAME=nvidia`（GLX 直连，走 present） | **60 fps** |
>
> 本报告当时依据"800×450 与 1080p 都是 60、与分辨率无关"推断出"存在节流"——
> **这个推断是对的，但节流点在 Xwayland/KWin 合成侧，不在驱动 vsync。**
> `vblank_mode=0` / `__GL_SYNC_TO_VBLANK=0` 作用在交换链内部，管不到下游的 present，故实测无效。
>
> **影响**：本报告里所有 Heaven 的 fps 数字只能当作"撞天花板的读数"，不能当作性能。

# 报告 157 — ★ 30fps 是 vsync 半速，不是 GPU 能力：Heaven 在 525 上原生 GL 是 60fps 满帧

> 日期：2026-10-27 23:10
> 实验机：`<user>@<testhost>`（<testhost>，P106-100，525.116.03）
> 触发：用户自己跑 Heaven 只有 30fps，问我怎么跑的
> 前置：报告 156（zink 对 legacy 快 6.9 倍）

---

## 0. 答案：你看到的 30fps 是 vsync 半速，不是 GPU 慢

| 配置 | 800×450 | 1920×1080 |
|---|---|---|
| **原生 NVIDIA GL（直连）** | **60.5 fps** | **60.8 fps** |
| **zink（你用的）** | **30.2 fps** | **29.5 fps** |

**显示器：2560×1080 @ 59.98 Hz（Xwayland + KWin 合成器）**

- 60.8 ≈ **满帧**（合成器 60Hz 上限）
- 29.5 ≈ **正好一半**（每次错过一个 vsync 周期）

**⇒ zink 的每帧耗时略超 16.67 ms，于是被锁到 30fps。**
这与报告 156 测的 zink M3 = **18.59 ms** 完全吻合（18.59 > 16.67）。

**⇒ 你"高帧率时抽搐、否则回 30"就是：有时赶上 16.67ms 就 60，错过了就掉半速 30。**

---

## 1. 现场对比：你的配置 vs 我的配置

| | 你的命令 | 我的命令 |
|---|---|---|
| 渲染后端 | **zink**（`MESA_LOADER_DRIVER_OVERRIDE=zink`） | **原生 NVIDIA GL** |
| 启动方式 | `./heaven`（→ `browser_x64 -config launcher.xml`） | `./heaven_x64 -engine_config ...` |
| 画质配置 | `~/.Heaven/heaven_4.0.cfg`（`QUALITY_HIGH`） | 同（我后来也用这份） |
| 分辨率 | 1920×1080 | **800×450**（早期）/ 1920×1080 |
| `DRI_PRIME` | =1 | 未设（用 `__GLX_VENDOR_LIBRARY_NAME=nvidia`） |
| `MESA_VK_WSI_PRESENT_MODE` | immediate | 未设 |

**关键点：我早期测的"62fps"是 800×450 + 原生 GL；你测的是 1080p + zink。**
两者差了后端，不只是分辨率。

**而且分辨率根本不是变量** —— 见下表。

---

## 2. 分辨率无关，证明这不是 GPU 吞吐问题

| 配置 | 分辨率 | fps | 像素量比 |
|---|---|---|---|
| 原生 GL | 800×450（0.36 MP） | **60.5** | 1× |
| 原生 GL | 1920×1080（2.07 MP） | **60.8** | **5.8×** |

**像素量差 5.8 倍，帧率一模一样（60.5 vs 60.8）。**

**⇒ 完全被 60Hz 节拍锁住，测不出 GPU 能力。**

顺带：zink 也是分辨率无关（800×450 → 30.2，1080p → 29.5），
因为它本来就只是"错过一半 vsync"。

---

## 3. 为什么 `vblank_mode=0` 和 `-video_vsync 0` 都没用

两种都试了：

| 手段 | 结果 |
|---|---|
| `-video_vsync 0`（命令行） | 60.8 / 29.2 |
| 再加 `vblank_mode=0`（Mesa 环境变量） | 60.8 / 29.2 |

**完全没变。**

原因：**显示路径是 Xwayland → KWin 合成器 → 显示器。**

```
Heaven → GLX → glcore → GPU 渲染
                    ↓
              Xwayland window
                    ↓
              KWin 合成（60Hz 节拍）
                    ↓
              2560×1080 @ 59.98Hz
```

`vblank_mode` 只控制 Mesa 的 GLX swap 策略，**管不到 KWin 合成器的节拍**。
窗口模式下，任何 X11 程序都被合成器以 60Hz 逐帧合成。

**⇒ 在 Xwayland 下用窗口模式测帧率，上限就是 60Hz。**
这也是为什么我之前的 n83b 能测到 500~1000 fps —— **它渲染到 FBO/pbuffer，根本不 present**
（`swaps_total=0`），绕过了整条上屏路径。

---

## 4. 这个发现推翻了一个长期前提

历史报告一直基于 **"Heaven 在 525 上只有 30fps，515 上有 101fps"**。

现在的实测：

| 场景 | 帧率 |
|---|---|
| **525 + 原生 GL 直连 + 1920×1080** | **60.8 fps（满帧）** |
| 525 + zink + 1920×1080 | 29.5 fps |
| 历史：525 + primus | 30-31 fps（报告 101） |
| 历史：515 + primus | 101 fps |

**⇒ 525 上用原生 GL 直连是 60fps 满帧，不慢。**

历史上的 30fps 来自 **primus 路径**（NVIDIA 渲染 + Intel 上屏）或 **zink**，
两者都不是"525 的 GPU 能力"。

> ⚠️ 这一条需要谨慎：515 的 101fps > 60Hz，说明那次测量**没有**被合成器限制
> （可能当时是纯 Xorg，或用了其它配置）。所以历史数据不能简单否定，
> 但**当前环境下所有 Heaven 帧率数据都必须注明"是否被 60Hz 限制"**。

---

## 5. 对报告的直接影响

| 报告 | 结论 | 影响 |
|---|---|---|
| 101 | zink 在 525 上 33fps、原生 primus 30fps | **数字真实，但都是 vsync 半速附近的读数** |
| 105 | "Heaven 30fps = 268 对 × 118.5 µs" | 已撤回（报告 156） |
| 149/152 | n83b 的各种 ms/帧、fps | **不受影响**（n83b 不 present，绕过合成器） |

---

## 6. 你真正该关心的：怎么突破 60fps 上限去测真实性能

如果你的目标是"看清 525 的 GPU 到底能跑多快"，需要绕过合成器：

| 方法 | 说明 | 可行性 |
|---|---|---|
| **全屏独占**（`-video_fullscreen 1`） | Xwayland 可能允许 unredirect | 值得试，成本低 |
| **纯 Xorg 会话**（不用 Wayland） | 关掉 KWin 合成器 | 需要改登录会话 |
| **用不 present 的基准**（如 n83b） | 直接测 GPU 提交吞吐 | **已验证可用**（500~1000 fps） |
| **Heaven `-benchmark`** | 引擎自带计时，可能不受合成器影响 | 需试 |
| **KWin 关闭合成**（`Shift+Alt+F12`） | 取消 60Hz 合成 | 窗口模式可能黑屏/但仍被节拍限制 |

**最快的验证**：试试 `-video_fullscreen 1`，如果帧率突破 60，说明合成器被绕过。

---

## 7. 如果你只是想让 Heaven 跑顺（不改代码）

**用原生 GL 直连，不要用 zink。**

```bash
# 你现在的（zink，30fps）
DRI_PRIME=1 __GLX_VENDOR_LIBRARY_NAME=mesa GALLIUM_DRIVER=zink \
  MESA_LOADER_DRIVER_OVERRIDE=zink MESA_VK_WSI_PRESENT_MODE=immediate ./heaven

# 改成（原生 GL，60fps 满帧）
__GLX_VENDOR_LIBRARY_NAME=nvidia __NV_PRIME_RENDER_OFFLOAD=1 ./heaven
```

实测对比（同一份 `~/.Heaven/heaven_4.0.cfg`，1920×1080）：

| 后端 | fps | 评价 |
|---|---|---|
| **原生 GL 直连** | **60.8** | **满帧，推荐** |
| zink | 29.5 | 半速，且功耗更高（64W vs 53W） |

**zink 在这里是负收益**：帧率减半，功耗反增（64.26 W vs 53.03 W）。

> 这与报告 156 的结论一致：**zink 只在"原生路径踩了 legacy 坑"时才划算**。
> Heaven 不用 legacy（0 次），所以 zink 只剩翻译开销。

---

## 8. 测量方法备忘（本轮踩的坑）

1. **`ls /usr/share/vulkan/icd.d/` 找不到 nvidia_icd.json** —— 它在
   **`/etc/vulkan/icd.d/`**。不要再 `find /` 全盘扫。
2. **`./heaven` 是启动器**（`browser_x64 -config launcher.xml`），
   读的是 `~/.Heaven/heaven_4.0.cfg`；直接跑 `heaven_x64` 时若传
   `data/heaven_4.0.cfg` 会得到**低画质默认配置**（`render_parallax=0` 等），
   与用户的实际画质不同。**要复现用户结果必须用 `~/.Heaven/heaven_4.0.cfg`。**
3. **zink 下 `glXSwapBuffers` 不在 `libGLX_nvidia`**，而在 `libGL.so.1`（glvnd 分发层，
   offset `0x45710`）。用它可以同时统计原生 GL 和 zink 的帧数。
4. **`bc2` 对 zink 进程会导致 `追踪或断点陷阱` 崩溃**，但崩溃前的计数有效。
   需要更温和的采集方式（下一轮改进）。

---

## 9. 一句话总结

> **你看到的 30fps 不是 GPU 慢，是 zink 每帧 19~20 ms 刚好超过 16.67 ms 的 vsync 周期，
> 被合成器锁到半速。换成原生 GL 直连，同一个 1080p 窗口是 60.8 fps 满帧。**
>
> **而 "Heaven 在 525 上只有 30fps" 这个长期前提，在当前环境下不成立。**

---

## 10. 追加验证：全屏独占也突破不了 60

```
-video_fullscreen 1，1920×1080，原生 GL → 60.5 fps
```

**与窗口模式的 60.8 相同。**

⇒ **Xwayland 不会为 X11 全屏窗口做 unredirect**，合成器节拍照旧生效。

**所以在这个会话里，用 Heaven（任何窗口/全屏模式）都无法测出 60fps 以上的真实 GPU 能力。**

### 10.1 想测真实能力的三条路

| 方法 | 预期 | 代价 |
|---|---|---|
| **纯 Xorg 会话**（不用 Wayland/KWin） | 合成器消失，可能突破 60 | 要改登录会话，需重启 |
| **n83b 式不 present 基准** | 已验证可到 500~1000 fps | 但测的是合成负载，不是 Heaven |
| **关 KWin 合成**（`Shift+Alt+F12`） | 窗口模式可能不再被合成节拍限制 | 需人工按键，且 Wayland 下不一定允许 |

### 10.2 一个必须复核的历史疑点

**515 曾被测出 Heaven 101 fps**（报告 101）。101 > 60，说明**那次测量没有被合成器限制**。

可能的原因：
- 当时是纯 Xorg 会话（没有 KWin 合成）
- 或者当时关掉了合成

**⇒ 之前所有 Heaven 帧率对比（515 vs 525）都必须先确认"是否被 60Hz 限制"，
否则对比的是"谁先撞到 60Hz 天花板"，而不是"谁的 GPU 更快"。**

这一点在当前环境里无法自证，需要：
1. 查当时的会话类型记录，或
2. 在纯 Xorg 下重测 515/525 的 Heaven

---

## 11. 最终回答你的问题

> **问：我自己跑怎么就 30fps 左右？你怎么跑的？**

**三点差异，按重要性排序：**

1. **你用了 zink，我用的是原生 NVIDIA GL。**
   zink 每帧 19~20 ms（Mesa GL→Vulkan 翻译开销），**刚好超过 16.67 ms 的 vsync 周期**，
   于是被合成器锁到半速 30fps。原生 GL 每帧 <16.67ms，满帧 60fps。

2. **显示器是 60Hz（Xwayland+KWin 合成），这决定了上限。**
   我早期说的"62fps"是同一台机器 800×450 下的值 —— 也是撞在这个 60Hz 天花板上，
   不是 GPU 只能跑 62。

3. **分辨率其实不是变量。**
   原生 GL 在 800×450 和 1920×1080 下都是 60.5~60.8 fps（像素量差 5.8 倍，帧率不变）。

**你看到的"抽搐"正是 zink 在 60/30 之间抖动：赶上 vsync 就 60，错过就 30。**

**建议：`__GLX_VENDOR_LIBRARY_NAME=nvidia __NV_PRIME_RENDER_OFFLOAD=1 ./heaven`
（去掉 zink 那一串），同一个 1080p 窗口就是 60fps 满帧，功耗还低（53W vs 64W）。**
