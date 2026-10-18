#!/bin/sh
# 把编好的插件装进游戏。   sh install.sh            安装/更新
#                          sh install.sh uninstall  卸载（只删 DLL，配置和日志留着）
# 配置已经存在时不覆盖，免得冲掉你在游戏外改过的设置。
set -e
GAME="D:/steam/steamapps/common/Monster Hunter World"
SRC="$(cd "$(dirname "$0")" && pwd)"
PLUG="$GAME/nativePC/plugins"

[ -f "$GAME/MonsterHunterWorld.exe" ] || { echo "找不到游戏目录：$GAME"; exit 1; }
if tasklist 2>/dev/null | grep -qi "MonsterHunterWorld.exe"; then
    echo "游戏正在运行，先退出游戏再装/卸"
    exit 1
fi

if [ "$1" = "uninstall" ]; then
    rm -f "$PLUG/BetterINQ.dll"
    echo "已卸载：$PLUG/BetterINQ.dll（配置和日志在 $PLUG/BetterINQ/，没动）"
    exit 0
fi

[ -f "$SRC/out/BetterINQ.dll" ] || { echo "还没编译：先 sh build.sh dll"; exit 1; }
mkdir -p "$PLUG/BetterINQ"
cp "$SRC/out/BetterINQ.dll" "$PLUG/BetterINQ.dll"
cmp -s "$SRC/out/BetterINQ.dll" "$PLUG/BetterINQ.dll" || { echo "复制后校验不一致"; exit 1; }
echo "已安装：$PLUG/BetterINQ.dll"

if [ -f "$PLUG/BetterINQ/BetterINQ.ini" ]; then
    echo "配置已存在，保留你的：$PLUG/BetterINQ/BetterINQ.ini"
else
    cp "$SRC/config/BetterINQ.ini" "$PLUG/BetterINQ/BetterINQ.ini"
    echo "已放入配置：$PLUG/BetterINQ/BetterINQ.ini"
fi

if tasklist 2>/dev/null | grep -qi "IN-Q.exe"; then
    echo "提醒：IN-Q.exe 还开着。进游戏前关掉它，两个会抢同一批挂钩点。"
fi
