# 71 · N80-B：`glMapBuffer` 内部没有"等 idle"——只有 2ms 量化的提交背压轮询；同时推翻报告 70 的 99.4%

日期：2026-10-26 11:40
驱动：525.116.03（P106-100，1920×1080，primus）
核心：**`glMapBuffer` 反汇编到 11 条指令，5 分钟内可以读完；但它同时把报告 70 的结论掀翻了**

---

## 0. 一句话

1. `glMapBuffer` 在 glcore 里只有 **11 条指令**：写一个 12 字节命令包（opcode `0x617b`）+ 调统一提交器 + 读 `ctx` 里一个预置指针槽返回。**没有任何 fence / sem / cond / ioctl —— 它自己不"等"任何东西。**
2. 真正的阻塞在**统一提交器**里，而提交器的"走不动"分支调的是 **报告 65/66 那个 `wait_ms`，超时参数 `2`（毫秒）**，在**重试循环**里。运行时实测相邻两次 `wait_ms` 间隔 **中位 2.073ms（p90 2.084）** —— 严格 2ms 量化。
3. **⚠️ 重大更正**：报告 70 说"99.4% 帧时间在 `glMapBuffer`（30.812ms）"——**错了**。
   同一条 trace 里两套探针同时打，`glMapBuffer` 真实时长是 **2.758ms = 帧时间的 9.9%**。
   报告 70 的 30.8ms 实际包含了 **primus 自己的 `usleep`（24.8ms）**。

---

## 1. 派发链：三层跳转，两版槽号逐个相同

`glMapBuffer` 是 `libGLX_nvidia` 的导出符号（`nm -D` 有），但 impl 在 `libnvidia-glcore`（只导出 30 个混淆符号，函数名全无）。链是三层：

```
libGLX_nvidia:glMapBuffer  @ +0xbd080 (525) / +0xc5080 (515)
   ↓  lea rax,[tls_off]; rax=[fs:rax]; rax=[rax]; rax+=glcore_base
   jmp [rax+0xf10]                       ← 派发表槽 0xf10（两版相同）
   ↓  glcore：又是同一套 TLS 取表
   jmp [rax+0xd90]  /  or  and r11d,7 / jmp [rax+r11*8+0xbf0]
   ↓
glcore+0x103a2b0 (525)  /  +0xa96b30 (515)   ← 真身
```

**槽号对照（2673 个 trampoline 全抽，`n80b_slots.py`）**

| 函数 | 槽 | index |
|---|---|---|
| glMapBuffer | `0xf10` | 482 |
| glUnmapBuffer | `0xf18` | 483 |
| glFinish | `0x548` | 169 |
| glFlush | `0x550` | 170 |
| glReadPixels | `0x688` | 209 |
| glBindBuffer | `0xec8` | 473 |
| glMapBufferRange | `0x15a0` | 692 |

- **515 与 525 的槽号 100% 相同**（最大槽 `0x3c48` vs `0x3c40`，只差最后一项，因为导出符号 2674 vs 2673）。
- 派发表规模 = **1930 项**，在 `.data.rel.ro` 里有 **20 份变体**（按渲染后端/profile 分），每份 1977 项；相邻槽 482/483、169/170 的实现地址固定相差 `0x20~0x30` —— 这是"批量生成的 thunk 按槽号顺序排"的结构指纹，可用来验证表基址猜对了。

> 定位真身的最省事办法不是继续解派发表，而是 **gdb 在 `libGLX_nvidia:glMapBuffer` 挂一个断点 + `si` 一步**，直接落到 glcore 地址。见 §7。

---

## 2. `glMapBuffer` 真身：11 条指令

**525 · `glcore+0x103a2b0`**

```asm
103a2b0: mov  rax,[rip+0x1991da9]      ; TLS 偏移变量 (glcore+0x29cc060)
103a2b7: push rbx
103a2b8: mov  rbx,fs:[rax]             ; rbx = ctx
103a2bc: mov  rax,[rbx]                ; rax = 命令缓冲写指针 W
103a2bf: mov  [rax+4],edi              ; 记录 target
103a2c2: add  rax,0xc
103a2c6: mov  rdi,rbx                  ; rdi = ctx
103a2c9: mov  [rax-4],esi              ; 记录 access
103a2cc: mov  esi,1                    ; 第 2 参数 = 1
103a2d1: mov  dword [rax-0xc],0x617b   ; ★ opcode 0x617b = "MapBuffer"
103a2d8: mov  [rbx],rax                ; 提交写指针（12 字节包入队）
103a2db: call 0x1238920                ; ★ 唯一调用：统一命令提交器
103a2e0: mov  rax,[rbx+0x8e0a10]       ; ★ 返回值 = ctx 里预置的映射指针槽
103a2e7: pop  rbx
103a2e8: ret
```

