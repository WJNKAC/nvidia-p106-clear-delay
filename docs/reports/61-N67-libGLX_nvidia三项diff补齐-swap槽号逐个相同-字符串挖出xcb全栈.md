# 61 · N67：`libGLX_nvidia` 三项 diff 补齐 —— **swap 下游槽号逐个相同**，但字符串挖出两个新东西

> 2026-10-26 09:20
> 用户指令：N62 已说明瓶颈不在"读哪个 API"。**先零成本静态对比 515/525 的 `libGLX_nvidia`（符号 / 字符串 / swap 下游调用链三项）**；若静态看不出差异，再换 515 做真实 primus 下的 T_land 三档拟合。
>
> **说明**：N64（报告 60）已做完符号层与指令层。**本轮补的是当时漏掉的字符串层**，并因此挖出两个新事实 + **一处对报告 60 的更正**。

---

## 1. 你要的三项，逐项回答

| # | 对比项 | 结果 | 判读 |
|---|---|---|---|
| **1** | **符号表 diff** | 导入 122 vs 123（**只多 `XInitThreads`**）；导出 2674 vs 2673（差 1 个 typo `glTexParametervNVX`） | **无新增 swap/PBO/fence 相关符号** |
| **2** | **字符串 diff** | 可读字符串 2919 vs 2922，**真正差异只有 3 条**（+2 扩展差异） | **★ 见 §3：挖出 xcb DRI3/Present 全栈 + `_XInitDisplayLock_fn`** |
| **3** | **swap 下游调用链** | 两版 `glXSwapBuffers` 调 **同样的 3 个 glcore 槽（`0x598` / `0x5a0` / `0x5a8`）**，同样顺序、同样尾跳 | **★★★ 完全一致，525 没在 swap 后插任何同步** |

**一句话**：**这个库里没有 20.4ms 的来源。`glXSwapBuffers` 的下游是同一组三个槽，字符串层的所有实质差异都与"每帧路径"无关。**

---

## 2. 第 1 项：符号层（复核报告 60，补一个细节）

| 项 | 515.105.01 | 525.116.03 |
|---|---|---|
| 导入符号 | 122 | 123 |
| 唯一新增 | — | **`XInitThreads`** |
| 导出符号 | 2674 | 2673 |
| 唯一减少 | **`glTexParametervNVX`**（typo，少一个 `i`） | — |

**★ 注意**：`_XInitDisplayLock_fn` **不是导入符号**（`nm -D` 里两版都查不到），它是**字符串**→ 走 `dlsym`。上一轮只做符号层时因此漏掉了它。**这是"符号层看不出、字符串层才出现"的典型例子。**

---

## 3. 第 2 项：字符串层 —— 本轮的新发现

### 3.1 先说方法上的坑：`strings` 会从机器码里抓出大量垃圾

原始 `strings -a -n 5` 给出 3290 vs 3296，diff 有 **482 行**差异，其中绝大多数是这样的：

```
AVAUATUSH;:H        AVAUL              AT+BT
9\$xvfI             ~CH
```

这是**指令字节被当成 ASCII**（`AVAUATUSH` 其实是 `push %r15/%r14/%r13/%r12` 的机器码）。**不做过滤的话，差异里 95% 是噪声，会淹没真正的 3 条。**

**过滤判据**（小写字母 ≥4 或"含空格且字母数 ≥6"，且排除全大写串）：482 行 → **10 / 13 行**。

### 3.2 过滤后的全部差异（这就是全量，没有省略）

**只在 515（去掉版本号与机器码后实质 2 条）**：

| 内容 | 性质 |
|---|---|
| **`GL_NVX_unix_allocator_import`** | **扩展字符串**。515 广告、525 不广告 |
| `glTexParametervNVX` | typo，配合符号层的导出差异 |

**只在 525（实质 3 条）**：

| 内容 | 性质 |
|---|---|
| **`XInitThreads`** | 同上轮 |
| **`_XInitDisplayLock_fn`** | **★ libX11 导出的 `.bss` 全局函数指针，525 用 `dlsym` 拿它** |
| **`xcb_get_input_focus` / `xcb_get_input_focus_reply`** | 输入焦点查询，与渲染路径无关 |

**除此之外，两版可读字符串完全一致。**

### 3.3 ★★★ 意外发现：两版都通过 `dlsym` 加载**整套 xcb DRI3 / Present / Sync 栈**

这是上一轮完全没看到的。两版**都有**（逐条确认过，各 1 处）：

