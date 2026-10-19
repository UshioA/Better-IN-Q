#define IMGUI_DEFINE_MATH_OPERATORS
#include "ui.h"

#include "gamedata.h"
#include "i18n.h"
#include "log.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "input.h"

#include <windows.h>
#include <xinput.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ui {
namespace {

// ---------------------------------------------------------------------------
//  配色：怪猎菜单的深褐黑半透明底 + 金色。层次靠字号和亮度，不靠颜色种类。
// ---------------------------------------------------------------------------
const ImU32 kGold     = IM_COL32(226, 194, 122, 255);
const ImU32 kGoldHot  = IM_COL32(238, 211, 148, 255);
const ImU32 kGoldDown = IM_COL32(201, 164, 92, 255);
const ImU32 kInk      = IM_COL32(26, 20, 11, 255);     // 金底上的字
const ImU32 kTitle    = IM_COL32(246, 236, 210, 255);
const ImU32 kText     = IM_COL32(237, 230, 214, 255);
const ImU32 kMuted    = IM_COL32(150, 140, 118, 255);
const ImU32 kFaint    = IM_COL32(111, 102, 86, 255);
const ImU32 kWarn     = IM_COL32(232, 140, 110, 255);
const ImU32 kSoft     = IM_COL32(237, 230, 214, 26);   // 次要按钮底
const ImU32 kSoftHot  = IM_COL32(237, 230, 214, 44);
ImVec4 V(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

float S = 1.0f;
ImFont* fReg = nullptr;
ImFont* fBold = nullptr;
float szBody, szSmall, szTitle, szPage, szBtn;

enum Page { kHome, kQuests, kSets, kSetEdit, kFood, kFoodEdit, kSettings };
Page g_page = kHome;
float g_contentH[8] = {};   // 每页上一帧量出来的内容高度（0 = 还没量过）
// 进页后把焦点放到默认项。连续两帧都设：面板刚弹出那一帧 ImGui 自己的「窗口默认焦点」
// 会在帧尾把焦点挪走，第二帧再设一次才稳。
int g_focus = 2;
bool g_focusUsed = false;
int g_editSet = -1;         // 正在编辑的全套（cfg.sets 下标）
int g_editFood = 0;         // 正在编辑的猫饭编号
// 改名：列表里选中一项后按 X（手柄）或 F（键盘）弹输入框
enum RenameKind { kRenameNone, kRenameSet, kRenameFood };
RenameKind g_renameKind = kRenameNone;
int g_renameId = 0;              // 套装 = cfg.sets 下标；猫饭 = 编号
std::string g_mouseDownRow;      // 鼠标按下的那一行（Selectable 要松开才回 true，先记住是谁）
char g_renameBuf[96] = {};
bool g_renameFocus = false;      // 刚打开：这一帧把光标放进输入框
bool g_renameOpen = false;       // 上一帧弹着（用来发现被 B / Esc 关掉）
bool g_renameCloseReq = false;   // 预览用：下一帧关掉
bool g_debugRename = false;      // 预览用：下一帧弹出来
std::string g_renameErr;
bool g_renameCreate = false;      // 新建时直接弹出来起名（空 = 用默认名）
int g_afterRename = 0;            // 起完名去哪个编辑页：1 = 套装，2 = 猫饭
// 排序：按下左摇杆把选中的那一项放进「移动」模式，上下键换位置，再按一次放下
enum MoveKind { kMoveNone, kMoveSet, kMoveFood };
MoveKind g_moveKind = kMoveNone;
int g_moveId = 0;
float g_listScroll = 0;            // 套装/猫饭列表这一帧的滚动位置（测试用）
int g_focusId = 0;                // 放下后把焦点还给刚移动的那一项
const int kMaxNameChars = 50;      // 名字最多 50 个字（按字符算，中英文一样）
int g_questTarget = -1;     // 给谁选任务：-1 = 全局，>=0 = cfg.sets[i]
char g_search[96] = {};
int g_scrollToSel = 0;      // 还要把选中的任务滚到视野里几帧（新建的列表头一帧还不知道自己多长，滚不过去）
bool g_groupOpen[16] = {};
bool g_questInit = true;      // 刚进「换任务」：只展开当前任务所在的那一组
bool g_sideFocusedLast = false;

Page g_questFrom = kHome;   // 从哪一页进的「换任务」（返回时回那里）
const char* const kNone = "—";   // 「不额外选择」：套装编辑页下拉的第一项，以及没选时显示的值
bool g_popupLastFrame = false;   // 上一帧有没有开着的弹窗（下拉选项、删除确认）
ImGuiID g_sideCurId = 0;    // 侧栏里当前页那一格（十字键从内容区往左就落到它）
int g_sideFocusIdx = -1;    // 这一帧焦点在侧栏第几格（测试用）
float g_animHc = 0;         // 内容区高度的动画值（0 = 下一帧直接到位）
float g_frameH = 0;         // 这一帧画出来的外框高度（测试用）

// 按 B / 点返回箭头回到哪一页（主页面返回自己 = 没有上一级）
Page Back(Page p)
{
    switch (p) {
    case kQuests: return g_questFrom;
    case kSetEdit: return kSets;
    case kFoodEdit: return kFood;
    default: return p;
    }
}

// 侧栏上高亮哪一页
Page MainOf(Page p)
{
    switch (p) {
    case kQuests: return g_questFrom == kSetEdit ? kSets : kHome;
    case kSetEdit: return kSets;
    case kFoodEdit: return kFood;
    default: return p;
    }
}

// 主页显示的、重开时真正会进的任务：当前套装指定了任务就用套装的
int EffectiveQuest(const Config& c)
{
    if (const LoadoutSet* s = c.FindSetById(c.activeSet))
        if (s->quest) return s->quest;
    return c.questId;
}

void Go(Page p)
{
    // 换页就退出移动模式：不然在移动中按 B 回到上一页，再进来还停在「移动中」，
    // 而且 L3 会被当成「放下」，再也进不去移动（用户报过这个）
    g_moveKind = kMoveNone;
    g_moveId = 0;
    g_page = p;
    g_focus = 2;
    if (p == kQuests) { g_search[0] = 0; g_scrollToSel = 3; g_questInit = true; }
}

bool NavVisible() { return GImGui->NavCursorVisible; }

// 刚画完的这个控件拿默认焦点（进页后头两帧）。
// 不用 SetKeyboardFocusHere：它按 Tab 键的逻辑找「下一个可输入的控件」，会跳过按钮，
// 落到后面的滑条上还会把滑条切成输入框。
void FocusDefault()
{
    if (g_focus <= 0) return;
    --g_focus;
    g_focusUsed = true;
    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
    ImGui::SetNavCursorVisible(true);
    // ImGui 上下移动时会保持一个横向位置，没有就靠最左。进页时定成默认项的中线：
    // 从通栏的「重开」往下落到正下方中间那个，而不是最左边
    ImGuiWindow* w = ImGui::GetCurrentWindow();
    const float cx = GImGui->LastItemData.NavRect.GetCenter().x - w->DC.CursorStartPos.x;
    w->RootWindowForNav->NavPreferredScoringPosRel[0] = ImVec2(cx, FLT_MAX);
}

void Text(ImFont* f, float size, ImU32 col, const char* s, bool wrap = false)
{
    ImGui::PushFont(f, size);
    ImGui::PushStyleColor(ImGuiCol_Text, V(col));
    if (wrap) ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(s);
    if (wrap) ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::PopFont();
}

ImVec2 TextSize(ImFont* f, float size, const char* s) { return f->CalcTextSizeA(size, FLT_MAX, 0.0f, s); }

void CenterText(ImDrawList* dl, ImFont* f, float size, ImVec2 center, ImU32 col, const char* s)
{
    const ImVec2 ts = TextSize(f, size, s);
    dl->AddText(f, size, ImVec2(std::floor(center.x - ts.x * 0.5f), std::floor(center.y - ts.y * 0.5f)), col, s);
}

// 不画 ImGui 默认的方形焦点框，自定义控件自己画
void BeginCustom() { ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0, 0, 0, 0)); }
void EndCustom() { ImGui::PopStyleColor(); }
bool Focused() { return ImGui::IsItemFocused() && NavVisible(); }

// ---------------------------------------------------------------------------
//  线条图标（在 c 为中心、半边长 r 的方块里画）
// ---------------------------------------------------------------------------
enum Icon { kIcRefresh, kIcSearch, kIcShield, kIcBowl, kIcFlag, kIcGear, kIcBack, kIcPlus };

void DrawIcon(ImDrawList* dl, Icon ic, ImVec2 c, float r, ImU32 col)
{
    const float t = std::fmax(1.6f, r * 0.16f);
    auto P = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
    switch (ic) {
    case kIcRefresh: {
        dl->PathArcTo(c, r * 0.72f, -1.25f, 4.15f, 28);
        dl->PathStroke(col, 0, t);
        const ImVec2 tip = ImVec2(c.x + std::cos(-1.25f) * r * 0.72f, c.y + std::sin(-1.25f) * r * 0.72f);
        dl->AddTriangleFilled(ImVec2(tip.x - r * 0.05f, tip.y - r * 0.30f), ImVec2(tip.x + r * 0.33f, tip.y + r * 0.02f),
                              ImVec2(tip.x - r * 0.12f, tip.y + r * 0.26f), col);
        break;
    }
    case kIcSearch:
        dl->AddCircle(P(-0.15f, -0.15f), r * 0.52f, col, 24, t);
        dl->AddLine(P(0.24f, 0.24f), P(0.72f, 0.72f), col, t * 1.2f);
        break;
    case kIcShield: {
        const ImVec2 pts[] = {P(-0.62f, -0.70f), P(0.0f, -0.86f), P(0.62f, -0.70f), P(0.62f, 0.02f),
                              P(0.0f, 0.82f), P(-0.62f, 0.02f)};
        dl->AddPolyline(pts, 6, col, ImDrawFlags_Closed, t);
        break;
    }
    case kIcBowl:
        dl->AddLine(P(-0.80f, -0.05f), P(0.80f, -0.05f), col, t);
        dl->PathArcTo(P(0.0f, -0.05f), r * 0.80f, 0.0f, 3.14159f, 20);
        dl->PathStroke(col, 0, t);
        dl->AddLine(P(-0.30f, 0.84f), P(0.30f, 0.84f), col, t);
        dl->AddLine(P(-0.25f, -0.35f), P(-0.25f, -0.75f), col, t * 0.8f);
        dl->AddLine(P(0.15f, -0.35f), P(0.15f, -0.80f), col, t * 0.8f);
        break;
    case kIcFlag: {
        dl->AddLine(P(-0.55f, -0.80f), P(-0.55f, 0.85f), col, t);
        const ImVec2 pts[] = {P(-0.55f, -0.78f), P(0.65f, -0.50f), P(-0.55f, -0.12f)};
        dl->AddPolyline(pts, 3, col, ImDrawFlags_Closed, t);
        break;
    }
    case kIcGear:
        dl->AddCircle(c, r * 0.48f, col, 24, t);
        dl->AddCircle(c, r * 0.16f, col, 12, t);
        for (int i = 0; i < 8; ++i) {
            const float a = i * 3.14159f / 4.0f;
            dl->AddLine(ImVec2(c.x + std::cos(a) * r * 0.50f, c.y + std::sin(a) * r * 0.50f),
                        ImVec2(c.x + std::cos(a) * r * 0.80f, c.y + std::sin(a) * r * 0.80f), col, t * 1.3f);
        }
        break;
    case kIcBack:
        dl->AddLine(P(0.25f, -0.55f), P(-0.25f, 0.0f), col, t);
        dl->AddLine(P(-0.25f, 0.0f), P(0.25f, 0.55f), col, t);
        break;
    case kIcPlus:
        dl->AddLine(P(-0.6f, 0.0f), P(0.6f, 0.0f), col, t);
        dl->AddLine(P(0.0f, -0.6f), P(0.0f, 0.6f), col, t);
        break;
    }
}

// ---------------------------------------------------------------------------
//  控件
// ---------------------------------------------------------------------------
// 胶囊按钮。一页最多一个主按钮（金色实心），其余是淡底金字；删除这类是不带底的淡红字。
enum BtnStyle { kPrimary, kSecondary, kDanger };

bool PillButton(const char* id, const char* label, float w, float h, BtnStyle style, Icon icon = (Icon)-1)
{
    BeginCustom();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    EndCustom();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rr = h * 0.5f;
    if (style == kPrimary) dl->AddRectFilled(a, b, act ? kGoldDown : (hov ? kGoldHot : kGold), rr);
    else if (style == kSecondary) dl->AddRectFilled(a, b, (hov || act) ? kSoftHot : kSoft, rr);
    else if (hov || act) dl->AddRectFilled(a, b, IM_COL32(232, 140, 110, 22), rr);
    if (Focused()) dl->AddRect(a - ImVec2(4, 4) * S, b + ImVec2(4, 4) * S, kGold, rr + 4 * S, 0, 2.0f * S);
    const float fs = style == kPrimary ? szBtn : szBody;
    ImFont* font = style == kDanger ? fReg : fBold;
    const ImVec2 ts = TextSize(font, fs, label);
    const float iconW = (int)icon >= 0 ? fs * 1.15f : 0.0f;
    const float x0 = (a.x + b.x - ts.x - iconW) * 0.5f;
    const float cy = (a.y + b.y) * 0.5f;
    const ImU32 col = style == kPrimary ? kInk : (style == kDanger ? kWarn : kGold);
    if ((int)icon >= 0) DrawIcon(dl, icon, ImVec2(x0 + fs * 0.42f, cy), fs * 0.48f, col);
    dl->AddText(font, fs, ImVec2(std::floor(x0 + iconW), std::floor(cy - ts.y * 0.5f)), col, label);
    return pressed;
}

