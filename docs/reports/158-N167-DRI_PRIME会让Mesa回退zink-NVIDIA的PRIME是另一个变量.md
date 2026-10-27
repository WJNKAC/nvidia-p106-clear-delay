> ## ⚠ 报告 165（N174）更正本报告对 primusrun 的定性
>
> 本报告把 `primusrun` 判定为"慢、不该用"，依据是 `525 + primusrun = 28 fps`。
> **该因果解释已被推翻。**
>
> | | 515 | 525 |
> |---|---|---|
> | GLX 直连 | 60（被 present 节流压住） | 60（同样被压住） |
> | **primusrun** | **100+ fps** | **28 fps** |
>
> 若 28 fps 是"primus 的固定回读开销"，515 也应是 28；**但 515 到了 100+**。
> ⇒ **28 fps 是 525 的真实渲染能力，不是 primus 的开销。**
>
> **结论修正**：`primusrun` 在"运行程序"语境下确实比直连慢（多一次 CPU 回读），
> 但在**"测量性能"语境下它是必要的** —— 它是唯一能绕过 present 节流、暴露真实帧率的路径。
> 本报告"正确命令只应使用 vendor 变量"这条建议，**仅适用于正常运行，不适用性能对比**。

# 报告 158 — 根因：`DRI_PRIME=1` 会让 Mesa 回退到 zink；NVIDIA 的 PRIME 是另一个变量

> 日期：2026-10-27 23:50
> 实验机：`<user>@<testhost>`（<testhost>，P106-100，525.116.03）
> 触发：用户用 `vblank_mode=0 DRI_PRIME=1 ./heaven` 仍只有 30fps
> 前置：报告 157（30fps 是 vsync 半速）

---

## 0. 一句话

> **`DRI_PRIME=1` 是 Mesa 的环境变量，不是 NVIDIA 的。**
> **NVIDIA 闭源驱动不实现 Mesa 的 DRI 接口，于是 Mesa 先试 nouveau（失败）、
> 再回退到 zink（GL→Vulkan 翻译层）。所以 `DRI_PRIME=1` 等于强制走 zink。**
>
> **NVIDIA 的 PRIME 卸载要用 `__NV_PRIME_RENDER_OFFLOAD=1` +
> `__GLX_VENDOR_LIBRARY_NAME=nvidia`。**

---

> ## ⚠️ 更正（见报告 159 §3）
>
> 本文若提到 `OpenGL version: 4.6.0`，那是 **n83b** 输出的（`GL_VERSION = 4.6.0`），
> **不是 Heaven**。Heaven 是 2013 年的程序，它请求的是 **GL 3.2 Core**，
> 原生直连下 `OpenGL version: 3.2.0 NVIDIA 525.116.03` 是**正常**的。
> 报告 159 已实测确认：3.2.0 下 60fps、画面正常（亮度 0.47）。

---

## 1. 用户日志里的三行铁证

```
glx: failed to create dri3 screen
failed to load driver: nouveau
OpenGL renderer: zink Vulkan 1.3(NVIDIA P106-100 (NVIDIA_PROPRIETARY))
```

- `failed to load driver: nouveau` —— Mesa 尝试加载**开源的** NVIDIA DRI 驱动（nouveau）
- 失败后回退到 **zink**
- 最终 `OpenGL renderer` 是 **zink**，不是 NVIDIA

**即便把 `MESA_LOADER_DRIVER_OVERRIDE=zink` 这些变量全去掉，只要还用 `DRI_PRIME=1`，
结果还是 zink。**

---

## 2. 本轮的干净复现对照（同一配置、同分辨率 1920×1080）

| 启动方式 | 加载的驱动 | `OpenGL renderer` | fps | 功耗 |
|---|---|---|---|---|
| `DRI_PRIME=1` | nouveau 失败 → **zink** | `zink Vulkan 1.3(NVIDIA P106-100)` | **30.5** | 61.56 W |
| `__GLX_VENDOR_LIBRARY_NAME=nvidia`<br>`__NV_PRIME_RENDER_OFFLOAD=1` | **NVIDIA 闭源** | `NVIDIA P106-100/PCIe/SSE2` | **60.0** | 62.71 W |

**同一台机器、同一份 `~/.Heaven/heaven_4.0.cfg`、同一个 1920×1080，
只是换启动方式：30 → 60，整整一倍。**

---

## 3. 两套 PRIME 机制的区别（这是核心）

| | Mesa 的 PRIME | NVIDIA 的 PRIME |
|---|---|---|
| 环境变量 | **`DRI_PRIME=1`** | **`__NV_PRIME_RENDER_OFFLOAD=1`** |
| 配合变量 | 无（Mesa 自己选 `$DRI_PRIME` 指定的 GPU） | **`__GLX_VENDOR_LIBRARY_NAME=nvidia`** |
| 需要的驱动 | Mesa 驱动（**nouveau** 才能驱动 NVIDIA 卡） | NVIDIA 闭源驱动 |
| 本项目实际效果 | nouveau 不存在 → **回退 zink** | 正确使用 P106 |

**关键**：NVIDIA 闭源驱动**不注册为 Mesa 的 DRI 驱动**，
所以 `DRI_PRIME=1` 对它完全无效 —— 它找的是 nouveau。

---

## 4. 正确命令（可直接用）

