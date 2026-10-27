#!/usr/bin/env python3
"""ctxscan.py —— 在 ctx 对象附近自动找出"在推进的指针"

问题
  从 ensure_space 反汇编猜出的偏移（ctx+0x10d0 / ctx+0x1130）实测几乎不动，
  说明那是别的字段，或者真正的指针在子对象里。

做法
  不猜，扫。对 [ctx, ctx+span) 每 8 字节取两次快照（间隔 dt），
  统计每个 8 字节位置的"净增量"，按增量排序，把最像写指针的几个挑出来。

  判据：
    - 增量单调（允许回绕，即偶发负跳）
    - 增量量级与"每帧命令字节数"相符（几十 KB/s ~ 几 MB/s）
    - 同一位置在多次采样中持续递增（不是一次性跳变）

用法
  ctxscan.py <pid> <ctx_hexaddr> [span_hex] [samples] [interval_ms]
"""
import sys, struct, time

def main():
    pid = int(sys.argv[1])
    ctx = int(sys.argv[2], 16)
    span = int(sys.argv[3], 16) if len(sys.argv) > 3 else 0x20000
    nsamp = int(sys.argv[4]) if len(sys.argv) > 4 else 12
    ivl = (float(sys.argv[5]) if len(sys.argv) > 5 else 25.0) / 1000.0

    mem = open(f"/proc/{pid}/mem", "rb", 0)
    n8 = span // 8

    prev = None
    # 每个位置的累计正增量 与 递增次数
    tot = [0] * n8
    rise = [0] * n8
    t0 = time.monotonic()
    for _ in range(nsamp):
        mem.seek(ctx)
        buf = mem.read(span)
        if len(buf) < span:
            print("# 读不全"); break
        cur = struct.unpack("<%dQ" % n8, buf)
        if prev is not None:
            for i in range(n8):
                d = cur[i] - prev[i]
                # 只统计"合理幅度"的正增量，过滤掉垃圾值之间的乱跳
                if 0 < d < 0x100000:
                    tot[i] += d
                    rise[i] += 1
        prev = cur
        time.sleep(ivl)
    mem.close()

    el = time.monotonic() - t0
    idx = sorted(range(n8), key=lambda i: -tot[i])[:14]
    print(f"# ctx={ctx:#x} span={span:#x} 样本={nsamp-1} 历时={el:.2f}s")
    print(f"{'offset':>10} {'累计增量':>12} {'递增次数':>9} {'速率KiB/s':>11} {'末值':>14}")
    for i in idx:
        if tot[i] == 0:
            continue
        off = i * 8
        print(f"{off:#10x} {tot[i]:>12} {rise[i]:>9} {tot[i]/el/1024:>11.1f} {prev[i]:#14x}")

if __name__ == "__main__":
    main()
