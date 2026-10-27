#!/usr/bin/env python3
"""pbmeth.py <pid> [采样次数] [间隔ms]

统计 nvidia pushbuffer 里的 method 分布（sub:meth -> 出现次数），用于跨版本/跨配置对比。

关键设计：只统计"稳定变化"的 pushbuffer 区域。
  - 先在 t0 读一遍所有可写 nvidia 映射
  - 再在 t1 读一遍
  - 只保留两次之间**有变化**的 4KB 块 → 这些才是当前正在被写的命令流
  - 在这些块里解码命令头，累计 method 直方图

为什么这么做：写死的常量区、未使用的环形缓冲尾巴会稀释统计，
只看"变化中的块"能把这帧真正在发的命令挑出来。
"""
import sys, struct, time, collections

def u32(b, o):
    return struct.unpack_from("<I", b, o)[0]

def valid_header(w):
    if w == 0:
        return False
    op = w >> 29
    cnt = (w >> 16) & 0x1fff
    sub = (w >> 13) & 0x7
    return op in (0, 1) and 1 <= cnt <= 64 and sub <= 7

def snapshot(pid, segs, mem):
    out = {}
    for (a, b) in segs:
        sz = min(b - a, 1 << 20)
        try:
            mem.seek(a)
            out[a] = mem.read(sz)
        except Exception:
            continue
    return out

def changed_blocks(s0, s1, blk=4096):
    """返回两次快照之间发生变化的块（虚拟地址列表）"""
    res = []
    for a, d0 in s0.items():
        d1 = s1.get(a)
        if d1 is None:
            continue
        n = min(len(d0), len(d1))
        for off in range(0, n, blk):
            if d0[off:off+blk] != d1[off:off+blk]:
                res.append((a + off, d1[off:off+blk]))
    return res

def decode_block(addr, blob, hist, samples, limit=2000):
    n = len(blob)
    p = 0
    while p + 4 <= n:
        w = u32(blob, p)
        if not valid_header(w):
            p += 4
            continue
        cnt = (w >> 16) & 0x1fff
        sub = (w >> 13) & 0x7
        meth = (w & 0x1fff) << 2
        end = p + 4 * (cnt + 1)
        if end > n:
            break
        hist[(sub, meth)] += 1
        if len(samples) < limit:
            args = [u32(blob, p + 4*k) for k in range(1, min(cnt, 2) + 1)]
            samples.append((addr + p, sub, meth, cnt, args))
        p = end

def main():
    pid = int(sys.argv[1])
    rounds = int(sys.argv[2]) if len(sys.argv) > 2 else 40
    gap = (int(sys.argv[3]) if len(sys.argv) > 3 else 30) / 1000.0

    segs = []
    for l in open(f"/proc/{pid}/maps"):
        if "nvidia" not in l:
            continue
        p = l.split()
        if "rw" not in p[1]:
            continue
        a, b = p[0].split("-")
        segs.append((int(a, 16), int(b, 16)))
    segs.sort()

    hist = collections.Counter()
    samples = []
    mem = open(f"/proc/{pid}/mem", "rb", 0)
    prev = snapshot(pid, segs, mem)
    nchanged = 0
    for _ in range(rounds):
        time.sleep(gap)
        cur = snapshot(pid, segs, mem)
        blocks = changed_blocks(prev, cur)
        nchanged = max(nchanged, len(blocks))
        for (addr, blob) in blocks:
            decode_block(addr, blob, hist, samples)
        prev = cur
    mem.close()

    total = sum(hist.values())
    print(f"# pid={pid}  可写映射 {len(segs)} 段  单轮最多变化块 {nchanged}")
    print(f"# 命令总数 {total}")
    print(f"{'sub':>3} {'meth':>7} {'count':>8} {'pct':>7}")
    for (sub, meth), c in hist.most_common(60):
        print(f"{sub:>3} {meth:#7x} {c:>8} {100.0*c/total if total else 0:>6.2f}%")

if __name__ == "__main__":
    main()
