// ============================================================================
//  Better-IN-Q —— MHW:IB 的快速重开任务，做成游戏插件（Stracker's Loader 加载）
//
//  核心逻辑移植自 INQ.CT：原作者 年糕狗三世（B 站 BV1yz2PBNEWn），作者允许
//  修 bug / 加功能后重新分发；CT 的中文化与修改由 QCanon 完成。
// ============================================================================
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "game.h"
#include "startup_gate.h"
#include "crashfilter.h"
#include "cursorfilter.h"
#include "hangdump.h"
#include "hdr.h"
#include "dinfilter.h"
#include "i18n.h"
#include "inqimport.h"
#include "profile.h"
#include "hook.h"
#include "inq.h"
#include "runtime_state.h"
#include "input.h"
#include "kbdfilter.h"
#include "log.h"
#include "overlay.h"
#include "padfilter.h"
#include "ui.h"

LONG WINAPI OnCrash(EXCEPTION_POINTERS* info);   // 定义在文件末尾（DllMain 旁边）

namespace {

HMODULE g_module = nullptr;
volatile LONG g_stop = 0;
volatile LONG g_teardown = 0;   // 窗口已销毁/进程在退出，这时候别再写日志了
// 窗口已经没了、我们正在收尾：异常处理器从此一律不碰（见 OnCrash）。
std::atomic<bool> g_exiting{false};
std::atomic<long> g_inCrash{0};
const char* const kVersion = "0.7.2.1";

// 主循环线程和游戏渲染线程（画面板）共用，都要先拿这把锁
std::mutex g_mx;
Config g_cfg;
ui::Model g_model;
bool g_refreshNames = false;

std::wstring ModuleDir()
{
    wchar_t buf[MAX_PATH] = {};
    ::GetModuleFileNameW(g_module, buf, MAX_PATH);
    std::wstring s(buf);
    const std::size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring() : s.substr(0, p + 1);
}

void LogHotkey(const char* what, const std::string& spec, const Hotkey& hk)
{
    Log("hotkey %s = \"%s\"（%zu 组）%s%s", what, spec.c_str(), hk.alts.size(),
        hk.bad.empty() ? "" : " 认不出：", hk.bad.c_str());
}

// ---- 渲染线程里的回调 ----
void OnOverlayInit(float scale)
{
    std::lock_guard<std::mutex> lk(g_mx);
    ui::Init(scale);
}
void OnOverlayOpened()
{
    std::lock_guard<std::mutex> lk(g_mx);
    ui::OnOpen(g_model);
    g_refreshNames = true;
}
void OnOverlayDraw(float w, float h)
{
    std::lock_guard<std::mutex> lk(g_mx);
    ui::Draw(g_model, w, h);
}

struct Hotkeys {
    Hotkey restart, panel, resetOnly;
    std::string specs[3];
    bool parsed = false;
    // 存盘时也会调：快捷键没变就不重新解析（按键的按下状态要保留，不然按着时存盘会误触发）
    void Parse(const Config& c)
    {
        if (parsed && specs[0] == c.hkRestart && specs[1] == c.hkPanel && specs[2] == c.hkResetOnly) return;
        parsed = true;
        specs[0] = c.hkRestart;
        specs[1] = c.hkPanel;
        specs[2] = c.hkResetOnly;
        restart = ParseHotkey(c.hkRestart);
        panel = ParseHotkey(c.hkPanel);
        // 和重开键一样旁路轮询手柄组合；面板关闭时不扣留、延迟或吞掉游戏输入。
        resetOnly = ParseHotkey(c.hkResetOnly);
        LogHotkey("Restart", c.hkRestart, restart);
        LogHotkey("Panel", c.hkPanel, panel);
        LogHotkey("ResetOnly", c.hkResetOnly, resetOnly);
    }
};

// [Debug] Disable：排查输入延迟时按段关掉。all = 前四个（面板还留着，否则没法改 ini）
struct Disabled {
    bool pad = false, cursor = false, kbd = false, din = false, overlay = false, ime = false;
};

Disabled ParseDisabled(const std::string& spec)
{
    Disabled d;
    std::string cur;
    for (std::size_t i = 0; i <= spec.size(); ++i) {
        const char c = i < spec.size() ? spec[i] : ',';
        if (c != ',' && c != ' ' && c != ';') { cur += (char)std::tolower((unsigned char)c); continue; }
        if (cur == "all") { d.pad = d.cursor = d.kbd = d.din = true; }
        else if (cur == "pad" || cur == "padfilter") d.pad = true;
        else if (cur == "cursor" || cur == "cursorfilter") d.cursor = true;
        else if (cur == "kbd" || cur == "kbdfilter") d.kbd = true;
        else if (cur == "din" || cur == "dinfilter") d.din = true;
        else if (cur == "overlay") d.overlay = true;
        else if (cur == "ime") d.ime = true;
        cur.clear();
    }
    return d;
}

double QpcUs(unsigned long long ticks)
{
    static double perTick = 0;
    if (perTick == 0) {
        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        perTick = 1e6 / (double)f.QuadPart;
    }
    return (double)ticks * perTick;
}

bool g_blocked = false;   // 现在是不是在挡游戏的输入（面板开着）

void SetPanel(bool open)
{
    const bool was = g_blocked;
    overlay::SetOpen(open);
    g_blocked = overlay::IsOpen();
    padfilter::SetBlock(g_blocked);
    cursorfilter::SetBlock(g_blocked);
    kbdfilter::SetBlock(g_blocked);
    dinfilter::SetBlock(g_blocked);
    if (g_blocked && !was) {          // 开始挡：计数清零
        kbdfilter::ResetStats();
        cursorfilter::ResetStats();
        dinfilter::ResetStats();
    } else if (!g_blocked && was) {   // 停止挡：这一轮拦下了多少次读输入，记进日志
        Log("面板关掉：游戏读输入 kbdfilter[%s] cursorfilter[%s] dinfilter[%s]",
            kbdfilter::Stats().c_str(), cursorfilter::Stats().c_str(), dinfilter::Stats().c_str());
    }
}

// 手柄、光标过滤：游戏的 XInput 可能晚一点才加载，装不上就隔两秒再试，最多试 2 分钟
struct Filter {
    const char* name;
    bool (*install)(std::string&);
    bool (*installed)();
    int tries = 0;
    ULONGLONG last = 0;
    void Try(ULONGLONG now)
    {
        if (installed() || tries >= 60 || now - last <= 2000) return;
        last = now;
        ++tries;
        std::string info;
        const bool ok = install(info);
        if (ok || tries == 1 || tries == 60) Log("%s: %s", name, info.c_str());
        if (ok && overlay::IsOpen()) SetPanel(true);   // 面板已经开着：补上拦截
    }
};

// Same player-chain predicate as game::InScene, plus save/UI/loading checks.
// ReadProcessMemory avoids VirtualQuery/memcpy races on the worker thread;
// these remain conservative snapshots, not a Steam/DXGI hook-readiness proof.
template <class T> bool StartupRead(std::uintptr_t address, T& value)
{
    SIZE_T read = 0;
    value = T{};
    return address >= 0x10000 &&
        ::ReadProcessMemory(::GetCurrentProcess(), reinterpret_cast<const void*>(address),
                            &value, sizeof(value), &read) && read == sizeof(value);
}

bool StartupSceneSample()
{
    std::uintptr_t player = 0, entity = 0, root = 0, ui = 0, load = 0;
    std::uintptr_t save = 0, base = 0;
    std::uint64_t slot = 0;
    std::uint32_t loading = 0;
    return StartupRead(game::pPlayerRoot, player) && player &&
        StartupRead(player + 0x50, entity) && entity &&
        StartupRead(game::pUiBase, root) && root &&
        StartupRead(root + 0x278, ui) && ui && StartupRead(ui + 0x20, load) && load &&
        StartupRead(load + 0x1D04, loading) && loading == 0 &&
        StartupRead(game::pSave, save) && save &&
        StartupRead(save + 0xA8, base) && base &&
        StartupRead(save + 0xA0, slot) && slot < 3;
}

DWORD WINAPI Worker(LPVOID)
{
    while (!::GetModuleHandleW(L"MonsterHunterWorld.exe") && !g_stop) ::Sleep(200);
    if (g_stop) return 0;

    const std::wstring dir = ModuleDir() + L"BetterINQ\\";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    LogInit(dir + L"BetterINQ.log");
    ::AddVectoredExceptionHandler(1, OnCrash);   // 崩溃时把出错地址写进日志
    hangdump::Init();                            // 现在就把 dbghelp 拿好，退出卡住时才有栈可看

    // 配置按「Steam 账号 + 存档位」分开放：同一台电脑换账号、同一账号换存档都不串
    profile::Init(dir);
    profile::EnsureMigrated();                  // 老的单份 BetterINQ.ini 复制成当前档案的底子
    std::wstring basePath = profile::BasePath();    // 所有存档共用：面板设置/快捷键/语言/HDR
    std::wstring cfgPath = profile::ConfigPath();   // 本存档：套装和猫饭
    Log("profile: 共用配置 %s", inqimport::Narrow(basePath).c_str());
    Log("profile: 本档配置 %s", inqimport::Narrow(cfgPath).c_str());
    Log("Better-IN-Q %s starting", kVersion);

    Hotkeys hk;
    std::wstring autoSrc;      // 插件目录旁边有没有原版 config.txt（第一次运行自动导入用）
    {
        std::lock_guard<std::mutex> lk(g_mx);
        std::string err;
        if (!LoadSplit(basePath, cfgPath, g_cfg, err)) Log("config: %s", err.c_str());
        i18n::Set(i18n::FromString(g_cfg.language));   // 越早越好：后面的日志和提示都按它来
        hdr::Configure(g_cfg.hdr, g_cfg.sdrNits);      // HDR 交换链上的面板颜色修正
        Log("config: QuestID=%d Reset=%d AcceptOnly=%d Wyvern=%d ActiveSet=%d Food=%d 全套%zu 猫饭%zu",
            g_cfg.questId, g_cfg.reset, g_cfg.acceptOnly, g_cfg.wyvern, g_cfg.activeSet, g_cfg.food,
            g_cfg.sets.size(), g_cfg.foods.size());
        g_model.cfg = &g_cfg;
        g_model.version = kVersion;
        g_model.slot = profile::Slot();   // 设置页里那排 1/2/3 的当前值
        autoSrc = inqimport::Resolve("", basePath);   // 旁边有就顺手带上
        if (!autoSrc.empty()) Log("import: 旁边有原版 config.txt：%s", inqimport::Narrow(autoSrc).c_str());
        hk.Parse(g_cfg);
    }
    // 第一次装的时候：旁边有原版 IN-Q 的 config.txt 就把全套直接搬过来，省得手写
    if (g_cfg.sets.empty() && !autoSrc.empty()) {
        std::string ierr, inotes;
        const int n = inqimport::Import(autoSrc, g_cfg, ierr, &inotes);
        if (n > 0) {
            if (!SaveSplit(basePath, cfgPath, g_cfg)) Log("config: 存盘失败");
            Log("import: 从原版 IN-Q 导入了 %d 个全套（%s）", n, inotes.c_str());
        } else {
            Log("import: %s", ierr.c_str());
        }
    }

    const Disabled off = ParseDisabled(g_cfg.debugDisable);
    if (off.pad || off.cursor || off.kbd || off.din || off.overlay || off.ime)
        Log("debug: 关掉了过滤器 pad=%d cursor=%d kbd=%d din=%d overlay=%d ime=%d", off.pad, off.cursor,
            off.kbd, off.din, off.overlay, off.ime);
    if (off.ime) overlay::SetImeEnabled(false);

    Input in;
    if (!in.Init()) Log("XInput 没加载到，手柄快捷键不可用");

    std::string report;
    bool ok;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        ok = inq::Install(g_cfg, report);
        g_model.ready = ok;
    }
    if (!report.empty()) Log("install report:\n%s", report.c_str());

