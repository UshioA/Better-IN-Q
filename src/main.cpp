// ============================================================================
//  Better-IN-Q —— MHW:IB 的快速重开任务，做成游戏插件（Stracker's Loader 加载）
//
//  核心逻辑移植自 INQ.CT：原作者 年糕狗三世（B 站 BV1yz2PBNEWn），作者允许
//  修 bug / 加功能后重新分发；CT 的中文化与修改由 QCanon 完成。
// ============================================================================
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "cursorfilter.h"
#include "hdr.h"
#include "dinfilter.h"
#include "i18n.h"
#include "inqimport.h"
#include "hook.h"
#include "inq.h"
#include "input.h"
#include "kbdfilter.h"
#include "log.h"
#include "overlay.h"
#include "padfilter.h"
#include "ui.h"

namespace {

HMODULE g_module = nullptr;
volatile LONG g_stop = 0;
const char* const kVersion = "0.7";

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
        // 手柄不能开面板（和 SharpPluginLoader 一样只用键盘）：ini 里写了手柄组合也丢掉。
        // 这样就不需要「先把按键扣住、看会不会凑成组合键」那一层，单键永远直通。
        {
            const std::size_t before = panel.alts.size();
            panel.alts.erase(std::remove_if(panel.alts.begin(), panel.alts.end(),
                                            [](const Combo& c) { return c.IsPad() && c.keys.empty(); }),
                             panel.alts.end());
            if (panel.alts.size() != before) Log("hotkey Panel: 手柄组合不能用，只留键盘");
        }
        resetOnly = ParseHotkey(c.hkResetOnly);
        LogHotkey("Restart", c.hkRestart, restart);
        LogHotkey("Panel", c.hkPanel, panel);
        LogHotkey("ResetOnly", c.hkResetOnly, resetOnly);
    }
};

