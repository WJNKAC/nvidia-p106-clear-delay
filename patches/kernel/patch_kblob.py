#!/usr/bin/env python3
"""patch_kblob.py —— 对 nv-kernel.o_binary 的设备 ID 表打补丁

补丁内容：把 P1XX 系列矿卡的 device ID 条目替换成哨兵 0x0007FFFF，
等价于把这些卡从"受限设备表"里抹掉。

用法：
    patch_kblob.py check  <blob路径> [--spec patch.json]
    patch_kblob.py apply  <blob路径> [--spec patch.json]
    patch_kblob.py revert <blob路径>

设计要点（与用户态补丁工具保持一致的纪律）：
  1. 动手前逐字节核对期望原值，不匹配立刻中止
  2. 备份只在首次创建（.orig），避免二次 apply 把已打补丁的文件当备份
  3. 原子写（临时文件 + fsync + os.replace）
"""
import os, sys, json, shutil, hashlib

def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()

def load_spec(p):
    spec = json.load(open(p, encoding="utf-8"))
    for e in spec["entries"]:
        e["_off"] = int(e["offset"], 16) if isinstance(e["offset"], str) else e["offset"]
        e["_old"] = bytes.fromhex(e["old"])
        e["_new"] = bytes.fromhex(e["new"])
    return spec

def check(path, spec):
    d = open(path, "rb").read()
    print(f"文件  : {path}")
    print(f"大小  : {len(d)}")
    print(f"md5   : {md5(path)}")
    if spec.get("blob_md5_orig"):
        same = md5(path) == spec["blob_md5_orig"]
        print(f"原始md5: {spec['blob_md5_orig']}  {'← 当前是原版' if same else ''}")
    print()
    bad = 0
    for e in spec["entries"]:
        cur = d[e["_off"]:e["_off"] + len(e["_old"])]
        if cur == e["_old"]:
            st = "未打补丁"
        elif cur == e["_new"]:
            st = "已打补丁"
        else:
            st = "!! 未知（位置可能不对）"
            bad += 1
        print(f"  {e['_off']:#x}  {cur.hex()}  [{st}]  {e['desc']}")
    return bad == 0

def apply(path, spec):
    d = bytearray(open(path, "rb").read())
    for e in spec["entries"]:
        cur = bytes(d[e["_off"]:e["_off"] + len(e["_old"])])
        if cur not in (e["_old"], e["_new"]):
            print(f"!! 中止：{e['_off']:#x} 既不是原值也不是新值（实际 {cur.hex()}）")
            return 1

    bak = path + ".orig"
    if not os.path.exists(bak):
        shutil.copy2(path, bak)
        print(f"备份 -> {bak}")
        print(f"  备份 md5 = {md5(bak)}")
    else:
        print(f"备份已存在，保留: {bak}")

    n = 0
    for e in spec["entries"]:
        cur = bytes(d[e["_off"]:e["_off"] + len(e["_old"])])
        if cur == e["_new"]:
            continue
        d[e["_off"]:e["_off"] + len(e["_new"])] = e["_new"]
        print(f"  改写 {e['_off']:#x}: {cur.hex()} -> {e['_new'].hex()}  {e['desc']}")
        n += 1
    if n == 0:
        print("无需改动"); return 0

    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(d); f.flush(); os.fsync(f.fileno())
    shutil.copymode(path, tmp)
    os.replace(tmp, path)
    print(f"完成。新 md5 = {md5(path)}")
    return 0

def revert(path):
    bak = path + ".orig"
    if not os.path.exists(bak):
        print(f"!! 找不到 {bak}"); return 1
    tmp = path + ".rvtmp"
    shutil.copy2(bak, tmp); os.replace(tmp, path)
    print(f"已恢复，md5 = {md5(path)}")
    return 0

def main():
    a = sys.argv[1:]
    if len(a) < 2:
        print(__doc__); return 2
    spec_path = "patch.json"
    if "--spec" in a:
        i = a.index("--spec"); spec_path = a[i+1]; del a[i:i+2]
    cmd, target = a[0], a[1]
    if not os.path.exists(target):
        print(f"!! 不存在: {target}"); return 2
    if cmd == "revert":
        return revert(target)

    here = os.path.dirname(os.path.abspath(__file__))
    for cand in ("patch.json", os.path.join(here, "..", "patch.json")):
        if spec_path == "patch.json" and os.path.exists(cand):
            spec_path = cand; break
    if not os.path.exists(spec_path):
        print(f"!! 找不到 patch.json"); return 2

    spec = load_spec(spec_path)
    print(f"spec: {spec_path}  (驱动 {spec.get('version','?')})\n")
    if cmd == "check":
        return 0 if check(target, spec) else 1
    if cmd == "apply":
        rc = apply(target, spec)
        if rc == 0:
            print(); check(target, spec)
        return rc
    print(__doc__); return 2

if __name__ == "__main__":
    sys.exit(main())
