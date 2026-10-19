# Better-IN-Q

《怪物猎人：世界 冰原》（PC 版 15.23.00）的快速重开任务插件 —— IN-Q 的游戏内置版。

由 [Stracker's Loader](https://www.nexusmods.com/monsterhunterworld/mods/1982) 加载，开游戏就在，不用另外开软件；
游戏里按键盘快捷键呼出面板，进去之后手柄全程可操作。

## 致谢

**快速重开的核心逻辑来自 IN-Q 原作者 年糕狗三世**（B 站视频 BV1yz2PBNEWn），原作者允许修改后重新分发。
移植依据的是 QCanon 中文化与修改的 CheatEngine 版（[QCAY1145/AutoCE-INQ-For-MHW](https://github.com/QCAY1145/AutoCE-INQ-For-MHW)）。

Better-IN-Q 只在界面、bug 修复和使用体验上做了优化，没有参与核心逻辑的开发。请大家支持原作者。

## 功能

- 一键重开当前任务（默认手柄 LB+RB+LT+RT），可选直接重开 / 重置后回据点再接 / 只接任务不出发
- 也能「只重置任务」（原 IN-Q 的 Reset only）：把当前任务重置回据点，不接任务也不出发；
  面板主页上就有这个按钮，想用快捷键就在 ini 的 `[Hotkeys]` 里给 `ResetOnly=` 设一个
- 重开时自动套用「全套」：装备套装、道具套装、猫饭、任务
- 套装和猫饭都能自己起名字：新建的时候就会先弹出来起名（留空就用默认名），之后在列表里选中一项
  按 `F`（键盘）或 `X`（手柄）也能改；名字最多 50 个字符（中英文都按字符算，输入框会跟着变宽，
  右下角是「已用字数/上限」（到上限变红））；名字只是显示用的，可以重名，ini 里套装是 `[Set.编号]` 加一行 `Name=`，
  套用按编号走
- 套装和猫饭都能改顺序：列表里按左摇杆（`L3`）或键盘 `E` 把选中的那一项放进移动模式（整行淡金底、右边写
  「移动中」），上下键换位置（按住可以一直移；挪到列表上下边缘时列表跟着一格一格滚），按 `L3` / `A` / `E` 放下、`B` 取消。移动中列表的
  十字键只用来换位置，不会把焦点挪到别的行；中途按 `B` 回到上一页时移动模式也会一起退出。猫饭的顺序单独存在 ini 的 `FoodOrder=` 里，只影响
  面板里的排列，编号不变
- 列表里手柄按 `Y` 直接套用 / 直接选，不用进编辑页按确认；空格 / 回车 / 手柄 `A` 和鼠标点一下同一个语义：
  还没在用的那个直接选中，已经在用的那个再按一次才进编辑页
- 配置分两份：`BetterINQ.ini` 放所有存档共用的设置（面板、快捷键、语言、HDR、`[Debug]`）；
  `profiles\<SteamID>_<档位>.ini` 只放套装和猫饭——换 Steam 账号或换存档时，这两样自动跟着换，面板设置不动
- 存档位在「设置 → 存档位」里点 1 / 2 / 3（游戏里换档之后来点一下）；文件名里的 `<SteamID>` 就是账号，换账号不会串
- 游戏内面板（默认键盘 Insert；手柄不能开面板，和 SharpPluginLoader 一样）：换任务（任务名中/英/日都能搜，还认中文拼音、拼音首字母、
  日文假名和罗马音，比如 `hei long` / `hl` / `でんせつ` / `densetsu` 都能搜到「传说中的黑龙」）、换套装、换猫饭、设置
- 输入法：面板里的输入框能直接用系统的中文 / 日文输入法打字（光标进输入框才打开，
  组字和候选条跟着光标位置走（面板按后台缓冲分辨率排版，喂给输入法的坐标会按客户区比例缩回去，
  否则系统判定位置在窗口外，候选条会被丢到屏幕角上）；关面板把输入法还给游戏窗口）。游戏自己也在管输入法上下文，
  面板每半秒确认一次，被游戏换掉就重新挂回来。另外输入法上下文（HIMC）属于线程，只有窗口自己的线程去挂才有用，
  而游戏的窗口在主线程建、渲染在另一个线程，所以这些调用都是投递回窗口线程执行的；
  日志里 `ime:` 开头的是这一块的诊断行（记了窗口线程、当前线程、布局是不是输入法、IME 文件名）
- HDR 面板：按后台缓冲格式自动适配（HDR10 走 PQ + BT.2020，scRGB 走线性），不再发白、串色；
  判断不对时 ini 里 `[General]` 的 `Hdr=` 可强制 `off` / `scrgb` / `hdr10`，`SdrNits=` 对齐 SDR 白点
- 面板开着时游戏收不到手柄、键盘和鼠标（手柄拦 XInput；键盘和光标拦 `GetAsyncKeyState` /
  `GetCursorPos` 这类函数，DirectInput 的键盘/鼠标再从设备虚表上截一道；DirectInput 这条不自己建设备，
  而是接住游戏建好的设备（自己建会让进程多一路输入通道，反而拖慢整机键鼠），鼠标可以自由点面板。
  还是漏的话看日志里「面板关掉：游戏读输入」那一行，三个计数全是 0 就说明游戏走的是别的路
- 界面有 **中文 / English / 日本語** 三套：设置页第一行切换，或 ini 里写 `Language=zh|en|ja|auto`（默认 `auto`，
  跟系统界面语言走）。任务名、猫饭技能名、套装摘要这些也跟着变；套装名和道具套装名是你存档里的原文，不翻
- 能直接导入原版 IN-Q 的 config.txt（设置页点按钮，面板里自己选文件）：**整体覆盖**，不跟现有合并——
  全套名、装备套装名、道具套装名、联动快捷栏、任务 ID、猫饭（`Meal=0` 是「体力+50,耐力+50」、`Meal=1` 是
  「猫饭无效」；`Status UP` 是字节偏移，4 / 8 / 12 对应攻击 / 防御 / 属性耐性，原版只有「（大）」一档；
  三个技能一起带）、界面语言、Reset / Accept only 一起搬过来；
  原版的手柄快捷键不搬（原版没有面板，面板只认键盘）。第一次运行且还没有全套时，会自动在 ini 同层和上一层找一次 config.txt
- 相比原版 IN-Q：做成插件，不再有「开游戏检测不到」；修了道具套装排过序后套错的问题；修了黑龙开局脱衣装

## 安装（玩家）

1. 先装好 Stracker's Loader（游戏根目录要有 `dinput8.dll` 和 `loader.dll`）。
2. 下载 Release 里的 `Better-IN-Q vX.Y.Z.zip`，用狩技 MOD 盒子安装，或者直接解压到游戏根目录。
3. 进游戏，回到据点时聊天框出现「Better-IN-Q x.y.z 已就绪」就装好了。

设置会自动存到 `nativePC\plugins\BetterINQ\BetterINQ.ini`，日志在同目录的 `BetterINQ.log`。
ini 里 `Language=` 一栏管界面语言（`zh` / `en` / `ja`，`auto` 或留空 = 跟系统界面语言）。
不要和 IN-Q.exe 同时开。

怀疑输入延迟时，`[Debug] Disable=` 可以按段关掉拦截（`pad` / `cursor` / `kbd` / `din` / `overlay`，
逗号分隔，`all` = 前四个）来二分；日志里每 30 秒有一行 `perf:`，记着主循环轮询耗时、Present 钩子耗时
和游戏帧间隔。

策略只看面板开没开：**面板关着时手柄状态原样交给游戏**（不扣留、不补发，所以没有额外输入延迟），
**面板开着时整块挡住**。面板只认键盘快捷键（默认 `Insert`，可在设置页改）；手柄不参与开面板，所以没有「组合键要先扣住再补发」这种 tricks，
上游那套「先扣 100 ms 再补发 4 帧」就是「原版也卡」的来源，已经删掉了。
游戏的 DirectX 12 开不开都能用：面板支持 DX11 和 DX12。

## 构建

**不需要 MinGW。** 装 Visual Studio 2022（或 Build Tools 2022，勾选「使用 C++ 的桌面开发」）就够了：
`build.ps1` 自己用 vswhere 找到 VS、进 MSVC 环境、编 ImGui、编 DLL、编并跑全部测试。

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1            # 编译 + 跑全部测试（默认 all）
powershell -ExecutionPolicy Bypass -File build.ps1 dll        # 只编 out\BetterINQ.dll
powershell -ExecutionPolicy Bypass -File build.ps1 test       # 只跑测试
powershell -ExecutionPolicy Bypass -File install.ps1          # 装进游戏目录（-Game 指定别的路径，-Uninstall 卸载）
py -3 tools/make_zip.py                                       # 打发布包
```

产物 `out\BetterINQ.dll`。`/MT` 静态链接 CRT，**运行游戏时也不依赖任何第三方运行库** ——
`dumpbin /dependents` 只看到 `d3d11 / dxgi / D3DCOMPILER_47 / imm32 / user32 / kernel32 / shell32`
这些 Windows 自带的东西，既不需要 MinGW 的 `libstdc++-6.dll`、也不需要 VC 运行库。

### 还想用 MinGW（可选）

`build.sh` / `install.sh` 原样留着，需要 [w64devkit](https://github.com/skeeto/w64devkit)（默认路径 `D:\w64devkit`）。
两条路编出来的 DLL 功能完全一样：`tests/hook_test.cpp` 里那四个假游戏函数桩，GCC 用文件里的 `asm(...)` 汇编，
MSVC 走 `tests/hook_test_asm.asm`（ml64 汇编），两边摆出来的指令字节一模一样。

### MSVC 这边的几个坑

- 源码是 UTF-8 **无 BOM**，必须加 `/utf-8`，否则中文按 GBK 读成乱码。
- 链接要显式写 `user32 / gdi32 / shell32`：MinGW 的 ld 会默认带上，MSVC 不会。
- `hook_test` 链接加了 `/OPT:NOICF`：MSVC 默认把机器码完全相同的函数折叠成同一个地址
  （`RpA(){return 1;}` 和 `VfA(){return 1;}` 会撞上），测试的「数据段里只有一处指针」就不成立了。
- `build.ps1` / `install.ps1` 是 **UTF-8 带 BOM**：Windows PowerShell 5.1 读无 BOM 的 .ps1 会按 GBK 解，
  里面的中文会把脚本本身解析坏，改文件时别把 BOM 弄丢。

`src/quests.inc` 是 `tools/gen_quests.py` 从上面那份 INQ.CT 生成的（含拼音，用 Windows 自带的 ICU）。
重新生成时把 CT 放在仓库根目录，命名为 `reference_INQ_QCanon.CT`。

生成完再跑 `py -3 tools/gen_ja_reading.py`：它用 [pykakasi](https://pypi.org/project/pykakasi/) 给每一行补上
日文名的假名读音和罗马字读音（`pip install pykakasi`），面板里的假名 / 罗马音搜索就靠这两列。
`gen_quests.py` 不跑也能单独跑它（它只读 `src/quests.inc`、原地补列，可重复跑）。

`src/foodskills.inc` 是 `tools/gen_foodskills.py` 从同一份 CT 的「Various editing script」里那三段猫饭技能表
（日 / 英 / 中）生成的，中英日三语一起出。用法：`py -3 tools/gen_foodskills.py <CT 路径>`。

## 目录

| 路径 | 内容 |
| --- | --- |
| `src/main.cpp` | 插件入口、主循环、快捷键 |
| `src/inq.*` | 重开的状态机和游戏挂钩（移植自 INQ.CT） |
| `src/hook.*` | 往游戏代码里插跳转、按值换函数指针 |
| `src/overlay.*` | 挂 DXGI Present，用 Dear ImGui 画面板（D3D11 / D3D12） |
| `src/ui.*` | 面板界面 |
| `src/i18n.*` | 界面语言（中文 / English / 日本語） |
| `src/padfilter.*` / `src/cursorfilter.*` / `src/kbdfilter.*` / `src/dinfilter.*` | 面板开着时挡住游戏的手柄、光标、键盘（Win32 读键 + DirectInput 设备虚表）|
| `src/inqimport.*` | 从原版 IN-Q 的 config.txt 导入全套 |
| `src/config.*` / `src/input.*` / `src/gamedata.*` | 配置文件、快捷键、任务表 |
| `tools/gen_quests.py` / `tools/gen_foodskills.py` | 从 INQ.CT 生成 `src/quests.inc` / `src/foodskills.inc` |
| `tools/gen_ja_reading.py` | 给 `src/quests.inc` 补日文假名 / 罗马音读音（pykakasi） |
| `tests/` | 单元测试（`build.ps1 all` 会跑） |
| `build.ps1` / `install.ps1` | MSVC 构建脚本 / 安装脚本（不需要 MinGW） |
| `build.sh` / `install.sh` | 同一套东西的 MinGW 版本（可选） |
| `third_party/imgui/` | [Dear ImGui](https://github.com/ocornut/imgui)（MIT 许可，见其中的 LICENSE.txt） |

## 许可证

[MIT](LICENSE)。`third_party/imgui/` 是 Dear ImGui，按它自己的 MIT 许可（版权归其作者）。
