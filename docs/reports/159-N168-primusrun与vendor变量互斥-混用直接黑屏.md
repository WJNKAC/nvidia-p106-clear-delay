> ## ⚠ 报告 165（N174）补充：primusrun 是"解除天花板"的测量工具
>
> 本报告的三路对比（环境变量 60.3 / 仅 primusrun 28.0 / 混用 0.0）里，
> **"仅 primusrun 28.0 fps" 的归因需要修正**：
>
> 28 fps 不是 primus 回读开销所致，而是 **525 在这一负载下的真实渲染能力**
> （用户实测同一 `primusrun` 方式在 **515 上可跑 100 fps+**）。
>
> 本报告关于"两者混用 → 黑屏"的核心结论**不受影响**（那是 GLX 接管冲突，与帧率无关）。
> 但"primusrun 慢所以别用"的推论**只适用于正常运行**，做性能对比时反而必须用它。

# 报告 159 — `primusrun` 与 `__GLX_VENDOR_LIBRARY_NAME=nvidia` 互斥，混用直接黑屏

> 日期：2026-10-28 00:20
> 实验机：`<user>@<testhost>`（<testhost>，P106-100，525.116.03）
> 触发：用户加 `primusrun` 后黑窗口
> 前置：报告 158（`DRI_PRIME` 会回退 zink，原生直连 60fps）

---

## 0. 结论

> **`primusrun` 和 `__GLX_VENDOR_LIBRARY_NAME=nvidia` 是两套互斥的 GLX 接管机制，
> 同时用会导致渲染通路断裂 —— 黑屏、fps=0。**
>
> **只用 `__GLX_VENDOR_LIBRARY_NAME=nvidia` + `__NV_PRIME_RENDER_OFFLOAD=1`，
> 去掉 `primusrun`，就是 60fps 正常画面。**

---

## 1. 实测证据（同一配置、同 1920×1080、抓窗口像素亮度判定黑屏）

| 启动方式 | fps | 窗口平均亮度 | renderer |
|---|---|---|---|
| **仅环境变量** | **60.3** | **0.4719**（正常画面） | NVIDIA P106-100/PCIe/SSE2 |
| 仅 `primusrun` | 28.0 | — | NVIDIA P106-100 |
| **环境变量 + `primusrun`（你的做法）** | **0.0** | **0.00**（**全黑**） | NVIDIA P106-100/PCIe/SSE2 |

**亮度 0.4719 vs 0.00 —— 一个是正常渲染的雪景，一个是纯黑。**

判据：`import -window <WID>` 抓窗口 → 转灰度 → 取均值。
本项目正常场景约 **0.47**，黑屏 **0.00**。

---

## 2. 为什么互斥：两者都在抢 GLX 的控制权

### 2.1 `primusrun` 做了什么

```bash
# /usr/bin/primusrun 的全部有效内容
PRIMUS_libGL=${PRIMUS_libGL:-/usr/lib/x86_64-linux-gnu/primus}
export LD_LIBRARY_PATH=${PRIMUS_libGL}${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
export ENABLE_PRIMUS_LAYER=${ENABLE_PRIMUS_LAYER:-1}
exec "$@"
```

**它只是把 primus 自带的 `libGL.so.1` 放到 `LD_LIBRARY_PATH` 最前面。**

```
/usr/lib/x86_64-linux-gnu/primus/libGL.so.1   (264 KB)
```

primus 的 `libGL` 会**接管整个 GLX 调用链**：它在离屏 surface 上渲染，
再通过 CPU 回读 + X11 传输把画面搬到屏幕上。

### 2.2 `__GLX_VENDOR_LIBRARY_NAME=nvidia` 做了什么

它告诉 **glvnd**（`libGLX.so.0` 的分发层）：
"创建 GLX context 时直接用 NVIDIA 的 `libGLX_nvidia`，不要走 Mesa/primus 那套"。

### 2.3 冲突点

```
primusrun:              LD_LIBRARY_PATH 让 primus 的 libGL 接管 GLX
__GLX_VENDOR_LIBRARY_NAME: 让 glvnd 把 GLX 转给 libGLX_nvidia
                                    ↓
                    两套机制对同一个 GLX 连接各有主张
                                    ↓
                    画面通路断裂：渲染发生了，但没有上屏
                                    ↓
                    黑屏 + fps=0（swap 都不再发生）
```

**证据：两者都用时，`OpenGL renderer` 仍报 `NVIDIA P106-100`，进程也活着，
但 `glXSwapBuffers` 调用次数 = 0，窗口亮度 = 0。**
⇒ **context 建成了、渲染可能也在跑，但帧没有提交到窗口。**

---

## 3. 一个被澄清的误解：GL 3.2.0 不是问题

三轮测试里 `OpenGL version` 都是 **3.2.0 NVIDIA 525.116.03**，
包括正常 60fps 那次。所以：

