// 面板的手柄导航：模拟十字键 / A / B，看页面和焦点是不是按设计走。离屏渲染，不开窗口。
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <vector>
#include <xinput.h>

#include "../src/config.h"
#include "../src/gamedata.h"
#include "../src/input.h"
#include "../src/ui.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx11.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) std::printf("  ok   " __VA_ARGS__); \
    else { std::printf("  FAIL " __VA_ARGS__); ++g_fail; } std::printf("\n"); } while (0)

static ID3D11Device* dev = nullptr;
static ID3D11DeviceContext* ctx = nullptr;
static ID3D11RenderTargetView* rtv = nullptr;
static ui::Model m;
static std::vector<int> g_sideTrace;   // 每帧焦点在侧栏第几格

static void Frame()
{
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    ui::Draw(m, io.DisplaySize.x, io.DisplaySize.y);
    ImGui::Render();
    ctx->OMSetRenderTargets(1, &rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_sideTrace.push_back(ui::DebugSidebarFocusIndex());
}

static float PanelH() { return ui::DebugFrameHeight(); }

// 录键：手柄 / 键盘按住一组，再全部松开
static void Hold(unsigned pad, std::vector<int> keys = {})
{
    m.padHeld = pad;
    m.keysHeld = keys;
    for (int i = 0; i < 3; ++i) Frame();
}

static void Press(ImGuiKey k)
{
    ImGui::GetIO().AddKeyEvent(k, true);
    Frame();
    ImGui::GetIO().AddKeyEvent(k, false);
    Frame();
    Frame();
}

static const char* PageName(int p)
{
    static const char* n[] = {"主页", "换任务", "套装", "套装编辑", "猫饭", "猫饭编辑", "设置"};
    return p >= 0 && p < 7 ? n[p] : "?";
}

int main()
{
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, nullptr, &ctx);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = 1920; td.Height = 1080; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D* rt = nullptr;
    dev->CreateTexture2D(&td, nullptr, &rt);
    dev->CreateRenderTargetView(rt, nullptr, &rtv);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1920, 1080);
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    ImGui_ImplDX11_Init(dev, ctx);
    ui::Init(1.0f);

    Config cfg;
    std::string err;
    LoadConfig(L"config\\BetterINQ.ini", cfg, err);
    m.cfg = &cfg;
    m.ready = true;

    Frame();
    ui::OnOpen(m);
    for (int i = 0; i < 4; ++i) Frame();
    CHECK(ui::DebugPage() == 0 && !ui::DebugSidebarFocused(), "打开面板：在主页，焦点在内容区（%s）", PageName(ui::DebugPage()));

    g_sideTrace.clear();
    Press(ImGuiKey_GamepadDpadLeft);
    CHECK(ui::DebugSidebarFocused() && ui::DebugPage() == 0, "按左：进侧栏，还停在「任务」这一格，没乱切页（%s）", PageName(ui::DebugPage()));
    {
        bool clean = true;
        for (int i : g_sideTrace) clean &= (i == -1 || i == 0);
        CHECK(clean, "按左的过程中焦点没在别的格上停过（之前会先闪到「猫饭」）");
    }

    Press(ImGuiKey_GamepadDpadDown);
    CHECK(ui::DebugPage() == 2 && ui::DebugSidebarFocused(), "按下：切到「套装」，焦点留在侧栏（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadDpadDown);
    CHECK(ui::DebugPage() == 4, "再按下：切到「猫饭」（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadDpadUp);
    CHECK(ui::DebugPage() == 2, "按上：回到「套装」（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadDpadRight);
    CHECK(!ui::DebugSidebarFocused() && ui::DebugPage() == 2, "按右：回到内容区（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadFaceDown);
    CHECK(ui::DebugPage() == 3, "按 A：打开第一套的编辑页（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadFaceRight);
    CHECK(ui::DebugPage() == 2, "按 B：回到套装列表（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadFaceRight);
    CHECK(ui::DebugPage() == 0 && !m.reqClose, "再按 B：回主页（%s）", PageName(ui::DebugPage()));

    Press(ImGuiKey_GamepadFaceRight);
    CHECK(m.reqClose, "主页按 B：请求关面板");
    m.reqClose = false;

    // 只重置任务：在「重开」下面那排圆按钮之后，按 A 只提「只重置」，不会顺手接任务
    ui::OnOpen(m);
    for (int i = 0; i < 4; ++i) Frame();
    Press(ImGuiKey_GamepadDpadDown);    // 重开 → 中间「换套装」
    Press(ImGuiKey_GamepadDpadDown);    // → 「只重置任务」
    Press(ImGuiKey_GamepadFaceDown);
    CHECK(m.reqResetOnly && m.reqClose && !m.reqRestart, "主页：往下两次、A → 只重置任务（不重开）");
    m.reqResetOnly = false;
    m.reqClose = false;

    // 侧栏里按 A：切页并把焦点送进内容区
    ui::OnOpen(m);
    for (int i = 0; i < 4; ++i) Frame();
    Press(ImGuiKey_GamepadDpadLeft);
    Press(ImGuiKey_GamepadDpadDown);
    Press(ImGuiKey_GamepadDpadDown);
    Press(ImGuiKey_GamepadDpadDown);
    CHECK(ui::DebugPage() == 6, "侧栏里一路往下到「设置」（%s）", PageName(ui::DebugPage()));
    Press(ImGuiKey_GamepadFaceDown);
    CHECK(ui::DebugPage() == 6 && !ui::DebugSidebarFocused(), "侧栏里按 A：焦点进内容区（%s）", PageName(ui::DebugPage()));

    // 第 4 条 bug 的回归：主页换任务。「米拉」套装自己带着任务（66845），重开时套装的任务说了算，
    // 所以主页换任务要改套装里的那个，主页显示的也要跟着变
    {
        const int globalBefore = cfg.questId;
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadDpadDown);    // 重开 → 下面一排圆按钮（落在中间那个）
        Press(ImGuiKey_GamepadDpadLeft);    // → 换任务
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugPage() == 1, "主页：下、左、A 进「换任务」（%s）", PageName(ui::DebugPage()));
        Press(ImGuiKey_GamepadDpadUp);      // 66845 上面一个是 66846
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugPage() == 0, "选中后回到主页（%s）", PageName(ui::DebugPage()));
        CHECK(cfg.FindSet("米拉") && cfg.FindSet("米拉")->quest == 66846, "改的是「米拉」套装里的任务（现在 %d）",
              cfg.FindSet("米拉") ? cfg.FindSet("米拉")->quest : -1);
        CHECK(cfg.questId == globalBefore, "全局任务没被误改（%d）", cfg.questId);
        CHECK(m.reqSave, "提了存盘请求");
        m.reqSave = false;
    }

    // 往左：内容区里左边还有控件就照常移动；没有了才进侧栏，而且落在当前页那一格
    ui::OnOpen(m);
    for (int i = 0; i < 4; ++i) Frame();
    Press(ImGuiKey_GamepadDpadDown);    // 重开 → 中间的「换套装」
    Press(ImGuiKey_GamepadDpadRight);   // → 「换猫饭」
    Press(ImGuiKey_GamepadDpadLeft);
    CHECK(!ui::DebugSidebarFocused(), "主页「换猫饭」往左：留在内容区（到「换套装」）");
    Press(ImGuiKey_GamepadDpadLeft);
    Press(ImGuiKey_GamepadDpadLeft);
    CHECK(ui::DebugSidebarFocused() && ui::DebugSidebarFocusIndex() == 0 && ui::DebugPage() == 0,
          "再往左两次：进侧栏「任务」（第 %d 格）", ui::DebugSidebarFocusIndex());
    ui::DebugGo(6, 0);
    for (int i = 0; i < 4; ++i) Frame();
    for (int i = 0; i < 6; ++i) Press(ImGuiKey_GamepadDpadDown);
    g_sideTrace.clear();
    Press(ImGuiKey_GamepadDpadLeft);
    {
        bool clean = true;
        for (int i : g_sideTrace) clean &= (i == -1 || i == 3);
        CHECK(clean && ui::DebugSidebarFocusIndex() == 3 && ui::DebugPage() == 6,
              "设置页中间往左：直接到侧栏「设置」（第 %d 格，%s）", ui::DebugSidebarFocusIndex(), PageName(ui::DebugPage()));
    }

    // 换页时面板高度是渐变的
    {
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(6, 0);
        for (int i = 0; i < 40; ++i) Frame();
        const float tall = PanelH();
        ui::DebugGo(0, 0);
        Frame();
        const float h1 = PanelH();
        for (int i = 0; i < 40; ++i) Frame();
        const float shortH = PanelH();
        CHECK(tall > shortH + 50 && h1 < tall && h1 > shortH + 20,
              "设置 → 主页：高度 %.0f → %.0f（第一帧 %.0f，中间有过渡）", tall, shortH, h1);
    }

    // 录快捷键
    {
        Config& c = cfg;
        c.hkPanel = "View+Menu | Insert";
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugStartCapture(m, 1, true);
        Hold(XINPUT_GAMEPAD_A);               // 按 A 开始录的那一下还按着：不算
        CHECK(m.capturing, "开始录手柄：主循环知道在录（%d）", (int)m.capturing);
        Hold(0);
        Hold(XINPUT_GAMEPAD_LEFT_SHOULDER);
        Hold(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER);
        Hold(XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER | kPadLT);
        Hold(0);
        CHECK(c.hkPanel == "LB+RB+LT | Insert" && !m.capturing && m.reqSave,
              "按 LB、RB、LT 再松开：手柄那组换成 LB+RB+LT，键盘那组不动（%s）", c.hkPanel.c_str());
        m.reqSave = false;

        ui::DebugStartCapture(m, 1, true);
        Hold(0);
        Hold(XINPUT_GAMEPAD_Y);
        Hold(0);
        CHECK(m.capturing && c.hkPanel == "LB+RB+LT | Insert" && !ui::DebugCaptureWhy().empty(),
              "只按一个 Y：不收，说明原因、接着录（%s）", ui::DebugCaptureWhy().c_str());
        // 录手柄时按 B：是组合里的键，不能被当成「返回」
        ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceRight, true);
        Hold(XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_Y);
        ImGui::GetIO().AddKeyEvent(ImGuiKey_GamepadFaceRight, false);
        Hold(0);
        CHECK(c.hkPanel == "B+Y | Insert" && ui::DebugPage() == 6, "B+Y 也能录，按 B 没把页面退回去（%s，%s）",
              c.hkPanel.c_str(), PageName(ui::DebugPage()));

        ui::DebugStartCapture(m, 1, false);
        Hold(0);
        Hold(0, {'R'});
        Hold(0);
        CHECK(m.capturing && c.hkPanel == "B+Y | Insert", "键盘单按 R：不收（%s）", ui::DebugCaptureWhy().c_str());
        Hold(0, {VK_CONTROL});
        Hold(0, {VK_CONTROL, 'R'});
        Hold(0);
        CHECK(c.hkPanel == "B+Y | Ctrl+R" && !m.capturing, "Ctrl+R：键盘那组换掉（%s）", c.hkPanel.c_str());

        // 清除：录键盘时单按 Delete，录手柄时单按 X；录手柄时按键盘 Delete 也算清除
        ui::DebugStartCapture(m, 1, false);
        Hold(0);
        Hold(0, {VK_DELETE});
        Hold(0);
        CHECK(c.hkPanel == "B+Y" && !m.capturing, "录键盘时单按 Delete：键盘那组清掉，不是把 Delete 设成快捷键（%s）",
              c.hkPanel.c_str());
        ui::DebugStartCapture(m, 1, true);
        Hold(0);
        Hold(XINPUT_GAMEPAD_X);
        Hold(0);
        CHECK(c.hkPanel.empty() && !m.capturing, "录手柄时单按 X：手柄那组清掉（「%s」）", c.hkPanel.c_str());
        c.hkPanel = "View+Menu | Insert";
        ui::DebugStartCapture(m, 1, true);
        Hold(0);
        Press(ImGuiKey_Delete);
        CHECK(c.hkPanel == "Insert" && !m.capturing, "录手柄时按键盘 Delete：手柄那组清掉（%s）", c.hkPanel.c_str());
        ui::DebugStartCapture(m, 1, false);
        Hold(0);
        Hold(0, {VK_CONTROL});
        Hold(0, {VK_CONTROL, VK_DELETE});
        Hold(0);
        CHECK(c.hkPanel == "Ctrl+Delete", "Ctrl+Delete 照样能录（%s）", c.hkPanel.c_str());
        c.hkPanel = "View+Menu | Insert";

        ui::DebugStartCapture(m, 0, false);
        Hold(0);
        const std::string before = c.hkRestart;
        Press(ImGuiKey_Escape);
        CHECK(!m.capturing && c.hkRestart == before && ui::DebugPage() == 6, "Esc：取消录制，快捷键不变、也没退页（%s）",
              PageName(ui::DebugPage()));
        Press(ImGuiKey_Escape);
        CHECK(ui::DebugPage() == 0, "不在录的时候 Esc 照常返回（%s）", PageName(ui::DebugPage()));
        m.reqSave = false;
    }

    // 套装 / 猫饭：列表里只有套装 / 猫饭本身，点进去按「确认」才选这一套
    {
        const int setBefore = cfg.activeSet;
        const int foodBefore = cfg.food;
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        cfg.activeSet = 0;
        ui::DebugGo(2, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadDpadUp);           // 焦点在第一套上，上面没有别的行了
        Press(ImGuiKey_GamepadFaceDown);         // 没选中的按 A：先选中，不进编辑页
        CHECK(ui::DebugPage() == 2 && cfg.activeSet == cfg.sets[0].id,
              "套装列表按 A：没选中的直接选中（%d，%s）", cfg.activeSet, PageName(ui::DebugPage()));
        Press(ImGuiKey_GamepadFaceDown);         // 已经在用的那一套再按 A：进编辑页
        CHECK(ui::DebugPage() == 3, "再按一次 A：进编辑页（%s）", PageName(ui::DebugPage()));
        for (int i = 0; i < 5; ++i) Press(ImGuiKey_GamepadDpadDown);   // 装备 道具 联动 猫饭 任务 → 确认
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugPage() == 2 && cfg.activeSet == setBefore, "编辑页按「确认」：选上这套并回列表（%d，%s）",
              cfg.activeSet, PageName(ui::DebugPage()));

        // Y：不用进编辑页，直接套用 / 直接选
        cfg.activeSet = 0;
        ui::DebugGo(2, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadFaceUp);
        CHECK(ui::DebugPage() == 2 && cfg.activeSet == cfg.sets[0].id, "套装列表按 Y 直接套用第一套（%d/%d）",
              cfg.activeSet, cfg.sets.empty() ? -1 : cfg.sets[0].id);

        // 猫饭列表的「选中」只在套装没指定猫饭时才生效，先把套装的选择清掉
        cfg.activeSet = 0;
        cfg.food = 0;
        ui::DebugGo(4, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadDpadUp);
        Press(ImGuiKey_GamepadFaceDown);         // 没选中的按 A：先选中
        CHECK(ui::DebugPage() == 4 && !cfg.foods.empty() && cfg.food == cfg.foods.begin()->first,
              "猫饭列表按 A：没选中的直接选中（%d，%s）", cfg.food, PageName(ui::DebugPage()));
        Press(ImGuiKey_GamepadFaceDown);         // 再按一次：进编辑页
        CHECK(ui::DebugPage() == 5, "再按一次 A：进编辑页（%s）", PageName(ui::DebugPage()));
        for (int i = 0; i < 8; ++i) Press(ImGuiKey_GamepadDpadDown);   // 五排档位、三个技能 → 确认
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugPage() == 4 && cfg.food == foodBefore, "编辑页按「确认」：每次进任务都吃这份（%d，%s）", cfg.food,
              PageName(ui::DebugPage()));

        // 猫饭列表的「选中」只在套装没指定猫饭时才生效，先把套装的选择清掉
        cfg.activeSet = 0;
        cfg.food = 0;
        ui::DebugGo(4, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadFaceUp);
        CHECK(ui::DebugPage() == 4 && !cfg.foods.empty() && cfg.food == cfg.foods.begin()->first,
              "猫饭列表按 Y 直接选第一份（%d）", cfg.food);

        // 套装自带猫饭时：猫饭列表的选中不生效（重开用套装的猫饭），按 A 只能进编辑页
        {
            LoadoutSet* first = cfg.sets.empty() ? nullptr : &cfg.sets[0];
            const int setFoodBefore = first ? first->food : 0;
            if (first) first->food = 3;
            cfg.foods[3] = FoodPreset();
            cfg.activeSet = first ? first->id : 0;
            cfg.food = 0;
            ui::DebugGo(4, 0);
            for (int i = 0; i < 4; ++i) Frame();
            Press(ImGuiKey_GamepadFaceUp);              // Y：不该改全局选择
            CHECK(cfg.food == 0, "套装自带猫饭时按 Y 不改全局选择（%d）", cfg.food);
            Press(ImGuiKey_GamepadFaceDown);            // A：直接进编辑页，而不是选中
            CHECK(ui::DebugPage() == 5, "套装自带猫饭时按 A 进编辑页（%s）", PageName(ui::DebugPage()));
            if (first) first->food = setFoodBefore;
            cfg.activeSet = 0;
            cfg.foods.erase(3);
            ui::DebugGo(4, 0);
        }

        // 套装编辑页的下拉第一项也是「—」：选它 = 这一项不换
        LoadoutSet* mila = const_cast<LoadoutSet*>(cfg.FindSet("米拉"));
        const std::string equipBefore = mila ? mila->equip : std::string();
        ui::DebugGo(3, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadFaceDown);          // 打开装备套装下拉，焦点在当前项（测试里没有游戏套装，就是「—」）
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(mila && mila->equip.empty() && ui::DebugPage() == 3, "装备套装下拉选「—」：清空（原来「%s」）", equipBefore.c_str());
        if (mila) mila->equip = equipBefore;
        m.reqSave = false;
    }

    // 下拉选项里按 B：只关下拉，不退页
    {
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(3, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadFaceDown);          // 焦点默认在「装备套装」，A 打开下拉
        CHECK(ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId), "套装编辑页按 A：打开装备套装的下拉");
        Press(ImGuiKey_GamepadFaceRight);
        CHECK(!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && ui::DebugPage() == 3,
              "下拉里按 B：关掉下拉，还在套装编辑页（%s）", PageName(ui::DebugPage()));
        Press(ImGuiKey_GamepadFaceRight);
        CHECK(ui::DebugPage() == 2, "再按 B：才回套装列表（%s）", PageName(ui::DebugPage()));
    }

    // 搜任务：拼音、首字母、混着打、多音字、ID 开头
    {
        const QuestInfo* q = gamedata::FindQuest(66845);   // 传说中的黑龙
        const QuestInfo* w = gamedata::FindQuest(66846);   // 最后的白骑士
        CHECK(q && w, "找到两个任务");
        if (q && w) {
            const char* yes[] = {"heilong", "hl", "HeiLong", "hlong", "heil", "chuanshuo", "cszdhl", "zhongdehei", "zhuanshuo",
                                 "chuan shuo", "668", "66845", "黑龙"};
            for (const char* n : yes) CHECK(gamedata::QuestMatches(*q, n), "「传说中的黑龙」能用「%s」搜到", n);
            const char* no[] = {"bailong", "hlz", "66846", "白", "xyz"};
            for (const char* n : no) CHECK(!gamedata::QuestMatches(*q, n), "「传说中的黑龙」用「%s」搜不到", n);
            CHECK(gamedata::QuestMatches(*w, "bqs") && gamedata::QuestMatches(*w, "zuihou"), "「最后的白骑士」：bqs、zuihou");
        }
    }

    // 排序：按 L3 进移动模式，上下键换位置，再按 L3 放下
    {
        LoadoutSet two;
        two.id = cfg.NextSetId();
        two.name = "第二套";
        cfg.sets.push_back(two);
        cfg.activeSet = cfg.sets[0].id;   // 焦点默认落在「已经在用」的那一套上，按 A 才是进编辑
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(2, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadL3);                // 焦点在第一行：把它放进移动模式
        Press(ImGuiKey_GamepadDpadDown);          // 往下挪一格
        CHECK(cfg.sets.size() == 2 && cfg.sets[0].name == "第二套" && cfg.sets[1].name == "米拉",
              "L3 + 下：套装换到第 2 位（%s / %s）", cfg.sets[0].name.c_str(), cfg.sets[1].name.c_str());
        Press(ImGuiKey_GamepadDpadUp);
        CHECK(cfg.sets[0].name == "米拉", "L3 + 上：换回第 1 位");
        Press(ImGuiKey_GamepadL3);                // 放下
        Press(ImGuiKey_GamepadFaceDown);
        CHECK(ui::DebugPage() == 3, "放下后按 A：进的是刚移动的那套（%s）", PageName(ui::DebugPage()));
    }

    // 移动模式的三条行为：长按要连续换位、B 是放下不是返回、换页后状态清空
    {
        cfg.sets.clear();
        for (int i = 0; i < 3; ++i) {
            LoadoutSet s;
            s.id = cfg.NextSetId();
            s.name = std::string("套") + std::to_string(i + 1);
            cfg.sets.push_back(s);
        }
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(2, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadL3);
        CHECK(ui::DebugMoveActive() && ui::DebugMoveId() == cfg.sets[0].id, "L3：进入移动模式");

        // 按住不放：ImGui 的按键重复要过 KeyRepeatDelay 才开始，所以按住 40 帧
        const std::string first = cfg.sets[0].name;
        ImGui::GetIO().AddKeyEvent(ImGuiKey_DownArrow, true);
        for (int i = 0; i < 40; ++i) Frame();
        ImGui::GetIO().AddKeyEvent(ImGuiKey_DownArrow, false);
        Frame();
        CHECK(cfg.sets.size() == 3 && cfg.sets[2].name == first,
              "按住下键不放：连续换位到最后一格（%s 在 %s）", first.c_str(), cfg.sets[2].name.c_str());

        const int pageBefore = ui::DebugPage();
        Press(ImGuiKey_GamepadFaceRight);   // B
        CHECK(!ui::DebugMoveActive(), "移动中按 B：只是放下，不留在移动模式");
        CHECK(ui::DebugPage() == pageBefore, "移动中按 B：不返回上一页（还在%s）", PageName(ui::DebugPage()));

        // 离开再回来：状态必须已经清掉，而且 L3 还能重新进
        ui::DebugGo(0, 0);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(2, 0);
        for (int i = 0; i < 4; ++i) Frame();
        CHECK(!ui::DebugMoveActive() && ui::DebugMoveId() == 0, "换了页再回来：移动模式已经清掉");
        Press(ImGuiKey_Escape);
        CHECK(ui::DebugPage() == 0, "移动模式清掉后，Esc 又能正常返回（%s）", PageName(ui::DebugPage()));
    }


    // 猫饭排序：只改面板里的显示顺序，编号不动
    {
        cfg.foods[5] = FoodPreset();
        cfg.foodOrder = {1, 5};
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(4, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadL3);
        Press(ImGuiKey_GamepadDpadDown);
        const std::vector<int> ids = cfg.FoodIds();
        CHECK(ids.size() == 2 && ids[0] == 5 && ids[1] == 1,
              "L3 + 下：猫饭显示顺序换过来（%d / %d，编号没变）", ids.empty() ? 0 : ids[0], ids.size() > 1 ? ids[1] : 0);
        Press(ImGuiKey_GamepadL3);
    }

    // 名字长度按「字」算，不按字节：上限 50，且不能切成半个字符
    {
        std::string wide;
        for (int i = 0; i < 60; ++i) wide += "字";          // 60 个字，每个 3 字节
        const std::string got = ui::DebugClampName(wide.c_str());
        CHECK(got.size() == 150, "60 个汉字只留前 50 个（%d 字节）", (int)got.size());
        CHECK(got.size() % 3 == 0, "截断没切坏多字节字符");
        CHECK(ui::DebugClampName("abc") == "abc", "短名字原样保留");
        std::string ascii(60, 'a');
        CHECK(ui::DebugClampName(ascii.c_str()).size() == 50, "纯英文最多 50 个字符");
        CHECK(ui::DebugClampName("abc字").size() == 6, "中英混写也在同一个上限内");
    }

    // 移动模式滚列表：往下挪出可视区要滚，往上挪回来要滚回去
    {
        cfg.sets.clear();
        for (int i = 0; i < 12; ++i) {
            LoadoutSet s;
            s.id = cfg.NextSetId();
            s.name = std::string("滚") + std::to_string(i + 1);
            cfg.sets.push_back(s);
        }
        cfg.activeSet = 0;
        ui::OnOpen(m);
        for (int i = 0; i < 4; ++i) Frame();
        ui::DebugGo(2, 0);
        for (int i = 0; i < 4; ++i) Frame();
        Press(ImGuiKey_GamepadL3);              // 进移动模式（焦点在第 1 行）
        Frame();
        const float base = ui::DebugListScroll();   // 内容区可能带着上一页的滚动位置，用相对量判断
        for (int i = 0; i < 10; ++i) {          // 一路往下挪到出界
            Press(ImGuiKey_GamepadDpadDown);
            Frame();
        }
        Frame();                                // SetScrollY 设的是目标值，下一帧才落到 Scroll.y
        const float scrolledDown = ui::DebugListScroll();
        CHECK(scrolledDown > base, "往下挪出可视区：列表跟着滚（%.1f → %.1f）", base, scrolledDown);
        for (int i = 0; i < 10; ++i) {          // 再一路挪回顶上
            Press(ImGuiKey_GamepadDpadUp);
            Frame();
        }
        Frame();
        const float scrolledBack = ui::DebugListScroll();
        CHECK(scrolledBack < scrolledDown, "往上挪回来：列表往回滚（%.1f → %.1f）", scrolledDown, scrolledBack);
        CHECK(scrolledBack == base, "挪回第一行：滚动位置回到进移动模式时（%.1f → %.1f）", base, scrolledBack);
        Press(ImGuiKey_GamepadFaceRight);       // B 放下，别把状态带给后面的用例
    }

    // 主页的「重开」按 A
    ui::OnOpen(m);
    for (int i = 0; i < 4; ++i) Frame();
    Press(ImGuiKey_GamepadFaceDown);
    CHECK(m.reqRestart && m.reqClose, "主页直接按 A：重开并关面板");

    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
    std::printf("\n%s（%d 项失败）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