```bash
cd <apps-dir>/unigine_heaven/main/x86_64/decomps/Unigine_Heaven-4.0

__GLX_VENDOR_LIBRARY_NAME=nvidia __NV_PRIME_RENDER_OFFLOAD=1 ./heaven
```

**如果系统装了 `prime-run`（NVIDIA 提供的脚本），它等价于上面两个变量：**

```bash
prime-run ./heaven
```

**要确认走对了，看这两行：**

```
OpenGL vendor:   NVIDIA Corporation          ← 正确
OpenGL renderer: NVIDIA P106-100/PCIe/SSE2   ← 正确

OpenGL vendor:   Mesa                        ← 走错了（zink）
OpenGL renderer: zink Vulkan 1.3(...)        ← 走错了
```

---

## 5. 为什么 zink 在这里是负收益

| | 原生 GL 直连 | zink |
|---|---|---|
| 帧率（1080p） | **60.0** | 30.5 |
| 功耗 | 62.71 W | 61.56 W |
| 每帧耗时 | <16.67 ms | **18.59 ms**（报告 156 实测） |

zink 每帧 18.59 ms **刚好越过 16.67 ms 的 vsync 周期**，被合成器锁到半速 30。

**而 zink 的唯一长处（绕开 glcore 的 legacy 固定管线）对 Heaven 无用** ——
报告 156 实测 Heaven 的 legacy 状态机调用次数是 **0**。

⇒ **Heaven 用 zink 是纯负收益：帧率减半、功耗不降。**

---

## 6. 这条链路现在完整了

```
用户命令 DRI_PRIME=1
    ↓ （Mesa 不认 NVIDIA 闭源驱动）
nouveau 加载失败
    ↓
回退 zink（GL → Vulkan 翻译层）
    ↓
zink 每帧 18.59 ms（Mesa 翻译开销 + 回读等待）
    ↓ （18.59 > 16.67 ms）
错过一个 vsync 周期
    ↓
合成器（KWin / Xwayland，2560×1080@59.98Hz）锁半速
    ↓
**30 fps**  ← 用户看到的现象
```

**换用原生直连后：**
```
原生 GL 每帧 <16.67 ms
    ↓
赶上每一个 vsync
    ↓
60 fps 满帧
```

---

## 7. 报告 157 的结论需要一处修正

报告 157 我写"zink 每帧 19~20 ms ⇒ 30fps"，这个因果链是对的。
但当时**没说清用户为什么会用 zink** —— 他并没有显式设 `MESA_LOADER_DRIVER_OVERRIDE`，
是 `DRI_PRIME=1` 让他被动走到了 zink。

**⇒ zink 有两种进入方式：**
1. **显式**：`MESA_LOADER_DRIVER_OVERRIDE=zink` / `GALLIUM_DRIVER=zink`
2. **被动**：`DRI_PRIME=1`（Mesa 在 nouveau 失败后自动回退）

**第 2 种更隐蔽**，因为用户以为自己只是"用独显"。

---

## 8. 排查清单（下次遇到"为什么走的是 zink"）

```
1. 看日志里 OpenGL renderer 是否含 zink
2. 检查环境：env | grep -iE "zink|GALLIUM|MESA_LOADER|DRI_PRIME|__NV_PRIME|__GLX"
3. 若含 DRI_PRIME 但不含 __GLX_VENDOR_LIBRARY_NAME → 必然回退 zink
4. 去掉 DRI_PRIME，换成 __GLX_VENDOR_LIBRARY_NAME=nvidia + __NV_PRIME_RENDER_OFFLOAD=1
5. 再确认 renderer 变成 NVIDIA P106-100
```

**注意 `DRI_PRIME` 不要和其它变量混用**：
`DRI_PRIME=1` 与 `__GLX_VENDOR_LIBRARY_NAME=nvidia` 同时存在时，
行为取决于谁先被查询，**不可靠**。二选一，选 NVIDIA 那套。

---

## 9. 这台机器上没有 `prime-run`

```
$ which prime-run
（无输出）
```

所以只能手动设变量。**建议的完整命令（可直接粘贴）：**

```bash
cd <apps-dir>/unigine_heaven/main/x86_64/decomps/Unigine_Heaven-4.0

env -u DRI_PRIME \
    __GLX_VENDOR_LIBRARY_NAME=nvidia \
    __NV_PRIME_RENDER_OFFLOAD=1 \
    vblank_mode=0 \
    ./heaven
```

> `env -u DRI_PRIME` 是保险：**万一你的 shell 里还残留 `DRI_PRIME=1`，
> 它会覆盖掉 NVIDIA 的 PRIME 变量**，导致又回退 zink。
> 用 `env -u DRI_PRIME` 显式清掉最稳。

### 9.1 验证是否走对（三行确认）

启动后看日志：

```
OpenGL vendor:   NVIDIA Corporation
OpenGL renderer: NVIDIA P106-100/PCIe/SSE2
```

**只要 `renderer` 里没有 `zink` 就是对的。**

### 9.2 你也可以先手动验证一次变量优先级

```bash
# 清掉所有残留，逐条确认
env | grep -iE "DRI_PRIME|__GLX|__NV_PRIME|MESA_LOADER|GALLIUM"
# 若看到 DRI_PRIME=1，它就是要清掉的那个
```