// 一排芯片单选（猫饭的档位）：手柄左右移动、A 选中，比拖滑条顺手
bool SegRow(const char* id, const char* label, int* v, int count, const char* const* labels, bool takeFocus)
{
    ImGui::PushID(id);
    const float w = ImGui::GetContentRegionAvail().x;
    const float rowH = 48 * S, h = 36 * S, gap = 6 * S;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddText(fReg, szBody, ImVec2(p.x + 14 * S, p.y + (rowH - szBody) * 0.5f), kMuted, label);
    const float x0 = p.x + w * 0.30f;
    const float segW = (w * 0.70f - 6 * S - gap * (count - 1)) / count;
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        ImGui::SetCursorScreenPos(ImVec2(x0 + i * (segW + gap), p.y + (rowH - h) * 0.5f));
        ImGui::PushID(i);
        BeginCustom();
        if (ImGui::InvisibleButton("##seg", ImVec2(segW, h), ImGuiButtonFlags_EnableNav)) { *v = i; changed = true; }
        EndCustom();
        if (takeFocus && i == *v) FocusDefault();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const bool on = i == *v, hov = ImGui::IsItemHovered();
        dl->AddRectFilled(a, b, on ? kGold : (hov ? kSoftHot : kSoft), h * 0.5f);
        if (Focused()) dl->AddRect(a - ImVec2(3, 3) * S, b + ImVec2(3, 3) * S, kGold, h * 0.5f + 3 * S, 0, 2.0f * S);
        CenterText(dl, on ? fBold : fReg, szSmall, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), on ? kInk : kText, labels[i]);
        ImGui::PopID();
    }
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
    ImGui::Dummy(ImVec2(0, 0));
    ImGui::PopID();
    return changed;
}

// 圆形图标按钮 + 下方短标签（主页那一排）
bool RoundButton(const char* id, Icon icon, const char* label, float colW)
{
    const float d = 54 * S;
    const float h = d + 10 * S + szSmall + 4 * S;
    BeginCustom();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(colW, h), ImGuiButtonFlags_EnableNav);
    EndCustom();
    const ImVec2 a = ImGui::GetItemRectMin();
    const ImVec2 c(a.x + colW * 0.5f, a.y + d * 0.5f);
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(c, d * 0.5f, (hov || act) ? kSoftHot : kSoft, 40);
    if (Focused()) dl->AddCircle(c, d * 0.5f + 4 * S, kGold, 40, 2.0f * S);
    DrawIcon(dl, icon, c, d * 0.26f, kGold);
    CenterText(dl, fReg, szSmall, ImVec2(c.x, a.y + d + 10 * S + szSmall * 0.5f), Focused() ? kText : kMuted, label);
    return pressed;
}

// 整行可点的列表项：左边主文字，右边次要文字
bool Row(const char* id, const char* left, const char* right, bool selected, float h = 0, bool quiet = false)
{
    if (h <= 0) h = 46 * S;
    ImGui::PushID(id);
    // quiet：不画 ImGui 自己的导航框（移动模式里我们自己描金边，两层框叠着难看）
    if (quiet) ImGui::PushStyleColor(ImGuiCol_NavCursor, IM_COL32(0, 0, 0, 0));
    const bool pressed = ImGui::Selectable("##row", selected, ImGuiSelectableFlags_None, ImVec2(0, h));
    if (quiet) ImGui::PopStyleColor();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float rightW = 0;
    if (right && *right) {
        const ImVec2 ts = TextSize(fReg, szSmall, right);
        rightW = ts.x + 16 * S;
        dl->AddText(fReg, szSmall, ImVec2(b.x - 14 * S - ts.x, std::floor(a.y + (h - ts.y) * 0.5f)), kMuted, right);
    }
    dl->PushClipRect(a, ImVec2(b.x - rightW - 8 * S, b.y), true);
    float x = a.x + 14 * S;
    if (selected) {
        dl->AddCircleFilled(ImVec2(x + 3 * S, (a.y + b.y) * 0.5f), 3.5f * S, kGold);
        x += 14 * S;
    }
    const ImVec2 ts = TextSize(fReg, szBody, left);
    dl->AddText(fReg, szBody, ImVec2(x, std::floor(a.y + (h - ts.y) * 0.5f)), selected ? kGold : kText, left);
    dl->PopClipRect();
    ImGui::PopID();
    return pressed;
}

// 开关行：左边文字（下面可带一行小字说明），右边开关
bool ToggleRow(const char* id, const char* label, const char* note, bool* v)
{
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = (note && *note) ? 58 * S : 46 * S;
    BeginCustom();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    EndCustom();
    if (pressed) *v = !*v;
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered() || Focused()) dl->AddRectFilled(a, b, IM_COL32(237, 230, 214, 12), 12 * S);
    if (Focused()) dl->AddRect(a, b, kGold, 12 * S, 0, 1.5f * S);
    const float ty = (note && *note) ? a.y + 8 * S : a.y + (h - szBody) * 0.5f;
    dl->AddText(fReg, szBody, ImVec2(a.x + 14 * S, ty), kText, label);
    if (note && *note) dl->AddText(fReg, szSmall, ImVec2(a.x + 14 * S, ty + szBody + 4 * S), kMuted, note);
    const float sw = 46 * S, sh = 26 * S;
    const ImVec2 sa(b.x - 14 * S - sw, (a.y + b.y - sh) * 0.5f);
    dl->AddRectFilled(sa, sa + ImVec2(sw, sh), *v ? kGold : IM_COL32(237, 230, 214, 46), sh * 0.5f);
    const float kx = *v ? sa.x + sw - sh * 0.5f : sa.x + sh * 0.5f;
    dl->AddCircleFilled(ImVec2(kx, sa.y + sh * 0.5f), sh * 0.5f - 3 * S, *v ? kInk : kText, 24);
    return pressed;
}

// 单选行
bool RadioRow(const char* id, const char* label, const char* note, bool on)
{
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = (note && *note) ? 58 * S : 46 * S;
    BeginCustom();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    EndCustom();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered() || Focused()) dl->AddRectFilled(a, b, IM_COL32(237, 230, 214, 12), 12 * S);
    if (Focused()) dl->AddRect(a, b, kGold, 12 * S, 0, 1.5f * S);
    const ImVec2 c(a.x + 24 * S, (a.y + b.y) * 0.5f);
    dl->AddCircle(c, 9 * S, on ? kGold : kMuted, 24, 1.8f * S);
    if (on) dl->AddCircleFilled(c, 5 * S, kGold, 20);
    const float tx = a.x + 44 * S;
    const float ty = (note && *note) ? a.y + 8 * S : a.y + (h - szBody) * 0.5f;
    dl->AddText(fReg, szBody, ImVec2(tx, ty), on ? kTitle : kText, label);
    if (note && *note) dl->AddText(fReg, szSmall, ImVec2(tx, ty + szBody + 4 * S), kMuted, note);
    return pressed;
}

// 「名称 …… 值 ›」的行，按下打开选择弹窗
bool ValueRow(const char* id, const char* label, const char* value)
{
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = 46 * S;
    BeginCustom();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    EndCustom();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (ImGui::IsItemHovered() || Focused()) dl->AddRectFilled(a, b, IM_COL32(237, 230, 214, 12), 12 * S);
    if (Focused()) dl->AddRect(a, b, kGold, 12 * S, 0, 1.5f * S);
    dl->AddText(fReg, szBody, ImVec2(a.x + 14 * S, a.y + (h - szBody) * 0.5f), kMuted, label);
    const ImVec2 ts = TextSize(fReg, szBody, value);
    dl->AddText(fReg, szBody, ImVec2(b.x - 34 * S - ts.x, a.y + (h - szBody) * 0.5f), kText, value);
    DrawIcon(dl, kIcBack, ImVec2(b.x - 18 * S, (a.y + b.y) * 0.5f), -7 * S, kMuted);   // r<0 = 镜像成 ›
    return pressed;
}

// 页标题。子页带返回箭头：箭头加标题整块可以用鼠标点（手柄用 B，所以不进十字键导航）
void PageHeader(const char* title, const char* sub, bool back)
{
    if (back) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const ImVec2 ts = TextSize(fBold, szPage, title);
        ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
        BeginCustom();
        if (ImGui::InvisibleButton("##back", ImVec2(ts.x + 34 * S, ts.y))) Go(Back(g_page));
        EndCustom();
        ImGui::PopItemFlag();
        const bool hov = ImGui::IsItemHovered();
        DrawIcon(dl, kIcBack, ImVec2(p.x + 6 * S, p.y + ts.y * 0.5f), 9 * S, hov ? kGold : kMuted);
        dl->AddText(fBold, szPage, ImVec2(p.x + 24 * S, p.y), hov ? kGold : kTitle, title);
    } else {
        Text(fBold, szPage, kTitle, title);
    }
    if (sub && *sub) Text(fReg, szSmall, kMuted, sub);
    ImGui::Dummy(ImVec2(0, 10 * S));
}

