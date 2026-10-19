﻿﻿# ============================================================================
#  Better-IN-Q —— 用 MSVC（Visual Studio 2022 / Build Tools 都行）构建，不需要 MinGW。
#    powershell -ExecutionPolicy Bypass -File build.ps1 [dll|test|all]   默认 all
#  产物：out\BetterINQ.dll
#  和 build.sh 的唯一区别是工具链：静态链接 CRT（/MT），DLL 不依赖任何第三方运行库。
#
#  注意：本文件是 UTF-8 带 BOM。Windows PowerShell 5.1 读无 BOM 的 .ps1 会按 GBK 解，
#        里面的中文会把脚本解析坏——改这个文件时别把 BOM 弄丢了。
# ============================================================================
param([string]$What = "all")

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Out = Join-Path $Root "out"
$Obj = Join-Path $Out "obj"
$IM = Join-Path $Root "third_party\imgui"
$Src = Join-Path $Root "src"
$Tests = Join-Path $Root "tests"

Push-Location $Root
try {

# ---- 1. 找 Visual Studio（vswhere.exe 随 VS / Build Tools 一起装） ----
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "找不到 vswhere.exe。先装 Visual Studio 2022 或 Build Tools 2022，勾选「使用 C++ 的桌面开发」。"
}
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) {
    throw "没装 MSVC 编译器。打开 Visual Studio Installer，勾选「使用 C++ 的桌面开发」再装一次。"
}
$vs = ($vs | Select-Object -First 1).Trim()

Import-Module (Join-Path $vs "Common7\Tools\Microsoft.VisualStudio.DevShell.dll")
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null

New-Item -ItemType Directory -Force -Path $Obj | Out-Null

# /utf-8：源码是 UTF-8 无 BOM，不加这个中文会被按 GBK 读成乱码
# /MT  ：静态链接 CRT，对应 build.sh 的 -static，DLL 不带 VC 运行库依赖
# /MP  ：多个 .cpp 并行编译
$Common = @("/nologo", "/utf-8", "/std:c++17", "/O2", "/EHsc", "/MT", "/W3", "/MP")

# MinGW 的 ld 会默认带上 user32 之类，MSVC 得一个个写出来
$WinLibs = @("kernel32.lib", "user32.lib", "gdi32.lib", "shell32.lib", "ole32.lib", "uuid.lib",
              "dinput8.lib", "dxguid.lib", "advapi32.lib")
$D3D11   = @("d3d11.lib", "d3dcompiler.lib", "dxgi.lib", "dwmapi.lib", "imm32.lib")

# cl 把错误写到 stderr，而 PowerShell 5.1 在 $ErrorActionPreference = "Stop" 时
# 会把 native stderr 当成终止错误（NativeCommandError）。所以整段重定向到临时文件再打印。
function Compile {
    param([string[]]$ClArgs, [string]$Tool = "cl")
    $log = Join-Path ([System.IO.Path]::GetTempPath()) ("biq_" + [guid]::NewGuid().ToString("n") + ".log")
    & $Tool @ClArgs *> $log
    $code = $LASTEXITCODE
    if (Test-Path $log) {
        Get-Content $log -Encoding Default | ForEach-Object { Write-Host $_ }
        Remove-Item $log -Force
    }
    if ($code -ne 0) { throw ("$Tool 失败（退出码 $code）：$Tool " + ($ClArgs -join " ")) }
}

# ImGui 本体和三个后端只在源码变了时重编
function Build-ImGui {
    $jobs = @(
        @{ Dir = $IM; Files = @("imgui", "imgui_draw", "imgui_tables", "imgui_widgets") },
        @{ Dir = (Join-Path $IM "backends"); Files = @("imgui_impl_dx11", "imgui_impl_dx12", "imgui_impl_win32") }
    )
    foreach ($j in $jobs) {
        foreach ($f in $j.Files) {
            $code = Join-Path $j.Dir "$f.cpp"
            $objf = Join-Path $Obj "$f.obj"
            if ((Test-Path $objf) -and ((Get-Item $objf).LastWriteTime -ge (Get-Item $code).LastWriteTime)) { continue }
            Write-Host "  cc $f.cpp"
            Compile ($Common + @("/c", "/I$IM", "/Fo$objf", $code))
        }
    }
}