    overlay::Callbacks cb;
    cb.init = OnOverlayInit;
    cb.opened = OnOverlayOpened;
    cb.draw = OnOverlayDraw;
    if (g_cfg.debugNoWndHook) overlay::SetWndHook(false);
    startup::Gate startupGate;
    bool panelOk = false;
    if (off.overlay) Log("debug: overlay 关掉了，没有面板（快捷键重开照常）");
    else Log("overlay: 等场景稳定并重新按面板快捷键后才安装（启动不建临时交换链）");

    if (ok) Chat(std::string("Better-IN-Q ") + kVersion + i18n::Tr(" 已就绪", " ready", " 準備完了"));
    else Chat(i18n::Tr("Better-IN-Q：重开功能没装上，详情见 BetterINQ.log",
                       "Better-IN-Q: the restart hook did not install, see BetterINQ.log",
                       "Better-IN-Q：再開機能を組み込めませんでした。BetterINQ.log を見てください"));

    // dinfilter 默认不自己建设备（会让进程多一路 DirectInput，键盘鼠标整体变钝）；
    std::vector<Filter> filters;
    if (!off.pad) filters.push_back({"padfilter", padfilter::Install, padfilter::Installed});
    if (!off.cursor) filters.push_back({"cursorfilter", cursorfilter::Install, cursorfilter::Installed});
    if (!off.kbd) filters.push_back({"kbdfilter", kbdfilter::Install, kbdfilter::Installed});
    if (!off.din) filters.push_back({"dinfilter", dinfilter::Install, dinfilter::Installed});
    ULONGLONG lastTick = 0, dirtyAt = 0, lastNamesRefresh = 0;
    bool migrated = false;      // 老配置里的套装名字只换一次编号
    int migrateTries = 0;
    // 诊断：主循环每 16ms 醒一次，看看我们自己花了多少时间（延迟排查用，每 30 秒写一行）
    ULONGLONG perfAt = ::GetTickCount64();
    double pollSum = 0, pollMax = 0;
    int pollN = 0;
    double tickSum = 0, tickMax = 0;
    int tickN = 0;
    // 装重开钩子偶尔会失败：要一段 rel32 内的空闲内存，开机那会儿地址空间可能正乱。
    // 只有「连 arena 都没拿到」才是安全可重试的（那种情况一个钩子都还没装）。
    ULONGLONG inqRetryAt = 0;
    int inqTries = 0;
    runtime::Mode runtimeMode = g_cfg.enabled ? runtime::Mode::Active : runtime::Mode::Dormant;
    while (!g_stop) {
        if (overlay::WindowGone()) {
            // 窗口没了说明游戏正在收尾：趁 D3D 设备还活着，自己先把手上的引用和
            // ImGui 后端放掉，别让游戏的析构去等我们，也别拖到 DLL_PROCESS_DETACH 那一刻。
            Log("worker: 游戏窗口已经销毁，提前收尾");
            g_exiting = true;      // 异常处理器从此闭嘴：退出路径上任何文件 IO 都可能把别人等死
            // 线程栈诊断先挂上：万一我们自己的收尾卡住，6 秒 / 14 秒的 dump 照样能写出来
            // （诊断线程是独立线程，不依赖主循环，也不依赖我们的收尾是否跑完）。
            hangdump::Arm(6000, 14000);
            SetPanel(false);       // 面板要是还开着，先把四路输入过滤全放开，别带着拦截进退出流程
            overlay::ShutdownForExit();
            if (g_cfg.debugForceQuit) {
                // 兜底：窗口已销毁、我们的收尾也做完了，游戏自己卡在 DXGI/音频销毁里不走
                // （画面没了、BGM 还在、手柄还能动）。窗口既然没了，存档早已落盘，
                // 10 秒（3 + 7）还没退就由我们把进程带走。
                std::thread([] {
                    ::Sleep(3000);
                    Log("watchdog: 窗口销毁 3 秒后进程仍在，继续等到 10 秒");
                    ::Sleep(7000);
                    Log("watchdog: 10 秒后进程仍未退出，主动结束");
                    ::TerminateProcess(::GetCurrentProcess(), 0);
                }).detach();
            }
            g_teardown = 1;
            Log("worker: 收尾完成，后台线程退出");
            break;
        }
        const ULONGLONG now = ::GetTickCount64();
        for (Filter& f : filters) f.Try(now);
        // arena 没拿到 → 一个钩子都没装，重试是安全的；拿到了还失败就别再试（会重复打补丁）
        if (!g_model.ready && hook::ArenaBase() == 0 && inqTries < 20 && now >= inqRetryAt) {
            inqRetryAt = now + 3000;
            ++inqTries;
            std::string again;
            bool ok2 = false;
            {
                std::lock_guard<std::mutex> lk(g_mx);
                ok2 = inq::Install(g_cfg, again);
                g_model.ready = ok2;
            }
            if (ok2) {
                Log("install 第 %d 次重试成功", inqTries);
                Chat(std::string("Better-IN-Q ") + kVersion + i18n::Tr(" 已就绪", " ready", " 準備完了"));
            } else if (!again.empty()) {
                Log("install 第 %d 次重试仍失败：%s", inqTries, again.c_str());
            }
        }

        LARGE_INTEGER q0, q1;
        ::QueryPerformanceCounter(&q0);
        in.Poll();
        ::QueryPerformanceCounter(&q1);
        {
            const double us = QpcUs((unsigned long long)(q1.QuadPart - q0.QuadPart));
            pollSum += us;
            if (us > pollMax) pollMax = us;
            ++pollN;
            if (now - perfAt > 30000) {
                Log("perf: 快捷键轮询 %d 次，平均 %.1f µs、最大 %.1f µs；我们自己调 XInput %d 次；"
                    "inq::Tick %d 次，平均 %.1f µs、最大 %.1f µs",
                    pollN, pollN ? pollSum / pollN : 0.0, pollMax, PollCalls(), tickN,
                    tickN ? tickSum / tickN : 0.0, tickMax);
                // 游戏自己调这些钩子的频率和耗时：延迟到底是不是我们这几条路上的，看这行
                Log("perf: 钩子 pad[%s] cursor[%s]", padfilter::Profile().c_str(), cursorfilter::Stats().c_str());
                perfAt = now;
                pollSum = pollMax = 0;
                pollN = 0;
                tickSum = tickMax = 0;
                tickN = 0;
            }
        }
        bool capturing;   // 面板正在录快捷键：按的键是要录的，不当快捷键用
        {
            std::lock_guard<std::mutex> lk(g_mx);
            capturing = g_model.capturing && overlay::IsOpen();
            g_model.padHeld = in.PadHeld();
            if (capturing) g_model.keysHeld = KeysHeld();
            else g_model.keysHeld.clear();
        }
        // Always consume edges, including title/loading frames. Never defer an
        // early held shortcut until the first ready scene sample.
        const bool panelKey = in.Fired(hk.panel);
        startupGate.Sample(StartupSceneSample(), now);
        if (!startupGate.SceneReady() && overlay::IsOpen()) SetPanel(false);
        const startup::Gate::PanelAction panelAction = startupGate.Panel(panelKey && !capturing, off.overlay);
        if (panelAction == startup::Gate::PanelAction::Install) {
            std::string installError;
            panelOk = overlay::Install(cb, installError);
            startupGate.CompleteInstall(panelOk);
            if (!panelOk) Log("overlay: %s", installError.c_str());
        }
        if (panelAction != startup::Gate::PanelAction::None) {
            if (overlay::Failed() || !panelOk) Chat(i18n::Tr("Better-IN-Q：面板画不出来，详情见 BetterINQ.log（重开快捷键照常能用）",
                                                             "Better-IN-Q: the panel cannot be drawn, see BetterINQ.log (the restart hotkey still works)",
                                                             "Better-IN-Q：パネルを描画できません。BetterINQ.log を見てください（再開ホットキーは使えます）"));
            else SetPanel(!overlay::IsOpen());
        }
        // 面板刚打开就发现画不出来：别一直挡着手柄和鼠标
        if (g_blocked && overlay::Failed()) {
            SetPanel(false);
            Chat(i18n::Tr("Better-IN-Q：面板画不出来，详情见 BetterINQ.log（重开快捷键照常能用）",
                          "Better-IN-Q: the panel cannot be drawn, see BetterINQ.log (the restart hotkey still works)",
                          "Better-IN-Q：パネルを描画できません。BetterINQ.log を見てください（再開ホットキーは使えます）"));
        }
        const bool open = overlay::IsOpen();
        const bool restartKey = in.Fired(hk.restart);       // 面板开着时不响应，免得在面板里误触
        const bool resetKey = in.Fired(hk.resetOnly);

        {
            std::lock_guard<std::mutex> lk(g_mx);
            ui::Model& m = g_model;

            // UI 在渲染线程先改 Config、再提 reqSave；必须先发布状态过渡，避免同一帧关掉功能又执行旧请求。
            const runtime::Mode wantedMode = g_cfg.enabled ? runtime::Mode::Active : runtime::Mode::Dormant;
            if (wantedMode != runtimeMode) {
                const runtime::TransitionPlan plan = runtime::PlanTransition(runtimeMode, wantedMode);
                if (plan.enableInq) inq::ApplyOptions(g_cfg);
                else inq::SetEnabled(false);
                if (plan.closePanel) SetPanel(false);   // 同时让四路输入过滤恢复，面板快捷键之后仍可再打开
                if (plan.unblockInputs && g_blocked) SetPanel(false);
                m.reqRestart = false;
                m.reqResetOnly = false;
                m.reqApplyEquip = false;
                runtimeMode = wantedMode;
                Log("runtime: %s", runtimeMode == runtime::Mode::Active ? "装载态" : "卸载态（业务旁路，钩子常驻）");
            }

            const bool sceneActions = startupGate.AllowsActions() && !capturing;
            const runtime::HotkeyActions actions = runtime::GateHotkeys(runtimeMode, false,
                                                                       sceneActions && restartKey,
                                                                       sceneActions && resetKey);
            if (!open && actions.restart) { Log("hotkey: restart"); inq::RequestRestart(g_cfg); }
            if (!open && actions.resetOnly) { Log("hotkey: reset only"); inq::RequestResetOnly(); }

            if (m.reqClose) { m.reqClose = false; SetPanel(false); }
            if (m.reqRestart) {
                m.reqRestart = false;
                if (runtimeMode == runtime::Mode::Active && startupGate.AllowsActions()) { Log("panel: restart"); inq::RequestRestart(g_cfg); }
            }
            if (m.reqResetOnly) {
                m.reqResetOnly = false;
                if (runtimeMode == runtime::Mode::Active && startupGate.AllowsActions()) { Log("panel: reset only"); inq::RequestResetOnly(); }
            }
            if (m.reqApplyEquip) {
                m.reqApplyEquip = false;
                if (runtimeMode == runtime::Mode::Active && !m.reqSlot && !m.reqImportInq)
                    inq::RequestActiveSetEquipApply(g_cfg);
            }
            if (m.reqImportInq) {        // 设置页的文件选择框里挑好了 config.txt
                m.reqImportInq = false;
                const std::wstring src = m.inqPath;      // 选择框里挑好的
                m.inqPath.clear();
                std::string ierr, inotes;
                const int n = src.empty() ? -1 : inqimport::Import(src, g_cfg, ierr, &inotes);
                if (n > 0) {
                    inq::CancelPending(); // 导入替换套装后，旧编号的待办不能继续提交。
                    // 原版存的是名字，我们存编号：趁现在游戏里的名单还在，换过来
                    const int lostNames = inq::NormalizeEquipItemNumbers(g_cfg);
                    // 导入把套装/猫饭整个换掉、编号重新发，生效值要跟着重算
                    inq::ApplyOptions(g_cfg);
                    if (!SaveSplit(basePath, cfgPath, g_cfg)) Log("config: 存盘失败");
                    Log("import: %d 个全套（%s）", n, inotes.c_str());
                    Chat(i18n::Fmt(i18n::Tr("Better-IN-Q：从原版 IN-Q 导入了 %d 个全套",
                                            "Better-IN-Q: imported %d loadouts from IN-Q",
                                            "Better-IN-Q：旧 IN-Q から %d 件取り込みました"), n));
                    if (lostNames > 0)
                        Chat(i18n::Fmt(i18n::Tr("Better-IN-Q：有 %d 个装备/道具套装名在游戏里没找到，已留空，请到套装页重选",
                                                "Better-IN-Q: %d equip/item loadout names were not found and left empty; pick them again in the sets page",
                                                "Better-IN-Q：%d 件の装備/アイテムセット名が見つからず空にしました。セット画面で選び直してください"),
                                      lostNames));
                } else if (src.empty()) {
                    Chat(i18n::Tr("Better-IN-Q：没选文件", "Better-IN-Q: no file picked",
                                  "Better-IN-Q：ファイルが選ばれていません"));
                } else {
                    Chat(i18n::Fmt(i18n::Tr("Better-IN-Q：导入失败：%s", "Better-IN-Q: import failed: %s",
                                            "Better-IN-Q：取り込みに失敗しました：%s"), ierr.c_str()));
                }
            }
            // 存档位是玩家在设置页选的（游戏里换档之后来点一下 1/2/3）
            if (m.reqSlot) {
                m.reqSlot = false;
                const int want = m.slot;
                if (want >= 1 && want <= 3 && want != profile::Slot()) {
                    // 换存档只换「这个档里的东西」：面板设置写进共用的 base，跟着人走；
                    // 套装和猫饭归存档——切到哪一档就读那一档的 profile，没配过就是空的。
                    if (!SaveSplit(basePath, cfgPath, g_cfg)) Log("config: 存盘失败");
                    inq::CancelPending(); // 丢弃旧存档的立即换装和其他未提交请求。
                    profile::SetSlot(want);
                    cfgPath = profile::ConfigPath();
                    g_cfg.ClearLoadouts();
                    {
                        std::string serr;
                        if (!LoadSplit(basePath, cfgPath, g_cfg, serr)) Log("config: %s", serr.c_str());
                    }
                    i18n::Set(i18n::FromString(g_cfg.language));
                    hdr::Configure(g_cfg.hdr, g_cfg.sdrNits);
                    hk.Parse(g_cfg);
                    inq::ApplyOptions(g_cfg);
                    m.reqSave = false;
                    dirtyAt = 0;
                    g_refreshNames = true;
                    Log("profile: 切到存档 %d（保留面板设置；套装 %zu 猫饭 %zu）", want, g_cfg.sets.size(),
                        g_cfg.foods.size());
                }
            }
            // 全局任务跟着「实际进去了的那一关」走：重开的和柜台自己接的都算。
            // 取消套用之后重开才不会退回更早手工选过的任务。
            if (g_cfg.enabled) {
                if (const int entered = inq::TakeEnteredQuest()) {
                    if (g_cfg.questId != entered) {
                        g_cfg.questId = entered;
                        m.reqSave = true;
                        Log("quest: 进过 %d，全局任务跟着改成它", entered);
                    }
                }
            } else {
                (void)inq::TakeEnteredQuest();
            }
            if (m.reqSave) {             // 设置马上生效；写盘等停手 0.4 秒，免得打一个字存一次
                m.reqSave = false;
                inq::ApplyOptions(g_cfg);
                dirtyAt = now;
            }
            if (dirtyAt && now - dirtyAt > 400) {
                dirtyAt = 0;
                if (!SaveSplit(basePath, cfgPath, g_cfg)) Log("config: 存盘失败");
                hk.Parse(g_cfg);
            }
            // 游戏里改名、保存/删除套装不会通知面板；定期重读而非等插件重启。
            // 开面板/切档仍立即刷新，平时每 0.5 秒刷新，编号与存放格引用不变。
            if (g_refreshNames || now - lastNamesRefresh >= 500) {
                g_refreshNames = false;
                lastNamesRefresh = now;
                m.equipNames.clear();
                m.itemNames.clear();
                for (const auto& e : inq::ListEquipLoadouts()) m.equipNames.push_back({e.number, e.name, e.store});
                for (const auto& it : inq::ListItemLoadouts()) m.itemNames.push_back({it.number, it.name, it.store});
            }
            // 0.7.1 及以前面板把装备/道具套装按名字存进 ini，删掉读取兼容之后这些值没人认了。
            // 进存档、拿得到游戏里的名单之后换成编号并写回（一次性改写，不是读取兼容）。
            // 一秒试一次，读到名单之前什么都不改。
            if (!migrated && NeedsLoadoutMigration(g_cfg) && ++migrateTries % 60 == 1) {
                if (const int changed = inq::MigrateLoadoutRefs(g_cfg)) {
                    migrated = true;
                    if (!SaveSplit(basePath, cfgPath, g_cfg)) Log("config: 存盘失败");
                    inq::ApplyOptions(g_cfg);
                    g_refreshNames = true;
                    Chat(i18n::Fmt(i18n::Tr("Better-IN-Q：老配置里的套装名字已换成编号（%d 处）；没对上的已留空，请到套装页重选",
                                            "Better-IN-Q: %d old name-based loadout references converted to numbers; unmatched ones are now empty, please re-pick them",
                                            "Better-IN-Q：古い設定のセット名を %d 件番号に変換しました。一致しなかったものは空にしたので選び直してください"),
                                      changed));
                }
            }
            if (now - lastTick >= 100) {
                lastTick = now;
                LARGE_INTEGER t0, t1;
                ::QueryPerformanceCounter(&t0);
                inq::Tick(g_cfg);
                ::QueryPerformanceCounter(&t1);
                const double us = QpcUs((unsigned long long)(t1.QuadPart - t0.QuadPart));
                tickSum += us;
                if (us > tickMax) tickMax = us;
                ++tickN;
                m.status = LastChat(6000);
            }
        }
        FlushChat();
        ::Sleep(16);
    }
    return 0;
}

} // namespace