// 选择弹窗：返回选中的下标（没选返回 -1）
int PickPopup(const char* id, const std::vector<std::string>& items, int current)
{
    int picked = -1;
    ImGui::SetNextWindowSizeConstraints(ImVec2(320 * S, 0), ImVec2(420 * S, 460 * S));
    if (ImGui::BeginPopup(id)) {
        for (int i = 0; i < (int)items.size(); ++i) {
            ImGui::PushID(i);
            if (Row("it", items[i].c_str(), nullptr, i == current, 40 * S)) { picked = i; ImGui::CloseCurrentPopup(); }
            if (i == current && ImGui::IsWindowAppearing()) {
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetScrollHereY(0.4f);
            }
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    return picked;
}

std::string QuestLabel(int id, const char* zeroText)
{
    if (id == 0) return zeroText;
    const QuestInfo* q = gamedata::FindQuest(id);
    if (q) return gamedata::QuestName(*q);
    return i18n::Fmt(i18n::Tr("任务 %d", "Quest %d", "クエスト %d"), id);
}

// 猫饭加成 / 属性耐性的档位（0~3）。中文和日文都是「无小中大」，英文用游戏里的 S/M/L。
const char* LevelName(int v)
{
    static const char* const zh[] = {"无", "小", "中", "大"};
    static const char* const en[] = {"none", "S", "M", "L"};
    static const char* const ja[] = {"なし", "小", "中", "大"};
    if (v < 0) v = 0;
    if (v > 3) v = 3;
    return i18n::Tr(zh[v], en[v], ja[v]);
}

std::string FoodSummary(const FoodPreset& f)
{
    std::string s = i18n::Fmt(i18n::Tr("体力+%d 耐力+%d", "HP+%d Stam+%d", "体力+%d スタミナ+%d"),
                              f.hp * 10, f.stamina * 25);
    if (f.attack > 0 && f.attack <= 3) s += std::string(i18n::Tr(" 攻击", " ATK ", " 攻撃")) + LevelName(f.attack);
    if (f.defense > 0 && f.defense <= 3) s += std::string(i18n::Tr(" 防御", " DEF ", " 防御")) + LevelName(f.defense);
    if (f.elemRes > 0 && f.elemRes <= 3) s += std::string(i18n::Tr(" 耐性", " RES ", " 耐性")) + LevelName(f.elemRes);
    return s;
}

// 猫饭在界面上的名字：起了名字就用名字，没起就显示序号
std::string FoodLabel(const Config& c, int id)
{
    auto it = c.foods.find(id);
    if (it != c.foods.end() && !it->second.name.empty()) return it->second.name;
    return i18n::Fmt(i18n::Tr("猫饭 %d", "Meal %d", "猫飯 %d"), id);
}

// 名字首尾的空格去掉（中间的不动，中文名字里偶尔有空格是对的）
std::string TrimName(const char* s)
{
    std::string r = s ? s : "";
    std::size_t b = r.find_first_not_of(" \t");
    if (b == std::string::npos) return std::string();
    std::size_t e = r.find_last_not_of(" \t");
    return r.substr(b, e - b + 1);
}

// 名字有几个字：UTF-8 里只数每个字符的首字节，一个汉字算 1 个字
int CountChars(const char* s)
{
    int n = 0;
    for (const unsigned char* p = (const unsigned char*)(s ? s : ""); *p; ++p)
        if ((*p & 0xC0) != 0x80) ++n;
    return n;
}

// 超长就砍掉尾巴。按首字节砍，不会把多字节字符切成半个
void CutChars(char* s, int max)
{
    int n = 0;
    for (char* p = s; *p; ++p) {
        if (((unsigned char)*p & 0xC0) != 0x80 && ++n == max + 1) { *p = 0; return; }
    }
}

void OpenRename(Model& m, RenameKind kind, int id)
{
    g_renameCreate = false;
    g_afterRename = 0;
    if (!m.cfg) return;
    std::string cur;
    if (kind == kRenameSet) {
        if (id < 0 || id >= (int)m.cfg->sets.size()) return;
        cur = m.cfg->sets[id].name;
    } else if (kind == kRenameFood) {
        cur = FoodLabel(*m.cfg, id);
    } else {
        return;
    }
    g_renameKind = kind;
    g_renameId = id;
    g_renameErr.clear();
    std::snprintf(g_renameBuf, sizeof(g_renameBuf), "%s", cur.c_str());
    g_renameFocus = true;
    g_renameOpen = false;
}

// 新建时先建出来（默认名），再弹框让用户直接起名；确认或取消都进编辑页
void OpenRenameCreate(Model& m, RenameKind kind, int id)
{
    if (!m.cfg) return;
    g_renameKind = kind;
    g_renameId = id;
    g_renameCreate = true;
    g_renameErr.clear();
    g_renameBuf[0] = 0;
    g_renameFocus = true;
    g_renameOpen = false;
}

// 移动模式下把选中的那一项往上 / 往下挪一格。套装是换 vector 里的位置（编号跟着走），
// 猫饭只改显示顺序（foodOrder），编号不动
void MoveItem(Model& m, int dir)
{
    Config& c = *m.cfg;
    Log("ui: MoveItem dir=%d kind=%d id=%d", dir, (int)g_moveKind, g_moveId);
    if (g_moveKind == kMoveSet) {
        int idx = -1;
        for (int i = 0; i < (int)c.sets.size(); ++i) if (c.sets[i].id == g_moveId) { idx = i; break; }
        if (idx < 0) return;
        const int to = idx + dir;
        if (to < 0 || to >= (int)c.sets.size()) return;
        std::swap(c.sets[idx], c.sets[to]);
        if (g_editSet == idx) g_editSet = to;
        else if (g_editSet == to) g_editSet = idx;
        if (to == 0) ImGui::SetScrollY(0.0f);                                 // 挪到两端直接贴边
        else if (to + 1 == (int)c.sets.size()) ImGui::SetScrollY(ImGui::GetScrollMaxY());
        m.reqSave = true;
    } else if (g_moveKind == kMoveFood) {
        std::vector<int> ids = c.FoodIds();
        int idx = -1;
        for (int i = 0; i < (int)ids.size(); ++i) if (ids[i] == g_moveId) { idx = i; break; }
        if (idx < 0) return;
        const int to = idx + dir;
        if (to < 0 || to >= (int)ids.size()) return;
        std::swap(ids[idx], ids[to]);
        c.foodOrder = ids;
        if (to == 0) ImGui::SetScrollY(0.0f);
        else if (to + 1 == (int)ids.size()) ImGui::SetScrollY(ImGui::GetScrollMaxY());
        m.reqSave = true;
    }
}

std::string SetSummary(const LoadoutSet& s, const Config& c)
{
    std::string r;
    auto add = [&](const std::string& t) { if (!r.empty()) r += " · "; r += t; };
    if (!s.equip.empty()) add(std::string(i18n::Tr("装备 ", "Equip ", "装備 ")) + s.equip);
    if (!s.item.empty()) add(std::string(i18n::Tr("道具 ", "Items ", "アイテム ")) + s.item);
    if (!s.food) {} else add(FoodLabel(c, s.food));
    return r.empty() ? i18n::Tr("空套装", "empty set", "空のセット") : r;
}

std::string HotkeyText(const std::string& spec)
{
    std::string r;
    for (char ch : spec) {
        if (ch == '+') r += " + ";
        else if (ch == '|') r += i18n::Tr("  或  ", " / ", " または ");
        else if (ch != ' ') r += ch;
    }
    return r;
}

// ---------------------------------------------------------------------------
//  各页
// ---------------------------------------------------------------------------
int ActiveSetIndex(const Config& c)
{
    for (int i = 0; i < (int)c.sets.size(); ++i) if (c.sets[i].id == c.activeSet) return i;
    return -1;
}

void PageHome(Model& m)
{
    Config& c = *m.cfg;
    const float w = ImGui::GetContentRegionAvail().x;
    const int setIdx = ActiveSetIndex(c);
    const LoadoutSet* set = setIdx >= 0 ? &c.sets[setIdx] : nullptr;
    const int quest = EffectiveQuest(c);

    ImGui::Dummy(ImVec2(0, 4 * S));
    Text(fReg, szSmall, kMuted, i18n::Tr("当前任务", "Current quest", "現在のクエスト"));
    ImGui::Dummy(ImVec2(0, 2 * S));
    Text(fBold, szTitle, kTitle, QuestLabel(quest, i18n::Tr("刚才打的那个任务", "the last quest you ran", "さっき戦ったクエスト")).c_str(), true);
    // 副标题：重开时会套用什么（套装、道具套装、猫饭）
    std::string sub;
    auto add = [&](const std::string& t) { sub += (sub.empty() ? "" : " · ") + t; };
    if (set) add(i18n::Fmt(i18n::Tr("套装「%s」", "set \"%s\"", "セット「%s」"), set->name.c_str()));
    if (set && !set->item.empty()) add(i18n::Fmt(i18n::Tr("道具「%s」", "items \"%s\"", "アイテム「%s」"), set->item.c_str()));
    const int food = (set && set->food) ? set->food : c.food;
    if (food) add(FoodLabel(c, food));
    if (!sub.empty()) Text(fReg, szSmall, kMuted, sub.c_str(), true);
    ImGui::Dummy(ImVec2(0, 24 * S));

    if (!m.ready) {
        Text(fReg, szSmall, kWarn, i18n::Tr("重开功能没装上，详情见 BetterINQ.log",
                                            "the restart hook did not install, see BetterINQ.log",
                                            "再開機能を組み込めませんでした。BetterINQ.log を見てください"), true);
        ImGui::Dummy(ImVec2(0, 6 * S));
    }
    if (PillButton("##restart", i18n::Tr("重开", "Restart", "再開"), w, 58 * S, kPrimary, kIcRefresh)) { m.reqRestart = true; m.reqClose = true; }
    FocusDefault();
    if (!c.hkRestart.empty()) {
        const std::string hk = HotkeyText(c.hkRestart);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        CenterText(ImGui::GetWindowDrawList(), fReg, szSmall, ImVec2(p.x + w * 0.5f, p.y + szSmall * 0.8f), kFaint, hk.c_str());
        ImGui::Dummy(ImVec2(0, szSmall * 1.6f));
    }
    ImGui::Dummy(ImVec2(0, 18 * S));

    const float col = w / 3.0f;
    if (RoundButton("##q", kIcSearch, i18n::Tr("换任务", "Quests", "クエスト"), col)) {
        // 套装里指定了任务的话，重开进的是套装的任务 —— 那就直接改套装里的
        g_questTarget = (set && set->quest) ? setIdx : -1;
        g_questFrom = kHome;
        Go(kQuests);
    }
    ImGui::SameLine(0, 0);
    if (RoundButton("##s", kIcShield, i18n::Tr("换套装", "Loadouts", "装備セット"), col)) Go(kSets);
    ImGui::SameLine(0, 0);
    if (RoundButton("##f", kIcBowl, i18n::Tr("换猫饭", "Meals", "猫飯"), col)) Go(kFood);

    // 次要动作：只把当前任务重置掉，回据点，不接任务也不出发（原 IN-Q 的 Reset only）。
    // 放在下面一排圆按钮之后：重开是主角，这一行是「这次只想要个干净的任务」时才用。
    ImGui::Dummy(ImVec2(0, 14 * S));
    if (PillButton("##resetonly", i18n::Tr("只重置任务", "Reset quest only", "クエストのリセットのみ"),
                   w, 42 * S, kSecondary)) { m.reqResetOnly = true; m.reqClose = true; }
    if (!c.hkResetOnly.empty()) {
        const std::string hk = HotkeyText(c.hkResetOnly);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        CenterText(ImGui::GetWindowDrawList(), fReg, szSmall, ImVec2(p.x + w * 0.5f, p.y + szSmall * 0.8f), kFaint, hk.c_str());
        ImGui::Dummy(ImVec2(0, szSmall * 1.6f));
    } else {
        // 没有快捷键就把这句话放这儿：和「重开」的区别只有一句
        const ImVec2 p = ImGui::GetCursorScreenPos();
        CenterText(ImGui::GetWindowDrawList(), fReg, szSmall, ImVec2(p.x + w * 0.5f, p.y + szSmall * 0.8f), kFaint,
                   i18n::Tr("重置完回据点，不接任务也不出发", "resets and returns to base, no accept, no depart",
                            "リセットして拠点に戻る（受注・出発なし）"));
        ImGui::Dummy(ImVec2(0, szSmall * 1.6f));
    }
}

// 可展开的分组行：左边箭头，右边这一组的任务数
bool GroupRow(const char* id, const char* label, int count, bool open)
{
    const float h = 44 * S;
    ImGui::PushID(id);
    const bool pressed = ImGui::Selectable("##grp", false, ImGuiSelectableFlags_None, ImVec2(0, h));
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 c(a.x + 18 * S, (a.y + b.y) * 0.5f);
    const float r = 5 * S, t = 1.8f * S;
    if (open) {   // ⌄
        dl->AddLine(ImVec2(c.x - r, c.y - r * 0.5f), ImVec2(c.x, c.y + r * 0.5f), kGold, t);
        dl->AddLine(ImVec2(c.x, c.y + r * 0.5f), ImVec2(c.x + r, c.y - r * 0.5f), kGold, t);
    } else {      // ›
        dl->AddLine(ImVec2(c.x - r * 0.5f, c.y - r), ImVec2(c.x + r * 0.5f, c.y), kMuted, t);
        dl->AddLine(ImVec2(c.x + r * 0.5f, c.y), ImVec2(c.x - r * 0.5f, c.y + r), kMuted, t);
    }
    const ImVec2 ts = TextSize(fBold, szBody, label);
    dl->AddText(fBold, szBody, ImVec2(a.x + 36 * S, std::floor(a.y + (h - ts.y) * 0.5f)), open ? kTitle : kText, label);
    char n[16];
    std::snprintf(n, sizeof(n), "%d", count);
    const ImVec2 ns = TextSize(fReg, szSmall, n);
    dl->AddText(fReg, szSmall, ImVec2(b.x - 14 * S - ns.x, std::floor(a.y + (h - ns.y) * 0.5f)), kFaint, n);
    ImGui::PopID();
    return pressed;
}

void PageQuests(Model& m)
{
    Config& c = *m.cfg;
    const bool forSet = g_questTarget >= 0 && g_questTarget < (int)c.sets.size();
    int& target = forSet ? c.sets[g_questTarget].quest : c.questId;
    const std::string sub = forSet
        ? i18n::Fmt(i18n::Tr("改的是套装「%s」里的任务",
                             "editing the quest of set \"%s\"",
                             "セット「%s」のクエストを変更中"),
                    c.sets[g_questTarget].name.c_str())
        : std::string();
    PageHeader(i18n::Tr("换任务", "Quests", "クエスト"), forSet ? sub.c_str() : nullptr, true);

    ImGui::SetNextItemWidth(-1);
    ImGui::PushFont(fReg, szBody);
    ImGui::InputTextWithHint("##search",
                             i18n::Tr("任务名、拼音、假名、罗马音或 ID",
                                      "name, pinyin, kana, romaji or ID",
                                      "名前・ピンイン・かな・ローマ字・ID"),
                             g_search, sizeof(g_search));
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 4 * S));

    const float listH = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##qlist", ImVec2(0, listH), ImGuiChildFlags_NavFlattened);
    auto pick = [&](int id) { target = id; m.reqSave = true; Go(Back(kQuests)); };

    int gn = 0, qn = 0;
    const QuestGroup* groups = gamedata::Groups(gn);
    const QuestInfo* qs = gamedata::Quests(qn);
    if (gn > 16) gn = 16;
    if (g_questInit) {
        g_questInit = false;
        for (bool& o : g_groupOpen) o = false;
        if (const QuestInfo* tq = gamedata::FindQuest(target))
            if (tq->group >= 0 && tq->group < gn) g_groupOpen[tq->group] = true;
    }

    const std::string needle = g_search;
    const bool searching = !needle.empty();
    // 全局任务可以是 0 =「当前任务」（重开刚打完 / 正在打的那个）。搜索时它和搜的字无关，不列
    const bool zeroRow = !forSet && !searching;
    if (zeroRow) {
        if (Row("q0", i18n::Tr("当前任务", "Current quest", "現在のクエスト"), nullptr, target == 0)) pick(0);
        if (target == 0) FocusDefault();
        if (target == 0 && g_scrollToSel > 0) { ImGui::SetScrollHereY(0.3f); --g_scrollToSel; }
    }
    const bool hasSel = gamedata::FindQuest(target) || (zeroRow && target == 0);   // 没有选中的就把焦点放在第一组上

    // 先按组归好（搜索时只留匹配的），再画：每帧只做一遍字符串比较
    std::vector<int> byGroup[16];
    for (int i = 0; i < qn; ++i)
        if (qs[i].group >= 0 && qs[i].group < gn && gamedata::QuestMatches(qs[i], needle)) byGroup[qs[i].group].push_back(i);

    for (int gi = 0; gi < gn; ++gi) {
        if (byGroup[gi].empty()) continue;
        const bool open = searching || g_groupOpen[gi];
        char gid[8];
        std::snprintf(gid, sizeof(gid), "g%d", gi);
        ImGui::Dummy(ImVec2(0, 2 * S));
        if (GroupRow(gid, gamedata::GroupName(groups[gi]), (int)byGroup[gi].size(), open) && !searching) g_groupOpen[gi] = !g_groupOpen[gi];
        if (!hasSel && gi == 0) FocusDefault();
        if (!open) continue;
        ImGui::Indent(10 * S);
        for (int i : byGroup[gi]) {
            const QuestInfo& q = qs[i];
            char idbuf[16];
            std::snprintf(idbuf, sizeof(idbuf), "%d", q.id);
            const bool sel = (q.id == target);
            if (Row(idbuf, gamedata::QuestName(q), idbuf, sel, 42 * S)) pick(q.id);
            if (sel) FocusDefault();
            if (sel && g_scrollToSel > 0) { ImGui::SetScrollHereY(0.3f); --g_scrollToSel; }
        }
        ImGui::Unindent(10 * S);
    }
    if (searching && std::all_of(byGroup, byGroup + gn, [](const std::vector<int>& v) { return v.empty(); }))
        Text(fReg, szSmall, kMuted, i18n::Tr("没找到", "nothing found", "見つかりません"));
    ImGui::EndChild();
}

