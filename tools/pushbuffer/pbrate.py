#!/usr/bin/env python3
"""pbrate.py —— 零干扰测量 pushbuffer 的"写入速率"与"未消费窗口"

原理
  glcore 的上下文对象里有三个关键字段（偏移从 ensure_space 反汇编得出）：
    put    : 本帧已写入到的位置（CPU 侧写指针）
    rp     : GPU 已消费到的位置（已被驱动缓存的 fence 值）
    窗口   = put - rp

  ensure_space 的判断是：
      free    = put - consumed
      需要    = put - need
      if (free <= 需要) → 空间不足 → 等 GPU

  所以 **put - rp 就是"当前有多少已提交但 GPU 还没消费的字节"**。
  这个窗口的大小直接决定"驱动要不要停下来等 GPU"。

为什么零干扰
  只做 /proc/<pid>/mem 的 pread，不 ptrace、不打断目标。
  ctx 地址只需在开始前用 entryst 抓一次（GL context 在整个进程生命周期内固定）。

用法
  pbrate.py <pid> <ctx_hexaddr> <put_off_hex> <rp_off_hex> [secs] [interval_ms]
"""
import sys, struct, time

def rd(mem, addr, n=8):
    mem.seek(addr)
    b = mem.read(n)
    if len(b) != n:
        raise IOError("short read at %#x" % addr)
    return struct.unpack("<Q", b)[0]

def main():
    pid = int(sys.argv[1])
    ctx = int(sys.argv[2], 16)
    put_off = int(sys.argv[3], 16)
    rp_off = int(sys.argv[4], 16)
    secs = float(sys.argv[5]) if len(sys.argv) > 5 else 3.0
    ivl = (float(sys.argv[6]) if len(sys.argv) > 6 else 20.0) / 1000.0

    mem = open(f"/proc/{pid}/mem", "rb", 0)

    t0 = time.monotonic()
    p0 = rd(mem, ctx + put_off)
    r0 = rd(mem, ctx + rp_off)

    win = []          # 未消费窗口样本
    dput = []         # put 增量样本
    n = 0
    while time.monotonic() - t0 < secs:
        time.sleep(ivl)
        try:
            p = rd(mem, ctx + put_off)
            r = rd(mem, ctx + rp_off)
        except Exception as e:
            print(f"# 读取失败: {e}")
            break
        win.append(p - r)
        dput.append(p - p0)
        p0, r0 = p, r
        n += 1
    mem.close()

    el = time.monotonic() - t0
    if not win:
        print("# 无样本")
        return

    total_put = dput[-1] if dput else 0
    # put 是环形指针，可能回绕；用逐次增量求和（增量可能为负 = 回绕，忽略）
    adv = sum(d for d in dput if d > 0)
    win_sorted = sorted(win)

    print(f"# ctx={ctx:#x}  put=+{put_off:#x}  rp=+{rp_off:#x}  样本={n}  历时={el:.2f}s")
    print(f"put 推进总量 = {adv} B  ({adv/el/1024:.1f} KiB/s)")
    print(f"窗口 put-rp : min={win_sorted[0]}  p50={win_sorted[len(win_sorted)//2]}  "
          f"p90={win_sorted[int(len(win_sorted)*0.9)]}  max={win_sorted[-1]}  (字节)")
    # 窗口分布：小于 4KB 的比例，反映"是否长期贴着墙"
    tight = sum(1 for w in win if w < 4096)
    print(f"窗口 < 4KiB 的样本占比 = {100.0*tight/len(win):.1f}%")

if __name__ == "__main__":
    main()