**515 · `glcore+0xa96b30`** —— 逐指令同构，只有三处地址不同：

| 项 | 515 | 525 |
|---|---|---|
| 提交器 | `+0xc83500` | `+0x1238920` |
| map 指针槽 | `ctx+0x55ed90` | `ctx+0x8e0a10` |
| opcode 写入点 | `+0xa96b49` | `+0x103a2d1` |

**判读（这一条最硬）：**

- 没有 `glFinish` 式的 drain，没有 fence wait，没有 `sem_wait`，没有 ioctl，没有 `syscall`。
- 返回值是 `ctx` 里一个**固定槽**，说明映射地址是**预先建好的**（PBO 一贯映射），map 不做"建立映射"这件事。
- ⇒ **"`glMapBuffer` 内部带一次等 GPU idle"这个假设，静态上直接否定。**
- 但它也**不能**推出"map 很快" —— 全部时间在 `call 0x1238920` 里。

---

## 3. 提交器：2ms 粒度的背压轮询

**525 · `glcore+0x1238920`**（515 · `+0xc83500`，结构相同，槽号平移）

```asm
1238920: push r13 ; r13d = esi          ; 记住 flavor（map 传的是 1）
1238927: mov  r12d,1
123892f: rbx = rdi                      ; ctx
1238936: cmp  sil,0x1
123893a: sbb  r12d,-1                   ; r12 = sil<1 ? 2 : 1
123893e: cmp  byte [ctx+0x8e0c4c],0     ; 统计开关
1238945: jne  123898e
           … 用 [ctx+0x8e0c40] 做时间戳差，累加进 [ctx+0x8e0c50+idx*4] …   ; ★ 时长累加器
123898e: call 0x1238d80
1238996: movss xmm0,[ctx+0x8e0c60]      ; ★ 另一个累加器
12389a4: addss xmm0,[now-prev]
12389bb: add  dword [ctx+rax*8+0x8e0c00],1
12389c3: sfence                          ; ★ 内存屏障
1238a28: cmp  [ctx+0x8e0a00], [ctx]      ; ★ 队列读指针 == 写指针？（空？）
1238a35: je   1238b48
1238a3b: test byte [ctx+0x30],1
1238a41: 三档 float 预算判据（见 §3.1）
1238ab1: cmp  byte [ctx+0x8e0c70],0
1238ab8: jne  12389d0                    ; ★ 回环
1238abe: rdi = [ctx+0x8e0908]
1238acc: esi = 2                          ; ★★ 超时 = 2（毫秒）
1238ad1: call [nv014glcore+0x2d0]         ; ★★ = glcore+0x146d280 = wait_ms
1238ad7: if [nv014+0x1b8] 非空 → call（运行时实测为 0，跳过）
1238af8: … ret
```

**`wait_ms` 的归属（运行时读出，不是猜）**

| vtable 槽 | 525 地址 | glcore 偏移 | 515 槽 / 偏移 |
|---|---|---|---|
| `_nv014glcore+0x2d0` | `0x7fa71a86d280` | **`+0x146d280`** | `+0x2b8`（同一函数族） |
| `_nv014glcore+0x290` | `0x7fa71a86c860` | `+0x146c860` | — |
| `_nv014glcore+0x280` | `0x7fa71a86d240` | `+0x146d240` | `+0x280` |

`+0x146d280` 正是报告 65/66 通过 gdb 栈 + futex uaddr 定出来的 **`wait_ms`（`pthread_cond_timedwait` 包装）**，报告 45 里"2ms 硬编码"也对上了。

### 3.1 三档 float 预算判据（525）