// 编辑页底部的「删除」：先弹框确认，确认了返回 true
bool DeleteButton(const std::string& what)
{
    if (PillButton("##del", i18n::Tr("删除", "Delete", "削除"), ImGui::GetContentRegionAvail().x, 46 * S, kDanger)) ImGui::OpenPopup("confirm_del");
    bool yes = false;
    if (ImGui::BeginPopup("confirm_del")) {
        Text(fReg, szBody, kText, i18n::Fmt(i18n::Tr("删除「%s」？", "Delete \"%s\"?", "「%s」を削除しますか？"), what.c_str()).c_str());
        ImGui::Dummy(ImVec2(0, 6 * S));
        if (PillButton("##yes", i18n::Tr("删除", "Delete", "削除"), 200 * S, 40 * S, kPrimary)) { yes = true; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
    }
    return yes;
}

// 移动模式下的自动滚动：项挪到「下一行会被挡住」时才滚一格。
// 边界用这一帧实际被裁到哪（内容区高度可能比面板真正能看见的部分大，拿内容区当边界
// 会永远判成界外，往下就一路滚到底）；每次只滚一行，上下对称，来回挪能精确回到原位。
void ScrollMovingIntoView(float rowTop, float rowH)
{
    const ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 clipMin = dl->GetClipRectMin();
    const ImVec2 clipMax = dl->GetClipRectMax();
    const float rowBottom = rowTop + rowH;
    const float pad = 4.0f;   // 贴着边界（差 0~3 px）也算要滚，否则收尾会差半格
    if (rowBottom > clipMax.y - pad) ImGui::SetScrollY(ImGui::GetScrollY() + rowH);
    else if (rowTop < clipMin.y + pad) ImGui::SetScrollY(ImGui::GetScrollY() - rowH);
    g_listScroll = ImGui::GetScrollY();   // 给回归测试读
}

void PageSets(Model& m)
{
    Config& c = *m.cfg;
    const int activeIdx = ActiveSetIndex(c);
    const std::string sub = activeIdx >= 0
        ? i18n::Fmt(i18n::Tr("重开时套用「%s」", "applied on restart: \"%s\"", "再開時に適用「%s」"),
                    c.sets[activeIdx].name.c_str())
        : std::string();
    PageHeader(i18n::Tr("套装", "Loadouts", "装備セット"), activeIdx >= 0 ? sub.c_str() : nullptr, false);
    // 左摇杆按下：把选中的这一套放进移动模式，上下键换位置，再按一次放下
    const bool moving = g_moveKind == kMoveSet && c.FindSetById(g_moveId) != nullptr;
    if (!moving && g_moveKind == kMoveSet) { g_moveKind = kMoveNone; g_moveId = 0; }
    if (moving) {
        // repeat = true：按住不放也要一直换位置（否则只有第一下有效），
        // 之前这里写 false，长按就变成了「Focus 在列表里上下走」
        const bool up = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, true) || ImGui::IsKeyPressed(ImGuiKey_UpArrow, true);
        const bool down = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, true) || ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
        if (up || down) MoveItem(m, up ? -1 : 1);
        const bool put = ImGui::IsKeyPressed(ImGuiKey_GamepadL3, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) ||
                        ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
                        ImGui::IsKeyPressed(ImGuiKey_E, false);
        const bool cancel = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (put || cancel) {
            g_moveKind = kMoveNone;
            g_focusId = g_moveId;
            ImGui::SetNavCursorVisible(true);
        }
    }
    // 移动中列表项不参与十字键导航：否则上下键一边换位置一边把 Focus 挪到邻行
    if (moving) ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    for (int i = 0; i < (int)c.sets.size(); ++i) {
        ImGui::PushID(i);
        const std::string rowId = "set" + std::to_string(c.sets[i].id);
        const bool isMoving = moving && g_moveId == c.sets[i].id;
        const float rowTop = ImGui::GetCursorScreenPos().y;   // 行被完全裁掉时 GetItemRect 不可靠，先记下行的顶
        const bool acted = Row(rowId.c_str(), c.sets[i].name.c_str(),
                               isMoving ? "" : SetSummary(c.sets[i], c).c_str(), i == activeIdx, 54 * S, isMoving);
        const ImGuiID rid = ImGui::GetItemID();   // 这一行的 ID：放下后把焦点还回来要用
        if (ImGui::IsItemClicked()) g_mouseDownRow = rowId;
        if (isMoving) {
            // 正在移动的那一项：整行淡金底 + 金边，右边写「移动中」
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            dl->AddRectFilled(a, b, IM_COL32(226, 194, 122, 30), 12 * S);
            dl->AddRect(a - ImVec2(2, 2) * S, b + ImVec2(2, 2) * S, kGold, 12 * S, 0, 2.0f * S);
            const char* tagTxt = i18n::Tr("移动中", "moving", "移動中");
            const ImVec2 ts = TextSize(fReg, szSmall, tagTxt);
            dl->AddText(fReg, szSmall, ImVec2(b.x - 14 * S - ts.x, std::floor(a.y + (b.y - a.y - ts.y) * 0.5f)), kGold, tagTxt);
        }
        if (!moving && acted) {
            g_mouseDownRow.clear();
            if (i != activeIdx) {
                c.activeSet = c.sets[i].id;   // 没选中的（鼠标点 / 键盘空格 / 手柄 A）：直接选中
                m.reqSave = true;
            } else {
                g_editSet = i;                // 已经在用的那一套再按一下：进编辑页
                Go(kSetEdit);
            }
        }
        // 选中一项后按 X（手柄）或 F（键盘）改名
        if (!moving && ImGui::IsItemFocused() &&
            (ImGui::IsKeyPressed(ImGuiKey_F, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)))
            OpenRename(m, kRenameSet, i);
        // 按 Y：直接套用这一套（已经在用的就不用再按了）
        if (!moving && ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) && i != activeIdx) {
            c.activeSet = c.sets[i].id;
            m.reqSave = true;
        }
        // 按左摇杆：进入移动模式（哪一项被选中就移动哪一项）
        if (!moving && ImGui::IsItemFocused() &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadL3, false) || ImGui::IsKeyPressed(ImGuiKey_E, false))) {
            Log("ui: 套装进入移动模式 id=%d", c.sets[i].id);
            g_moveKind = kMoveSet;
            g_moveId = c.sets[i].id;
        }
        if (!moving && i == (activeIdx >= 0 ? activeIdx : 0)) FocusDefault();
        if (isMoving) ScrollMovingIntoView(rowTop, 54 * S);
        if (!moving && g_focusId == c.sets[i].id) {
            g_focusId = 0;
            ImGui::SetFocusID(rid, ImGui::GetCurrentWindow());
            ImGui::SetNavCursorVisible(true);
        }
        ImGui::PopID();
    }
    if (moving) ImGui::PopItemFlag();
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) g_mouseDownRow.clear();
    ImGui::Dummy(ImVec2(0, 8 * S));
    const bool newSet = PillButton("##newset", i18n::Tr("新建套装", "New loadout", "新規セット"), ImGui::GetContentRegionAvail().x, 46 * S, kSecondary, kIcPlus);
    if (c.sets.empty()) FocusDefault();
    ImGui::Dummy(ImVec2(0, 2 * S));
    Text(fReg, szSmall, kMuted,
         i18n::Tr("按 A / 空格 选中（再按一次进编辑）· 按 Y 直接套用 · 按 F 或 X 改名 · 按 L3 移动顺序",
                  "A or Space selects (press again to edit) · Y applies it · F or X renames · L3 reorders",
                  "A / スペースで選択（もう一度で編集）· Y で適用 · F / X で名前変更 · L3 で並べ替え"));
    if (newSet) {
        LoadoutSet s;
        s.id = c.NextSetId();
        s.name = std::string(i18n::Tr("套装", "Loadout", "セット")) + std::to_string((int)c.sets.size() + 1);
        c.sets.push_back(s);
        g_editSet = (int)c.sets.size() - 1;
        m.reqSave = true;
        g_afterRename = 1;                 // 起完名（或取消）直接进编辑页
        OpenRenameCreate(m, kRenameSet, g_editSet);
    }
}

void PageSetEdit(Model& m)
{
    Config& c = *m.cfg;
    if (g_editSet < 0 || g_editSet >= (int)c.sets.size()) { Go(kSets); return; }
    LoadoutSet& s = c.sets[g_editSet];
    const bool active = s.id == c.activeSet;
    PageHeader(s.name.c_str(), active ? i18n::Tr("重开时会套用这套", "applied on restart", "再開時に適用されます") : nullptr, true);

    // 装备
    std::vector<std::string> equips = {kNone};
    int curE = 0;
    for (const auto& e : m.equipNames) {
        equips.push_back(e.name);
        if (e.name == s.equip) curE = (int)equips.size() - 1;
    }
    if (ValueRow("##eq", i18n::Tr("装备套装", "Equip loadout", "装備セット"), s.equip.empty() ? kNone : s.equip.c_str())) ImGui::OpenPopup("pick_eq");
    FocusDefault();
    int p = PickPopup("pick_eq", equips, curE);
    if (p >= 0) { s.equip = p == 0 ? std::string() : equips[p]; m.reqSave = true; }

    // 道具（名字可能重复，选中时存名字；游戏里只认第一个同名的）
    std::vector<std::string> items = {kNone};
    int curI = 0;
    for (const auto& it : m.itemNames) {
        items.push_back(std::to_string(it.number) + "  " + it.name);
        if (it.name == s.item || std::to_string(it.number) == s.item) curI = (int)items.size() - 1;
    }
    if (ValueRow("##it", i18n::Tr("道具套装", "Item loadout", "アイテムセット"), s.item.empty() ? kNone : s.item.c_str())) ImGui::OpenPopup("pick_it");
    p = PickPopup("pick_it", items, curI);
    if (p >= 0) { s.item = p == 0 ? std::string() : m.itemNames[p - 1].name; m.reqSave = true; }

    if (ToggleRow("##lr", i18n::Tr("联动快捷栏", "Link shortcut bar", "ショートカットを連動"), nullptr, &s.linkRadial)) m.reqSave = true;

    // 猫饭
    std::vector<std::string> foods = {kNone};
    std::vector<int> foodIds = {0};
    int curF = 0;
    for (int fid : c.FoodIds()) {
        foods.push_back(FoodLabel(c, fid));
        foodIds.push_back(fid);
        if (fid == s.food) curF = (int)foods.size() - 1;
    }
    const std::string foodText = s.food ? FoodLabel(c, s.food) : kNone;
    if (ValueRow("##fd", i18n::Tr("猫饭", "Meal", "猫飯"), foodText.c_str())) ImGui::OpenPopup("pick_fd");
    p = PickPopup("pick_fd", foods, curF);
    if (p >= 0) { s.food = foodIds[p]; m.reqSave = true; }

    if (ValueRow("##qs", i18n::Tr("任务", "Quest", "クエスト"), QuestLabel(s.quest, kNone).c_str())) {
        g_questTarget = g_editSet;
        g_questFrom = kSetEdit;
        Go(kQuests);
    }

    // 确认 = 重开时套用这套（改的内容是随改随存的，不用确认）
    ImGui::Dummy(ImVec2(0, 14 * S));
    if (PillButton("##ok", i18n::Tr("确认", "OK", "決定"), ImGui::GetContentRegionAvail().x, 54 * S, kPrimary)) {
        c.activeSet = s.id;
        m.reqSave = true;
        Go(kSets);
    }
    ImGui::Dummy(ImVec2(0, 4 * S));
    if (DeleteButton(s.name)) {
        if (active) c.activeSet = 0;
        c.sets.erase(c.sets.begin() + g_editSet);
        g_editSet = -1;
        m.reqSave = true;
        Go(kSets);
    }
}

// 猫饭列表：没套用套装（或那套没指定猫饭）时，选中 = 每次进任务都吃它；
// 套装自带猫饭时，重开用的是套装的猫饭，这里只能编辑内容
void PageFood(Model& m)
{
    Config& c = *m.cfg;
    // 套装的猫饭会盖掉这里的全局选择（重开时 `if (s.food) g.food = s.food`），
    // 所以只有没套用套装、或者这套自己没指定猫饭时，「选中」才有意义。
    const LoadoutSet* act = c.FindSetById(c.activeSet);
    const int setFood = (act && act->food) ? act->food : 0;
    const bool locked = setFood != 0;            // 这套自带猫饭：列表只能编辑，不能选
    const int eff = setFood ? setFood : c.food; // 重开时实际会吃的那一份
    const bool has = c.foods.count(eff) > 0;
    PageHeader(i18n::Tr("猫饭", "Meals", "猫飯"),
               locked ? i18n::Fmt(i18n::Tr("这套套装自带猫饭「%s」", "this loadout brings \"%s\"", "このセットは「%s」を持参"),
                                  FoodLabel(c, eff).c_str()).c_str()
                      : has ? i18n::Fmt(i18n::Tr("每次进任务都吃「%s」", "eaten on every quest: \"%s\"", "クエストごとに「%s」を食べる"),
                                        FoodLabel(c, eff).c_str()).c_str()
                             : nullptr,
               false);
    // 左摇杆按下：把选中的这份猫饭放进移动模式，上下键换位置（只改显示顺序，编号不动）
    const std::vector<int> ids = c.FoodIds();
    const bool moving = g_moveKind == kMoveFood && c.foods.count(g_moveId) > 0;
    if (!moving && g_moveKind == kMoveFood) { g_moveKind = kMoveNone; g_moveId = 0; }
    if (moving) {
        const bool up = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, true) || ImGui::IsKeyPressed(ImGuiKey_UpArrow, true);
        const bool down = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, true) || ImGui::IsKeyPressed(ImGuiKey_DownArrow, true);
        if (up || down) MoveItem(m, up ? -1 : 1);
        const bool put = ImGui::IsKeyPressed(ImGuiKey_GamepadL3, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false) ||
                        ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_Space, false) ||
                        ImGui::IsKeyPressed(ImGuiKey_E, false);
        const bool cancel = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (put || cancel) {
            g_moveKind = kMoveNone;
            g_focusId = g_moveId;
            ImGui::SetNavCursorVisible(true);
        }
    }
    if (moving) ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    for (int fid : ids) {
        FoodPreset& f = c.foods[fid];
        ImGui::PushID(fid);
        const std::string name = FoodLabel(c, fid);
        const std::string rowId = "food" + std::to_string(fid);
        const bool isMoving = moving && g_moveId == fid;
        const float rowTop = ImGui::GetCursorScreenPos().y;   // 行被完全裁掉时 GetItemRect 不可靠，先记下行的顶
        const bool acted = Row(rowId.c_str(), name.c_str(), isMoving ? "" : FoodSummary(f).c_str(), fid == eff, 54 * S, isMoving);
        const ImGuiID rid = ImGui::GetItemID();
        if (ImGui::IsItemClicked()) g_mouseDownRow = rowId;
        if (isMoving) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
            dl->AddRectFilled(a, b, IM_COL32(226, 194, 122, 30), 12 * S);
            dl->AddRect(a - ImVec2(2, 2) * S, b + ImVec2(2, 2) * S, kGold, 12 * S, 0, 2.0f * S);
            const char* tagTxt = i18n::Tr("移动中", "moving", "移動中");
            const ImVec2 ts = TextSize(fReg, szSmall, tagTxt);
            dl->AddText(fReg, szSmall, ImVec2(b.x - 14 * S - ts.x, std::floor(a.y + (b.y - a.y - ts.y) * 0.5f)), kGold, tagTxt);
        }
        if (!moving && acted) {
            g_mouseDownRow.clear();
            if (locked || fid == eff) {
                g_editFood = fid;            // 选中的再按一下进编辑；锁定时所有行都只能编辑
                Go(kFoodEdit);
            } else {
                c.food = fid;                // 没选中的（鼠标 / 空格 / A）：直接选它
                m.reqSave = true;
            }
        }
        if (!moving && ImGui::IsItemFocused() &&
            (ImGui::IsKeyPressed(ImGuiKey_F, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)))
            OpenRename(m, kRenameFood, fid);
        // 按 Y：直接选这份猫饭
        if (!moving && ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false) && !locked && fid != eff) {
            c.food = fid;
            m.reqSave = true;
        }
        // 按左摇杆：进入移动模式
        if (!moving && ImGui::IsItemFocused() &&
            (ImGui::IsKeyPressed(ImGuiKey_GamepadL3, false) || ImGui::IsKeyPressed(ImGuiKey_E, false))) {
            Log("ui: 猫饭进入移动模式 id=%d", fid);
            g_moveKind = kMoveFood;
            g_moveId = fid;
        }
        if (!moving && (has ? fid == eff : ids.empty() == false && fid == ids.front())) FocusDefault();
        if (isMoving) ScrollMovingIntoView(rowTop, 54 * S);
        if (!moving && g_focusId == fid) {
            g_focusId = 0;
            ImGui::SetFocusID(rid, ImGui::GetCurrentWindow());
            ImGui::SetNavCursorVisible(true);
        }
        ImGui::PopID();
    }
    if (moving) ImGui::PopItemFlag();
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) g_mouseDownRow.clear();
    ImGui::Dummy(ImVec2(0, 8 * S));
    const bool newFood = PillButton("##newfood", i18n::Tr("新建猫饭", "New meal", "新規猫飯"), ImGui::GetContentRegionAvail().x, 46 * S, kSecondary, kIcPlus);
    if (c.foods.empty()) FocusDefault();
    ImGui::Dummy(ImVec2(0, 2 * S));
    Text(fReg, szSmall, kMuted,
         locked ? i18n::Tr("这套自带猫饭，这里的行只能编辑，不能选 · 按 F 或 X 改名 · 按 L3 移动顺序",
                           "this loadout brings its own meal, so rows are edit-only · F or X renames · L3 reorders",
                           "このセットは猫飯を持参するため、行は編集のみ · F / X で名前変更 · L3 で並べ替え")
                : i18n::Tr("按 A / 空格 选中（再按一次进编辑）· 按 Y 直接选中 · 按 F 或 X 改名 · 按 L3 移动顺序",
                           "A or Space selects (press again to edit) · Y selects it · F or X renames · L3 reorders",
                           "A / スペースで選択（もう一度で編集）· Y で選択 · F / X で名前変更 · L3 で並べ替え"));
    if (newFood) {
        int n = 1;
        while (c.foods.count(n)) ++n;
        c.foods[n] = FoodPreset();
        c.foodOrder.push_back(n);
        g_editFood = n;
        m.reqSave = true;
        g_afterRename = 2;
        OpenRenameCreate(m, kRenameFood, n);
    }
}

