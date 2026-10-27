#!/usr/bin/env python3
"""pbdump.py <pid> [最大条数]
从 /proc/<pid>/mem 读 nvidia 的 pushbuffer 并按命令头解码，打印 method 序列。

判据来自此前验证可用过的版本（报告 140 的 pbstats2.py）：
  op  = w >> 29            必须 ∈ {0,1}
  cnt = (w >> 16) & 0x1fff 必须 1 <= cnt <= 256（注意：cnt=0 的全零字要排除）
  sub = (w >> 13) & 0x7
  meth= (w & 0x1fff) << 2
锚点：'\\r\\x00\\x1c\\x00' + '\\x00\\x00\\x00\\x70' 出现处即 pushbuffer
"""
import sys, struct, time

ANCHOR = struct.pack("<II", 0x2001c00d, 0x70000000)

def u32(b, o): return struct.unpack_from("<I", b, o)[0]

def find_pb(pid):
    maps = []
    for l in open(f"/proc/{pid}/maps"):
        if "nvidiactl" not in l:
            continue
        p = l.split(); a, b = p[0].split("-")
        lo, hi = int(a, 16), int(b, 16)
        if hi - lo > 8 << 20:
            continue
        maps.append((lo, hi))

    mem = open(f"/proc/{pid}/mem", "rb", buffering=0)
    def rd(a, s):
        mem.seek(a); return mem.read(s)

    best = None
    for lo, hi in maps:
        sz = hi - lo
        try: A = rd(lo, sz)
        except Exception: continue
        if ANCHOR not in A:
            continue
        time.sleep(0.03)
        try: B = rd(lo, sz)
        except Exception: continue
        ch = sum(1 for i in range(0, sz // 4) if u32(A, i * 4) != u32(B, i * 4))
        if best is None or ch > best[0]:
            best = (ch, lo, sz)
    return best, rd

def decode(buf, limit):
    out = []
    i, n = 0, len(buf) // 4
    while i < n and len(out) < limit:
        w = u32(buf, i * 4)
        op = w >> 29
        if op in (0, 1):
            cnt = (w >> 16) & 0x1fff
            sub = (w >> 13) & 0x7
            meth = (w & 0x1fff) << 2
            if 1 <= cnt <= 256 and i + cnt < n:
                args = [u32(buf, (i + 1 + k) * 4) for k in range(min(cnt, 4))]
                out.append((sub, meth, cnt, args))
                i += 1 + cnt
                continue
        i += 1
    return out

def main():
    pid = int(sys.argv[1])
    limit = int(sys.argv[2]) if len(sys.argv) > 2 else 200
    best, rd = find_pb(pid)
    if not best:
        print("找不到 pushbuffer"); return 2
    ch, lo, sz = best
    print(f"# pb={lo:#x} size={sz:#x} 30ms变化字={ch}")
    buf = rd(lo, sz)
    cmds = decode(buf, limit)
    print(f"# 解出 {len(cmds)} 条")
    print(f"{'sub':>3} {'meth':>7} {'cnt':>4}  args")
    for sub, meth, cnt, args in cmds:
        print(f"{sub:>3} {meth:#7x} {cnt:>4}  " + " ".join(f"{x:#x}" for x in args))

main()