```
xmm1 = [ctx+0x8e0e00]            ; 某个预算/阈值
xmm3 = [rip+0xa197e7]            ; 常数 A
xmm0 = [ctx+0x8e0c60]            ; 累加器（运行时实测 9942）
if A <= xmm1        → 走 0x1238b08
if xmm1 <= [rip]    → 走 0x1238b08
if xmm1*[rip] >= xmm0 → 走 0x1238b08
else                → esi=7; call 0x12386d0
```
即"**距离上次提交攒了多久 / 攒了多少**"→ 决定"现在刷"还是"再等等"。**这是一套节流（pacing）策略**，不是等待 GPU 完成的同步原语。

### 3.2 515 与 525 的一处真实差异

> ⚠️ **本节结论已被报告 72（N81-B）更正**：那道 gate 在 **515 / 525 / 535 都存在**（都是 bit 1），580 换成 bit 30。本报告只反汇编了 525 提交器前 0x1f0 字节，而 gate 在 **+0x211**（`0x1238b31`），刚好在视野外。**"515 独有" 不成立，它不是版本分界点。**

515 在 `c835c6` 有一段 525 里位置更靠后的东西：

```asm
c835c6: lea  rax,[rip+..# 252cee0 = _nv026glcore+0x8f0]
c835e5: mov  eax,[rax+0xb48]        ; 全局 flags
c835eb: and  eax,0x2                ; 测试 bit 1
c835ee: ucomiss xmm3,xmm1           ; 之后才进同样的三档比较
```

即 **515 的刷队列决策多受一个全局配置位控制，525 直接进三档比较**。这是本轮唯一找到的"提交节流策略上的版本差异"，值得单独跟进（见 §6 N81-B）。

---

## 4. 运行时验证：`wait_ms` 严格 2ms 量化

uprobe 挂在 `glcore+0x103a2b0`（map 入口）、`+0x103a2e8`（ret）、`+0x146d280`（`wait_ms`），稳态采 10s：

| 量 | 值 |
|---|---|
| `glMapBuffer` 次数 | 413 / 10s |
| map 时长 | 中位 **1.93ms**，均值 2.35，max 5.70 |
| 每帧 `wait_ms` 次数 | 中位 **1**，均值 1.2，max 4 |
| **相邻 `wait_ms` 间隔** | **中位 2.073ms，p90 2.084ms**（直方图双峰：≈0 或 ≈2ms） |

间隔直方图 `{0: 117, 2: 109}`：一半是"立刻返回"（条件已满足），一半是**整 2ms**（睡满超时）。**没有一个落在 0.5~1.5ms** —— 这是超时量化，不是"等某个事件被唤醒"。

---

## 5. ⚠️ 重大更正：报告 70 的「99.4% 在 glMapBuffer」作废

### 5.1 怎么发现的

N80-B 顺手核对了"map 到底多久"，结果与报告 70 差一个数量级。**在同一条 trace 里同时挂两套探针**做对照（`n80b_map5.sh`）：

| 探针 | 位置 | 10s 中位时长 |
|---|---|---|
| primus `mb_in→mb_out` | `libGL.so.1:0x18902 → 0x18908` | **2.737 ms** |
| glcore `gmap_in→gmap_ret` | `glcore+0x103a2b0 → +0x103a2e8` | **2.725 ms** |
| 两者偏移 | `mb_in→gmap_in` | **0.007 ms** |

两套探针 358 次配对，**差值 0.012ms** —— 它们测的是同一个东西，就是 `glMapBuffer` 本身。

**所以 `glMapBuffer` = 2.7ms，不是 30.8ms。**

### 5.2 正确的全帧分解（525，1920×1080，稳态 10s，353 帧）

执行顺序（事件流实测）：

```
us_out → mb_in → mb_out → ws_in/ws_out → rp_in/rp_out → us_in → …usleep… → us_out
         └─ map ─┘        └ glWaitSync ┘  └ glReadPixels ┘   └─ 睡 24.8ms ─┘
```

| 段 | 中位 | 占帧 |
|---|---|---|
| `glMapBuffer` | **2.758 ms** | **9.9%** |
| `glWaitSync` | 0.008 ms | 0.03% |
| `glReadPixels` | 0.089 ms | 0.3% |
| **`usleep`（primus autosleep）** | **24.823 ms** | **89.2%** |
| 合计 | 27.678 ms | 99.5% |