void PageFoodEdit(Model& m)
{
    Config& c = *m.cfg;
    auto it = c.foods.find(g_editFood);
    if (it == c.foods.end()) { Go(kFood); return; }
    FoodPreset& f = it->second;
    const bool active = c.food == g_editFood;
    const std::string title = FoodLabel(c, g_editFood);
    PageHeader(title.c_str(), active ? i18n::Tr("每次进任务都吃这个", "eaten on every quest", "クエストごとにこれを食べる") : nullptr, true);

    static const char* hp[] = {"+0", "+10", "+20", "+30", "+40", "+50"};
    static const char* st[] = {"+0", "+25", "+50"};
    const char* lv[4] = {LevelName(0), LevelName(1), LevelName(2), LevelName(3)};
    if (SegRow("hp", i18n::Tr("体力", "Health", "体力"), &f.hp, 6, hp, true)) m.reqSave = true;
    if (SegRow("st", i18n::Tr("耐力", "Stamina", "スタミナ"), &f.stamina, 3, st, false)) m.reqSave = true;
    if (SegRow("at", i18n::Tr("攻击", "Attack", "攻撃"), &f.attack, 4, lv, false)) m.reqSave = true;
    if (SegRow("df", i18n::Tr("防御", "Defense", "防御"), &f.defense, 4, lv, false)) m.reqSave = true;
    if (SegRow("er", i18n::Tr("属性耐性", "Elem. resist", "属性耐性"), &f.elemRes, 4, lv, false)) m.reqSave = true;

    int sn = 0;
    const FoodSkillName* skills = gamedata::FoodSkills(sn);
    std::vector<std::string> names;
    for (int i = 0; i < sn; ++i) names.push_back(gamedata::FoodSkillNameOf(skills[i].id));
    for (int k = 0; k < 3; ++k) {
        char id[16], pid[16];
        std::snprintf(id, sizeof(id), "##sk%d", k);
        std::snprintf(pid, sizeof(pid), "pick_sk%d", k);
        const std::string label = i18n::Fmt(i18n::Tr("技能 %d", "Skill %d", "スキル %d"), k + 1);
        int cur = 0;
        for (int i = 0; i < sn; ++i) if (skills[i].id == f.skill[k]) cur = i;
        if (ValueRow(id, label.c_str(), gamedata::FoodSkillNameOf(f.skill[k]))) ImGui::OpenPopup(pid);
        const int p = PickPopup(pid, names, cur);
        if (p >= 0) { f.skill[k] = skills[p].id; m.reqSave = true; }
    }

    // 确认 = 每次进任务都吃这个
    ImGui::Dummy(ImVec2(0, 14 * S));
    if (PillButton("##ok", i18n::Tr("确认", "OK", "決定"), ImGui::GetContentRegionAvail().x, 54 * S, kPrimary)) {
        c.food = g_editFood;
        m.reqSave = true;
        Go(kFood);
    }
    ImGui::Dummy(ImVec2(0, 4 * S));
    if (DeleteButton(title)) {
        if (active) c.food = 0;
        for (auto& s : c.sets) if (s.food == g_editFood) s.food = 0;
        c.foods.erase(it);
        c.NormalizeFoodOrder();
        m.reqSave = true;
        Go(kFood);
    }
}

// ---------------------------------------------------------------------------
//  快捷键：选中一格按 A（或点一下）开始录，按下想用的键、全部松开就记住
// ---------------------------------------------------------------------------
struct Capture {
    std::string* spec = nullptr;   // 录完写进哪个快捷键（nullptr = 没在录）
    bool pad = false;              // 录手柄那一组还是键盘那一组
    bool armed = false;            // 开始录之前按着的键（比如刚按下去的 A）都松开了
    unsigned padAcc = 0;           // 这一轮按过的键，全松开时认这一组
    std::vector<int> keyAcc;
    unsigned prevPad = 0;
    double lastInput = 0;          // 太久没动静就取消
    ImRect rect;                   // 正在录的那一格（点别处 = 取消）
};
Capture g_cap;
std::string g_capWhy;              // 刚才那组为什么不能用（录的过程中显示）

void EndCapture()
{
    g_cap = Capture();
    g_capWhy.clear();
}

// 快捷键写法拆成手柄那组和键盘那组（各取第一组）
void SplitSpec(const std::string& spec, std::string& pad, std::string& key)
{
    pad.clear();
    key.clear();
    for (std::size_t p = 0; p <= spec.size();) {
        std::size_t bar = spec.find('|', p);
        if (bar == std::string::npos) bar = spec.size();
        std::string alt = spec.substr(p, bar - p);
        p = bar + 1;
        alt.erase(std::remove(alt.begin(), alt.end(), ' '), alt.end());
        const Hotkey h = ParseHotkey(alt);
        if (h.alts.size() != 1) continue;
        std::string& to = h.alts[0].keys.empty() ? pad : key;
        if (to.empty()) to = alt;
    }
}

std::string JoinSpec(const std::string& pad, const std::string& key)
{
    if (pad.empty()) return key;
    if (key.empty()) return pad;
    return pad + " | " + key;
}

// 把录到的（或清空的）一组写回快捷键
void SetCapturedPart(Model& m, const std::string& combo)
{
    std::string pad, key;
    SplitSpec(*g_cap.spec, pad, key);
    (g_cap.pad ? pad : key) = combo;
    *g_cap.spec = JoinSpec(pad, key);
    m.reqSave = true;
    EndCapture();
}

// 每帧调一次：录键的状态机。单按手柄 X 或键盘 Delete / Backspace 再松开 = 清除这一格
//（手柄单键、单按 Delete 本来就不能当快捷键，不会和录键冲突）
void UpdateCapture(Model& m)
{
    if (!g_cap.spec) return;
    const double now = ImGui::GetTime();
    const bool padB = (m.padHeld & XINPUT_GAMEPAD_B) && !(g_cap.prevPad & XINPUT_GAMEPAD_B);
    g_cap.prevPad = m.padHeld;
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || now - g_cap.lastInput > 8.0 ||
        (ImGui::IsMouseClicked(0) && !g_cap.rect.Contains(ImGui::GetIO().MousePos)) ||
        (!g_cap.pad && padB)) {   // 录键盘时按手柄 B = 取消（录手柄时 B 本身可以是组合里的键）
        EndCapture();
        return;
    }
    if (g_cap.pad && g_cap.armed &&
        (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
        SetCapturedPart(m, std::string());   // 录手柄时键盘不参与，Delete 直接就是清除
        return;
    }
    const bool held = g_cap.pad ? m.padHeld != 0 : !m.keysHeld.empty();
    if (!g_cap.armed) {
        if (!held) { g_cap.armed = true; g_cap.lastInput = now; }
        return;
    }
    if (held) {
        g_cap.lastInput = now;
        if (g_cap.pad) g_cap.padAcc |= m.padHeld;
        else
            for (int vk : m.keysHeld)
                if (std::find(g_cap.keyAcc.begin(), g_cap.keyAcc.end(), vk) == g_cap.keyAcc.end()) g_cap.keyAcc.push_back(vk);
        return;
    }
    if (!g_cap.padAcc && g_cap.keyAcc.empty()) return;   // 还没按
    const bool clear = g_cap.pad ? g_cap.padAcc == XINPUT_GAMEPAD_X
                                 : std::all_of(g_cap.keyAcc.begin(), g_cap.keyAcc.end(),
                                               [](int vk) { return vk == VK_DELETE || vk == VK_BACK; });
    if (clear) { SetCapturedPart(m, std::string()); return; }
    std::string why;
    const std::string combo = g_cap.pad ? PadComboSpec(g_cap.padAcc, why) : KeyComboSpec(g_cap.keyAcc, why);
    g_cap.padAcc = 0;
    g_cap.keyAcc.clear();
    if (combo.empty()) { g_capWhy = why; return; }      // 不能用：说明原因，接着录
    SetCapturedPart(m, combo);
}

constexpr float kHkLabelW = 0.30f;   // 快捷键行左边标签占的宽度比例

// 一格：某个快捷键的手柄组或键盘组。按 A / 点一下开始录（录的时候再点一下取消）
void HotkeyChip(const char* id, std::string& spec, bool pad, float w, float h, Model& m)
{
    std::string padPart, keyPart;
    SplitSpec(spec, padPart, keyPart);
    const std::string cur = pad ? padPart : keyPart;
    BeginCustom();
    const bool pressed = ImGui::InvisibleButton(id, ImVec2(w, h), ImGuiButtonFlags_EnableNav);
    EndCustom();
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    const bool focused = Focused(), hov = ImGui::IsItemHovered();
    bool mine = g_cap.spec == &spec && g_cap.pad == pad;
    if (pressed) {
        if (mine) EndCapture();
        else if (!g_cap.spec) {
            EndCapture();
            g_cap.spec = &spec;
            g_cap.pad = pad;
            g_cap.lastInput = ImGui::GetTime();
        }
        mine = g_cap.spec == &spec && g_cap.pad == pad;
    }
    if (mine) g_cap.rect = ImRect(a, b);
    if (focused && !g_cap.spec && !cur.empty() &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false) || ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
         ImGui::IsKeyPressed(ImGuiKey_Backspace, false))) {
        spec = pad ? keyPart : padPart;
        m.reqSave = true;
    }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rr = 12 * S;
    std::string text;
    ImU32 col = kText;
    if (mine) {
        const float t = 0.5f + 0.5f * std::sin((float)ImGui::GetTime() * 6.0f);
        dl->AddRectFilled(a, b, IM_COL32(226, 194, 122, (int)(22 + 28 * t)), rr);
        dl->AddRect(a, b, kGold, rr, 0, 2.0f * S);
        if (g_cap.armed) {
            std::vector<int> keys = g_cap.keyAcc;
            for (int vk : m.keysHeld) if (std::find(keys.begin(), keys.end(), vk) == keys.end()) keys.push_back(vk);
            text = pad ? PadComboText(g_cap.padAcc | m.padHeld) : KeyComboText(keys);
        }
        if (text.empty()) text = g_cap.armed ? i18n::Tr("请按键…", "Press a key…", "キーを押してください…")
                                            : i18n::Tr("先松开…", "Release first…", "先に離してください…");
        col = kGold;
    } else {
        dl->AddRectFilled(a, b, hov ? kSoftHot : kSoft, rr);
        if (focused) dl->AddRect(a - ImVec2(3, 3) * S, b + ImVec2(3, 3) * S, kGold, rr + 3 * S, 0, 2.0f * S);
        text = cur.empty() ? i18n::Tr("不设", "Not set", "未設定") : cur;
        col = cur.empty() ? kFaint : kText;
    }
    float fs = szBody;
    if (TextSize(fReg, fs, text.c_str()).x > w - 16 * S) fs = szSmall;   // 长组合缩小字号
    dl->PushClipRect(a, b, true);
    CenterText(dl, fReg, fs, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), col, text.c_str());
    dl->PopClipRect();
}

