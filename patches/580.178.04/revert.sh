#!/bin/bash
# revert.sh —— 撤销本目录的补丁，恢复为原版
#
# 优先顺序：
#   1) 系统里的 .orig 备份（apply.sh 自动创建）
#   2) 本目录 before/ 下的原版文件（备份丢了也能恢复）
#
# 用法：
#   sudo ./revert.sh                 # 恢复到默认路径
#   sudo ./revert.sh /path/to/lib.so
#
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPEC="$HERE/patch.json"

TARGET="${1:-$(python3 -c "import json;print(json.load(open('$SPEC'))['target_path'])")}"
LIBNAME="$(python3 -c "import json;print(json.load(open('$SPEC'))['library'])")"
BEFORE="$HERE/before/$LIBNAME"

if [ "$(id -u)" -ne 0 ]; then
    echo "需要 root 权限（要写 /usr/lib）。请用: sudo $0 $*"
    exit 1
fi

if [ -f "$TARGET.orig" ]; then
    echo "=== 从 .orig 备份恢复 ==="
    cp -f "$TARGET.orig" "$TARGET"
elif [ -f "$BEFORE" ]; then
    echo "=== 没有 .orig 备份，改用 before/ 下的原版文件 ==="
    cp -f "$BEFORE" "$TARGET"
else
    echo "!! 既没有 $TARGET.orig，也没有 $BEFORE"
    echo "   补救：重装同版本驱动即可恢复原版文件"
    exit 1
fi

echo "恢复后 md5: $(md5sum "$TARGET" | awk '{print $1}')"
echo
python3 "$HERE/../patch_glcore.py" check "$TARGET" --spec "$SPEC"
echo
echo "=== 完成 ==="
echo "注意: 正在运行的图形程序不受影响，需重启程序（或重新登录）才生效。"