帧间隔（相邻 `mb_in`）中位 **27.83ms = 35.9 fps**；另一次 30s 采样帧间隔中位 **30.72ms = 32.5 fps**（正是此前所有报告引用的 30.99ms/帧）。

### 5.3 报告 70 错在哪

报告 70 的四段分解：

```
W→WS 0.037 | WS→RP 0.006 | RP→MB 0.137 | MB→W 30.812 (99.4%)
```

- `WS→RP`（6µs）、`RP→MB`（137µs）与本轮实测（8µs / 89µs）**同量级、方向一致** → 这两段是对的。
- 只有 **`MB→W` 被写成了 30.812ms**，而同一对探针（`0x18902 → 0x18908`）实测是 **2.758ms**。
- 30.812 ≈ **map(2.76) + usleep(24.8) + 余量**，也就是它把 **primus 的 `usleep` 算进了 `glMapBuffer`**。
- 四段之和恰好等于帧时间（30.992ms）这一点具有欺骗性：**任何"把整帧分成连续几段"的错误配对都会自动满足这个和**，所以"总和吻合"不能当作配对正确的证据。
- 独立旁证：报告 59（N62）早就测出"**sleep 89.9%、map 9.8~9.9%**"，与本轮 89.2%/9.9% 一致。**报告 70 与报告 59 冲突时，该被推翻的是报告 70。**

### 5.4 连带作废

| 报告 70 的说法 | 现状 |
|---|---|
| 「99.4% 帧时间在 `glMapBuffer` 一个调用里」 | ❌ **错**，9.9% |
| 「`glMapBuffer = 20.25 + 1.336×MiB`」 | ❌ 拟合的是"map + usleep"，不是 map |
| 「`T_land` 的实体就是 `glMapBuffer` 的阻塞时间」 | ❌ `T_land` ≈ **帧周期**，主导项是 primus autosleep |
| 「固定项 20.25ms ≈ 1000 次 GL 调用 × 20µs」 | ❌ 那 20ms 是 **autosleep 的固定分量**，与 GL 调用数无关 |
| 「`glWaitSync` 快是设计使然，不能判 GPU 进度」 | ✅ **仍成立**（§2 的结论方向正确） |
| 「旧 trace 的探针可重新组合」 | ⚠️ 成立，但**必须验证配对完整性**（见 §7 教训） |

---

## 6. 对 N80 判据表的修订

原判据表的前提（"map = 30ms"）不成立，实验要重设。**但"`glMapBuffer` 里在等什么"这个问题已被静态回答：它不等任何东西，它只是提交。**

修订后的表（如果还要做 `glFinish` 实验）：

| N80 结果 | 原判读 | **修订判读** |
|---|---|---|
| `glFinish`≈20ms，map 降到 ~10ms | 等渲染 | 仍可能是"等渲染"，但**map 本来就只有 2.7ms**，降不到你说的量级；此格实际测的是 **`glFinish` 自己** |
| `glFinish`≈0，map 仍 30ms | 等拷贝 | **不可能出现**：map 上界实测 9.99ms |
| 两者都≈0，总时间消失 | fence 语义 | 不适用 |

**⇒ N80（插 `glFinish`）建议不做。** 它设计出来是为了分辨 map 的 30ms 归谁，而 map 没有 30ms。

### 6.1 那 24.8ms 是不是问题？

不是"等待"，是 **primus 的 autosleep 主动睡眠**（`sleep_usec = (map_time*1e6 + sleep_usec) * autosleep/100`）。

- 它是**果**：map 只要 2.76ms，autosleep 按比例睡到 ~9× map ≈ 24.8ms 来压 CPU 占用。
- 报告 59（N62）4 个变体把 autosleep 关掉，**fps 不变** ⇒ CPU 让出来以后，时间被别人占走。
- ⇒ 帧周期由 **`readback_work` 之外的环节**决定。worker 的 24.8ms 睡眠只是"填补"。

---

## 7. 方法论（已入技能库）

1. **★ 闭源库的函数体，用"运行态一跳"定位比静态解派发表快一个数量级。**
   `nm -D` 有符号 → `gdb -p PID` 挂断点 → `si` 一步 → `info symbol $pc` / 用 maps 反推偏移。
   全程 10 秒，不改任何二进制。静态那三层派发表（`.data.rel.ro` 里 20 份 1977 项变体）只能用来**验证**，不适合用来**发现**。