void HotkeyRow(const char* id, const char* label, std::string& spec, Model& m, bool padOk = true)
{
    const float w = ImGui::GetContentRegionAvail().x;
    const float rowH = 50 * S, h = 38 * S, gap = 8 * S;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddText(fReg, szBody, ImVec2(p.x + 14 * S, p.y + (rowH - szBody) * 0.5f), kMuted, label);
    const float x0 = p.x + w * kHkLabelW;
    const float cw = (w * (1.0f - kHkLabelW) - 6 * S - gap) * 0.5f;
    ImGui::PushID(id);
    if (padOk) {
        ImGui::SetCursorScreenPos(ImVec2(x0, p.y + (rowH - h) * 0.5f));
        HotkeyChip("##pad", spec, true, cw, h, m);
    } else {
        // 这一项设不了手柄键：画成不可聚焦的灰块（不建控件，手柄导航也不会停在这）
        const ImVec2 a(x0, p.y + (rowH - h) * 0.5f), b(x0 + cw, a.y + h);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, b, kSoft, 12 * S);
        CenterText(dl, fReg, szSmall, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), kFaint,
                   i18n::Tr("仅键盘", "Keyboard only", "キーボードのみ"));
    }
    ImGui::SetCursorScreenPos(ImVec2(x0 + cw + gap, p.y + (rowH - h) * 0.5f));
    HotkeyChip("##key", spec, false, cw, h, m);
    ImGui::PopID();
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH));
    ImGui::Dummy(ImVec2(0, 0));
}

// 两列的列名（手柄 / 键盘），和 HotkeyRow 的两格对齐
void HotkeyHeader()
{
    const float w = ImGui::GetContentRegionAvail().x, gap = 8 * S;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float x0 = p.x + w * kHkLabelW;
    const float cw = (w * (1.0f - kHkLabelW) - 6 * S - gap) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    CenterText(dl, fReg, szSmall, ImVec2(x0 + cw * 0.5f, p.y + szSmall * 0.6f), kFaint, i18n::Tr("手柄", "Gamepad", "パッド"));
    CenterText(dl, fReg, szSmall, ImVec2(x0 + cw + gap + cw * 0.5f, p.y + szSmall * 0.6f), kFaint, i18n::Tr("键盘", "Keyboard", "キーボード"));
    ImGui::Dummy(ImVec2(0, szSmall * 1.3f));
}

// ---------------------------------------------------------------------------
//  原版 config.txt 的选择框。用 ImGui 自己的弹窗，不弹系统对话框：
//  游戏几乎是独占全屏的，系统对话框一抢焦点，画面就冻在那儿了。
// ---------------------------------------------------------------------------
namespace {

const char* const kPickId = "###pickconfig";

struct PickEntry {
    std::wstring name;
    bool dir = false;
};

bool g_pickOpen = false;
std::wstring g_pickDir;
std::vector<PickEntry> g_pickList;

std::string U8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((std::size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

bool IsTxt(const std::wstring& n)
{
    if (n.size() < 5) return false;
    const std::size_t at = n.size() - 4;
    if (n[at] != L'.') return false;
    wchar_t a = n[at + 1], b = n[at + 2], c = n[at + 3];
    if (a >= L'A' && a <= L'Z') a = (wchar_t)(a - L'A' + L'a');
    if (b >= L'A' && b <= L'Z') b = (wchar_t)(b - L'A' + L'a');
    if (c >= L'A' && c <= L'Z') c = (wchar_t)(c - L'A' + L'a');
    return a == L't' && b == L'x' && c == L't';
}

std::wstring Join(const std::wstring& dir, const std::wstring& name)
{
    if (dir.empty()) return name;
    if (dir.back() == L'\\' || dir.back() == L'/') return dir + name;
    return dir + L"\\" + name;
}

std::wstring ParentDir(const std::wstring& d)
{
    std::wstring s = d;
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    const std::size_t p = s.find_last_of(L"\\/");
    if (p == std::wstring::npos) return s;
    if (p <= 2) return s.substr(0, 3);          // 盘符根的上一级还是它自己
    return s.substr(0, p);
}

void ScanDir(const std::wstring& dir)
{
    g_pickDir = dir;
    g_pickList.clear();
    WIN32_FIND_DATAW fd;
    HANDLE h = ::FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!std::wcscmp(fd.cFileName, L".") || !std::wcscmp(fd.cFileName, L"..")) continue;
            const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (!isDir && !IsTxt(fd.cFileName)) continue;      // 只列 txt，省得翻半天
            g_pickList.push_back({fd.cFileName, isDir});
        } while (::FindNextFileW(h, &fd));
        ::FindClose(h);
    }
    std::sort(g_pickList.begin(), g_pickList.end(), [](const PickEntry& a, const PickEntry& b) {
        if (a.dir != b.dir) return a.dir;                      // 目录排前面
        return a.name < b.name;
    });
}

std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH] = {};
    ::GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return ParentDir(buf);
}

void OpenPick()
{
    ScanDir(ExeDir());      // 从游戏目录起步，往上两层就能到盘符
    g_pickOpen = true;
}

void PickPopup(Model& m)
{
    static bool opened = false;
    if (!g_pickOpen) { opened = false; return; }
    if (opened && !ImGui::IsPopupOpen(kPickId)) { g_pickOpen = false; opened = false; return; }  // 被 B / Esc 关掉
    if (!opened) { ImGui::OpenPopup(kPickId); opened = true; }

    // 弹窗用和面板同一套配色：ImGui 默认的蓝标题栏跟这个界面差太远
    ImGui::SetNextWindowSize(ImVec2(680 * S, 470 * S), ImGuiCond_Appearing);
    const ImVec4 cardBg = V(IM_COL32(30, 26, 21, 250));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, cardBg);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, cardBg);
    ImGui::PushStyleColor(ImGuiCol_Border, V(IM_COL32(201, 164, 92, 90)));
    ImGui::PushStyleColor(ImGuiCol_Text, V(kText));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0.45f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 18 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 18 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22 * S, 18 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    if (!ImGui::BeginPopupModal(kPickId, nullptr,
                                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoMove)) {
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(5);
        return;
    }

    Text(fReg, szBody, kTitle, i18n::Tr("选原版 IN-Q 的 config.txt", "Pick the original IN-Q config.txt",
                                        "旧 IN-Q の config.txt を選ぶ"));
    ImGui::Dummy(ImVec2(0, 4 * S));
    ImGui::PushStyleColor(ImGuiCol_Text, kMuted);
    ImGui::TextWrapped("%s", U8(g_pickDir).c_str());
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0, 4 * S));

    if (PillButton("##pickup", i18n::Tr("上一级", "Up", "上へ"), 96 * S, 34 * S, kSecondary))
        ScanDir(ParentDir(g_pickDir));
    const DWORD drives = ::GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        char name[4] = {(char)('A' + i), ':', '\\', '\0'};
        char id[8] = {'#', '#', 'd', (char)('a' + i), '\0'};
        ImGui::SameLine(0, 6 * S);
        if (PillButton(id, name, 46 * S, 34 * S, kSecondary)) ScanDir(std::wstring(name, name + 3));
    }
    ImGui::Dummy(ImVec2(0, 6 * S));

    const float listH = ImGui::GetContentRegionAvail().y - 54 * S;   // 给底下的取消键留位置
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {   // 手柄 B 关掉
        g_pickOpen = false;
        opened = false;
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(5);
        return;
    }
    ImGui::BeginChild("##list", ImVec2(0, listH > 80 * S ? listH : 80 * S), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_None);
    if (g_pickList.empty()) {
        ImGui::Dummy(ImVec2(0, 6 * S));
        ImGui::PushStyleColor(ImGuiCol_Text, V(kMuted));
        Text(fReg, szSmall, kMuted, i18n::Tr("这层没有文件夹或 config.txt，点上面的盘符换个地方找",
                                             "No folders or config.txt here; try a drive above",
                                             "ここにフォルダも config.txt もありません"));
        ImGui::PopStyleColor();
    }
    for (const PickEntry& e : g_pickList) {
        std::string label = e.dir ? (std::string("> ") + U8(e.name)) : U8(e.name);
        label += "##";
        label += U8(e.name);
        if (ImGui::Selectable(label.c_str(), false, 0, ImVec2(0, 30 * S))) {
            if (e.dir) {
                ScanDir(Join(g_pickDir, e.name));
            } else {
                m.inqPath = Join(g_pickDir, e.name);
                m.reqImportInq = true;
                g_pickOpen = false;
                opened = false;
                ImGui::CloseCurrentPopup();
            }
        }
    }
    ImGui::EndChild();

    ImGui::Dummy(ImVec2(0, 6 * S));
    if (PillButton("##pickcancel", i18n::Tr("取消", "Cancel", "キャンセル"), 130 * S, 40 * S, kSecondary)) {
        g_pickOpen = false;
        opened = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(5);
}

}  // namespace

// ---------------------------------------------------------------------------
//  改名：X（手柄）/ F（键盘）弹出输入框，确定后写回配置（套装名字就是 ini 的段名）
// ---------------------------------------------------------------------------
void RenamePopup(Model& m)
{
    if (g_renameKind == kRenameNone) {
        g_renameOpen = false;
        return;
    }
    Config& c = *m.cfg;
    static const char* kId = "###rename";
    if (!g_renameOpen) {
        ImGui::OpenPopup(kId);
        g_renameOpen = true;
    } else if (!ImGui::IsPopupOpen(kId)) {
        g_renameKind = kRenameNone;
        g_renameOpen = false;
        g_renameErr.clear();
        g_renameCreate = false;
        const int after = g_afterRename;
        g_afterRename = 0;
        if (after == 1) Go(kSetEdit);
        else if (after == 2) Go(kFoodEdit);
        return;
    }

    ImGui::SetNextWindowSize(ImVec2(460 * S, 0), ImGuiCond_Appearing);
    const ImVec4 cardBg = V(IM_COL32(30, 26, 21, 250));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, cardBg);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, cardBg);
    ImGui::PushStyleColor(ImGuiCol_Border, V(IM_COL32(201, 164, 92, 90)));
    ImGui::PushStyleColor(ImGuiCol_Text, V(kText));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0.45f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 18 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, 18 * S);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22 * S, 18 * S));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    if (!ImGui::BeginPopupModal(kId, nullptr,
                                ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(5);
        return;
    }

    auto close = [&]() {
        g_renameKind = kRenameNone;
        g_renameOpen = false;
        g_renameErr.clear();
        g_renameCreate = false;
        const int after = g_afterRename;
        g_afterRename = 0;
        ImGui::CloseCurrentPopup();
        if (after == 1) Go(kSetEdit);        // 新建那套 / 那份猫饭，起完名直接进编辑页
        else if (after == 2) Go(kFoodEdit);
    };

    if (g_renameCloseReq) {
        g_renameCloseReq = false;
        close();
        ImGui::EndPopup();
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(5);
        return;
    }

    const bool isSet = g_renameKind == kRenameSet;
    const char* title = g_renameCreate
        ? (isSet ? i18n::Tr("新建套装", "New loadout", "新規セット")
                 : i18n::Tr("新建猫饭", "New meal", "新規猫飯"))
        : (isSet ? i18n::Tr("重命名套装", "Rename loadout", "セット名を変更")
                 : i18n::Tr("重命名猫饭", "Rename meal", "猫飯名を変更"));
    Text(fReg, szBody, kTitle, title);
    ImGui::Dummy(ImVec2(0, 6 * S));
    if (g_renameFocus) {
        ImGui::SetKeyboardFocusHere();
        g_renameFocus = false;
    }
    // 名字长的时候输入框跟着变宽（到上限为止），不挤在一个固定宽度里
    float want = ImGui::CalcTextSize(g_renameBuf).x + 120 * S;
    if (want < 420 * S) want = 420 * S;
    if (want > 760 * S) want = 760 * S;
    ImGui::SetNextItemWidth(want);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, V(IM_COL32(48, 42, 34, 255)));
    ImGui::PushStyleColor(ImGuiCol_Text, V(kText));
    std::string hint;
    if (g_renameCreate) {
        hint = isSet ? c.sets[g_renameId].name
                     : i18n::Fmt(i18n::Tr("猫饭 %d", "Meal %d", "猫飯 %d"), g_renameId);
    }
    const bool enter = ImGui::InputTextWithHint("##name", hint.c_str(), g_renameBuf, sizeof(g_renameBuf),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleColor(2);
    CutChars(g_renameBuf, kMaxNameChars);   // 粘一大段进来也只留前 50 个字
    // 输入框外面右下角：还能输几个字
    const int used = CountChars(g_renameBuf);
    const std::string cnt = i18n::Fmt("%d/%d", used, kMaxNameChars);
    const float cw = ImGui::CalcTextSize(cnt.c_str()).x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - cw);
    Text(fReg, szSmall, used >= kMaxNameChars ? IM_COL32(214, 122, 92, 255) : kMuted, cnt.c_str());
    ImGui::Dummy(ImVec2(0, 2 * S));
    if (!g_renameErr.empty()) Text(fReg, szSmall, IM_COL32(214, 122, 92, 255), g_renameErr.c_str());
    ImGui::Dummy(ImVec2(0, 8 * S));

    bool ok = enter || PillButton("##rnok", i18n::Tr("确定", "OK", "決定"), 150 * S, 44 * S, kPrimary);
    ImGui::SameLine(0, 10 * S);
    if (PillButton("##rncancel", i18n::Tr("取消", "Cancel", "キャンセル"), 150 * S, 44 * S, kSecondary) ||
        ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        close();
        ImGui::EndPopup();
        ImGui::PopStyleVar(4);
        ImGui::PopStyleColor(5);
        return;
    }
    if (ok) {
        const std::string name = TrimName(g_renameBuf);
        if (name.empty() && !g_renameCreate) {
            g_renameErr = i18n::Tr("名字不能为空", "name cannot be empty", "名前を入れてください");
        } else if (isSet) {
            // 名字只是显示用的，重名没关系；套用靠的是编号。新建时留空 = 用默认名
            if (!name.empty()) c.sets[g_renameId].name = name;
            m.reqSave = true;
            close();
        } else {
            auto it = c.foods.find(g_renameId);
            if (it != c.foods.end() && !name.empty()) it->second.name = name;
            m.reqSave = true;
            close();
        }
    }
    ImGui::EndPopup();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(5);
}