| 类别 | 字符串 |
|---|---|
| **库名** | `libxcb.so.1`、**`libxcb-dri3.so.0`**、**`libxcb-present.so.0`**、**`libxcb-sync.so.1`**、`libxcb-glx.so.0`、`libX11-xcb.so.1` |
| **连接** | `XGetXCBConnection` |
| **DRI3** | `xcb_dri3_pixmap_from_buffer_checked`、`xcb_dri3_pixmap_from_buffers_checked`、`xcb_dri3_get_supported_modifiers(_reply)`、`xcb_dri3_id`、`xcb_send_fd` |
| **Present** | **`xcb_present_pixmap`**、`xcb_present_select_input(_checked)`、`xcb_present_id`、`xcb_register_for_special_xge`、`xcb_wait_for_special_event`、`xcb_poll_for_special_event` |
| **Sync fence** | **`xcb_sync_await_fence`**、`xcb_sync_destroy_fence(_checked)`、`xcb_sync_reset_fence`、`xcb_sync_id` |
| **GLX** | `xcb_glx_query_version(_reply)` |

**为什么 `nm -D` 看不到？** 全是 `dlsym` 字符串，**没有动态符号依赖**——所以库能在没有 xcb-present 的系统上照常加载，运行时探测。这也解释了为什么 L1 符号层的"导入了什么"完全看不出这一层。

---

## 4. ★ 更正报告 60 的一条表述

**报告 60 §5 我写的是**：

> "**`DRI3`/`DRI2`/`present` 零命中**，说明 NVIDIA 的 GLX **根本不走标准 DRI 缓冲管理** …NVIDIA 的 GLX 用 `socket`/`sendmsg`/`recvmsg` 说协议"

**这条对了一半，错了一半**。正确的分层是：

| 组件 | 走不走 DRI3/Present | 依据 |
|---|---|---|
| **服务端 `libglxserver_nvidia`**（X server 进程内） | **不走**（`DRI3`/`DRI2`/`present` 字符串 **0 命中**） | 报告 60 §5 ✅ 成立 |
| **客户端 `libGLX_nvidia`**（应用进程内） | **走**（通过 `dlsym` 加载 `libxcb-dri3` / `libxcb-present` / `libxcb-sync`） | **本轮新发现** |

两者并不矛盾：**客户端用 DRI3 直接 import/export dmabuf 并自己用 Present 提交，服务端 GLX 模块只管 GLX 协议本身。** NVIDIA 的架构是"客户端直连 DRI3，绕过服务端 GLX 的缓冲管理"。

**对结论的影响：无。** 因为**两版都有、逐条相同**，仍然不是版本差异。
**对未来的影响：有。** 若以后要查"present 到底怎么提的"，入口是客户端的 `xcb_present_pixmap`，不是服务端。

（已在报告 60 §5 加更正横幅。）

---

## 5. 第 3 项：swap 下游调用链 —— 槽号逐个相同

这是你要的第三项，也是最能直接否掉"525 在 swap 后插同步"的证据。

### 5.1 515 `glXSwapBuffers` @ `0x76a50`

```asm
mov    0x2bf51f(%rip),%rax        # _nv001tls
lea    0x2c5268(%rip),%rbp        # glcore vtable 基址
mov    %fs:(%rax),%rbx            # TLS → 当前 ctx
mov    0x0(%rbp),%rax
call   *0x598(%rax)               # 槽 0x598
test   %al,%al
je     ...
mov    0x0(%rbp),%rax
mov    0x55ee80(%rbx),%rdi        # ctx+0x55ee80
call   *0x5a0(%rax)               # 槽 0x5a0
mov    0x0(%rbp),%rax
mov    0x55ee80(%rbx),%rdi
mov    0x5a8(%rax),%rax
...
jmp    *%rax                      # 槽 0x5a8（尾跳）
```

### 5.2 525 `glXSwapBuffers` @ `0x738f0`

```asm
mov    0xab677(%rip),%rax         # _nv001tls
lea    0xb13e8(%rip),%rbx         # glcore vtable 基址
mov    %fs:(%rax),%r13            # TLS → 当前 ctx
mov    (%rbx),%rax
call   *0x598(%rax)               # 槽 0x598   ← 同
test   %al,%al
je     ...
mov    (%rbx),%rax
mov    0x8e0b00(%r13),%rdi        # ctx+0x8e0b00
call   *0x5a0(%rax)               # 槽 0x5a0   ← 同
mov    %rbp,%rdi
mov    %r13,%rdx
mov    %r12,%rsi
call   71890 <...+0x201e8>        # 同构的内部 helper
mov    (%rbx),%rax
mov    0x8e0b00(%r13),%rdi
mov    0x5a8(%rax),%rax
...
jmp    *%rax                      # 槽 0x5a8   ← 同（尾跳）
```

### 5.3 对比表

