# 26 · 三刀拆解判决：26ms 在 autosleep 不在 fence，真凶是回读完成延迟 D≈29ms

> 2026-10-25 18:50
> 方法：不换驱动、不改配置。在 525.116.03 上用 17 个 perf uprobe 把 primus readback 流水线
> （主线程 sem_wait / worker glWaitSync→glReadPixels→usleep→glMapBuffer）逐段夹住，
> 再用 20kHz /proc/pid/mem 轮询直接观测 PBO 数据落地时刻；隔离环境最小回读测试做对照。
> **六条结论：**
>
> 1. **报告 25 的机制判决被推翻**：`glWaitSync` 实测中位 **5µs**（n=521，max 39µs）——调用时 fence 早已完成（年龄中位 21.8ms）。「驱动察觉 fence 完成被推迟 ~26ms」没有测量支撑。
> 2. **修正后的流水线语义**：`sem_post(relsem)` 在 `glReadPixels` 之后、`usleep` 之前（反汇编 0x18a10 出站块 + 事件流铁证）。主线程在 rp_out 后 **57µs** 即醒；那 22.0ms 的 `sem_wait` 等的是「worker 还在跑上一轮的 usleep+map」，不是等像素。
> 3. **IIR 二进制级坐实**：`s_{n+1}=0.9(Δ_map+s_n)` 稳态 `s*=9m`；帧级残差中位 **13.1µs**（n=519）；`us/D = 0.9000`、`us/mb = 9.24`。
> 4. **真凶候选 = D = mb_out−rp_out = 29.369ms**（us 26.434 + mb 2.862 = 29.296）。usleep 占 D 的 90%但它是果：`PRIMUS_SLEEP=0` 只快 2ms，因为关掉后 glMapBuffer 直接阻塞等数据，轮次时长仍是 D。
> 5. **数据落地时刻直接测量**（pbo_watch，440/440 匹配）：PBO 数据在 rp_out 后中位 **16.5ms**（0.01~36.6ms）才落地，**84% 在 usleep 中段、16% 在 map 调用窗口内、0% 在 map 返回后**；到达在 [0,D] 上近似均匀、与 D 相关性弱（r=0.15）→ 应用内拷贝铺满整个 30ms 窗口。
> 6. **隔离环境回读路径健康**：525 直连最小测试 fence 0.000 / rp 0.003 / **D=2.488**（gpu_copy 2.487）/ 通知 0.000；pbuffer 模式 D=0.000 → 29ms 不是回读路径 intrinsic，是应用语境造成的。

---

## 1. 修正后的流水线语义（事件流铁证）

报告 25 的源码阅读漏了一处执行序细节：`sem_post(&di.r.relsem)` 虽然在源码里位于 `glReadPixels` 之后、`usleep` 之前，但编译后落在 0x18a10 的出站块里，光看反汇编地址顺序会误判。原始事件流一锤定音（pbo2.script，tid 22374=worker / 22326=主线程）：

```
11174.886059: rp_out   ← worker glReadPixels 返回（拷贝命令已推给驱动）
11174.886066: us_in    ← 7µs 后 worker 已进入 usleep
11174.886121: msw_out  ← 55µs 后【主线程】sem_wait 返回
11174.890769: msw_in   ← 4.6ms 后主线程渲染完下一帧，再次进入 sem_wait
11174.912542: us_out   ← 26.4ms usleep 睡完
11174.915294: mb_out   ← glMapBuffer 返回（2.75ms）
11174.915400: sw_in    ← sem_wait(acqsem)（早已被投递，3µs 返回）
```

统计（n=521）：**msw_out − 前一个 rp_out 中位 0.057ms**（p95 0.093）。
主线程根本不等像素——它在回读请求发出后 57µs 就拿着「下一帧可以渲染了」的许可走了。

**报告 25 §3「24.77ms 基本全在 glWaitSync」是错的**：那 22.0ms 里 worker 在跑上一轮的 usleep(26.4ms)+map(2.9ms)+尾(0.1ms)，glWaitSync 只占 5µs。

## 2. 第一刀：glWaitSync 实测 5µs——报告 25 机制判决推翻

| 量（c1_clean.script，n=521 帧，15.01s，34.7fps） | 中位 | p95 | min | max |
|---|---|---|---|---|
| glWaitSync | **0.005ms** | 0.015 | 0.004 | 0.039 |
| glReadPixels 发出 | 0.119ms | 0.233 | 0.039 | 0.338 |
| usleep(autosleep) | 26.434ms | 29.153 | 21.635 | 29.792 |
| glMapBuffer | 2.862ms | 3.369 | 0.124 | 9.944 |
| sem_wait(relsem)（主线程） | 22.000ms | 27.786 | 15.076 | 29.328 |
| **D = mb_out − rp_out** | **29.369ms** | 32.363 | 24.061 | 33.084 |

fence 年龄（ws_in − 投递它的 msw_in）中位 **21.814ms**：glWaitSync 被调用时 fence 已存在 21.8ms，5µs 返回只证明「早已完成」。GPU 何时真正渲染完 frame N 无法从 glWaitSync 反推——但它一定早于 ws_in。

