#!/usr/bin/env python3
"""findrp.py —— 已知 put 指针，找与之配对的服务端消费指针(rp)

判据（同时满足）：
  1. 候选字段也在递增（单调或带缓存的阶梯上升）
  2. 任意时刻 0 <= put - 候选 <= LIMIT（默认 32 MiB，窗口不会超过环形缓冲大小）
  3. put - 候选 的中位数明显小于 LIMIT（否则只是恒定的偏移常量）

用法
  findrp.py <pid> <ctx_hexaddr> <put_off_hex> [span_hex] [samples] [interval_ms]
"""
import sys, struct, time

def main():
    pid = int(sys.argv[1]); ctx = int(sys.argv[2], 16)
    put_off = int(sys.argv[3], 16)
    span = int(sys.argv[4], 16) if len(sys.argv) > 4 else 0x20000
    nsamp = int(sys.argv[5]) if len(sys.argv) > 5 else 12
    ivl = (float(sys.argv[6]) if len(sys.argv) > 6 else 20.0) / 1000.0
    LIMIT = 32 << 20

    mem = open(f"/proc/{pid}/mem", "rb", 0)
    n8 = span // 8
    prev = None
    cands = {}     # off -> {"rise":n, "wins":[...]}
    t0 = time.monotonic()
    for _ in range(nsamp):
        mem.seek(ctx); buf = mem.read(span)
        if len(buf) < span: break
        cur = struct.unpack("<%dQ" % n8, buf)
        put = cur[put_off // 8]
        if prev is not None:
            pput = prev[put_off // 8]
            for i in range(n8):
                if i == put_off // 8:
                    continue
                v, pv = cur[i], prev[i]
                d = v - pv
                if not (0 < d < 0x2000000):      # 只关心在涨的
                    continue
                if put < v:
                    continue
                w = put - v
                if w > LIMIT:
                    continue
                e = cands.setdefault(i * 8, {"rise": 0, "wins": [], "rate": 0})
                e["rise"] += 1
                e["wins"].append(w)
                e["rate"] += d
            del pput
        prev = cur
        time.sleep(ivl)
    mem.close()
    el = time.monotonic() - t0

    rows = []
    for off, e in cands.items():
        if e["rise"] < max(2, nsamp // 3):
            continue
        ws = sorted(e["wins"])
        rows.append((off, e["rise"], ws[len(ws)//2], ws[0], ws[-1], e["rate"]/el/1024))
    # 排序：中位窗口最小的在前（最像"紧跟 put 的 rp"）
    rows.sort(key=lambda r: r[2])
    print(f"# ctx={ctx:#x} put=+{put_off:#x} 样本={nsamp-1} 历时={el:.2f}s  候选 {len(rows)} 个")
    print(f"{'offset':>9} {'递增':>5} {'窗口中位':>11} {'窗最小':>11} {'窗最大':>11} {'速率KiB/s':>10}")
    for off, rise, med, mn, mx, rate in rows[:14]:
        print(f"{off:#9x} {rise:>5} {med:>11} {mn:>11} {mx:>11} {rate:>10.1f}")

if __name__ == "__main__":
    main()