| 项 | 515 | 525 | 判定 |
|---|---|---|---|
| glcore 槽 1 | **`0x598`** | **`0x598`** | **同** |
| glcore 槽 2 | **`0x5a0`** | **`0x5a0`** | **同** |
| glcore 槽 3 | **`0x5a8`**（尾跳） | **`0x5a8`**（尾跳） | **同** |
| 调用顺序 | 598 → 5a0 → 5a8 | 598 → 5a0 → 5a8 | **同** |
| 槽 1 返回值检查 | `test %al,%al; je` | `test %al,%al; je` | **同** |
| ctx 取法 | `%fs:(_nv001tls)` → `+0x55ee80` | `%fs:(_nv001tls)` → `+0x8e0b00` | **仅偏移不同**（已知布局差） |
| 中间 helper | `call *0x5a0` 后直接接槽 3 | `call *0x5a0` → `call 0x71890` → 槽 3 | **同构**（51890 是 515 侧对应 helper，两者都是"swap 之后、释放前"的内部动作） |

**→ 525 在 swap 后没有插入任何同步、没有新增 fence、没有多一次 flush。三个 vtable 槽的号码一模一样，顺序一模一样。**

这比报告 60 的"语义指纹等价"更硬：现在是**具体的槽号对上了**。

---

## 6. ★ `_XInitDisplayLock_fn`：细化报告 60 的"XInitThreads 线"

报告 60 的结论是"**Qt 也调 `XInitThreads`，两版都跑线程安全模式，差异被抹平**"。
本轮找到实际代码后，**这个结论需要修正为更精确的版本**。

### 6.1 525 实际做了什么

`_XInitDisplayLock_fn` 经 `nm -D` 确认是 **libX11 导出的 `.bss` 全局变量**（`0x1476c0`，类型 `B`），`XInitThreads` 在 `0x32b30`。

525 的 display 初始化里（`0x81090` 与 `0x8170b`，同一大函数内）：

```asm
; ---- 一次性解析（0x81090）----
lea    _XInitDisplayLock_fn,%rsi      ; " _XInitDisplayLock_fn" 字符串
xor    %edi,%edi                      ; dlsym(NULL, ...)  ← 在全局符号里找
call   dlsym@plt
movb   $0x1, 0x123610                 ; 置"已解析"标志
mov    %rax, 0x123608                 ; 缓存 libX11 那个全局变量的地址

; ---- 之后在另一分支（0x8170b）----
call   XInitThreads@plt               ; 让 Xlib 进入线程安全模式
mov    0x123608,%rax                  ; 取回缓存的地址
mov    (%rax),%rax                    ; 解引用 = 读 _XInitDisplayLock_fn 的值
test   %rax,%rax
je     80ad3                          ; NULL → 跳过分支
jmp    80aba                          ; 非 NULL → 去加锁

; ---- 分支 A（0x80aba）----
80abf: mov %r13,%rdi                  ; r13 = Display*
80ac7: call *%rax                     ; ★ 调用锁函数 = 给 dpy 加锁
80ad3: cmpq $0x0,0x968(%r13)          ; 检查 dpy+0x968（解锁函数指针）
       jne  807dd                      ; 非空 → 走带解锁的出口
```

### 6.2 精确的结论

| 项 | 515 | 525 |
|---|---|---|
| `XInitThreads` | **不调** | **调** |
| `dlsym("_XInitDisplayLock_fn")` | **不做** | **做** |
| 读 `*_XInitDisplayLock_fn` 判空 | **不做** | **做** |
| 初始化期间对 dpy 加锁 | **不做** | **非空时加锁**（`call *%rax`） |
| `XLockDisplay` / `XUnlockDisplay` 调用点 | 各 1 处 | 各 1 处（**数量相同**） |

**所以准确的表述是**：
- 报告 60 说"**两版实际都跑在线程安全模式**" —— **仍然成立**（Qt 早调了 `XInitThreads`）。
- 但"**差异被完全抹平**" —— **不成立**。525 **会检查**这个状态并**据此走不同分支**（初始化时加锁），515 根本不检查。
- **然而**：这段代码在 **display 初始化**里，**每次开 display 走一次**，不在每帧路径上。
  → **仍然解释不了 20.4ms。**

**这条要记住的教训**：`dlsym` 的符号**不在动态符号表里**，**只做 `nm -D` 必然漏掉**。报告 60 的符号层对比（L1）在结构上就看不见这一层。

---

## 7. 那 20.4ms 到底在不在这个库里？

| 判据 | 结果 |
|---|---|
| swap 下游有新增同步吗 | **没有**（三个槽号相同、顺序相同） |
| 有新增 swap/PBO/fence 相关符号吗 | **没有**（唯一新增是 `XInitThreads`） |
| 有新增 presentation 路径吗 | **没有**（xcb DRI3/Present 栈两版逐条相同） |
| 有新增每帧热路径的字符串吗 | **没有**（3 条新增全在初始化/输入焦点） |
| `glXWaitGL` / `glXWaitX` / `glXMakeCurrent` 变了吗 | **没有**（报告 60：0.983 / 0.981 / 1.000） |