→ **「驱动察觉 fence 完成的时机被推迟 ~26ms」（报告 25 核心论点）判决推翻。**
（报告 23 的 `wait_ms(cond,100ms)` + notify 0x146c680 观测仍有效，但那只是驱动内部 fence 追踪的基础设施，不是 26ms 的所在。）

## 3. IIR 二进制级验证：usleep = 9× map_time 坐实

primus 源码（`primus_libglfork.cpp:558-561`）：`map_time` 就是 glMapBuffer 时长，`sleep_usec = (map_time*1e6 + sleep_usec) * autosleep/100`，autosleep=90 → 稳态 `s* = 9m`。

帧级实证（n=519）：`us_{n+1} − 0.9*(us_n + mb_n)` 残差中位 **13.1µs**（p95 37.7µs）。
`us+mb = 29.296ms ≈ D = 29.369ms`；`us/D = 0.9000`、`us/mb = 9.24`。

→ autosleep 是回读完成延迟 D 的**跟随者**：它按上一轮 map 的阻塞时长比例退避。这解释了报告 25 的困惑「睡眠 28ms 但关掉只快 2ms」——**睡眠只是把 D 藏起来了**：睡着睡着数据就到了（84% 的帧），map 几乎不等；关掉睡眠，map 直接阻塞等数据，worker 轮次 = D，一点没省。

## 4. 数据落地时刻：pbo_watch 直接测量

### 4.1 仪器与陷阱

- 20kHz 轮询 `/proc/PID/mem` 两个 PBO 主机地址（glMapBuffer 返回值，perf uprobe 0x18908 抓取），64 字节窗口任一变化即记录
- 时钟域：/proc/uptime（BOOTTIME）vs perf（sched_clock）差 **300.296s**——用「落入迭代窗口命中数最大」扫描 offset 定标，**440/440 命中**
- uptime 只有 10ms 分辨率 → 到达时间被单向量化到 10ms 网格（不影响「分布很宽」的结论）
- 仪器自身开销：D 从 29.37ms 涨到 31.92ms（轮询抢 CPU），引用时两套数据分开

### 4.2 结果（pbo2.script，497 完整迭代，440 个到达事件）

| 量 | 中位 | min | max |
|---|---|---|---|
| 数据到达 − rp_out | **16.471ms** | 0.014 | 36.632 |
| mb_out − 数据到达 | 13.517ms | 0.006 | 37.414 |
| 到达/D 比值 | 0.53 | 0.00 | 1.00 |

- 阶段分布：**371/440（84%）在 usleep 中段落地，69（16%）在 map 调用窗口内，0 在 map 返回后**
- 到达延迟分布（每 3ms 一档）近似均匀铺满 [0, 30ms]；与 D 相关性弱（r=0.150）
- 两缓冲严格交替（220+220），连续同缓冲仅 1.1%

**解读**：拷贝不是「提交后 2.5ms 固定完成」，而是**在整个 30ms 窗口上铺开**——有时 14µs 就落地（min），有时 36ms。这与「固定延迟」图景不相容，指向**排队/调度**：拷贝命令发出后，真正被执行的时间点逐帧变化。

### 4.3 缓冲相位翻转 = 源码里的丢帧路径

观测到缓冲交替相位在迭代 28→30 间翻转一次。primus 源码给出机制：`cbuf ^= 1` 位于 `sem_timedwait(&di.d.relsem)` 的 **else 分支**内——等 display worker 超时（1s）丢帧时不换 PBO。相位翻转是丢帧的签名（本段 15s 内 1 次）。

## 5. 隔离环境对照：回读路径本身健康

525 直连最小 GL 回读测试（rb_lat.c，FBO 渲染→fence→glWaitSync→glReadPixels→PBO→glMapBuffer，timer query 夹住）：

| 段 | 中位 |
|---|---|
| fence_wait | 0.000ms |
| glReadPixels | 0.003ms |
| **D（rp→map 返回）** | **2.488ms** |
| gpu_copy（timer query） | 2.487ms |
| 通知延迟 | 0.000ms |

pbuffer 模式（rb_lat2.c，不 swap）：**D=0.000ms**、gpu_copy=0.138ms。

→ **同样的驱动、同样的 PBO 回读调用，隔离环境 2.5ms，应用里 29.4ms。** 回读路径 intrinsic 健康，29ms 是应用语境（GPU 上挂着下一帧渲染 / 队列 / 调度）造成的。这也顺带削弱了报告 25 援引的「580 同构」类比（那次 GPU 2.64ms / 驱动 30.24ms 的鸿沟在 525 隔离测试里复现不出来）。

## 6. 假说清算表（更新）

