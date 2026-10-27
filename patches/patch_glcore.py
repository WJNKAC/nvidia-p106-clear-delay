#!/usr/bin/env python3
"""patch_glcore.py —— libnvidia-glcore.so 字节补丁工具（支持多驱动版本）

补丁定义从 patch.json 读取，不再硬编码偏移，便于按版本扩展。

用法：
    patch_glcore.py check  <so路径> [--spec patch.json]
    patch_glcore.py apply  <so路径> [--spec patch.json]
    patch_glcore.py revert <so路径>
    patch_glcore.py verify <so路径> [--spec patch.json]    # 校验 md5 是否等于 after 的 md5

patch.json 格式：
{
  "version": "525.116.03",
  "library": "libnvidia-glcore.so.525.116.03",
  "before_md5": "...",
  "after_md5": "...",
  "patches": [
    {"offset": "0x15c375e", "old": "74", "new": "eb", "desc": "..."}
  ]
}

设计要点（都是踩过的坑）：
  1. 动手前逐字节核对 old，不匹配立刻中止 —— 防止补丁漂移到别的驱动版本
  2. 备份只在首次创建 —— 否则第二次 apply 会把"已打补丁的文件"当备份，从此丢失回滚能力
  3. 原子写（临时文件 + fsync + os.replace）—— 防中断把 .so 写坏导致图形栈起不来
  4. check 能区分"未打 / 已打 / 未知" —— "未知"是位置漂移的告警
"""
import sys, os, json, shutil, hashlib

def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()

def load_spec(spec_path):
    with open(spec_path, encoding="utf-8") as f:
        spec = json.load(f)
    for p in spec["patches"]:
        p["_off"] = int(p["offset"], 16) if isinstance(p["offset"], str) else int(p["offset"])
        p["_old"] = int(p["old"], 16)
        p["_new"] = int(p["new"], 16)
    return spec

def check(path, spec):
    d = bytearray(open(path, "rb").read())
    cur_md5 = md5(path)
    print(f"文件   : {path}")
    print(f"大小   : {len(d)}")
    print(f"当前md5: {cur_md5}")
    if spec.get("before_md5"):
        print(f"原始md5: {spec['before_md5']}  {'← 当前是原版' if cur_md5 == spec['before_md5'] else ''}")
    if spec.get("after_md5"):
        print(f"补丁md5: {spec['after_md5']}  {'← 当前是已打补丁版' if cur_md5 == spec['after_md5'] else ''}")
    print()
    unknown = 0
    for p in spec["patches"]:
        cur = d[p["_off"]]
        if cur == p["_old"]:
            state = "未打补丁"
        elif cur == p["_new"]:
            state = "已打补丁"
        else:
            state = "!! 未知值（位置可能不对）"
            unknown += 1
        print(f"  {p['_off']:#x}: {cur:#04x}  [{state}]  {p['desc']}")
    return unknown == 0

def apply(path, spec, keep_backup=True):
    d = bytearray(open(path, "rb").read())

    # 先整体校验，全部通过才动手
    bad = []
    for p in spec["patches"]:
        cur = d[p["_off"]]
        if cur not in (p["_old"], p["_new"]):
            bad.append((p, cur))
    if bad:
        print("!! 中止：以下位置既不是原值也不是新值，可能是别的驱动版本")
        for p, cur in bad:
            print(f"   {p['_off']:#x}  期望 {p['_old']:#04x}  实际 {cur:#04x}")
        return 1

    # 备份（只在首次创建）
    if keep_backup:
        bak = path + ".orig"
        if not os.path.exists(bak):
            shutil.copy2(path, bak)
            print(f"已备份 -> {bak}")
            print(f"  备份md5 = {md5(bak)}")
        else:
            print(f"备份已存在，保留原备份: {bak}")

    changed = 0
    for p in spec["patches"]:
        if d[p["_off"]] == p["_new"]:
            print(f"  跳过 {p['_off']:#x}（已是新值 {p['_new']:#04x}）")
            continue
        print(f"  改写 {p['_off']:#x}: {d[p['_off']]:#04x} -> {p['_new']:#04x}   {p['desc']}")
        d[p["_off"]] = p["_new"]
        changed += 1

    if changed == 0:
        print("无需改动。")
        return 0

    # 原子写
    tmp = path + ".tmp_patch"
    with open(tmp, "wb") as f:
        f.write(d)
        f.flush()
        os.fsync(f.fileno())
    shutil.copymode(path, tmp)
    os.replace(tmp, path)

    new_md5 = md5(path)
    print(f"完成。新 md5 = {new_md5}")
    if spec.get("after_md5") and new_md5 != spec["after_md5"]:
        print(f"!! 注意：与 spec 里记录的 after_md5（{spec['after_md5']}）不同")
    return 0

def revert(path):
    bak = path + ".orig"
    if not os.path.exists(bak):
        print(f"!! 找不到备份 {bak}")
        print("   补救办法：从 patch/<版本>/before/ 目录复制原版过来")
        return 1
    tmp = path + ".tmp_revert"
    shutil.copy2(bak, tmp)
    os.replace(tmp, path)
    print(f"已从 {bak} 恢复，md5 = {md5(path)}")
    return 0

def verify(path, spec):
    cur = md5(path)
    ok = True
    if spec.get("before_md5"):
        if cur == spec["before_md5"]:
            print(f"当前是【原版】(md5={cur})"); ok = False
        elif cur == spec["after_md5"]:
            print(f"当前是【已打补丁版】(md5={cur})")
        else:
            print(f"md5 不匹配已知的任何一个：{cur}"); ok = False
    return 0 if ok else 1

def main():
    argv = sys.argv[1:]
    if len(argv) < 2:
        print(__doc__); return 2

    # 解析 --spec
    spec_path = None
    if "--spec" in argv:
        i = argv.index("--spec")
        spec_path = argv[i + 1]
        del argv[i:i + 2]

    cmd, target = argv[0], argv[1]
    if not os.path.exists(target):
        print(f"!! 文件不存在: {target}"); return 2

    if cmd == "revert":
        return revert(target)

    # 其余命令需要 spec
    if spec_path is None:
        # 默认：在目标文件同目录或脚本同级的 <版本>/patch.json 里找
        cand = [
            os.path.join(os.path.dirname(os.path.abspath(target)), "patch.json"),
            os.path.join(os.path.dirname(os.path.abspath(__file__)), "patch.json"),
        ]
        for c in cand:
            if os.path.exists(c):
                spec_path = c; break
    if spec_path is None or not os.path.exists(spec_path):
        print("!! 找不到 patch.json，请用 --spec 指定"); return 2

    spec = load_spec(spec_path)
    print(f"spec: {spec_path}  (驱动 {spec.get('version','?')})\n")

    if cmd == "check":
        return 0 if check(target, spec) else 1
    elif cmd == "apply":
        rc = apply(target, spec)
        if rc == 0:
            print(); check(target, spec)
        return rc
    elif cmd == "verify":
        return verify(target, spec)
    else:
        print(__doc__); return 2

if __name__ == "__main__":
    sys.exit(main())
