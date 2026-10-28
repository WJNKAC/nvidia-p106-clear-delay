#!/usr/bin/env bash
# ============================================================
#  nvidia-p106-clear-delay · 演示脚本（仅 580.178.04）
#  用法：
#     sudo ./patch.sh            查看说明与当前状态
#     sudo ./patch.sh apply      打补丁（自动备份）
#     sudo ./patch.sh revert     恢复原版
#  原理：
#     libnvidia-glcore.so 会在每个批次边界向 gr3d 写 0x3998=240，
#     GPU 收到就白等 122 µs。本补丁把"受限才发"改成"永远不发"，
#     只改 1 个字节。
# ============================================================
set -u

TARGET="/usr/lib/x86_64-linux-gnu/libnvidia-glcore.so.580.178.04"
OFFSET=$((0xb51c82))
OLD_BYTE="74"          # je   —— 受限时跳去发射延迟命令
NEW_BYTE="eb"          # jmp  —— 无条件跳过，永远不发
BACKUP="$TARGET.show-orig"

RED=$'\e[31m'; GRN=$'\e[32m'; YLW=$'\e[33m'; CYN=$'\e[36m'; BLD=$'\e[1m'; RST=$'\e[0m'
LINE="$CYN════════════════════════════════════════════════════${RST}"

cur_byte() { od -An -tx1 -j "$OFFSET" -N1 "$TARGET" 2>/dev/null | tr -d ' \n'; }

banner() {
    echo "$LINE"
    echo "${BLD}  nvidia-p106-clear-delay${RST}   演示 · 驱动 580.178.04"
    echo "$LINE"
    echo
    echo "  目标文件 : $TARGET"
    echo "  修改位置 : 偏移 0xb51c82（共 1 个字节）"
    echo
    echo "  ${YLW}原始${RST}  74 → je   skip     只有矿卡才发延迟命令"
    echo "  ${GRN}改成${RST}  eb → jmp  skip     无条件跳过，永远不发"
    echo
    echo "  那条命令的效果：每个批次让 GPU 白等 ${BLD}122 µs${RST}。"
    echo "  一帧 400 批 = ${BLD}48.8 ms${RST} 纯等待，帧率只有原来的 1/14。"
    echo
}

need_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "${RED}需要 root 权限（要写 /usr/lib 下的系统文件）${RST}"
        echo "请用： sudo $0 $1"
        exit 1
    fi
}

do_check() {
    local b
    b=$(cur_byte)
    if [ -z "$b" ]; then
        echo "${RED}读不到文件，检查路径是否正确${RST}"; exit 1
    fi
    case "$b" in
        74) echo "  当前字节: 0x$b   状态: ${YLW}未打补丁（原版）${RST}" ;;
        eb) echo "  当前字节: 0x$b   状态: ${GRN}已打补丁${RST}" ;;
        *)  echo "  当前字节: 0x$b   状态: ${RED}未知值，偏移可能不对${RST}"; exit 1 ;;
    esac
}

do_apply() {
    need_root apply
    local b
    b=$(cur_byte)
    if [ "$b" = "eb" ]; then
        echo "${YLW}已经是补丁状态，无需重复操作${RST}"; exit 0
    fi
    if [ "$b" != "$OLD_BYTE" ]; then
        echo "${RED}!! 位置 0xb51c82 的字节是 0x$b，不是预期的 0x74${RST}"
        echo "   驱动版本可能不对，或偏移已失效。拒绝修改。"; exit 1
    fi

    echo "  [1/3] 备份原文件"
    if [ ! -f "$BACKUP" ]; then
        cp -f "$TARGET" "$BACKUP" && echo "        ${GRN}✓${RST} $BACKUP"
    else
        echo "        已存在，保留: $BACKUP"
    fi

    echo "  [2/3] 校验原始字节"
    echo "        ${GRN}✓${RST} 0x$OLD_BYTE (je) —— 与预期一致"

    echo "  [3/3] 写入新字节"
    printf "\\x$NEW_BYTE" | dd of="$TARGET" bs=1 seek="$OFFSET" conv=notrunc status=none
    local n; n=$(cur_byte)
    if [ "$n" = "$NEW_BYTE" ]; then
        echo "        ${GRN}✓${RST} 0x$OLD_BYTE → 0x$n (jmp)"
    else
        echo "        ${RED}!! 写入后读回 0x$n，与预期不符${RST}"; exit 1
    fi

    echo
    echo "  ${GRN}${BLD}补丁完成。${RST}已在运行的程序不受影响，重新打开 GL 程序即生效。"
    echo "  回滚: sudo $0 revert"
}

do_revert() {
    need_root revert
    local b
    b=$(cur_byte)
    if [ "$b" = "74" ]; then
        echo "${YLW}本来就是原版状态${RST}"; exit 0
    fi
    if [ "$b" != "eb" ]; then
        echo "${RED}!! 当前字节 0x$b 不是补丁值 0xeb，拒绝盲目恢复${RST}"; exit 1
    fi
    if [ ! -f "$BACKUP" ]; then
        echo "${RED}!! 找不到备份 $BACKUP${RST}"; exit 1
    fi
    cp -f "$BACKUP" "$TARGET"
    local n; n=$(cur_byte)
    if [ "$n" = "$OLD_BYTE" ]; then
        echo "  ${GRN}✓${RST} 已恢复原版（0x$n）"
    else
        echo "  ${RED}!! 恢复后读回 0x$n${RST}"; exit 1
    fi
}

cd "$(dirname "$0")" 2>/dev/null
MODE="${1:-check}"

case "$MODE" in
    apply)  banner; do_apply ;;
    revert) banner; do_revert ;;
    check)  banner; do_check ;;
    *)
        echo "用法: $0 {apply|revert|check}"; exit 1 ;;
esac
echo "$LINE"