| 假说 | 状态 | 依据 |
|---|---|---|
| 驱动察觉 fence 完成慢 ~26ms（报告 25） | **❌ 推翻** | glWaitSync 5µs；fence 年龄 21.8ms |
| primus autosleep 睡眠是因 | **❌ 推翻** | 关掉只快 2ms（报告 25 实测）；本文 §3 给出机制：睡眠是 D 的 9× 跟随者 |
| 拷贝引擎本身慢 | **⚠️ 削弱** | 隔离 gpu_copy 2.49ms（FBO）/ 0.14ms（pbuffer） |
| 驱动通知路径慢 | **⚠️ 削弱** | 隔离通知延迟 0.000ms |
| GPU 忙导致拷贝排队/铺开 | **◑ 主嫌** | 数据落地铺满整个窗口（§4）；525「100% util / 48-64W / 41°C」假忙签名 vs 515 116W |
| 与像素量无关（报告 25 §5.1） | **✅ 仍成立** | 本文更强：min 0.014ms 说明拷贝有时立即完成，不是带宽问题 |
| 与 vblank 无关（报告 24） | ✅ 不变 | sync=0 语义 + 相位均匀 |

**新的统一图景**：525 的回归 = **PBO 回读数据从「拷贝命令提交」到「主机内存可见」的完成延迟 D 从 ~10ms（515 推测）涨到 ~29.4ms**。autosleep 把 map 时长放大 9 倍变成 26ms 睡眠，与 D 数值接近，造成「固定 26ms 同步延迟」的错觉（报告 25 误判的来源）。fps 34.7 = 1/D 的直接后果。

## 7. 方法论教训（本轮新增，务必带入后续实验）

1. **perf 多 `-p` 重复投递**：`-p tid1,tid2` 会让每个 uprobe 事件被重复投递 N 份（N=列表长度），帧率虚增 31 倍。**必须单 `-p PID`**（进程=线程组，perf 自动跟随全部线程）。
2. **索引配对伪影（连踩两次）**：perf 录制从迭代中间开始，事件列表整体错位一位（c1_clean 首事件是录制前迭代的 mb_out）。按索引 zip 会得到负时长/空窗口。**必须按 ws_in 边界切迭代，或时间配对**。c1_analyze.py 的「0.291ms」、pbo_arrival.py 初版「0 命中」都是伪影。
3. **时钟域**：/proc/uptime（BOOTTIME，含挂起）vs perf（sched_clock）差 300.296s。**用结构互相关/命中数扫描定标，不要手填**。
4. **uptime 10ms 量化**：到达时间单向抹到网格；结论只能用到「分布宽度 >> 10ms」的层面。
5. **仪器开销要单独量**：pbo_watch 让 D +2.5ms，两套数据分开引用。
6. **缓冲奇偶校验是免费的 sanity check**：交替性（1.1% 例外）+ 相位翻转（丢帧签名）能自动抓出匹配错位。
7. **本地/远端分工**：Bash 工具在本地机（仅 AMD Vega 集显，无 NVIDIA 硬件）；GPU 实验走 rssh.sh（desktop@<testhost>）；直连 GL 需 `__GLX_VENDOR_LIBRARY_NAME=nvidia`。<workdir> 两边同一目录（NFS <lan-host-b>:/data/用户），本地写=桌面可见。
8. **反汇编地址 ≠ 执行序**：sem_post(relsem) 在 0x18a10（地址上在 mb_out 之后），但执行序在 rp 之后。判执行序只能靠事件流。

## 8. 产物

```
analysis/traces/c1/
├── c1_clean.script          17 探针干净轨迹（521 帧/15.01s，本文 §2/§3 全部数字）
├── c1_full.script           全量轨迹
└── pbo/
    ├── pbo2.script          同运行 17 探针（497 完整迭代）
    └── pbo_watch.log        440 个 PBO 数据到达事件（20kHz 轮询）

analysis/probes/
├── c1_cut123.sh             17 探针采集脚本（单 -p PID 铁律）
├── c1_pbo2.sh               同运行内抓 PBO 地址 + 轮询 + 17 探针
├── c1_analyze2.py           修正配对分析器（ws_in 切迭代）
├── pbo_arrival.py           到达时刻关联（offset 扫描 + 缓冲奇偶过滤）
├── pbo_watch.c / pbo_peek.c /proc/pid/mem 轮询观测工具
├── rb_lat.c / rb_lat2.c     隔离回读测试（4 模式）
└── rssh.sh                  远端执行 helper（WorkBuddy tmp）
```

## 9. 下一步（按价值排序）

| 优先级 | 动作 | 能答什么 |
|---|---|---|
| **1** | **515 同探针对比**：装回 515.105.01，同一 c1_cut123.sh 采集 | 若 D_515 ≈ 10ms → 回归钉死在回读完成路径；同时拿到「健康拷贝」的到达分布形状做对照 |
| **2** | **rb_lat「忙 GPU」变体**：rp 之后立刻提交重渲染（复刻主线程 57µs 后醒来表示渲染下一帧） | 若 D 从 2.5ms 涨到 ~29ms → 机制 = 拷贝排在后续 GPU 命令之后；若不变 → 排除排队，转向 CE 拥塞/驱动轮询 |
| **3** | 若排队坐实：区分 CE vs 图形引擎（glcore copy kickoff uprobe / nvidia-smi -q -d ENGINE） | 回归代码的具体引擎归属 |
| 4 | 4.17ms 台阶（报告 25 §5.2） | 可能在 D 的分布里自然解释，优先级降 |