extern "C" __declspec(dllexport) BOOL BetterINQ_IsInstalled() { return TRUE; }

// 崩溃时留个记录：异常码、出错地址（哪个模块 + 偏移）、线程、以及当时在哪个钩子里。
// 只写日志，不吞异常（EXCEPTION_CONTINUE_SEARCH），游戏该崩还是崩 —— 我们要的是线索。
//
// 但「记什么」要挑：这是进程里第一个异常处理器，此刻别的线程可能正拿着 loader lock
// 或者堆锁，我们一写文件就可能把它等死 —— 表现就是「游戏不退」。所以：
//   · 退出路径（窗口已经销毁）一律不记；
//   · CLR / C++ 抛异常这类正常控制流不记（SharpPluginLoader 一次启动能抛近百条，
//     记下来既刷满日志、又正好把危险窗口撑大）；
//   · 同一个异常已经被记过一遍就不再记（递归/风暴只留第一条）。
LONG WINAPI OnCrash(EXCEPTION_POINTERS* info)
{
    if (g_teardown || g_exiting) return EXCEPTION_CONTINUE_SEARCH;
    if (!info || !info->ExceptionRecord) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (!crashfilter::Recordable(code)) return EXCEPTION_CONTINUE_SEARCH;
    if (g_inCrash.exchange(1)) return EXCEPTION_CONTINUE_SEARCH;   // 已经在记了/记录过程本身出错
    void* addr = info->ExceptionRecord->ExceptionAddress;
    wchar_t path[MAX_PATH] = L"?";
    HMODULE mod = nullptr;
    if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(addr), &mod) &&
        mod)
        ::GetModuleFileNameW(mod, path, MAX_PATH);
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\') base = p + 1;
    const unsigned long long off =
        mod ? (unsigned long long)(reinterpret_cast<const char*>(addr) - reinterpret_cast<const char*>(mod)) : 0;
    Log("crash: 异常 0x%08X 在 %p（%ls+0x%llX），线程 %lu，stage=%s", code, addr, base, off, ::GetCurrentThreadId(),
        overlay::Stage());
    // 栈帧：栈溢出时未必抓得到，但抓到了就能看出是不是同一段代码在递归
    void* frames[12] = {};
    const USHORT n = ::RtlCaptureStackBackTrace(1, 12, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        wchar_t fpath[MAX_PATH] = L"?";
        HMODULE fm = nullptr;
        if (::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCWSTR>(frames[i]), &fm) &&
            fm)
            ::GetModuleFileNameW(fm, fpath, MAX_PATH);
        const wchar_t* fbase = fpath;
        for (const wchar_t* p = fpath; *p; ++p)
            if (*p == L'\\') fbase = p + 1;
        const unsigned long long foff =
            fm ? (unsigned long long)(reinterpret_cast<const char*>(frames[i]) - reinterpret_cast<const char*>(fm)) : 0;
        Log("crash:   #%u %p（%ls+0x%llX）", (unsigned)i, frames[i], fbase, foff);
    }
    Log("crash: 同一个异常后续不再记录（只留第一条）");
    return EXCEPTION_CONTINUE_SEARCH;
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        ::DisableThreadLibraryCalls(module);
        HANDLE t = ::CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (t) ::CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        ::InterlockedExchange(&g_stop, 1);
        // 进程退出阶段一律不干活：不写日志（CreateFile 可能被别的线程的 loader lock 挡住）、
        // 不碰 D3D、不还原窗口过程。正常收尾已经在窗口销毁时做完了（见 Worker）。
        // 这里只把崩溃记录关掉，免得退出时的异常风暴把日志拖住。
        g_teardown = 1;
    }
    return TRUE;
}
