# 87 · N96：push buffer 抓到了，里面**没有** SEMAPHORE_WAIT/FENCE_WAIT —— 等待是驱动内部状态机

日期：2026-10-26 14:15
驱动：525.116.03
方法：直接 dump 进程里 `/dev/nvidiactl` 的 rw-s 映射（= push buffer），按 envytools 的 puller 方法表做严格匹配
（`docs/hw/fifo/puller.rst`：header = bit31 非增量 | bits28:16 count−1 | bits15:0 method>>2）

---

## 0. 结论（按你给的判据）

**判据**：若 push buffer 里有 SEMAPHORE_WAIT / FENCE_WAIT ⇒ 根因闭环；若没有 ⇒ 等待是驱动内部状态机。

**实测：没有。**

| puller 方法 | header 低 16 位 | kind=0（legacy） | kind=1（program 切换，卡的那个） |
|---|---|---|---|
| `0x0068` **SEMAPHORE_ACQUIRE** | 0x1A | **0** | **0** |
| `0x006c` **SEMAPHORE_RELEASE** | 0x1B | **0** | **0** |
| `0x001c` SEMAPHORE_TRIGGER | 0x07 | 23669 | 6022 |
| `0x0020` NOTIFY_INTR | 0x08 | 23 | 6032 |
| `0x0000` OBJECT | 0x00 | 23708 | 6037 |
| `0x0008` NOP | 0x04 | 47342 | 6022 |

**方法空间是可信的**：同一次扫描同时抓到了 `OBJECT`/`NOP`/`SEM_TRIGGER`/`NOTIFY_INTR` 四类已知命令，
说明 header 解析对、映射找对；`ACQUIRE`/`RELEASE` 的 0 是**真 0**，不是"没扫到"。

**⇒ GPU 从来没有收到"等信号量"的命令。** 等待不在命令流里。

---

## 1. 那 NOTIFY_INTR 呢？

envytools 对 `0x0020 NOTIFY_INTR` 的定义（`puller.rst`）：

> G84 introduced the NOTIFY_INTR method, which **simply raises an interrupt** that notifies the host
> of its execution. It can be used for sync primitives. `PFIFO_NOTIFY_INTR();`

**它只是"抬一根中断"，不阻塞 GPU** ⇒ 不是等待原语。

但计数差异值得记一笔：kind=1 里 `TRIGGER` 与 `NOTIFY` **成对出现**（6022 / 6032），
而 kind=0 跑了两万帧 `NOTIFY` 只有 23 次 ⇒ **"program 切换导致的批次"每批都带一次 trigger+notify**，
这正好是驱动"每批告诉主机一次"的形态，与报告 33/44 的"每帧 notify、92.9% 收不到"一脉相承。

---

## 2. 因此：等待是驱动内部状态机（主机侧串行化）

把已知事实拼起来：

```
app 线程：写 bind 命令（1µs 返回）→ 写 draw 命令（1µs 返回）→ 下一个
驱动内部：每遇到"批次断开"，提交该批 → 等它完成 → 才提交下一批
                    ↑ 这个"等完成"就是 155µs（报告 86 的 draw 段）
```

- 命令流里**没有**任何"等信号量/fence"的命令（本报告）；
- GPU 侧 `util.memory=0%`（报告 86 §0bis）⇒ 不是在等数据；
- `util.gpu=99%` + 37.7W ⇒ 引擎"有 kernel 在跑"但在空转；
- 主机侧每对 42 次 `sched_yield`（报告 83，且 `pair_only` 是**单线程**进程、`info threads` 只有 1 个，
  所以自旋就发生在应用线程上）⇒ **应用线程确实在自旋等"这批完成"**；
  ⚠️ 但 n83a 的 `submit_p50` 只有 1µs/对 —— 两者需要一个解释：最可能是 n83a 的 `submit` 只夹了
  **bind 那一次调用**（driver 把 flush 推迟到 draw），而阻塞发生在 draw 里（报告 86 的 draw 段 155µs）。
  这一条留作待核；不影响"没有等待命令"这个结论。
- CPU 满载 ±1%（报告 85）⇒ 等的不是 CPU 时间片，而是**完成通知本身的固定延迟**。

**⇒ 525 的批次是"提交—等完成—再提交"的串行模型，每批等一次完成通知（~155µs）；
515 是流水模型，不等，所以 0.03µs/次。**

---

## 3. 一处必要的自我更正（关于报告 86 的 GPU_span）

报告 86 用 `GPU_span = q1−q0 = 122ms` 推出"GPU 自己的时间轴被拉长 ⇒ GPU 内部在等"。
**这个推论过强**：timer query 的时间戳在 GPU 空闲等命令时**照样往前走**，
所以 `GPU_span≈墙钟` 只说明"这批命令从提交到执行完用了 122ms 墙钟"，**不能区分**
"GPU 在等"与"命令晚到"。真正有效的是本报告的**命令流证据**（无等待命令）+
`util.memory=0%` + 主机侧自旋计数——这三条合起来才指向"主机侧串行化"。

（报告 86 的 bind/draw 分段仍然有效：它证明**停顿落在 draw 命令的执行前后**，
即"批次断开发生在 draw 处"。）

---

## 4. 产物

```
analysis/probes/n100_pbdump.py    dump /dev/nvidiactl 映射里最后写入点附近内容（找到 push buffer）
analysis/probes/n101_semscan.py   按 envytools puller 方法表严格匹配同步命令并计数
analysis/probes/n99_stackctx.py   早前的 ctx 扫描尝试（0 命中，留作记录）
```
