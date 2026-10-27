# 报告 137 — 内核侧否证（bpftrace）：122 µs 不在内核；垫片在 n83a 上验证有效、Heaven 集成受阻

> 编号：137（对应 N146）
> 日期：2026-10-27
> 驱动：525.116.03 / P106-100 / 内核 6.1.0-50-amd64
> 前置：报告 136（环境去污 + 122 µs 定位到「绑对象→draw」的固定 GPU 开销）

---

## 0. 三句话结论

1. **内核完全无罪**：M3 每帧只有 ~2.2 次 `nvidia_ioctl`、合计 ~130 µs（占 127 ms 的 **0.1%**）；
   `schedule_timeout` / `msleep` **零次**，进程 5 秒内只被换出 **27 次且 prev_state 全 0**（`sched_yield` 主动让出）。
   ⇒ 不存在"内核让 CPU 睡 120 µs"这回事。
2. **垫片按"减少【绑定→draw】转换次数"设计，效果严格正比于冗余率**：
   n83a 的 M6（1000 次重复绑同一 program，100% 冗余）**129.7 → 7.22 ms（18.0×），校验和不变**；
   M3（真交替、0.1% 冗余）**127.1 → 127.2 ms，一点不省**。这条边界是数学决定的，不是实现问题。
3. **Heaven 集成卡住了**：垫片已能正确接住 Heaven 的 GL 调用（有 8 次纹理绑定的实证），
   但 Unigine 在建立 GL 上下文后立刻打印 `Shutdown` 退出。**根因未定位**，四条嫌疑已排除，
   剩下的排查方向写在 §5。

---

## 1. 内核侧验证（bpftrace）

### 1.1 环境

```
bpftrace 0.23.2（apt 新装）
/proc/kallsyms: nvidia_ioctl = ffffffffc19270d0 [nvidia]
内核符号地址可读（kptr_restrict=0，读取需 root）
```

### 1.2 脚本

```bash
# 只统计被测进程（pid 过滤），避免 ISR/BH 线程混进来
kprobe:nvidia_ioctl       /pid==$P/ { @st[tid]=nsecs; @ioctl++; }
kretprobe:nvidia_ioctl    /pid==$P && @st[tid]!=0/ {
    $d = nsecs-@st[tid]; @ioctl_sum += $d; @ioctl_us_max = max($d/1000);
    @ioctl_us = hist($d/1000); delete(@st[tid]); }
kprobe:schedule_timeout   /pid==$P/ { @sleep_timeout++; }     # 真正的"睡"
kprobe:msleep             /pid==$P/ { @sleep_msleep++; }
tracepoint:sched:sched_switch /args->prev_pid==$P/ { @prev_state[args->prev_state] = count(); }
```

### 1.3 结果（干净环境，5 秒窗口）

| 指标 | M1（7.16 ms/帧） | M3（127 ms/帧） |
|---|---|---|
| `nvidia_ioctl` 次数 | 438 | **86 ~ 95** |
| 累计耗时 | 26.6 ms | **5.06 ~ 5.36 ms** |
| 单次最大 | 610 µs | **610 µs** |
| **每帧 ioctl 次数** | 0.61 | **~2.2** |
| **每帧 ioctl 时间** | 37 µs | **~130 µs** |
| **占帧时间** | 0.5% | **0.1%** |
| `schedule_timeout` | **0** | **0** |
| `msleep` | **0** | **0** |
| `sched_switch`（本进程被换出） | — | **27 次** |
| `prev_state` 分布 | — | **全是 0（TASK_RUNNING）** |

耗时直方图（M3，µs）集中在 8~128 µs，最大 610 µs：

```
[8,16) 8 |@@@@@@@@   [32,64) 20 |@@@@@@@@@@@...   [512,1K) 2 |@@@@
```

### 1.4 判读

- **ioctl 不是热点**：M3 每帧 2.2 次、130 µs。126,870 µs 的帧时间里有 126,740 µs 与内核无关。
  直接回答"nvidia_ioctl 是不是只调用几十次而不是上万次"：**每帧两次，不是上万次**。
- **内核没有让 CPU 睡**：`schedule_timeout`/`msleep` 计数为零。
- **`sched_switch` 只有 27 次且 prev_state=0**：这是 `sched_yield` 的自愿让出
  （只有一个可运行任务时 `__schedule` 会选回自己，不产生真正的切换）。
- 注意：`kprobe:schedule` 计数高达 **615 万次/5 秒**，但 `sched_switch` 只有 27 次 ——
  这个差值本身就是"CPU 在用户态疯狂 `sched_yield` 轮询、但没人可切"的指纹，
  与报告 124 的 `sched_yield` 结论一致。**"调用 schedule" ≠ "睡眠"**，这是本轮要修正的一个易错点。