**⇒ GL 3.2.0 是 Heaven 自己请求的版本，不是 primus 限制的。**

Heaven 是 2013 年的程序，用 `glXCreateContextAttribsARB` 请求 GL 3.2 Core。
P106 本身支持 4.6，但 Heaven 只要 3.2。

> 顺带更正报告 158 的一个说法：那里写的 `OpenGL version: 4.6.0` 是 **n83b**
> 输出的（`GL_VERSION = 4.6.0 NVIDIA 525.116.03`），不是 Heaven。
> **Heaven 在原生直连下是 3.2.0，且这是正常的。**

---

## 4. `primusrun` 单独用也只有 28fps

| 方式 | fps | 说明 |
|---|---|---|
| 仅 `primusrun` | **28.0** | primus 的离屏渲染 + CPU 回读开销 |
| 仅环境变量 | **60.3** | 直连，无中间层 |

**primus 的每帧开销让它刚好越过 16.67 ms 的 vsync 周期 → 锁半速 28fps。**

这解释了历史报告中"525 用 primus 只有 30fps"的来源：
**那个 30fps 是 primus 自己的开销造成的，不是 525 的 GPU 能力。**

---

## 5. 正确命令（最终版）

```bash
cd <apps-dir>/unigine_heaven/main/x86_64/decomps/Unigine_Heaven-4.0

env -u DRI_PRIME \
    __GLX_VENDOR_LIBRARY_NAME=nvidia \
    __NV_PRIME_RENDER_OFFLOAD=1 \
    ./heaven
```

**要点：**
- **不要加 `primusrun`**（会和 vendor 变量冲突，黑屏）
- **不要用 `DRI_PRIME=1`**（会回退 zink，30fps，见报告 158）
- `env -u DRI_PRIME` 是保险（残留变量会覆盖 NVIDIA 的）

**验证三行（缺一不可）：**

```
OpenGL vendor:   NVIDIA Corporation            ← 必须是 NVIDIA
OpenGL renderer: NVIDIA P106-100/PCIe/SSE2     ← 必须是 P106
窗口有画面（亮度约 0.47）
```

---

## 6. 三种后端在本机的完整对照表

| 方式 | renderer | fps | 功耗 | 画面 |
|---|---|---|---|---|
| **原生直连**（vendor + prime 变量） | NVIDIA P106-100 | **60.3** | 62.7 W | ✅ |
| `DRI_PRIME=1` | **zink Vulkan**（Mesa） | 30.5 | 61.6 W | ✅ |
| `primusrun` | NVIDIA P106-100 | 28.0 | — | ✅ |
| **primusrun + vendor 变量** | NVIDIA P106-100 | **0.0** | — | ❌ **黑屏** |

**⇒ 三套"用独显"的方法里，只有原生直连是对的。另两套都比它慢一倍，
而混用直接不能用。**

---

## 7. 为什么这台机器会有这么多套

| 机制 | 来源 | 在本机是否适用 |
|---|---|---|
| `DRI_PRIME=1` | **Mesa** 的 PRIME | ❌ 需要 nouveau（没装）→ 回退 zink |
| `primusrun` | **primus**（独立项目） | ⚠️ 能用但慢一倍（离屏+回读） |
| `__NV_PRIME_RENDER_OFFLOAD=1`<br>`__GLX_VENDOR_LIBRARY_NAME=nvidia` | **NVIDIA 官方** | ✅ **正确** |
| `prime-run` | NVIDIA 提供的脚本 | 本机**未安装** |

**四套方法并存，其中三套要么错、要么慢。这也是为什么排查容易走偏。**

---

## 8. 排查口诀

> **看到 `OpenGL renderer` 是 `zink` ⇒ 用了 Mesa 的 `DRI_PRIME`，改 vendor 变量。**
> **看到黑屏 + fps=0 但进程活着 ⇒ 检查是不是 `primusrun` 和 vendor 变量混用。**
> **看到 28~30fps（刚好半速）⇒ 该后端每帧越过了 vsync 周期，换原生直连。**

---

## 9. 本轮方法备忘

**判断"黑屏"必须用像素亮度，不能只看进程活着。**

```bash
WID=$(xwininfo -root -tree | grep -i 'Unigine\|Heaven' | grep -oE '0x[0-9a-f]+' | head -1)
import -window $WID /tmp/s.png
convert /tmp/s.png -colorspace Gray -format '%[fx:mean]' info:
# 正常约 0.47；黑屏 0.00
```

**注意**：`scrot` 在 Xwayland 下抓游戏窗口会全黑（历史记忆已记录），
必须用 `import`/`xwd` 直接抓窗口 ID。

**另外 `fps=0` 本身也是个强信号**：进程活着但 swap 次数为 0
⇒ 渲染没有提交到窗口，而不是"渲染很慢"。