2. **★★ 用"总和吻合帧时间"验证分段分解是无效的。**
   连续切的任何分段方案，和都等于帧时间。**必须另找独立锚点**：本例是"同一 trace 挂两套不同库的探针做交叉"。

3. **★★ 两个探针的地址相邻 ≠ 它们之间的时间只包含那个调用。**
   报告 70 认定的 `MB→W` 确实相邻（本轮实测 0.006ms 就到 `ws_in`），但旧 trace 的**配对**跨了帧。
   ⇒ 事件配对必须校验：**配对成功数 == 各事件计数**，且**段内顺序与实际指令顺序一致**。

4. **★ 静态能秒杀"你这个假设不成立"。**
   `glMapBuffer` 11 条指令、无 fence/sem/ioctl —— 一句话终结"它内部等 GPU idle"。**任何"某函数内部在等什么"的问题，先读函数体，再考虑上机。**

5. **★ 闭源 glcore 的定位锚点（本案固化）**

| 项 | 525 | 515 |
|---|---|---|
| `glMapBuffer` impl | `+0x103a2b0` | `+0xa96b30` |
| 统一提交器 | `+0x1238920` | `+0xc83500` |
| `wait_ms` | `+0x146d280` | 同族（vtable `+0x2b8`） |
| map 指针槽 | `ctx+0x8e0a10` | `ctx+0x55ed90` |
| 提交器 flavor 参数 | `esi=1` | `esi=1` |
| opcode | `0x617b` | `0x617b` |

---

## 8. 产物

```
analysis/n64/n80b_slots.py     # 抽 2673 个 trampoline 的槽号（表规模、版本对照）
analysis/n64/n80b_runs.py      # 在 .rela.dyn 里按 8 字节连续切出候选表
analysis/n64/n80b_impl.py      # 取候选表某槽的实现地址
analysis/n64/n80b_pair.py      # 用相邻槽"实现地址相邻"验证表基址
analysis/n64/n80b_l2.py        # 第二层槽
analysis/n64/n80b_slots2.py    # 解 _nv014glcore 的驱动槽
analysis/n64/n80b_xver.py      # 跨 6 版本找 opcode 0x617b 写入点 + 提交器序言
analysis/n64/n80b_cnt.py       # trace 事件计数
analysis/n64/n80b_frame.py     # 按 map 配对统计 wait_ms 次数
analysis/n64/n80b_ts.py        # 30s 时间序列（场景依赖检查）
analysis/n64/n80b_cmp.py       # ★ 两套探针交叉验证
analysis/n64/n80b_full.py      # ★ 正确全帧分解
analysis/probes/n80b_map{,2,3,4,5,6}.sh
analysis/n80b/trace{3,4,5,6}.txt
```

---

## 9. 下一步

**框架变了**：不是"map 里 20ms 在等什么"，而是"**谁把帧周期定在 27.8~30.7ms**"。
worker 自己睡了 89%，且关掉它 fps 不变（报告 59）⇒ 定周期的是 **`readback_work` 之外的环节**。

| 编号 | 做什么 | 回答什么 |
|---|---|---|
| **N81-A（推荐）** | uprobe 主线程的 `glXSwapBuffers` 窗口 + `display_work` 的 `glTexSubImage2D`/`glXSwapBuffers(ddpy)` 窗口 + 主线程两个 `sem_wait` 窗口，一条 trace 出四段，**并用 §7 纪律校验配对** | 主线程 27ms 到底在哪一段：`render 提交` / `等 worker` / `swap 到 Intel`。这是唯一还没被正确分解的一段 |
| **N81-B（零成本，静态）** | 对比 515 `c835c6` 那段"全局 flags `_nv026glcore+0x8f0+0xb48` bit1"在 525 里迁到哪去了（或是否被删）。同看 535/550/580 | **提交节流策略的版本差异**——目前唯一找到的候选分界点 |
| ~~N80~~ | ~~插 `glFinish` 分 map 的 30ms~~ | **不做**：map 只有 2.7ms，前提没了 |
| N80-C（可选） | `autosleep` 设 0 时同时采 §5.2 的四段 | 验证"睡眠让出时间后被谁占走"（报告 59 只看了 fps，没有分段） |