void PageSettings(Model& m)
{
    Config& c = *m.cfg;
    PageHeader(i18n::Tr("设置", "Settings", "設定"), nullptr, false);

    Text(fReg, szSmall, kFaint, i18n::Tr("语言", "Language", "言語"));
    ImGui::Dummy(ImVec2(0, 2 * S));
    {
        // 语言名不翻译：界面读不懂的时候就靠这行找回来
        static const char* const kNames[3] = {"中文", "English", "日本語"};
        static const char* const kCodes[3] = {"zh", "en", "ja"};
        const int cur = i18n::Current();
        const float lw = (ImGui::GetContentRegionAvail().x - 2 * 8 * S) / 3.0f;
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine(0, 8 * S);
            const bool on = cur == i;
            if (PillButton(i ? (std::string("##lg") + std::to_string(i)).c_str() : "##lg0", kNames[i], lw, 40 * S,
                           on ? kPrimary : kSecondary)) {
                c.language = kCodes[i];
                i18n::Set(i18n::FromString(c.language));
                m.reqSave = true;
            }
        }
    }
    ImGui::Dummy(ImVec2(0, 10 * S));

    Text(fReg, szSmall, kFaint, i18n::Tr("重开方式", "Restart mode", "再開方法"));
    ImGui::Dummy(ImVec2(0, 2 * S));
    const int mode = c.acceptOnly ? 2 : (c.reset ? 1 : 0);
    if (RadioRow("##m0", i18n::Tr("直接重开", "Restart directly", "直接再開"), nullptr, mode == 0)) { c.reset = false; c.acceptOnly = false; m.reqSave = true; }
    if (mode == 0) FocusDefault();
    if (RadioRow("##m1", i18n::Tr("重置后回据点再接", "Reset, go to base, accept again", "リセットして拠点で再受注"), nullptr, mode == 1)) { c.reset = true; c.acceptOnly = false; m.reqSave = true; }
    if (mode == 1) FocusDefault();
    if (RadioRow("##m2", i18n::Tr("只接任务，不出发", "Accept only, do not depart", "受注のみ、出発しない"), nullptr, mode == 2)) { c.reset = false; c.acceptOnly = true; m.reqSave = true; }
    if (mode == 2) FocusDefault();
    ImGui::Dummy(ImVec2(0, 10 * S));

    if (ToggleRow("##wy", i18n::Tr("翼龙起始", "Start with the wingdrake", "翼竜で開始"),
                  i18n::Tr("普通地图坐翼龙开局，关闭即为在营地开局",
                           "normal maps start on the wingdrake; off = start at camp",
                           "通常マップは翼竜で開始。オフなら拠点から開始"),
                  &c.wyvern)) m.reqSave = true;
    if (ToggleRow("##bo", i18n::Tr("缩短转场黑屏", "Shorten transition fades", "遷移の暗転を短縮"),
                  i18n::Tr("下次开游戏生效", "takes effect on the next launch", "次回起動時に反映"),
                  &c.quickBlackout)) m.reqSave = true;
    ImGui::Dummy(ImVec2(0, 10 * S));

    Text(fReg, szSmall, kFaint, i18n::Tr("面板", "Panel", "パネル"));
    ImGui::Dummy(ImVec2(0, 2 * S));
    {
        const float w = ImGui::GetContentRegionAvail().x;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddText(fReg, szBody, ImVec2(p.x + 14 * S, p.y + 10 * S), kMuted,
                                            i18n::Tr("不透明度", "Opacity", "不透明度"));
        ImGui::SetCursorScreenPos(ImVec2(p.x + w * 0.42f, p.y + 4 * S));
        ImGui::SetNextItemWidth(w * 0.58f - 6 * S);
        ImGui::PushFont(fReg, szBody);
        if (ImGui::SliderInt("##op", &c.panelOpacity, 30, 100, "%d%%", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput))
            m.reqSave = true;
        ImGui::PopFont();
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + 46 * S));
        ImGui::Dummy(ImVec2(0, 0));
    }
    ImGui::Dummy(ImVec2(0, 10 * S));

    Text(fReg, szSmall, kFaint, i18n::Tr("快捷键", "Hotkeys", "ホットキー"));
    ImGui::Dummy(ImVec2(0, 2 * S));
    HotkeyHeader();
    HotkeyRow("##hk1", i18n::Tr("重开", "Restart", "再開"), c.hkRestart, m);
    HotkeyRow("##hk3", i18n::Tr("打开面板", "Open panel", "パネルを開く"), c.hkPanel, m, false);
    HotkeyRow("##hk4", i18n::Tr("只重置任务", "Reset only", "リセットのみ"), c.hkResetOnly, m);
    ImGui::Dummy(ImVec2(0, 4 * S));
    const char* hint = !g_cap.spec
        ? i18n::Tr("选中一格按 A 或点一下，再按想用的键",
                   "select a cell (A or click), then press the keys you want",
                   "セルを選んで（A かクリック）使うキーを押す")
        : g_cap.pad
              ? i18n::Tr("按住想用的手柄键（至少两个），全部松开就记住 · Esc 取消",
                         "hold the buttons you want (at least two), release all to save · Esc cancels",
                         "使うボタンを押し続け（2 つ以上）、全部離すと記録 · Esc で中止")
              : i18n::Tr("按下想用的键，全部松开就记住 · Esc 取消",
                         "press the keys you want, release all to save · Esc cancels",
                         "使うキーを押し、全部離すと記録 · Esc で中止");
    Text(fReg, szSmall, g_capWhy.empty() ? kFaint : kWarn, g_capWhy.empty() ? hint : g_capWhy.c_str(), true);
    Text(fReg, szSmall, kFaint, i18n::Tr("不设：选中后，手柄那格单按 X，键盘那格单按 Delete",
                                         "to clear: focus a cell, then X (gamepad) / Delete (keyboard)",
                                         "未設定にする：セルを選び、パッドは X、キーボードは Delete"),
         true);
    ImGui::Dummy(ImVec2(0, 14 * S));

    Text(fReg, szSmall, kFaint, i18n::Tr("存档位", "Save slot", "セーブスロット"));
    ImGui::Dummy(ImVec2(0, 2 * S));
    {
        const int slots[3] = {1, 2, 3};
        const float gap = 10 * S;
        const float bw = (ImGui::GetContentRegionAvail().x - gap * 2) / 3;
        for (int i = 0; i < 3; ++i) {
            if (i) ImGui::SameLine(0, gap);
            const std::string id = "##slot" + std::to_string(slots[i]);
            const std::string label = std::to_string(slots[i]);
            const bool on = m.slot == slots[i];
            if (PillButton(id.c_str(), label.c_str(), bw, 44 * S, on ? kPrimary : kSecondary) && !on) {
                m.slot = slots[i];
                m.reqSlot = true;
            }
        }
    }
    ImGui::Dummy(ImVec2(0, 2 * S));
    Text(fReg, szSmall, kFaint,
         i18n::Tr("游戏里换存档之后来这里切一下；套装和猫饭按存档分开存",
                  "switch here after changing the save slot in game; loadouts are per slot",
                  "ゲームでスロットを変えたらここで切り替え。セットはスロットごとに保存されます"),
         true);
    ImGui::Dummy(ImVec2(0, 14 * S));

    Text(fReg, szSmall, kFaint,
         i18n::Tr("从原版 IN-Q 导入", "Import from the original IN-Q", "旧 IN-Q から取り込む"));
    ImGui::Dummy(ImVec2(0, 2 * S));
    if (PillButton("##impinq", i18n::Tr("选择 config.txt…", "Choose config.txt…", "config.txt を選ぶ…"),
                   ImGui::GetContentRegionAvail().x, 44 * S, kSecondary))
        OpenPick();
    ImGui::Dummy(ImVec2(0, 2 * S));
    Text(fReg, szSmall, kFaint,
         i18n::Tr("挑原版 IN-Q 文件夹里的 config.txt，里面的全套会整套搬过来",
                  "pick the config.txt inside the original IN-Q folder; its loadouts are copied over",
                  "旧 IN-Q フォルダの config.txt を選ぶと、セットをまとめて取り込みます"),
         true);
    ImGui::Dummy(ImVec2(0, 14 * S));

    ImGui::Dummy(ImVec2(0, 14 * S));

    Text(fReg, szSmall, kFaint, ("Better-IN-Q " + m.version).c_str());
    Text(fReg, szSmall, kFaint, i18n::Tr("原作者 年糕狗三世", "Original author: 年糕狗三世", "原作者：年糕狗三世"), true);
}

// ---------------------------------------------------------------------------
//  左侧栏。返回这一帧焦点在不在侧栏上。
// ---------------------------------------------------------------------------
bool Sidebar(float x, float y, float w, float h)
{
    // id 用固定的短串，别用会翻译的文字：切语言时 ImGui 的焦点记忆不该跟着变
    struct It { Page p; Icon ic; const char* id; const char* zh; const char* en; const char* ja; };
    static const It items[] = {{kHome, kIcFlag, "q", "任务", "Quests", "クエスト"},
                               {kSets, kIcShield, "s", "套装", "Loadouts", "装備セット"},
                               {kFood, kIcBowl, "f", "猫饭", "Meals", "猫飯"},
                               {kSettings, kIcGear, "g", "设置", "Settings", "設定"}};
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 o = ImGui::GetWindowPos();
    dl->AddLine(ImVec2(o.x + x + w, o.y + y + 14 * S), ImVec2(o.x + x + w, o.y + y + h - 14 * S),
                IM_COL32(201, 164, 92, 40), 1.0f);
    // 十字键：内容里往左就进侧栏（落在当前页那一格），在侧栏里上下移直接切页，往右或按 A 回到内容
    const Page cur = MainOf(g_page);
    ImGuiID ids[4] = {};
    int focused = -1, pressed = -1, curIdx = 0;
    for (int k = 0; k < 4; ++k) {
        ImGui::PushID(items[k].id);
        ids[k] = ImGui::GetID("##side");
        ImGui::PopID();
        if (items[k].p == cur) curIdx = k;
    }
    g_sideCurId = ids[curIdx];
    // 焦点不在侧栏时，侧栏不参与十字键导航：不然 ImGui 会按几何位置挑离得最近的一格
    //（从「重开」往左会先落到「猫饭」）。从内容区进侧栏由 Draw 接手，直接落到当前页那一格。
    const bool inSide = std::find(ids, ids + 4, GImGui->NavId) != ids + 4;
    if (!inSide) ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    float iy = y + 18 * S;
    for (int k = 0; k < 4; ++k) {
        const It& it = items[k];
        const float bw = 60 * S, bh = 62 * S;
        ImGui::SetCursorPos(ImVec2(x + (w - bw) * 0.5f, iy));
        ImGui::PushID(it.id);
        BeginCustom();
        if (ImGui::InvisibleButton("##side", ImVec2(bw, bh), ImGuiButtonFlags_EnableNav)) pressed = k;
        EndCustom();
        if (ImGui::IsItemFocused()) focused = k;
        ImGui::PopID();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        const bool on = cur == it.p;
        if (on) dl->AddRectFilled(a, b, IM_COL32(226, 194, 122, 40), 14 * S);
        else if (ImGui::IsItemHovered()) dl->AddRectFilled(a, b, IM_COL32(237, 230, 214, 14), 14 * S);
        if (focused == k && NavVisible()) dl->AddRect(a - ImVec2(3, 3) * S, b + ImVec2(3, 3) * S, kGold, 16 * S, 0, 2.0f * S);
        const ImU32 col = on ? kGold : kMuted;
        DrawIcon(dl, it.ic, ImVec2((a.x + b.x) * 0.5f, a.y + 22 * S), 10 * S, col);
        CenterText(dl, fReg, szSmall, ImVec2((a.x + b.x) * 0.5f, a.y + 46 * S), col, i18n::Tr(it.zh, it.en, it.ja));
        iy += bh + 6 * S;
    }
    if (!inSide) ImGui::PopItemFlag();
    g_sideFocusIdx = focused;
    const bool rightKey = ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, false) || ImGui::IsKeyPressed(ImGuiKey_RightArrow, false);
    if (pressed >= 0) {
        Go(items[pressed].p);              // 鼠标点 / 按 A：切过去，焦点进内容的默认项
        g_sideFocusedLast = false;
    } else if (focused >= 0 && rightKey) {
        g_focus = 2;                       // 往右：回到这一页的默认项（不让 ImGui 按几何位置乱挑）
        g_sideFocusedLast = false;
    } else if (focused >= 0) {
        if (!g_sideFocusedLast && focused != curIdx) {
            // 刚从内容移进来：落点是离原焦点最近的那格，不一定是当前页 —— 挪回当前页那格
            ImGui::SetFocusID(ids[curIdx], ImGui::GetCurrentWindow());
        } else if (focused != curIdx) {
            g_page = items[focused].p;     // 在侧栏里上下移：直接切页，焦点留在侧栏
            g_focus = 0;
        }
        g_sideFocusedLast = true;
    } else {
        if (g_sideFocusedLast) g_focus = 2;   // 从侧栏往右回到内容：落到这一页的默认项
        g_sideFocusedLast = false;
    }
    ImGui::SetCursorPos(ImVec2(x, y + h - 28 * S));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    CenterText(dl, fReg, szSmall * 0.9f, ImVec2(p.x + w * 0.5f, p.y), kFaint, i18n::Tr("B 返回", "B Back", "B 戻る"));
    return focused >= 0;
}

} // namespace