- 后台慢 ioctl（>50 µs）的内核栈只有一条，就是 ioctl 本身，没有 `schedule_timeout` 夹在里面：
  ```
  nvidia_frontend_unlocked_ioctl+56
  __x64_sys_ioctl+144
  do_syscall_64+93
  entry_SYSCALL_64_after_hwframe+110
  ```

⇒ 与报告 136 的 `-R` 实验（不回读时 CPU 只在退出时阻塞一次、GPU 仍 132 ms/帧）完全一致：
**122 µs 是 GPU 侧的固定开销，内核与 CPU 调度都不是原因。**

---

## 2. 垫片设计

代码：`analysis/shim/glbatch.c`（约 700 行，`gcc -O2 -fPIC -shared -o glbatch.so glbatch.c -ldl`）

### 2.1 为什么不是"合并批次"而是"减少绑定→draw 的转换"

报告 136 的三条实测把设计钉死了：

| 事实 | 对设计的约束 |
|---|---|
| M8（1000 次绑定、只画 1 次）= 2.97 ms，绑定几乎免费 | 不要试图"攒起来一起发"，绑定本身不花钱 |
| M3（绑定紧跟 draw，交替 1000 次）= 127 ms | 花钱的是**每一次"绑定→draw"的转换** |
| M9（先切完 1000 次再画 1000 次）= 7.5 ms | 把绑定与 draw 分离到极致就不花钱 |

所以垫片的目标函数是：**在不改变渲染结果的前提下，减少"状态发生变化后又立刻 draw"的次数**。
只有两条路能做到：
1. **去掉冗余绑定**（绑的值和当前一样 → 本来就没效果）→ 零语义变化；
2. **真正重排 draw**（按材质分组）→ 改变绘制顺序，透明/混合会错，LD_PRELOAD 层无法保证正确性。

### 2.2 三种模式

| 模式 | 行为 | 语义风险 |
|---|---|---|
| `count` | 只统计、**无条件转发** | 无（基线，用来量潜力上限） |
| `dedupe` | 绑定值与"已下发值"相同则吞掉 | **无**（把同一对象再绑一次本来就是 no-op） |
| `reorder` | program/纹理绑定延迟到 draw 前统一下发；所有已知的修改/查询入口先 flush | 低（但 bind→修改 的写法需靠 flush 兜住） |

### 2.3 拦截面

- **绑定**：`glUseProgram` / `glBindTexture` / `glActiveTexture[ARB]` / `glBindFramebuffer[EXT]` /
  `glBindRenderbuffer` / `glBindVertexArray` / `glBindBuffer[Base|Range]` / `glBindSampler` / `glBindProgramPipeline`
- **绘制**：`glDrawArrays` / `glDrawElements` / `*Instanced` / `glDrawRangeElements` / `glMultiDraw*`
- **刷新**：`glFlush` / `glFinish` / `glXSwapBuffers`（帧边界）/ `glXMakeCurrent`（换上下文→清缓存）
- **flush 触发点**：`glClear` / `glReadPixels` / `glTexImage2D*` / `glTexParameteri` / `glBufferData*` /
  `glUniform*` / `glFramebufferTexture2D` / `glLinkProgram` / `glViewport` / `glGetIntegerv` / `glMapBuffer` / `glDelete*`
- **符号劫持三路**：PLT 符号插桩 + `glXGetProcAddress[ARB]` + **`dlsym`**

> `dlsym` 这层是必需的：Unigine 是 `dlopen("libGL.so.1")` 之后按句柄 `dlsym` 取入口，
> 前两种全被绕过（实测垫片已加载 5 个映射、但 GL 入口调用计数为 0）。

### 2.4 统计接口

`GLBATCH_LOG=<file>` + `kill -USR1 <pid>` 随时把累计计数写盘：

```
[usr1] t=86.83s swaps=… draws=… binds=… elided=… deliver=… flush=… fps=…
```

`fps = swaps / t`，用交换计数算帧率，不依赖画面上的 FPS 数字（避免 OCR 与遮罩问题）。

---

## 3. n83a 上的定量结果（这是本轮最硬的部分）

### 3.1 三模式 × 三场景

| 场景 | 无垫片 | `count`（不改行为） | `dedupe` | 校验和 |
|---|---|---|---|---|
| **M1**（1000 draw，0 绑定） | 7.189 ms | 7.196 ms | 7.197 ms | 7000 全同 |
| **M3**（交替，140147 次绑定） | 127.247 ms | 127.141 ms | **127.246 ms** | 7000 全同 |
| **M6**（重复绑同一 program，140287 次绑定） | **129.705 ms** | 129.743 ms | **7.224 ms** | 7000 全同 |