$ImGuiObjs = @("imgui", "imgui_draw", "imgui_tables", "imgui_widgets",
               "imgui_impl_dx11", "imgui_impl_dx12", "imgui_impl_win32") |
             ForEach-Object { Join-Path $Obj "$_.obj" }
$ImGuiCore = $ImGuiObjs[0..3]

# ---- 2. 插件 DLL ----
if ($What -eq "dll" -or $What -eq "all") {
    Write-Host "==> BetterINQ.dll"
    Build-ImGui
    $srcs = @("main", "inq", "hook", "config", "input", "log",
              "ui", "gamedata", "overlay", "padfilter", "cursorfilter", "kbdfilter", "dinfilter",
              "hdr", "inqimport", "profile",
              "i18n") |
            ForEach-Object { Join-Path $Src "$_.cpp" }
    Compile ($Common + @("/LD", "/I$IM", "/Isrc", "/Fe$Out\BetterINQ.dll", "/Fo$Obj\") +
        $srcs + $ImGuiObjs + @("/link", "/IMPLIB:$Obj\BetterINQ.lib") + $D3D11 + $WinLibs)
    $dll = Join-Path $Out "BetterINQ.dll"
    if (-not (Test-Path $dll)) { throw "没生成 $dll" }
    Write-Host ("     {0}  {1:N0} 字节" -f $dll, (Get-Item $dll).Length)
}

# ---- 3. 测试 ----
if ($What -eq "test" -or $What -eq "all") {
    Write-Host "==> tests"
    Build-ImGui

    # hook_test 的四个桩函数：GCC 用 hook_test.cpp 里的 asm(...)，MSVC 先用 ml64 汇编这个 .asm
    $hookAsm = Join-Path $Obj "hook_test_asm.obj"
    Compile (@("/nologo", "/c", "/Fo$hookAsm", "$Tests\hook_test_asm.asm")) -Tool "ml64"
    # /OPT:NOICF：MSVC 会把机器码完全一样的函数折叠成同一个地址（RpA{return 1;} 和 VfA{return 1;}），
    # 而测试里 vtA[] 正好填满了 VfA，于是 ReplaceModulePointers 会多找到十几处。测试是按「没有折叠」
    # 写的（MinGW 的 ld 默认不折叠），这里关掉折叠保持原意。
    Compile ($Common + @("/Fo$Obj\", "/Fe$Out\hook_test.exe",
        "$Tests\hook_test.cpp", $hookAsm, "$Src\hook.cpp") +
        @("/link", "/OPT:NOICF") + $WinLibs)

    Compile ($Common + @("/Fo$Obj\", "/Isrc", "/Fe$Out\gamedata_test.exe",
        "$Tests\gamedata_test.cpp", "$Src\gamedata.cpp", "$Src\i18n.cpp") +
        @("/link") + $WinLibs)

    Compile ($Common + @("/Fo$Obj\", "/Isrc", "/Fe$Out\config_test.exe",
        "$Tests\config_test.cpp", "$Src\config.cpp", "$Src\input.cpp", "$Src\padfilter.cpp",
        "$Src\hook.cpp", "$Src\log.cpp", "$Src\i18n.cpp") +
        @("/link") + $WinLibs)

    Compile ($Common + @("/Fo$Obj\", "/Isrc", "/Fe$Out\cursor_test.exe",
        "$Tests\cursor_test.cpp", "$Src\cursorfilter.cpp", "$Src\hook.cpp", "$Src\log.cpp",
        "$Src\i18n.cpp") +
        @("/link") + $WinLibs)

    Compile ($Common + @("/Fo$Obj\", "/Isrc", "/Fe$Out\profile_test.exe",
                         "$Tests\profile_test.cpp", "$Src\profile.cpp", "$Src\log.cpp", "$Src\i18n.cpp") + @("/link") + $WinLibs)
    if ($LASTEXITCODE -ne 0) { throw "profile_test 编译失败" }

    Compile ($Common + @("/Fo$Obj\", "/Isrc", "/Fe$Out\inqimport_test.exe",
        "$Tests\inqimport_test.cpp", "$Src\inqimport.cpp", "$Src\config.cpp", "$Src\i18n.cpp",
        "$Src\log.cpp") +
        @("/link") + $WinLibs)
    if ($LASTEXITCODE -ne 0) { throw "inqimport_test 编译失败" }

    Compile ($Common + @("/Fo$Obj\", "/I$IM", "/Isrc", "/Fe$Out\hdr_test.exe",
        "$Tests\hdr_test.cpp", "$Src\hdr.cpp", "$Src\log.cpp", "$Src\i18n.cpp") +
        $D3D11 + $WinLibs)
    if ($LASTEXITCODE -ne 0) { throw "hdr_test 编译失败" }

    Compile ($Common + @("/Fo$Obj\", "/Isrc", "/Fe$Out\kbdfilter_test.exe",
        "$Tests\kbdfilter_test.cpp", "$Src\kbdfilter.cpp", "$Src\hook.cpp", "$Src\log.cpp",
        "$Src\i18n.cpp") +
        @("/link") + $WinLibs)

    # 真建一个 DirectInput 键盘设备，换完虚表后调 GetDeviceState：挡的时候全是 0，不挡时和原来一样
    Compile ($Common + @("/Fo$Obj\", "/Isrc", "/Fe$Out\dinfilter_test.exe",
        "$Tests\dinfilter_test.cpp", "$Src\dinfilter.cpp", "$Src\hook.cpp", "$Src\log.cpp",
        "$Src\i18n.cpp") +
        @("/link") + $WinLibs)

    Compile ($Common + @("/Fo$Obj\", "/I$IM", "/Isrc", "/Fe$Out\ui_nav_test.exe",
        "$Tests\ui_nav_test.cpp", "$Src\ui.cpp", "$Src\gamedata.cpp", "$Src\config.cpp", "$Src\input.cpp",
        "$Src\padfilter.cpp", "$Src\hook.cpp", "$Src\log.cpp", "$Src\i18n.cpp") +
        $ImGuiCore + @(Join-Path $Obj "imgui_impl_dx11.obj") +
        @("/link") + @("d3d11.lib", "d3dcompiler.lib") + $WinLibs)

    Compile ($Common + @("/Fo$Obj\", "/I$IM", "/Isrc", "/Fe$Out\overlay_test.exe",
        "$Tests\overlay_test.cpp", "$Src\overlay.cpp", "$Src\hdr.cpp", "$Src\padfilter.cpp", "$Src\ui.cpp",
        "$Src\gamedata.cpp", "$Src\config.cpp", "$Src\input.cpp", "$Src\log.cpp", "$Src\hook.cpp",
        "$Src\i18n.cpp") +
        $ImGuiObjs + @("/link") + $D3D11 + $WinLibs + @("windowscodecs.lib", "xinput.lib"))

    Compile ($Common + @("/Fo$Obj\", "/I$IM", "/Isrc", "/Fe$Out\overlay_dx12_test.exe",
        "$Tests\overlay_dx12_test.cpp", "$Src\overlay.cpp", "$Src\hdr.cpp", "$Src\ui.cpp", "$Src\gamedata.cpp",
        "$Src\config.cpp", "$Src\input.cpp", "$Src\padfilter.cpp", "$Src\log.cpp", "$Src\hook.cpp",
        "$Src\i18n.cpp") +
        $ImGuiObjs + @("/link") + @("d3d12.lib") + $D3D11 + $WinLibs + @("windowscodecs.lib"))

    $fail = 0
    foreach ($t in @("hook_test", "gamedata_test", "config_test", "cursor_test", "kbdfilter_test", "hdr_test", "profile_test", "inqimport_test",
                     "dinfilter_test", "ui_nav_test")) {
        Write-Host "  run $t.exe"
        & (Join-Path $Out "$t.exe")
        if ($LASTEXITCODE -ne 0) { $fail = $fail + 1 }
    }
    # 这两个要一个输出目录当参数（截图存在那儿）
    foreach ($t in @("overlay_test", "overlay_dx12_test")) {
        Write-Host "  run $t.exe"
        & (Join-Path $Out "$t.exe") $Out
        if ($LASTEXITCODE -ne 0) { $fail = $fail + 1 }
    }

    if ($fail -ne 0) { throw "$fail 个测试失败" }
    Write-Host "全部测试通过"
}

} finally {
    Pop-Location
}