// ---------------------------------------------------------------------------
void Init(float scale)
{
    S = scale < 0.7f ? 0.7f : (scale > 2.5f ? 2.5f : scale);
    szBody = 19 * S;
    szSmall = 15 * S;
    szTitle = 34 * S;
    szPage = 26 * S;
    szBtn = 22 * S;

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;

    wchar_t win[MAX_PATH] = {};
    ::GetWindowsDirectoryW(win, MAX_PATH);
    char fonts[MAX_PATH * 3] = {};
    ::WideCharToMultiByte(CP_UTF8, 0, win, -1, fonts, sizeof(fonts), nullptr, nullptr);
    const std::string dir = std::string(fonts) + "\\Fonts\\";
    ImFontConfig fc;
    fc.OversampleH = 1;
    fReg = io.Fonts->AddFontFromFileTTF((dir + "msyh.ttc").c_str(), szBody, &fc);
    fBold = io.Fonts->AddFontFromFileTTF((dir + "msyhbd.ttc").c_str(), szBody, &fc);
    if (!fReg) fReg = io.Fonts->AddFontDefault();
    if (!fBold) fBold = fReg;
    io.FontDefault = fReg;

    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowRounding = 22 * S;
    st.WindowBorderSize = 1.0f;
    st.WindowPadding = ImVec2(0, 0);
    st.ChildRounding = 0;
    st.ChildBorderSize = 0;
    st.PopupRounding = 16 * S;
    st.PopupBorderSize = 1.0f;
    st.FrameRounding = 12 * S;
    st.FramePadding = ImVec2(14 * S, 10 * S);
    st.ItemSpacing = ImVec2(8 * S, 6 * S);
    st.GrabRounding = 10 * S;
    st.GrabMinSize = 18 * S;
    st.ScrollbarSize = 6 * S;
    st.ScrollbarRounding = 6 * S;
    st.SelectableTextAlign = ImVec2(0, 0.5f);

    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = V(kText);
    c[ImGuiCol_TextDisabled] = V(kMuted);
    c[ImGuiCol_WindowBg] = ImVec4(0.063f, 0.051f, 0.035f, 0.80f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = ImVec4(0.094f, 0.078f, 0.055f, 0.97f);
    c[ImGuiCol_Border] = V(IM_COL32(201, 164, 92, 90));
    c[ImGuiCol_FrameBg] = ImVec4(0, 0, 0, 0.35f);
    c[ImGuiCol_FrameBgHovered] = V(IM_COL32(226, 194, 122, 30));
    c[ImGuiCol_FrameBgActive] = V(IM_COL32(226, 194, 122, 46));
    c[ImGuiCol_Header] = V(IM_COL32(226, 194, 122, 30));
    c[ImGuiCol_HeaderHovered] = V(IM_COL32(237, 230, 214, 18));
    c[ImGuiCol_HeaderActive] = V(IM_COL32(226, 194, 122, 56));
    c[ImGuiCol_SliderGrab] = V(kGold);
    c[ImGuiCol_SliderGrabActive] = V(kGoldHot);
    c[ImGuiCol_CheckMark] = V(kGold);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = V(IM_COL32(237, 230, 214, 40));
    c[ImGuiCol_ScrollbarGrabHovered] = V(IM_COL32(237, 230, 214, 70));
    c[ImGuiCol_ScrollbarGrabActive] = V(IM_COL32(226, 194, 122, 120));
    c[ImGuiCol_NavCursor] = V(kGold);
    c[ImGuiCol_TextSelectedBg] = V(IM_COL32(226, 194, 122, 80));
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.3f);
}

void DebugGo(int page, int editIndex)
{
    static const Page map[] = {kHome, kQuests, kSets, kSetEdit, kFood, kFoodEdit, kSettings};
    if (page < 0 || page > 6) return;
    g_editSet = editIndex;
    g_editFood = editIndex;
    g_questTarget = -1;
    g_questFrom = kHome;
    Go(map[page]);
}

float DebugListScroll()
{
    return g_listScroll;
}

int DebugPage()
{
    static const Page map[] = {kHome, kQuests, kSets, kSetEdit, kFood, kFoodEdit, kSettings};
    for (int i = 0; i < 7; ++i) if (map[i] == g_page) return i;
    return -1;
}

bool DebugSidebarFocused() { return g_sideFocusedLast; }
int DebugSidebarFocusIndex() { return g_sideFocusIdx; }
float DebugFrameHeight() { return g_frameH; }

void DebugStartCapture(Model& m, int row, bool pad)
{
    std::string* const specs[] = {&m.cfg->hkRestart, &m.cfg->hkPanel, &m.cfg->hkResetOnly};
    if (row < 0 || row > 2) return;
    g_page = kSettings;
    EndCapture();
    g_cap.spec = specs[row];
    g_cap.pad = pad;
    g_cap.lastInput = ImGui::GetTime();
}

std::string DebugCaptureWhy() { return g_capWhy; }

void DebugSetSearch(const char* text) { std::snprintf(g_search, sizeof(g_search), "%s", text); }
void DebugOpenPick() { OpenPick(); }
void DebugClosePick() { g_pickOpen = false; }
void DebugOpenRename() { g_debugRename = true; }
void DebugCloseRename() { g_renameCloseReq = true; }

std::string DebugClampName(const char* utf8)
{
    char tmp[256];
    std::snprintf(tmp, sizeof(tmp), "%s", utf8 ? utf8 : "");
    CutChars(tmp, kMaxNameChars);
    return std::string(tmp);
}

void DebugMove(int kind, int id)
{
    g_moveKind = kind == 1 ? kMoveSet : (kind == 2 ? kMoveFood : kMoveNone);
    g_moveId = id;
}

bool DebugMoveActive() { return g_moveKind != kMoveNone; }
int DebugMoveId() { return g_moveId; }

void DebugNewSet(Model& m)
{
    Config& c = *m.cfg;
    LoadoutSet s;
    s.id = c.NextSetId();
    s.name = i18n::Fmt(i18n::Tr("套装 %d", "Loadout %d", "セット %d"), s.id);
    c.sets.push_back(s);
    g_editSet = (int)c.sets.size() - 1;
    g_afterRename = 1;
    OpenRenameCreate(m, kRenameSet, g_editSet);
    m.reqSave = true;
}

void OnOpen(Model& m)
{
    g_page = kHome;
    g_focus = 2;
    g_search[0] = 0;
    g_animHc = 0;
    g_popupLastFrame = false;
    EndCapture();
    m.capturing = false;
    ImGui::SetNavCursorVisible(true);
}

void Draw(Model& m, float screenW, float screenH)
{
    if (!m.cfg) return;
    const float W = 560 * S;
    const float sideW = 86 * S;
    const float pad = 26 * S;
    const ImVec2 inPad(12 * S, 8 * S);   // 内容区内边距：控件的焦点描边画在控件外面，贴边会被裁
    const float statusH = m.status.empty() ? 0 : szSmall + 14 * S;
    // 内容区高度按这一页上一帧量出来的实际内容收紧（主页就很矮），长的页面封顶后滚动；
    // 换任务是长列表，直接用满
    const float maxHc = 560 * S, minHc = 300 * S;
    const bool measured = g_page != kQuests && g_contentH[g_page] > 0;
    const float hc = measured ? std::fmin(maxHc, std::fmax(minHc, g_contentH[g_page])) : maxHc;
    // 换页时面板外框的高度平滑过渡（约 0.2 秒），以屏幕中线为轴上下一起收放。
    // 只有画出来的外框在动：ImGui 窗口一换页就放在最终的位置和大小，内容按最终排版，
    // 外框外面的部分这一帧裁掉。窗口自己每帧挪动或变高的话，ImGui 的十字键导航和滚动定位会乱。
    // 第一次进这页还没量过：先别动，下一帧量完再变。刚打开面板就碰上没量过的页时 g_animHc 还是 0，
    // 这一帧整个不画（外框高度 0，内容全裁掉），下一帧量完直接到位，免得先撑满再缩回来
    float target = hc;
    if (g_page != kQuests && !measured) target = g_animHc;
    if (g_animHc <= 0) g_animHc = target;
    else g_animHc += (target - g_animHc) * (1.0f - std::exp(-ImGui::GetIO().DeltaTime * 16.0f));
    if (std::fabs(target - g_animHc) < 0.5f) g_animHc = target;
    const float H = (pad - inPad.y) + hc + statusH + pad;               // 这一页的最终大小
    const float Hf = g_animHc > 0 ? (pad - inPad.y) + g_animHc + statusH + pad : 0.0f;   // 看得见的外框（动画中）
    // 左边有侧栏、上面是标题，正中摆放时视觉重心会偏右下。整体往左挪四分之一个侧栏宽、
    // 再往上提一点，主页的「重开」落在屏幕正中略偏上
    const float cy = std::floor(screenH * 0.5f - 26 * S);
    const ImVec2 pos(std::floor((screenW - W) * 0.5f - sideW * 0.25f), std::floor(cy - H * 0.5f));
    const ImVec2 fMin(pos.x, std::floor(cy - Hf * 0.5f)), fMax(pos.x + W, std::floor(cy - Hf * 0.5f) + Hf);
    g_frameH = Hf;
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(W, H), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);   // 底和边框自己画（按动画中的外框大小）
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    const ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    const bool shown = ImGui::Begin("IN-Q##panel", nullptr, wf);
    ImGui::PopStyleVar();
    if (!shown) { ImGui::End(); return; }
    if (ImGui::IsWindowAppearing()) ImGui::SetWindowFocus();
    ImGuiWindow* const panel = ImGui::GetCurrentWindow();
    {
        const ImGuiStyle& st = ImGui::GetStyle();
        ImVec4 bg = st.Colors[ImGuiCol_WindowBg];
        bg.w = m.cfg->panelOpacity / 100.0f;
        ImDrawList* bdl = Hf > 0 ? ImGui::GetBackgroundDrawList() : nullptr;
        if (bdl) bdl->AddRectFilled(fMin, fMax, ImGui::ColorConvertFloat4ToU32(bg), st.WindowRounding);
        if (bdl) bdl->AddRect(fMin, fMax, ImGui::GetColorU32(ImGuiCol_Border), st.WindowRounding, 0, 1.0f);
    }

    // 录快捷键时手柄和键盘都不用来导航（按什么都是在录），下一帧起生效
    const bool capturing = g_cap.spec != nullptr;
    UpdateCapture(m);
    const ImGuiConfigFlags navFlags = ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
    if (g_cap.spec || g_moveKind != kMoveNone) ImGui::GetIO().ConfigFlags &= ~navFlags;
    else ImGui::GetIO().ConfigFlags |= navFlags;

    // B / Esc 返回（主页上就是关面板）。弹窗、输入框、录快捷键时不算。
    // 弹窗里按 B 是关弹窗：ImGui 在这一帧开头就已经把弹窗关掉了，所以还要看上一帧开没开着，
    // 不然同一下 B 又会被当成「返回上一页」
    const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) || g_popupLastFrame;
    const bool typing = ImGui::GetIO().WantTextInput;
    // 移动模式里 B / Esc 是「放下」，由列表页自己吃掉，这里不能再当成「返回上一页」
    if (!popup && !typing && !capturing && g_moveKind == kMoveNone &&
        (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
        if (Back(g_page) != g_page) Go(Back(g_page));
        else if (g_page != kHome) Go(kHome);
        else m.reqClose = true;
    }

    const bool movingNow = g_moveKind != kMoveNone;
    if (movingNow) ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    const bool sideNav = Sidebar(0, 0, sideW, H);
    if (movingNow) ImGui::PopItemFlag();

    // 焦点在侧栏时，内容区不参与十字键导航：上下只在侧栏里走，不会被右边离得近的一行抢走；
    // 回内容区由侧栏自己处理（按右 / 按 A 落到这一页的默认项）
    if (sideNav) ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
    ImGui::SetCursorPos(ImVec2(sideW + pad - inPad.x, pad - inPad.y));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, inPad);
    ImGui::BeginChild("##content", ImVec2(W - sideW - pad * 2 + inPad.x * 2, hc),
                      ImGuiChildFlags_NavFlattened | ImGuiChildFlags_AlwaysUseWindowPadding,
                      g_page == kQuests ? ImGuiWindowFlags_NoScrollbar : 0);
    ImGui::PopStyleVar();
    g_focusUsed = false;
    const Page drawn = g_page;
    switch (g_page) {
    case kHome: PageHome(m); break;
    case kQuests: PageQuests(m); break;
    case kSets: PageSets(m); break;
    case kSetEdit: PageSetEdit(m); break;
    case kFood: PageFood(m); break;
    case kFoodEdit: PageFoodEdit(m); break;
    case kSettings: PageSettings(m); break;
    }
    // 默认项这一帧没画出来（比如被搜索过滤掉了）就作废，免得之后抢焦点。
    // 但这一帧中途换了页的话，请求是给新页的，留到下一帧
    if (!g_focusUsed && g_page == drawn) g_focus = 0;
    if (g_debugRename) {
        g_debugRename = false;
        if (g_page == kSets && !m.cfg->sets.empty()) OpenRename(m, kRenameSet, 0);
        else if (g_page == kFood && !m.cfg->foods.empty()) OpenRename(m, kRenameFood, m.cfg->foods.begin()->first);
    }
    g_contentH[drawn] = ImGui::GetCursorPosY() + inPad.y;   // 这一页内容实际多高，下一帧按它收紧面板
    ImGui::EndChild();
    if (sideNav) ImGui::PopItemFlag();

    // 十字键往左、内容区里左边已经没有能去的控件：直接去侧栏当前页那一格。
    // 这一帧所有控件都已经参与过打分，结果是空的就取消这次移动，换成我们指定的落点。
    ImGuiContext& g = *GImGui;
    if (!sideNav && !movingNow && !popup && g_sideCurId && g.NavMoveSubmitted && g.NavMoveScoringItems && g.NavMoveDir == ImGuiDir_Left &&
        !g.NavMoveResultLocal.ID && !g.NavMoveResultLocalVisible.ID && !g.NavMoveResultOther.ID) {
        ImGui::NavMoveRequestCancel();
        ImGui::SetFocusID(g_sideCurId, ImGui::GetCurrentWindow());
        ImGui::SetNavCursorVisible(true);
    }
    m.capturing = g_cap.spec != nullptr;
    g_popupLastFrame = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);

    if (!m.status.empty()) {
        ImGui::SetCursorPos(ImVec2(sideW + pad, (pad - inPad.y) + hc + 6 * S));
        Text(fReg, szSmall, kMuted, m.status.c_str());
    }
    ImGui::End();

    // 外框在收放时，面板（连同里面的子窗口）这一帧画出来的东西裁到外框里。弹窗是独立的窗口，不受影响。
    if (Hf < H - 0.5f)
        for (ImGuiWindow* w : GImGui->Windows)
            if (w->RootWindow == panel && w->WasActive && w->DrawList)
                for (ImDrawCmd& c : w->DrawList->CmdBuffer) {
                    c.ClipRect.y = std::fmax(c.ClipRect.y, fMin.y);
                    c.ClipRect.w = std::fmin(c.ClipRect.w, fMax.y);
                }

    PickPopup(m);
    RenamePopup(m);
}

} // namespace ui