`elided`（被吞掉的冗余绑定）：

| 场景 | 绑定总数 | 冗余数 | 冗余率 | 加速 |
|---|---|---|---|---|
| M1 | 147 | 140 | 95%（全是 setup） | 1.00× |
| **M3** | 140,147 | **140** | **0.1%** | **1.00×** |
| **M6** | 140,287 | **140,280** | **100.0%** | **17.96×** |

### 3.2 这张表能说明三件事

1. **垫片实现是对的**：`count` 与无垫片差 0.03%（129.743 vs 129.705），说明插桩本身没有开销；
   `dedupe` 在校验和完全不变（7000）的前提下把 M6 拉快 **18 倍**。
2. **M6 = 18× 是"零风险上限"的实测值**：M6 的 1000 次绑定全是重复绑同一个 program，
   吞掉它们**在 GL 语义上等价于什么都没做**，所以这 18 倍是白拿的、不改变任何像素。
3. **M3 = 1.00× 是"零风险方案的天花板"**：M3 真的在交替两个 program，每次绑定都改变了状态，
   没有冗余可以吞。**要救 M3 只能重排 draw 顺序**——那就进入"会改画面"的领域。

⇒ **对 Heaven 的全部判断，归结为一个可测的数：Heaven 每帧的绑定里有多少是冗余的。**
`count` 模式就是用来零风险量这个数的（`binds per draw` 与 `elided share`）。

---

## 4. Heaven 集成：走到哪一步，卡在哪

### 4.1 已完成

- 用 `engine_config ../data/heaven_4.0.cfg` + 完整参数直接启动 `heaven_x64`（不经启动器），
  确认落在 P106 上（`libnvidia-glcore.so.525.116.03` 已映射、显存 348~681 MiB、功耗 46 W）。
  启动器窗口走的是 Mesa（未映射 glcore），不能用来测量 —— 这也是历史上"没加 primusrun 就落到核显"的同一个坑。
- 修掉三个真实 bug（每一个都会导致垫片完全不工作或把应用搞死）：

| # | Bug | 现象 | 修法 |
|---|---|---|---|
| 1 | `LOAD()` 用 `#fn` 把变量名 `r_UseProgram` 当符号名，解析结果全 NULL | 一进 GL 就段错误（调用 0 地址） | `ld_sym()` 去掉 `r_` 前缀拼 `gl…` |
| 2 | `dlsym` 拦截后，库内部 `dlsym(RTLD_NEXT,…)` 也走自己的包装 → 拿回自己的函数 → **自递归卡死** | 进程 6 W、32 MiB、70 秒不动 | `handle == RTLD_NEXT` 直通 |
| 3 | `glBindFramebuffer(GL_DRAW/READ_FRAMEBUFFER)` 被改写成 `GL_FRAMEBUFFER` | 破坏"只绑 draw 侧"语义 | 非 `GL_FRAMEBUFFER` 一律透传 |

- 确认垫片**真的接住了 Heaven**：`GLBATCH_TRACE` 显示前 8 次 `call` 全是 `slot=9`（unit0/GL_TEXTURE_2D）
  绑定 `1,0,1,0,2,0,2,0` —— 正是 Unigine 建纹理的标准序列。

### 4.2 卡点

加上垫片后，Unigine 打印完 `OpenGL version: 3.2.0` 就 `Shutdown` 退出：

```
---- Render ----
OpenGL vendor:   NVIDIA Corporation
OpenGL renderer: NVIDIA P106-100/PCIe/SSE2
OpenGL version:  3.2.0 NVIDIA 525.116.03
Shutdown
```

**注意：不加垫片时版本也是 3.2.0 并能继续跑到加载材质**，所以不是"版本被降级"，
而是上下文建立后紧随其后的某个调用被我们改变了。

已排除的四条：

| 嫌疑 | 排除方式 | 结果 |
|---|---|---|
| 查询/修改类包装（`glGetIntegerv`/`glViewport`/`glTexImage2D`…） | `GLBATCH_MIN=1` 只包绑定/绘制/刷新 | 仍在同一处退出 |
| FBO target 被改写 | 非 `GL_FRAMEBUFFER` 透传 | 仍是同一处退出 |
| 纹理绑定的 active unit 保存/恢复用了自己的记录 | 改为向驱动查 `GL_ACTIVE_TEXTURE` 再恢复 | 仍是同一处退出 |
| `load_all()` 一次性解析，而 Heaven 稍后才 `dlopen` libGL | 增加"缺了就补"的二次解析 | 仍是同一处退出 |

### 4.3 下一步（给接手的人）

