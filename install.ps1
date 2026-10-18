# ============================================================================
#  把编好的插件装进游戏（install.sh 的 PowerShell 版，不需要 MinGW / sh）。
#    powershell -ExecutionPolicy Bypass -File install.ps1                 安装/更新
#    powershell -ExecutionPolicy Bypass -File install.ps1 -Uninstall      卸载（只删 DLL，配置和日志留着）
#    powershell -ExecutionPolicy Bypass -File install.ps1 -Game "E:\MHW"  游戏装在别处
#  配置已经存在时不覆盖，免得冲掉你在游戏外改过的设置。
#
#  注意：本文件是 UTF-8 带 BOM（Windows PowerShell 5.1 读无 BOM 的 .ps1 会按 GBK 解，中文会把它弄坏）。
# ============================================================================
param(
    [string]$Game = "D:\steam\steamapps\common\Monster Hunter World",
    [switch]$Uninstall
)

$ErrorActionPreference = "Stop"
$Src = Split-Path -Parent $MyInvocation.MyCommand.Path
$Plug = Join-Path $Game "nativePC\plugins"
$Dll = Join-Path $Plug "BetterINQ.dll"
$DataDir = Join-Path $Plug "BetterINQ"
$Ini = Join-Path $DataDir "BetterINQ.ini"

if (-not (Test-Path (Join-Path $Game "MonsterHunterWorld.exe"))) {
    throw "找不到游戏目录：$Game（装在别处的话用 -Game 指定游戏根目录）"
}
if (Get-Process -Name "MonsterHunterWorld" -ErrorAction SilentlyContinue) {
    throw "游戏正在运行，先退出游戏再装/卸"
}

if ($Uninstall) {
    if (Test-Path $Dll) { Remove-Item $Dll -Force }
    Write-Host "已卸载：$Dll（配置和日志在 $DataDir，没动）"
    exit 0
}

$Built = Join-Path $Src "out\BetterINQ.dll"
if (-not (Test-Path $Built)) {
    throw "还没编译：先 powershell -ExecutionPolicy Bypass -File build.ps1 dll"
}
New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
Copy-Item $Built $Dll -Force
# install.sh 用 cmp，这里比 SHA256
if ((Get-FileHash $Built).Hash -ne (Get-FileHash $Dll).Hash) { throw "复制后校验不一致" }
Write-Host "已安装：$Dll"

if (Test-Path $Ini) {
    Write-Host "配置已存在，保留你的：$Ini"
} else {
    Copy-Item (Join-Path $Src "config\BetterINQ.ini") $Ini
    Write-Host "已放入配置：$Ini"
}

if (Get-Process -Name "IN-Q" -ErrorAction SilentlyContinue) {
    Write-Host "提醒：IN-Q.exe 还开着。进游戏前关掉它，两个会抢同一批挂钩点。"
}