// [Debug] Disable：排查输入延迟时按段关掉。all = 前四个（面板还留着，否则没法改 ini）
struct Disabled {
    bool pad = false, cursor = false, kbd = false, din = false, overlay = false;
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

DWORD WINAPI Worker(LPVOID)
{
    while (!::GetModuleHandleW(L"MonsterHunterWorld.exe") && !g_stop) ::Sleep(200);
    if (g_stop) return 0;

    const std::wstring dir = ModuleDir() + L"BetterINQ\\";
    const std::wstring cfgPath = dir + L"BetterINQ.ini";
    ::CreateDirectoryW(dir.c_str(), nullptr);
    LogInit(dir + L"BetterINQ.log");
    Log("Better-IN-Q %s starting", kVersion);

    Hotkeys hk;
    std::wstring autoSrc;      // 插件目录旁边有没有原版 config.txt（第一次运行自动导入用）
    {
        std::lock_guard<std::mutex> lk(g_mx);
        std::string err;
        if (!LoadConfig(cfgPath, g_cfg, err)) Log("config: %s", err.c_str());
        i18n::Set(i18n::FromString(g_cfg.language));   // 越早越好：后面的日志和提示都按它来
        hdr::Configure(g_cfg.hdr, g_cfg.sdrNits);      // HDR 交换链上的面板颜色修正
        Log("config: QuestID=%d Reset=%d AcceptOnly=%d Wyvern=%d ActiveSet='%s' Food=%d 全套%zu 猫饭%zu",
            g_cfg.questId, g_cfg.reset, g_cfg.acceptOnly, g_cfg.wyvern, g_cfg.activeSet.c_str(), g_cfg.food,
            g_cfg.sets.size(), g_cfg.foods.size());
        g_model.cfg = &g_cfg;
        g_model.version = kVersion;
        autoSrc = inqimport::Resolve("", cfgPath);   // 旁边有就顺手带上
        if (!autoSrc.empty()) Log("import: 旁边有原版 config.txt：%s", inqimport::Narrow(autoSrc).c_str());
        hk.Parse(g_cfg);
    }
    // 第一次装的时候：旁边有原版 IN-Q 的 config.txt 就把全套直接搬过来，省得手写
    if (g_cfg.sets.empty() && !autoSrc.empty()) {
        std::string ierr, inotes;
        const int n = inqimport::Import(autoSrc, g_cfg, ierr, &inotes);
        if (n > 0) {
            if (!SaveConfig(cfgPath, g_cfg)) Log("config: 存盘失败");
            Log("import: 从原版 IN-Q 导入了 %d 个全套（%s）", n, inotes.c_str());
        } else {
            Log("import: %s", ierr.c_str());
        }
    }

    const Disabled off = ParseDisabled(g_cfg.debugDisable);
    if (off.pad || off.cursor || off.kbd || off.din || off.overlay)
        Log("debug: 关掉了过滤器 pad=%d cursor=%d kbd=%d din=%d overlay=%d", off.pad, off.cursor, off.kbd,
            off.din, off.overlay);

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
    std::string err;
    const bool panelOk = off.overlay ? false : overlay::Install(cb, err);
    if (off.overlay) Log("debug: overlay 关掉了，没有面板（快捷键重开照常）");
    else if (!panelOk) Log("overlay: %s", err.c_str());

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
    ULONGLONG lastTick = 0, dirtyAt = 0;
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
    while (!g_stop) {
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
        if (in.Fired(hk.panel) && !capturing) {
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
            if (!open && restartKey) { Log("hotkey: restart"); inq::RequestRestart(g_cfg); }
            if (!open && resetKey) { Log("hotkey: reset only"); inq::RequestResetOnly(); }

            ui::Model& m = g_model;
            if (m.reqClose) { m.reqClose = false; SetPanel(false); }
            if (m.reqRestart) { m.reqRestart = false; Log("panel: restart"); inq::RequestRestart(g_cfg); }
            if (m.reqResetOnly) { m.reqResetOnly = false; Log("panel: reset only"); inq::RequestResetOnly(); }
            if (m.reqImportInq) {        // 设置页的文件选择框里挑好了 config.txt
                m.reqImportInq = false;
                const std::wstring src = m.inqPath;      // 选择框里挑好的
                m.inqPath.clear();
                std::string ierr, inotes;
                const int n = src.empty() ? -1 : inqimport::Import(src, g_cfg, ierr, &inotes);
                if (n > 0) {
                    if (!SaveConfig(cfgPath, g_cfg)) Log("config: 存盘失败");
                    Log("import: %d 个全套（%s）", n, inotes.c_str());
                    Chat(i18n::Fmt(i18n::Tr("Better-IN-Q：从原版 IN-Q 导入了 %d 个全套",
                                            "Better-IN-Q: imported %d loadouts from IN-Q",
                                            "Better-IN-Q：旧 IN-Q から %d 件取り込みました"), n));
                } else if (src.empty()) {
                    Chat(i18n::Tr("Better-IN-Q：没选文件", "Better-IN-Q: no file picked",
                                  "Better-IN-Q：ファイルが選ばれていません"));
                } else {
                    Chat(i18n::Fmt(i18n::Tr("Better-IN-Q：导入失败：%s", "Better-IN-Q: import failed: %s",
                                            "Better-IN-Q：取り込みに失敗しました：%s"), ierr.c_str()));
                }
            }
            if (m.reqSave) {             // 设置马上生效；写盘等停手 0.4 秒，免得打一个字存一次
                m.reqSave = false;
                inq::ApplyOptions(g_cfg);
                dirtyAt = now;
            }
            if (dirtyAt && now - dirtyAt > 400) {
                dirtyAt = 0;
                if (!SaveConfig(cfgPath, g_cfg)) Log("config: 存盘失败");
                hk.Parse(g_cfg);
            }
            if (g_refreshNames) {
                g_refreshNames = false;
                m.equipNames.clear();
                m.itemNames.clear();
                for (const auto& e : inq::ListEquipLoadouts()) m.equipNames.push_back({e.number, e.name});
                for (const auto& it : inq::ListItemLoadouts()) m.itemNames.push_back({it.number, it.name});
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

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = module;
        ::DisableThreadLibraryCalls(module);
        HANDLE t = ::CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (t) ::CloseHandle(t);
    } else if (reason == DLL_PROCESS_DETACH) {
        ::InterlockedExchange(&g_stop, 1);
    }
    return TRUE;
}
