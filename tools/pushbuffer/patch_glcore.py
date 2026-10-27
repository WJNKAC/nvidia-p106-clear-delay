#!/usr/bin/env python3
"""patch_glcore.py —— 把"跳过 gr3d 延迟命令发射"的补丁固化进 libnvidia-glcore.so 文件

补丁内容（与运行时补丁 patchpb 完全等价）：
    0x15c375e : 74 -> eb    发射点1 的条件跳转改成无条件跳转
    0x1841fe0 : 75 -> eb    发射点2 同上

两者都是 2 字节 rel8 短跳转（74/75 = je/jne, eb = jmp），长度相同，可原地替换。

用法:
    patch_glcore.py apply  <so路径>   # 先备份，再打补丁
    patch_glcore.py revert <so路径>   # 从 .orig 备份恢复
    patch_glcore.py check  <so路径>   # 只检查当前状态

设计要点:
  - 备份为 <so>.orig，只在第一次 apply 时创建（避免重复 apply 把已打补丁的文件当成备份）
  - 改写前逐字节核对"期望原值"，不匹配就中止（防止补丁位置漂移到别的版本）
  - 打印修改前后的字节作为证据
"""
import sys, os, shutil, hashlib

PATCHES = [
    (0x15C375E, 0x74, 0xEB, "发射点1: je -> jmp（无条件跳过 gr3d 延迟命令）"),
    (0x1841FE0, 0x75, 0xEB, "发射点2: jne -> jmp（无条件跳过 gr3d 延迟命令）"),
]

def md5(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()

def check(path):
    with open(path, "rb") as f:
        d = f.read()
    print(f"文件: {path}  大小={len(d)}  md5={md5(path)}")
    ok = True
    for off, old, new, desc in PATCHES:
        cur = d[off]
        if cur == old:
            state = "未打补丁"
        elif cur == new:
            state = "已打补丁"
        else:
            state = "!!! 未知值，位置可能不对"
            ok = False
        print(f"  {off:#x}: {cur:#04x}  [{state}]  {desc}")
    return ok

def apply(path):
    bak = path + ".orig"
    with open(path, "rb") as f:
        d = bytearray(f.read())

    # 先校验所有位置，全部匹配才动手
    for off, old, new, desc in PATCHES:
        cur = d[off]
        if cur == new:
            print(f"  {off:#x} 已经是新值 {new:#04x}，跳过")
            continue
        if cur != old:
            print(f"!! 中止：{off:#x} 期望 {old:#04x}，实际 {cur:#04x} —— 补丁位置不匹配，可能是别的驱动版本")
            return 1

    # 备份（只在首次创建）
    if not os.path.exists(bak):
        shutil.copy2(path, bak)
        print(f"已备份 -> {bak}  md5={md5(bak)}")
    else:
        print(f"备份已存在，保留原备份: {bak}  md5={md5(bak)}")

    for off, old, new, desc in PATCHES:
        if d[off] == new:
            continue
        print(f"  改写 {off:#x}: {d[off]:#04x} -> {new:#04x}   {desc}")
        d[off] = new

    # 原子写：先写临时文件再 rename，避免写到一半断电/中断导致文件损坏
    tmp = path + ".tmp_patch"
    with open(tmp, "wb") as f:
        f.write(d)
        f.flush()
        os.fsync(f.fileno())
    shutil.copymode(path, tmp)
    os.replace(tmp, path)

    print(f"完成。新 md5={md5(path)}")
    return 0

def revert(path):
    bak = path + ".orig"
    if not os.path.exists(bak):
        print(f"!! 找不到备份 {bak}")
        return 1
    tmp = path + ".tmp_revert"
    shutil.copy2(bak, tmp)
    os.replace(tmp, path)
    print(f"已从 {bak} 恢复。md5={md5(path)}")
    return 0

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(2)
    cmd, target = sys.argv[1], sys.argv[2]
    if not os.path.exists(target):
        print(f"!! 文件不存在: {target}")
        sys.exit(2)
    if cmd == "apply":
        rc = apply(target)
        if rc == 0:
            check(target)
        sys.exit(rc)
    elif cmd == "revert":
        rc = revert(target)
        if rc == 0:
            check(target)
        sys.exit(rc)
    elif cmd == "check":
        sys.exit(0 if check(target) else 1)
    else:
        print(__doc__)
        sys.exit(2)