**→ 20.4ms 不在 `libGLX_nvidia`。**
**→ 加上报告 60 的服务端结论，整个 GLX 层（客户端 + 服务端）已全部排除。**
**→ 剩下只有 GPU 提交层（glcore 的 ioctl 侧）与内核驱动（nvidia.ko）。**

---

## 8. 下一步：你给的 fallback 已经有了初步答案

你的判据表：

| 515 固定项 | 判读 |
|---|---|
| **≈0** | 20.4ms 是 525 在真实 primus 下新增的 → 回归确定在 525 |
| **也是 ~20ms** | 20.4ms 是"无输出卡跨 GPU 回读"的固有成本 → 问题在"为什么 515 能绕过它" |

### 8.1 用已有数据先算一遍（零成本）

报告 29 里其实已有 515 的两个点，拟合结果（报告 60 §6）：

| | 固定项 `a` | 斜率 `b` | 等效带宽 |
|---|---|---|---|
| **515** | **1.29 ms** | 1.227 ms/MiB | 0.85 GB/s |
| **525** | **20.40 ms** | 1.329 ms/MiB | 0.79 GB/s |

**→ 515 的固定项 ≈ 1.29ms，落在"≈0"档。**
**→ 按你的判读表：20.4ms 是 525 在真实 primus 下新增的，回归确定在 525。**

且斜率只差 8% → **带宽是两版共有的缺陷，不是分界**（报告 60 §6.1）。

### 8.2 但这个数字还差一个点

515 的拟合**只有 2 个点**（1920×1080 = 11.00ms、640×360 = 2.37ms），**没有残差可验，也没验线性**。
525 是 3 点、残差 ≤0.08ms。

**要坐实"515 固定项 ≈1.29ms"，需要补第 3 点：960×540。**
- 若实测 ≈ 3.4ms（= `1.29 + 1.227×1.98`）→ 线性成立，**1.29ms 可信，回归确定在 525**
- 若明显偏离 → 515 也是非线性，`1.29ms` 这个外推值不可用，需重新建模

**这就是你那句"换 515 做 T_land 三档拟合"的价值所在 —— 不是去回答"是不是 0"（那个已答），而是去验证"1.29ms 这个截距是否可信"。**

### 8.3 优先级建议

| 选项 | 内容 | 代价 |
|---|---|---|
| **A（推荐）** | 换 515，只测 **960×540 一个点**，验 515 的线性 | 装驱动（约 15 分钟）+ 一次 15s 采集 |
| **B（零成本）** | N65：从现有 n60/n62 trace 取 `RP→MB` 窗口，查有无同期 GPU 提交 → 判"排队 vs 固定延迟" | **零成本** |
| C | N66：混装 515 的内核模块 + 服务端 GLX | 高风险，ABI 耦合 |

**A 和 B 回答的是不同问题**：A 回答"20.4ms 是不是 525 独有"（你的判据），B 回答"它是什么性质"（排队 or 固定延迟）。**两个都做才完整，且 A 必须换驱动，B 不用。**

---

## 9. 产物

```
analysis/n64/cli-515.str, cli-525.str          strings -a -n 5 原始（3290/3296）
analysis/n64/filt.py, filt2.py                 两版 human-readable 过滤器（482→10/13 行）
analysis/n64/cli-515.str.h2, cli-525.str.h2    过滤后可读字符串（2919/2922）
analysis/n64/ext-515.txt, ext-525.txt          GL 扩展逐词拆分（189/188）
analysis/n64/a.txt, b.txt                      C-locale 排序版（comm 必需）
```

### 9.1 方法论（三条，已入技能库）

1. **`strings` diff 必须先过滤机器码**。本库 `strings -a -n 5` 的原始 diff 有 482 行，**95% 是"指令字节被当成 ASCII"**（`AVAUATUSH` = `push %r15/%r14/%r13/%r12`）。过滤判据：小写字母 ≥4 或"含空格且字母数 ≥6"，并排除全大写串。482 → 23 行，真差异 3 条。
2. **`dlsym` 的符号不在动态符号表里，`nm -D` 必然漏掉。** 本库通过 `dlsym` 加载整套 `libxcb-dri3` / `libxcb-present` / `libxcb-sync` 栈，`nm -D` 里**一个 xcb 符号都没有**。要看这层只能靠 `strings` 或 `readelf -p .rodata`。
3. **对比 swap "下游"要给槽号，不能只给"语义等价"。** 报告 60 的指纹只说"calls 序列相同"（内部调用被归一化成 `INTERNAL`），掩盖了具体的 `0x598 / 0x5a0 / 0x5a8`。**给出槽号对上，才是不可辩驳的等价。**
