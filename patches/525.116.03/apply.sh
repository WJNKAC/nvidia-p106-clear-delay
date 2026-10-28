#!/bin/bash
# apply.sh —— 把本目录的补丁应用到系统库（自动备份）
#
# 用法：
#   sudo ./apply.sh                 # 应用到默认路径
#   sudo ./apply.sh /path/to/lib.so # 应用到指定文件
#
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SPEC="$HERE/patch.json"
TOOL="$HERE/../patch_glcore.py"

# 从 patch.json 读默认目标路径
TARGET="${1:-$(python3 -c "import json;print(json.load(open('$SPEC'))['target_path'])")}"

if [ "$(id -u)" -ne 0 ]; then
    echo "需要 root 权限（要写 /usr/lib）。请用: sudo $0 $*"
    exit 1
fi
if [ ! -f "$TARGET" ]; then
    echo "目标文件不存在: $TARGET"
    exit 1
fi

echo "=== 打补丁前检查 ==="
python3 "$TOOL" check "$TARGET" --spec "$SPEC"
echo
echo "=== 应用补丁 ==="
python3 "$TOOL" apply "$TARGET" --spec "$SPEC"
RC=$?
echo
if [ $RC -eq 0 ]; then
    echo "=== 完成 ==="
    echo "回滚请执行: sudo $HERE/revert.sh"
    echo "注意: 正在运行的图形程序不会被改变，需重启程序（或重新登录）才生效。"
fi
exit $RC