1. **用最保守的"纯透传"验证基线**：加一个 `GLBATCH_MODE=pass`（所有包装立即转发、不记账、
   不做 ActiveTexture 舞蹈），若它也不退出，则说明问题一定在"我们改变参数/顺序"的三处：
   纹理 unit 舞蹈、FBO 合并、`flush_pending` 的下发时机。
2. **在真机上二分**：`GLBATCH_TRACE` 已经能打出前 4000 次调用，加一个"只在第 N 次之后开始包装"的
   `GLBATCH_SKIP=N`，快速定位是初始化阶段还是渲染阶段被破坏。
3. **换一个更简单的真实负载**先用：例如一个自写的多材质场景（几百个 draw、几十个材质），
   垫片在这种场景下的收益可以直接用 `binds per draw` 预判，不必依赖 Heaven。
4. Heaven 的 `-video_app opengl` 之外，可以试 `-video_app opengl` + `-video_width/height` 更小，
   排除"大尺寸 RT 触发不同路径"。

---

## 5. 本轮的操作事故（记录，避免重犯）

### 5.1 两个 heaven 同时跑

原因：用 `timeout 40 ./heaven_x64 … | grep … | head -25` 启动 GUI 进程。
- `head` 读够行数后关闭管道 → `grep` 收到 SIGPIPE 退出，但 `heaven_x64` 已经 `setsid` 脱离了会话，
  不受影响，继续跑；
- 更早一次 `gdb … | tail -25` 被命令超时（120 s）杀掉时，SIGTERM 只终止了 ssh 客户端，
  **远端的 gdb + heaven_x64 变成孤儿，继续跑了 4 分 15 秒**，占着 688 MiB 显存、53 W。

后果不只是"多一个窗口"：那段孤儿进程正好覆盖了我做 bpftrace M1/M3 采样的两分钟，
把 M3 的 ioctl 计数从 86 压到 44（帧率被拉低、5 秒内帧数变少）。**已在干净环境重测并采用重测值。**

纪律：
- 远端启动 GUI/长跑进程，**必须自己拿 pid、自己显式 kill**，不依赖 `timeout` 和管道；
- 脚本里 `kill -9 $P` 之后要 `pgrep` 复核"残留: 无"；
- 测量前先 `nvidia-smi` 确认显存 0 MiB、功耗 6~8 W。

（本轮已清干净：无 heaven/browser/gdb 残留，GPU 6.31 W / 0 MiB。）

### 5.2 ssh 弹窗

我在 `tmp/h/r.sh` 里用 `SSH_ASKPASS` + `DISPLAY=:0` 把密码喂给 ssh，实现非交互登录。
当某次调用回落到交互认证时，ssh 会调用图形化的 askpass 助手，**那个框就弹在你的桌面上**。
密码是硬编码在 `tmp/h/ap.sh` 里的本地账户口令，**本轮结束时已删除该文件**。
如果你希望我以后完全不用这种方式，我可以改成免密公钥（需要你在 `~/.ssh/authorized_keys` 里放一次公钥）。

---

## 6. 复现命令

```bash
# 内核侧
sudo /home/<user>/knl2.sh 3 5      # M3：ioctl 次数/耗时/睡眠/prev_state

# 垫片（n83a，最能说明问题的两个用例）
cd <workdir>/analysis/n83a
LD_PRELOAD=<workdir>/analysis/shim/glbatch.so GLBATCH_MODE=dedupe GLBATCH_STATS=1 \
  ./n83b -M 6 -n 1000      # 129.7 → 7.22 ms，chk 仍 7000
LD_PRELOAD=... GLBATCH_MODE=dedupe GLBATCH_STATS=1 ./n83b -M 3 -n 1000   # 127.2 ms，一点不省

# Heaven（当前会 Shutdown，用于继续排查）
GLBATCH_TRACE=/tmp/t.txt /home/<user>/hab.sh count 15
```

---

## 7. 与本项目主线的衔接

报告 136 把病灶定位为"每次【绑对象→draw】的固定 122 µs"。本报告补上两件事：

- **内核侧彻底排除**（ioctl 2.2 次/帧、无睡眠）；
- **给出"不改画面能救多少"的定量上限**：救得动的只有冗余绑定，
  M6 那种 100% 冗余能拿 18 倍，M3 那种真交替拿 0 倍。

因此对 Heaven 的判断不需要先跑通垫片：**先量它的 `binds per draw` 和 `elided share`**。
若冗余率很高（引擎常见的"每个 draw 都重绑一次"），`dedupe` 就能拿到接近线性的收益且不改画面；
若冗余率很低（真交替），剩下的只有"重排 draw"，而那必须接受画面语义变化
—— 这也解释了报告 128/129 里"改 bit 拿到 85 倍但画面抽搐"的本质：
**收益与正确性在这里是同一个开关。**
