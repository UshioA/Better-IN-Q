// MinGW 的 d3d12.h 默认按 GCC 的规矩返回结构体（GetDesc、GetCPUDescriptorHandleForHeapStart 之类），
// 和 COM 实际的调用约定（隐藏的返回指针）对不上，会读到错的值。打开这个开关就按 COM 的来。
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include "overlay.h"

#include "hdr.h"
#include "i18n.h"
#include "log.h"
#include "panel_gamepad_gate.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <imm.h>
#include <atomic>
#include <array>
#include <windowsx.h>
#include <mutex>
#include <cstdio>
#include <string>
#include <vector>

#include <cfloat>

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

extern ImGuiKey ImGui_ImplWin32_KeyEventToImGuiKey(WPARAM, LPARAM);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace overlay {
namespace {

typedef HRESULT(WINAPI* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(WINAPI* ResizeFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(WINAPI* Resize1Fn)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
typedef void(WINAPI* ExecuteFn)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

enum Api { kNone, kD3D11, kD3D12 };

std::recursive_mutex g_rendererMutex;
Callbacks g_cb;
PresentFn g_present = nullptr;
ResizeFn g_resize = nullptr;
Resize1Fn g_resize1 = nullptr;
// One publication carries both visibility and open generation. Separate atomics
// could expose open=true before its new generation (or mix two transitions).
std::atomic<unsigned long long> g_openState{0}; // bit 0 open; upper bits generation
unsigned long long g_seenOpenGeneration = 0;   // render-thread only
bool g_wasOpen = false;
bool PanelOpen() { return (g_openState.load() & 1) != 0; }
PanelGamepadReleaseGate g_panelGamepadGate;
std::atomic<bool> g_inited{false};
Api g_api = kNone;
std::atomic<bool> g_failed{false};
std::atomic<unsigned long long> g_openAt{0};   // 这次打开面板的时间（还没初始化成功时，用来判断要不要放弃）
IDXGISwapChain* g_sc = nullptr;                // 游戏的交换链（只比指针，不持有引用）
IDXGISwapChain* g_foreign = nullptr;           // 上一个确认过不是游戏的交换链，免得每帧再查
std::atomic<HWND> g_hwnd{nullptr};
// The predecessor belongs to the HWND, not to a renderer generation. Another
// overlay may wrap us; rebuilding must never install us above that wrapper.
constexpr wchar_t kPreviousProc[] = L"BetterINQ.PreviousWndProc.8c6f10a";
WNDPROC PreviousProc(HWND h)
{
    return reinterpret_cast<WNDPROC>(::GetPropW(h, kPreviousProc));
}

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
thread_local int g_wndDepth = 0;   // 窗口过程的重入层数，见 WndProc 顶部

// 只跟踪当前活动调用；节点在调用栈上，不分配堆、不留下永久消息槽。
// HWND 也是键的一部分：同一消息同步发给另一个窗口不是转发链成环。
struct MsgNest;
thread_local MsgNest* g_msgNest = nullptr;
struct MsgNest {
    HWND hwnd;
    UINT msg;
    MsgNest* previous;
    MsgNest(HWND h, UINT m) : hwnd(h), msg(m), previous(g_msgNest) { g_msgNest = this; }
    ~MsgNest() { g_msgNest = previous; }
};

int NestOf(HWND hwnd, UINT msg)
{
    int count = 0;
    for (const MsgNest* entry = g_msgNest; entry; entry = entry->previous)
        if (entry->hwnd == hwnd && entry->msg == msg) ++count;
    return count;
}

// 前驱窗口过程。首选 SetPropW 存下的不可变值；万一 property 没了（别的插件
// RemovePropW、或者我们先前装过又拆过），退到类过程，仍然不许把消息交给
// DefWindowProcW —— WM_CLOSE 的默认处理就是 DestroyWindow，游戏收不到关闭
// 通知，进程会留在后台（"窗口没了、BGM 还在"就是这么来的）。
// 注意别在这里判重入层数：DestroyWindow 之类的嵌套消息也会把层数抬高，按层数
// 关掉兜底会让 WM_DESTROY 落到 DefWindowProcW（实测丢掉 destroy/ncdestroy）。
// 自环靠 WndProc 顶部的层数闸门兜住，不靠这里。
WNDPROC ResolvePrevious(HWND h, WNDPROC previous)
{
    if (previous && previous != WndProc) return previous;
    const WNDPROC cls = reinterpret_cast<WNDPROC>(::GetClassLongPtrW(h, GCLP_WNDPROC));
    if (cls && cls != WndProc) return cls;
    return nullptr;
}

// 窗口类过程：游戏真正注册的那个窗口过程。实测游戏切场景时会把窗口过程临时换成
// WINMM 里的函数（21:18 那段日志里前驱是 WINMM+0x4A5F0），转发给它等于把消息塞回
// 我们自己，所以一旦发现同一条消息回到手里，就改投这里。
WNDPROC ClassProc(HWND h)
{
    const WNDPROC cls = reinterpret_cast<WNDPROC>(::GetClassLongPtrW(h, GCLP_WNDPROC));
    return (cls && cls != WndProc) ? cls : nullptr;
}

// 把地址写成「模块+偏移」，日志里才看得懂接管的是谁的程序
void ModuleOf(const void* p, char* buf, size_t cap)
{
    buf[0] = 0;
    HMODULE mod = nullptr;
    if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(p), &mod) ||
        !mod) {
        snprintf(buf, cap, "%p", p);
        return;
    }
    wchar_t path[MAX_PATH]{};
    ::GetModuleFileNameW(mod, path, MAX_PATH);
    const wchar_t* base = wcsrchr(path, L'\\');
    base = base ? base + 1 : path;
    char name[MAX_PATH * 2]{};
    ::WideCharToMultiByte(CP_UTF8, 0, base, -1, name, (int)sizeof(name) - 1, nullptr, nullptr);
    const unsigned long long off = (unsigned long long)((const unsigned char*)p - (const unsigned char*)mod);
    snprintf(buf, cap, "%s+0x%llX", name, off);
}

// 关窗相关消息的埋点：只打这几条（全会话也就几行），用来判断「关窗口」到底
// 有没有送到游戏手里。
void TraceWindowMessage(HWND h, UINT msg, WPARAM w, WNDPROC previous)
{
    const char* name = nullptr;
    switch (msg) {
    case WM_CLOSE: name = "WM_CLOSE"; break;
    case WM_DESTROY: name = "WM_DESTROY"; break;
    case WM_NCDESTROY: name = "WM_NCDESTROY"; break;
    case WM_QUERYENDSESSION: name = "WM_QUERYENDSESSION"; break;
    case WM_ENDSESSION: name = "WM_ENDSESSION"; break;
    default:
        if (msg == WM_SYSCOMMAND && (w & 0xFFF0) == SC_CLOSE) name = "WM_SYSCOMMAND/SC_CLOSE";
        break;
    }
    if (!name) return;
    const WNDPROC target = ResolvePrevious(h, previous);
    Log("window: %s 前驱=%p%s", name, reinterpret_cast<void*>(target),
        target == previous ? "" : "（property 丢了，用类过程兜底）");
}

LRESULT ForwardToGame(HWND h, UINT msg, WPARAM w, LPARAM l, WNDPROC previous)
{
    // 同一条消息又回到我们手里（nest > 1）：前驱是那种「把消息再派回窗口」的东西，
    // 继续喂它只会套娃。改投窗口类过程 —— 那才一定是游戏自己的窗口过程。
    const int nest = NestOf(h, msg);
    if (nest > 1) {
        const WNDPROC cls = ClassProc(h);
        if (cls && cls != previous) {
            if (nest == 2)
                Log("wnd: 消息 0x%04X 又回到自己手里，改投窗口类过程", (unsigned)msg);
            return ::CallWindowProcW(cls, h, msg, w, l);
        }
    }
    if (previous && previous != WndProc) return ::CallWindowProcW(previous, h, msg, w, l);
    const WNDPROC cls = ClassProc(h);
    if (cls) return ::CallWindowProcW(cls, h, msg, w, l);
    return ::DefWindowProcW(h, msg, w, l);
}

// 崩溃诊断：当前在哪个钩子/阶段（异常处理里会读它）
std::atomic<const char*> g_stage{"idle"};
void SetStage(const char* s) { g_stage = s; }
// 重入计数：我们自己的渲染里再触发 Present/面板渲染会无限套娃（栈溢出）
thread_local int g_depth = 0;
void (*g_debugAfterDraw)(void*) = nullptr;
void (*g_debugAfterDraw12)(void*, void*) = nullptr;
ImU32 g_lastScaledEvent = 0;

// ---- D3D11
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;

// ---- D3D12
// 交换链拿不到它的命令队列，只能挂 ExecuteCommandLists，等游戏自己提交时把队列记下来
ExecuteFn g_execute = nullptr;
std::atomic<ID3D12Device*> g_wantQueueOf{nullptr};   // 要找哪个设备的队列
std::atomic<ID3D12CommandQueue*> g_queue{nullptr};
ID3D12Device* g_dev12 = nullptr;
IDXGISwapChain3* g_sc3 = nullptr;
ID3D12DescriptorHeap* g_rtvHeap = nullptr;
ID3D12DescriptorHeap* g_srvHeap = nullptr;
UINT g_rtvInc = 0, g_srvInc = 0;
std::vector<int> g_srvFree;                    // ImGui 要的纹理描述符，从 g_srvHeap 里分
ID3D12GraphicsCommandList* g_list = nullptr;
ID3D12Fence* g_fence = nullptr;
UINT64 g_fenceVal = 0;
HANDLE g_fenceEvent = nullptr;
constexpr UINT kMaxBuffers = 16;
ID3D12CommandAllocator* g_alloc[kMaxBuffers] = {};
UINT64 g_allocFence[kMaxBuffers] = {};
DXGI_FORMAT g_bbFormat = DXGI_FORMAT_UNKNOWN;

// ---- 输入法
// 游戏聊天也管理 IME。HIMC/owner 只由窗口线程访问；跨线程只发布期望状态。
HIMC g_imc = nullptr;
HIMC g_imcPrev = nullptr;
HWND g_imeOwner = nullptr;
std::atomic<bool> g_imeWant{false};
std::atomic<unsigned> g_imeGeneration{0};
bool g_imeAppliedWant = false;
thread_local bool g_imeSyncing = false;
int g_imeStart = 0, g_imeComp = 0, g_imeResult = 0, g_imeChar = 0, g_imeFix = 0;   // 诊断用

// 输入法上下文（HIMC）属于线程，而且只有窗口自己的线程去挂才有用：
// MHW 的主线程建窗口，Present 却在渲染线程上，所以这里必须把 IMM 调用
// 投递到窗口线程去执行，不能顺手在 Present 里干。
constexpr UINT kImeSync = WM_APP + 0x51;   // 让窗口线程去碰输入法上下文
constexpr UINT kImePos = WM_APP + 0x52;    // 让窗口线程去摆组字窗/候选窗的位置
std::atomic<bool> g_imeAttached{false};
std::atomic<bool> g_imeEnabled{true};
// 游戏窗口已经没了（要退出了）：面板逻辑一律停用，Present 只做转发。
// 窗口销毁后继续提交面板可能碰到失效资源；这里阻止新的插件渲染，
// 不据此断言它是所有退出挂起的唯一原因。
std::atomic<bool> g_dead{false};
// 是否接管游戏窗口过程（[Debug] NoWndHook=1 时关掉，排查「退出不退」和闪退用）
std::atomic<bool> g_wndHook{true};
unsigned g_imeTick = 0;

HIMC ImeContext(HWND h)
{
    HIMC c = ::ImmGetContext(h);
    if (c) ::ImmReleaseContext(h, c);
    return c;
}

void ImeOffWindowThread(HWND h = nullptr)
{
    if (!h) h = g_imeOwner;
    if (!h || h != g_imeOwner || g_imeSyncing ||
        ::GetWindowThreadProcessId(h, nullptr) != ::GetCurrentThreadId()) return;
    g_imeSyncing = true; // IMM 会同步重入 WndProc，不能二次恢复/销毁。
    const HIMC owned = g_imc;
    const HIMC previous = g_imcPrev;
    g_imc = nullptr;
    g_imcPrev = nullptr;
    g_imeOwner = nullptr;
    if (owned) {
        // 游戏已经换成其他上下文（包括 nullptr）时，不能覆盖它的新决定。
        if (ImeContext(h) == owned) {
            ::ImmNotifyIME(owned, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
            ::ImmAssociateContext(h, previous);
        }
        ::ImmDestroyContext(owned);
    }
    g_imeAppliedWant = false;
    g_imeSyncing = false;
    g_imeStart = g_imeComp = g_imeResult = g_imeChar = g_imeFix = 0;
}

void ImeSyncOnWindowThread(HWND h)
{
    if (!h || g_imeSyncing || ::GetWindowThreadProcessId(h, nullptr) != ::GetCurrentThreadId()) return;
    const bool attach = h == g_hwnd && PanelOpen() && g_inited.load() &&
        !g_dead.load() && g_wndHook.load() && g_imeEnabled.load() && g_imeAttached.load();
    if (!attach) { ImeOffWindowThread(h); return; }
    // 不允许新 HWND 接管另一个线程/窗口拥有的 HIMC。
    if (g_imeOwner && g_imeOwner != h) return;
    g_imeSyncing = true;
    if (!g_imc) {
        g_imc = ::ImmCreateContext();
        if (!g_imc) { g_imeSyncing = false; return; }
        g_imeOwner = h;
        g_imcPrev = ::ImmAssociateContext(h, g_imc);
        g_imeAppliedWant = !g_imeWant.load(); // 强制应用初始开关状态
    } else if (ImeContext(h) != g_imc) {
        // 保存游戏最新的关联，包含“主动摘下”的 nullptr。
        g_imcPrev = ::ImmAssociateContext(h, g_imc);
        ++g_imeFix;
    }
    const bool want = g_imeWant.load();
    if (want != g_imeAppliedWant) {
        ::ImmSetOpenStatus(g_imc, want ? TRUE : FALSE);
        g_imeAppliedWant = want;
    }
    g_imeSyncing = false;
}

// 组字窗和候选窗要摆在输入框底下。ImGui 给的 InputPos 是「后台缓冲」坐标（我们改过
// io.DisplaySize），输入法要的是窗口客户区坐标；只有 1:1 时才一样，系统缩放 125% 时
// 缓冲比客户区大，不换算的话这个点会被判成「在窗口外面」，输入法就把窗口丢到屏幕角上。
void ImeSetPosOnWindowThread(HWND h, int x, int y, unsigned generation)
{
    if (generation != g_imeGeneration.load() || !PanelOpen() || !g_imeAttached.load() ||
        !g_imeWant.load() || h != g_imeOwner || !g_imc || g_imeSyncing ||
        ::GetWindowThreadProcessId(h, nullptr) != ::GetCurrentThreadId()) return;
    HIMC c = ::ImmGetContext(h);
    if (!c) return;
    if (c != g_imc) { ::ImmReleaseContext(h, c); return; }
    COMPOSITIONFORM cf{};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos.x = x;
    cf.ptCurrentPos.y = y;
    ::ImmSetCompositionWindow(c, &cf);
    CANDIDATEFORM cand{};
    cand.dwIndex = 0;
    cand.dwStyle = CFS_CANDIDATEPOS;
    cand.ptCurrentPos.x = x;
    cand.ptCurrentPos.y = y + 2;
    ::ImmSetCandidateWindow(c, &cand);
    ::ImmReleaseContext(h, c);
}

void ImeSetPos(int x, int y)
{
    if (!g_hwnd || !PanelOpen() || !g_imeAttached.load()) return;
    const unsigned generation = g_imeGeneration.load();
    if (::GetWindowThreadProcessId(g_hwnd, nullptr) == ::GetCurrentThreadId())
        ImeSetPosOnWindowThread(g_hwnd, x, y, generation);
    else {
        // 客户区坐标用有符号16位打包；wParam独立携带生命周期票据，无堆对象残留。
        ::PostMessageW(g_hwnd, kImePos, generation, MAKELPARAM((short)x, (short)y));
    }
}

// 换掉 ImGui 默认的输入法摆位实现：它不知道我们把 DisplaySize 设成了缓冲分辨率
void SetImeDataFn(ImGuiContext*, ImGuiViewport*, ImGuiPlatformImeData* data)
{
    if (!data || !data->WantVisible || !g_hwnd) return;
    RECT rc{};
    if (!::GetClientRect(g_hwnd, &rc)) return;
    const ImGuiIO& io = ImGui::GetIO();
    const float fx = io.DisplaySize.x > 0 ? (float)(rc.right - rc.left) / io.DisplaySize.x : 1.0f;
    const float fy = io.DisplaySize.y > 0 ? (float)(rc.bottom - rc.top) / io.DisplaySize.y : 1.0f;
    ImeSetPos((int)(data->InputPos.x * fx), (int)((data->InputPos.y + data->InputLineHeight) * fy));
}

void ImeRequest(bool attach, bool want)
{
    if (g_imeAttached.exchange(attach) != attach) ++g_imeGeneration;
    g_imeWant = want;
    if (!g_hwnd) return;
    if (::GetWindowThreadProcessId(g_hwnd, nullptr) == ::GetCurrentThreadId()) {
        ImeSyncOnWindowThread(g_hwnd);
        return;
    }
    // 消息只是唤醒；处理时读取最新期望值，旧消息不能重挂/摧毁新一代上下文。
    if (!::PostMessageW(g_hwnd, kImeSync, 0, 0))
        Log("ime: 投递同步消息失败 %lu", ::GetLastError());
}

// WndProc never touches ImGui or waits for the renderer. Normalize thread-bound
// Win32 state here, then hand off values only. The small queue lock never spans
// IMM/User32/backend calls, callbacks, or renderer locking (including on drain).
std::atomic<unsigned long long> g_inputEpoch{0};
struct InputTicket {
    unsigned long long open, renderer;
    bool operator==(const InputTicket& b) const { return open == b.open && renderer == b.renderer; }
};
InputTicket CurrentInputTicket() { return {g_openState.load(), g_inputEpoch.load()}; }
enum class InputKind { Key, Character, Position, Button, Wheel, Focus, DeviceChange };
struct PanelInput {
    InputKind kind;
    int code = 0;
    float x = 0, y = 0;
    bool down = false;
    ImGuiMouseSource source = ImGuiMouseSource_Mouse;
};
constexpr size_t kInputCapacity = 512;
std::mutex g_inputMutex;
std::array<PanelInput, kInputCapacity> g_inputs{};
size_t g_inputCount = 0;
InputTicket g_inputTicket{};
bool g_inputOverflow = false;
InputTicket g_consumedTicket{}; // renderer only

void QueuePanelInput(InputTicket ticket, PanelInput event)
{
    std::lock_guard<std::mutex> lock(g_inputMutex);
    if (!(ticket == CurrentInputTicket()) || !(ticket.open & 1) || !g_inited.load() || g_dead.load()) return;
    if (!(ticket == g_inputTicket)) {
        g_inputTicket = ticket;
        g_inputCount = 0;
        g_inputOverflow = false;
    }
    // Do not retain a suffix of a truncated gesture: a lost release must never
    // leave a held key/button. Overflow sacrifices this batch, not boundedness.
    if (g_inputOverflow) return;
    if (event.kind == InputKind::Position && g_inputCount &&
        g_inputs[g_inputCount - 1].kind == InputKind::Position &&
        g_inputs[g_inputCount - 1].source == event.source) {
        g_inputs[g_inputCount - 1] = event;
        return;
    }
    if (g_inputCount == kInputCapacity) {
        g_inputCount = 0;
        g_inputOverflow = true;
        return;
    }
    g_inputs[g_inputCount++] = event;
}

void DrainPanelInput(InputTicket ticket)
{
    std::array<PanelInput, kInputCapacity> batch;
    size_t count = 0;
    bool reset = !(ticket == g_consumedTicket);
    {
        std::lock_guard<std::mutex> lock(g_inputMutex);
        if (ticket == g_inputTicket) {
            reset |= g_inputOverflow;
            count = g_inputCount;
            for (size_t i = 0; i < count; ++i) batch[i] = g_inputs[i];
        }
        // A concurrent close/reopen may already have produced the next batch.
        // Never erase it while finishing an older frame.
        if (ticket == CurrentInputTicket()) {
            g_inputCount = 0;
            g_inputOverflow = false;
            g_inputTicket = ticket;
        } else return;
    }
    ImGuiIO& io = ImGui::GetIO();
    if (GImGui->InputEventsQueue.Size + (int)count > (int)kInputCapacity * 2) {
        reset = true;
        count = 0;
    }
    if (reset) { io.ClearEventsQueue(); io.ClearInputKeys(); io.ClearInputMouse(); }
    g_consumedTicket = ticket;
    for (size_t i = 0; i < count; ++i) {
        const PanelInput& e = batch[i];
        switch (e.kind) {
        case InputKind::Key: io.AddKeyEvent((ImGuiKey)e.code, e.down); break;
        case InputKind::Character: io.AddInputCharacterUTF16((ImWchar16)e.code); break;
        case InputKind::Position:
            io.AddMouseSourceEvent(e.source); io.AddMousePosEvent(e.x, e.y); break;
        case InputKind::Button:
            io.AddMouseSourceEvent(e.source); io.AddMouseButtonEvent(e.code, e.down); break;
        case InputKind::Wheel: io.AddMouseWheelEvent(e.x, e.y); break;
        case InputKind::Focus: io.AddFocusEvent(e.down); break;
        case InputKind::DeviceChange:
            // This backend branch only invalidates the gamepad cache. Never
            // replay keyboard/mouse messages here (they call thread-bound APIs).
            ImGui_ImplWin32_WndProcHandler(g_hwnd.load(), WM_DEVICECHANGE, 0x0007 /* DBT_DEVNODES_CHANGED */, 0);
            break;
        }
    }
}

ImGuiMouseSource WindowMouseSource()
{
    const LPARAM extra = ::GetMessageExtraInfo();
    if ((extra & 0xFFFFFF80) == 0xFF515700) return ImGuiMouseSource_Pen;
    if ((extra & 0xFFFFFF80) == 0xFF515780) return ImGuiMouseSource_TouchScreen;
    return ImGuiMouseSource_Mouse;
}

// Accessed exclusively by the hooked window's thread.
thread_local InputTicket g_mouseTicket{};
thread_local unsigned g_mouseButtons = 0;
thread_local HWND g_inputCapture = nullptr;
void SyncInputCapture(HWND h, InputTicket ticket)
{
    if (!(g_mouseTicket == ticket) || !(ticket.open & 1) || !g_inited.load() || g_dead.load()) {
        g_mouseTicket = ticket;
        g_mouseButtons = 0;
        const bool release = g_inputCapture == h;
        g_inputCapture = nullptr;
        if (release && ::GetCapture() == h) ::ReleaseCapture();
    }
}

bool QueueWindowInput(HWND h, UINT msg, WPARAM w, LPARAM l, InputTicket ticket)
{
    // SetCapture/ReleaseCapture stay on the window thread and may reenter us.
    PanelInput e{};
    switch (msg) {
    case WM_DEVICECHANGE:
        if (w == 0x0007 /* DBT_DEVNODES_CHANGED */) QueuePanelInput(ticket, {InputKind::DeviceChange});
        return false;
    case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
        const bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        WPARAM vk = w;
        if (vk == VK_SHIFT) vk = ::MapVirtualKeyW((UINT)((l >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX);
        if (vk == VK_CONTROL) vk = (l & (1LL << 24)) ? VK_RCONTROL : VK_LCONTROL;
        if (vk == VK_MENU) vk = (l & (1LL << 24)) ? VK_RMENU : VK_LMENU;
        const ImGuiKey key = ImGui_ImplWin32_KeyEventToImGuiKey(vk, l);
        const int mods[] = {ImGuiMod_Ctrl, ImGuiMod_Shift, ImGuiMod_Alt, ImGuiMod_Super};
        const bool states[] = { (::GetKeyState(VK_CONTROL) & 0x8000) != 0,
            (::GetKeyState(VK_SHIFT) & 0x8000) != 0, (::GetKeyState(VK_MENU) & 0x8000) != 0,
            ((::GetKeyState(VK_LWIN) | ::GetKeyState(VK_RWIN)) & 0x8000) != 0 };
        for (int i = 0; i < 4; ++i) QueuePanelInput(ticket, {InputKind::Key, mods[i], 0, 0, states[i]});
        if (key == ImGuiKey_PrintScreen && !down) QueuePanelInput(ticket, {InputKind::Key, key, 0, 0, true});
        if (key != ImGuiKey_None) QueuePanelInput(ticket, {InputKind::Key, key, 0, 0, down});
        return true;
    }
    case WM_CHAR:
        if (::IsWindowUnicode(h)) {
            if (w > 0 && w < 0x10000) QueuePanelInput(ticket, {InputKind::Character, (int)w});
        } else {
            wchar_t ch = 0;
            const char bytes[2] = {(char)(w & 0xFF), (char)((w >> 8) & 0xFF)};
            const UINT cp = LOWORD(::GetKeyboardLayout(0));
            wchar_t cpText[16]{};
            const LCID locale = MAKELCID(cp, SORT_DEFAULT);
            const UINT codepage = ::GetLocaleInfoW(locale, LOCALE_IDEFAULTANSICODEPAGE, cpText, 16) ? (UINT)_wtoi(cpText) : CP_ACP;
            if (::MultiByteToWideChar(codepage, MB_PRECOMPOSED, bytes, bytes[1] ? 2 : 1, &ch, 1))
                QueuePanelInput(ticket, {InputKind::Character, (int)ch});
        }
        return true;
    case WM_SETFOCUS: case WM_KILLFOCUS:
        QueuePanelInput(ticket, {InputKind::Focus, 0, 0, 0, msg == WM_SETFOCUS});
        if (msg == WM_KILLFOCUS) {
            g_mouseButtons = 0;
            const bool release = g_inputCapture == h;
            g_inputCapture = nullptr;
            if (release && ::GetCapture() == h) ::ReleaseCapture();
        }
        return false; // game still needs its focus notification
    case WM_MOUSEMOVE: case WM_NCMOUSEMOVE: {
        const ImGuiMouseSource source = WindowMouseSource();
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        if (msg == WM_NCMOUSEMOVE) ::ScreenToClient(h, &p);
        TRACKMOUSEEVENT track{sizeof(track), (DWORD)(TME_LEAVE | (msg == WM_NCMOUSEMOVE ? TME_NONCLIENT : 0)), h, 0};
        ::TrackMouseEvent(&track);
        QueuePanelInput(ticket, {InputKind::Position, 0, (float)p.x, (float)p.y, false, source});
        return msg == WM_MOUSEMOVE;
    }
    case WM_CAPTURECHANGED:
        if ((HWND)l != h) {
            g_inputCapture = nullptr;
            g_mouseButtons = 0;
            for (int b = 0; b < 5; ++b) QueuePanelInput(ticket, {InputKind::Button, b, 0, 0, false});
        }
        return false;
    case WM_MOUSELEAVE: case WM_NCMOUSELEAVE:
        QueuePanelInput(ticket, {InputKind::Position, 0, -FLT_MAX, -FLT_MAX}); return false;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: case WM_LBUTTONUP:
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK: case WM_RBUTTONUP:
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK: case WM_MBUTTONUP:
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK: case WM_XBUTTONUP: {
        const ImGuiMouseSource source = WindowMouseSource();
        int button = 0;
        if (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK || msg == WM_RBUTTONUP) button = 1;
        if (msg == WM_MBUTTONDOWN || msg == WM_MBUTTONDBLCLK || msg == WM_MBUTTONUP) button = 2;
        if (msg == WM_XBUTTONDOWN || msg == WM_XBUTTONDBLCLK || msg == WM_XBUTTONUP) button = GET_XBUTTON_WPARAM(w) == XBUTTON1 ? 3 : 4;
        const bool down = msg != WM_LBUTTONUP && msg != WM_RBUTTONUP && msg != WM_MBUTTONUP && msg != WM_XBUTTONUP;
        if (::GetCapture() != h) g_mouseButtons = 0;
        if (down) g_mouseButtons |= 1u << button; else g_mouseButtons &= ~(1u << button);
        QueuePanelInput(ticket, {InputKind::Button, button, 0, 0, down, source});
        if (down && ::GetCapture() == nullptr) { g_inputCapture = h; ::SetCapture(h); }
        if (!down && !g_mouseButtons && g_inputCapture == h) {
            g_inputCapture = nullptr;
            if (::GetCapture() == h) ::ReleaseCapture();
        }
        return true;
    }
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        e.kind = InputKind::Wheel;
        if (msg == WM_MOUSEWHEEL) e.y = (float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
        else e.x = -(float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA;
        QueuePanelInput(ticket, e); return true;
    }
    return false;
}

// 面板开着时把键鼠消息吃掉，游戏收不到（手柄另由 padfilter 挡）
LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    // 万一窗口过程被装了两层（别的叠加层在我们外面，它又把消息转回我们），
    // 消息会在我们和 CallWindowProc 之间原地打转，最后栈溢出（0xC00000FD 在
    // USER32 —— 0.7.1 实测过一次，就是这条路径）。
    // 于是分两级着陆：先试一次「游戏自己的窗口过程」（property 里那份，绕开我们
    // 和中间那层），还转回来就直接交回系统，宁可不画面板也别把游戏带崩。
    ++g_wndDepth;
    struct DepthGuard {
        ~DepthGuard() { --g_wndDepth; }
    } depthGuard;
    MsgNest nestGuard(h, msg);
    const int nest = NestOf(h, msg);
    const WNDPROC previous = PreviousProc(h);
    // Keep the predecessor available to synchronous reentry until forwarding
    // returns. Remove it on every exit path, including the recursion gates.
    struct PropertyGuard {
        HWND hwnd;
        bool remove;
        ~PropertyGuard() { if (remove) ::RemovePropW(hwnd, kPreviousProc); }
    } propertyGuard{h, msg == WM_NCDESTROY};
    TraceWindowMessage(h, msg, w, previous);
    if (nest > 3) {
        // 同一条消息已经在我们手里转了三圈：链条在打转，再转只会把栈耗尽
        // （0.7.1 实测 0xC00000FD，闪退在 USER32）。这里只交回系统，不再往下转。
        if (nest == 4)
            Log("wnd: 消息 0x%04X 重入 %d 层，判定窗口过程成环，直接交回系统",
                (unsigned)msg, nest);
        return ::DefWindowProcW(h, msg, w, l);
    }
    if (g_wndDepth > 10) {
        // 最后一道闸：链条已经绕回我们十次，再往下只会有更多递归。
        if (g_wndDepth == 11)
            Log("wnd: 窗口过程重入 11 次（消息 0x%04X），直接交回系统", (unsigned)msg);
        return ::DefWindowProcW(h, msg, w, l);
    }
    if (g_wndDepth > 8) {
        // 先绕开中间那层，直接把消息送到游戏自己的窗口过程。
        if (g_wndDepth == 9)
            Log("wnd: 窗口过程重入 9 次（消息 0x%04X），改为跳过中间层", (unsigned)msg);
        if (previous && previous != WndProc) return ::CallWindowProcW(previous, h, msg, w, l);
        return ::DefWindowProcW(h, msg, w, l);
    }
    if (msg == WM_NCDESTROY) {
        if (h == g_hwnd.load()) {
            g_openState.fetch_and(~1ULL);
            g_imeAttached = false;
            g_imeWant = false;
            ++g_imeGeneration;
        }
        ImeOffWindowThread(h);
        return ForwardToGame(h, msg, w, l, previous);
    }
    if (h == g_hwnd.load()) SyncInputCapture(h, CurrentInputTicket());
    // 归还输入法不能依赖渲染器仍初始化，也不能等下一次 Present。
    if (msg == kImePos) {
        ImeSetPosOnWindowThread(h, (short)LOWORD(l), (short)HIWORD(l), (unsigned)w);
        return 0;
    }
    ImeSyncOnWindowThread(h);
    if (msg == kImeSync) return 0;
    if (h != g_hwnd || !g_inited || g_dead.load())
        return ForwardToGame(h, msg, w, l, previous);
    const InputTicket inputTicket = CurrentInputTicket();
    if ((inputTicket.open & 1) && g_inited.load() && !g_dead.load()) {
        // 输入法：自己取结果串塞进 ImGui，不交给后端（后端会再调 DefWindowProcW，
        // 结果串可能被算两遍，也不保证一定生成 WM_CHAR）
        switch (msg) {
        case WM_IME_STARTCOMPOSITION:
            ++g_imeStart;
            return ::DefWindowProcW(h, msg, w, l);
        case WM_IME_COMPOSITION:
            ++g_imeComp;
            if (l & GCS_RESULTSTR) {
                ++g_imeResult;
                if (HIMC imc = ::ImmGetContext(h)) {
                    const LONG bytes = ::ImmGetCompositionStringW(imc, GCS_RESULTSTR, nullptr, 0);
                    if (bytes > 0 && bytes <= (LONG)(kInputCapacity * sizeof(wchar_t))) {
                        std::array<wchar_t, kInputCapacity> text{};
                        const LONG copied = ::ImmGetCompositionStringW(imc, GCS_RESULTSTR, text.data(), bytes);
                        if (copied > 0 && copied <= bytes)
                            for (LONG i = 0; i < copied / (LONG)sizeof(wchar_t); ++i)
                                QueuePanelInput(inputTicket, {InputKind::Character, (int)text[i]});
                    }
                    ::ImmReleaseContext(h, imc);
                }
            }
            return 0;   // 不给游戏，也不让系统再生成一份
        case WM_IME_CHAR:
            ++g_imeChar;
            return 0;
        case WM_IME_ENDCOMPOSITION:
        case WM_IME_NOTIFY:
        case WM_IME_REQUEST:
        case WM_IME_SELECT:
        case WM_IME_KEYDOWN:
        case WM_IME_KEYUP:
            return ::DefWindowProcW(h, msg, w, l);
        case WM_IME_SETCONTEXT:
            // 组字窗和候选窗的位置由 ImGui 的 IME 数据决定，别让系统再画一份
            return ::DefWindowProcW(h, msg, w, l & ~ISC_SHOWUICOMPOSITIONWINDOW);
        }
        if (QueueWindowInput(h, msg, w, l, inputTicket)) return 0;
        if (msg == WM_INPUT)
            return ::DefWindowProcW(h, msg, w, l);   // 原始输入：交给系统收尾，不给游戏
    }
    return ForwardToGame(h, msg, w, l, previous);
}

// 记下我们改过的虚表，退出时好还原（诊断用）
void** g_swapVt = nullptr;
void** g_swapVt3 = nullptr;
void** g_queueVt = nullptr;

// 还原一个虚表槽。expect 是「装进去的那个钩子」：只有槽里仍然是它才写回，
// 免得把别的叠加层后装的钩子覆盖掉（也免得对象已经变了还硬写）。
// 调用方必须保证 slot 指向的对象此刻还活着 —— 见 ShutdownForExit 里的顺序说明。
void UnpatchSlot(void** slot, void* expect, void* orig)
{
    if (!slot || !orig) return;
    if (expect && *slot != expect) {
        Log("overlay: 虚表槽 %p 里不是我们的钩子（%p），跳过还原", reinterpret_cast<void*>(slot), *slot);
        return;
    }
    DWORD old = 0;
    if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return;
    *slot = orig;
    ::VirtualProtect(slot, sizeof(void*), old, &old);
    ::FlushInstructionCache(::GetCurrentProcess(), slot, sizeof(void*));
}

bool PatchSlot(void** slot, void* hook, void** orig)
{
    DWORD old = 0;
    if (!::VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return false;
    *orig = *slot;
    *slot = hook;
    ::VirtualProtect(slot, sizeof(void*), old, &old);
    return true;
}

// ================================================================ D3D11
void ReleaseRtv()
{
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

bool CreateRtv(IDXGISwapChain* sc)
{
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) || !bb) return false;
    const HRESULT hr = g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv);
    bb->Release();
    return SUCCEEDED(hr);
}

// ================================================================ D3D12
void WINAPI HkExecute(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    {
    // Never wait here: drivers can submit work while another thread owns the
    // renderer lock. Capturing a queue can safely wait for the next submission.
    std::unique_lock<std::recursive_mutex> lock(g_rendererMutex, std::try_to_lock);
    ID3D12Device* want = lock.owns_lock() && !g_dead.load() ? g_wantQueueOf.load() : nullptr;
    if (want && !g_queue.load() && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ID3D12Device* d = nullptr;
        if (SUCCEEDED(q->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&d))) && d) {
            if (d == want) {
                ID3D12CommandQueue* expected = nullptr;
                if (g_queue.compare_exchange_strong(expected, q)) {
                    // 不加引用：游戏回标题时会放掉队列，我们攥着会把它拖住不释放；
                    // 队列真正销毁前 DropRenderer 会把这里的指针清空。
                    Log("overlay: 记下游戏的 D3D12 命令队列 %p（不持引用）", (void*)q);
                }
            }
            d->Release();
        }
    }
    }
    g_execute(q, n, lists);
}

// 在游戏的设备上建一个临时队列，拿到队列的虚表，挂 ExecuteCommandLists（第 10 项）
bool HookExecute(ID3D12Device* dev)
{
    if (g_execute) return true;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* tmp = nullptr;
    if (FAILED(dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&tmp))) || !tmp) return false;
    void** vt = *reinterpret_cast<void***>(tmp);
    g_queueVt = vt;
    const bool ok = PatchSlot(&vt[10], reinterpret_cast<void*>(HkExecute), reinterpret_cast<void**>(&g_execute));
    tmp->Release();
    Log("overlay: %s ExecuteCommandLists", ok ? "已挂" : "挂不上");
    return ok;
}

void SrvAlloc(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* cpu, D3D12_GPU_DESCRIPTOR_HANDLE* gpu)
{
    const int i = g_srvFree.empty() ? 0 : g_srvFree.back();
    if (!g_srvFree.empty()) g_srvFree.pop_back();
    *cpu = g_srvHeap->GetCPUDescriptorHandleForHeapStart();
    *gpu = g_srvHeap->GetGPUDescriptorHandleForHeapStart();
    cpu->ptr += (SIZE_T)i * g_srvInc;
    gpu->ptr += (UINT64)i * g_srvInc;
}

void SrvFree(ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE)
{
    g_srvFree.push_back((int)((cpu.ptr - g_srvHeap->GetCPUDescriptorHandleForHeapStart().ptr) / g_srvInc));
}

void WaitGpu12()
{
    ID3D12CommandQueue* q = g_queue.load();
    if (!q || !g_fence) return;
    q->Signal(g_fence, ++g_fenceVal);
    if (g_fence->GetCompletedValue() < g_fenceVal) {
        g_fence->SetEventOnCompletion(g_fenceVal, g_fenceEvent);
        ::WaitForSingleObject(g_fenceEvent, 2000);
    }
}

// 0 = 用不了，1 = 好了，2 = 是 D3D12 的，但还没等到游戏的命令队列
int InitD3D12Objects(IDXGISwapChain* sc, ID3D12Device* dev)
{
    if (!HookExecute(dev)) return 0;
    g_wantQueueOf = dev;
    ID3D12CommandQueue* q = g_queue.load();
    if (!q) return 2;
    if (FAILED(sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&g_sc3))) || !g_sc3) return 0;
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kMaxBuffers;
    if (FAILED(dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&g_rtvHeap)))) return 0;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 64;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&g_srvHeap)))) return 0;
    for (int i = 63; i >= 0; --i) g_srvFree.push_back(i);
    g_rtvInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    g_srvInc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&g_fence)))) return 0;
    g_fenceEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    for (UINT i = 0; i < kMaxBuffers; ++i)
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                               reinterpret_cast<void**>(&g_alloc[i])))) return 0;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0], nullptr, __uuidof(ID3D12GraphicsCommandList),
                                      reinterpret_cast<void**>(&g_list)))) return 0;
    g_list->Close();
    g_dev12 = dev;
    // 不 AddRef：设备归游戏管，多一个引用会让它在退出到标题时死不掉          // 同上：设备归游戏管，但我们每帧都在用，得自己拿一份引用
    return 1;
}

void Render12(IDXGISwapChain* sc, ImDrawData* dd)
{
    SetStage("render12");
    ID3D12CommandQueue* q = g_queue.load();
    const UINT i = g_sc3->GetCurrentBackBufferIndex();
    if (i >= kMaxBuffers) return;
    // 每帧现拿后台缓冲、用完就放：不长期持有引用，游戏 ResizeBuffers / 重建交换链都不受影响
    ID3D12Resource* bb = nullptr;
    if (FAILED(sc->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&bb))) || !bb) return;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += (SIZE_T)i * g_rtvInc;
    g_dev12->CreateRenderTargetView(bb, nullptr, rtv);

    if (g_fence->GetCompletedValue() < g_allocFence[i]) {        // 这块分配器上次的命令还没跑完
        g_fence->SetEventOnCompletion(g_allocFence[i], g_fenceEvent);
        ::WaitForSingleObject(g_fenceEvent, 1000);
    }
    g_alloc[i]->Reset();
    g_list->Reset(g_alloc[i], nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_list->ResourceBarrier(1, &b);
    g_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    g_list->SetDescriptorHeaps(1, &g_srvHeap);
    ImGui_ImplDX12_RenderDrawData(dd, g_list);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    g_list->ResourceBarrier(1, &b);
    g_list->Close();
    ID3D12CommandList* lists[] = {g_list};
    g_execute(q, 1, lists);
    q->Signal(g_fence, ++g_fenceVal);
    g_allocFence[i] = g_fenceVal;
    if (g_debugAfterDraw12) g_debugAfterDraw12(bb, q);
    bb->Release();
}

// ================================================================ 公共
// 用这个交换链初始化。0 = 用不了（别人的、或者是 D3D11/12 以外的），1 = 好了，2 = 等一等（D3D12 还没拿到命令队列）。
// 用不了不算失败：进程里所有交换链共用一张虚表，别人的 Present 也会走到这里。
int InitRenderer(IDXGISwapChain* sc)
{
    SetStage("init");
    if (sc == g_foreign) return 0;
    // 按快捷键时游戏窗口在前台（快捷键只在游戏有焦点时才响应）：别的窗口的交换链（别的模组、工具）不认
    HWND fg = ::GetForegroundWindow();
    DWORD pid = 0;
    ::GetWindowThreadProcessId(fg, &pid);
    if (fg && pid == ::GetCurrentProcessId()) {
        DXGI_SWAP_CHAIN_DESC d{};
        if (SUCCEEDED(sc->GetDesc(&d)) && d.OutputWindow != fg && ::GetAncestor(d.OutputWindow, GA_ROOT) != fg) {
            g_foreign = sc;
            return 0;
        }
    }
    ID3D12Device* dev12 = nullptr;
    g_dev = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_dev))) && g_dev) {
        g_dev->GetImmediateContext(&g_ctx);
        g_api = kD3D11;
    } else if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&dev12))) && dev12) {
        g_dev = nullptr;
        const int r = InitD3D12Objects(sc, dev12);
        dev12->Release();            // 设备归游戏管，交换链活着它就活着
        if (r == 0) {                // 是 D3D12 的游戏画面，但建不起我们要的东西：别每帧重试
            Log("overlay: D3D12 初始化失败，面板不可用；重开快捷键不受影响");
            g_failed = true;
        }
        if (r != 1) return r;
        g_api = kD3D12;
    } else {
        g_dev = nullptr;
        g_foreign = sc;
        DXGI_SWAP_CHAIN_DESC d{};
        sc->GetDesc(&d);
        Log("overlay: 跳过一个既不是 D3D11 也不是 D3D12 的交换链（缓冲 %ux%u）", d.BufferDesc.Width, d.BufferDesc.Height);
        return 0;
    }
    DXGI_SWAP_CHAIN_DESC d{};
    sc->GetDesc(&d);
    g_hwnd = d.OutputWindow;
    g_sc = sc;
    g_bbFormat = d.BufferDesc.Format;
    RECT rc{};
    ::GetClientRect(g_hwnd, &rc);
    const float h = (float)d.BufferDesc.Height;   // 字号按真实分辨率定，不按缩小后的窗口

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;
    bool ok = ImGui_ImplWin32_Init(g_hwnd);
    if (ok && g_api == kD3D11) {
        ok = ImGui_ImplDX11_Init(g_dev, g_ctx);
    } else if (ok) {
        ImGui_ImplDX12_InitInfo info;
        info.Device = g_dev12;
        info.CommandQueue = g_queue.load();
        info.NumFramesInFlight = (int)(d.BufferCount < 2 ? 2 : d.BufferCount);
        info.RTVFormat = d.BufferDesc.Format;
        info.DSVFormat = DXGI_FORMAT_UNKNOWN;
        info.SrvDescriptorHeap = g_srvHeap;
        info.SrvDescriptorAllocFn = SrvAlloc;
        info.SrvDescriptorFreeFn = SrvFree;
        ok = ImGui_ImplDX12_Init(&info);
    }
    if (!ok) {
        Log("overlay: ImGui 后端初始化失败");
        g_failed = true;
        return 0;
    }
    if (g_cb.init) g_cb.init(h > 0 ? h / 1080.0f : 1.0f);
    hdr::Detect(sc);   // 按交换链的色域决定要不要做 HDR 转换
    if (!g_wndHook.load()) {
        Log("overlay: 按配置不接管窗口过程（NoWndHook=1）：面板收不到键鼠，只用于排查");
    } else if (!PreviousProc(g_hwnd)) {
        const LONG_PTR prev = ::GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC);
        // Publish the immutable predecessor before installing the callback.
        if (!prev || prev == reinterpret_cast<LONG_PTR>(WndProc) ||
            !::SetPropW(g_hwnd, kPreviousProc, reinterpret_cast<HANDLE>(prev))) {
            Log("overlay: 无法保存窗口过程，停止初始化");
            g_failed = true;
            return 0;
        }
        ::SetLastError(0);
        const LONG_PTR replaced = ::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc));
        if (!replaced && ::GetLastError() != 0) {
            ::RemovePropW(g_hwnd, kPreviousProc);
            g_failed = true;
            return 0;
        }
        if (replaced != prev)
            Log("overlay: WARNING: concurrent window subclass change detected during installation");
        char prevDesc[160]{};
        ModuleOf(reinterpret_cast<const void*>(prev), prevDesc, sizeof(prevDesc));
        char clsDesc[160]{};
        ModuleOf(reinterpret_cast<const void*>(ClassProc(g_hwnd)), clsDesc, sizeof(clsDesc));
        Log("overlay: 接管窗口过程 %s -> %p（类过程 %s）", prevDesc, (void*)&WndProc, clsDesc);
    } else {
        Log("overlay: 保留已有窗口过程转发链，不重复接管");
    }
    ++g_inputEpoch;
    g_inited = true;
    Log("overlay: 就绪（%s，窗口 %ldx%ld，缓冲 %ux%u x%u，格式 %d）", g_api == kD3D11 ? "D3D11" : "D3D12",
        rc.right - rc.left, rc.bottom - rc.top, d.BufferDesc.Width, d.BufferDesc.Height, d.BufferCount, (int)d.BufferDesc.Format);
    return 1;
}

// 把「我们自建、绑在这个设备/交换链上」的资源全丢掉，等下次开面板重新建。
// 游戏退出存档回标题时会换设备（甚至换格式），旧资源再碰就是野指针。
void DropRenderer()
{
    Log("overlay: 丢掉渲染资源（设备要换了）");
    ImeRequest(false, false);
    g_inited = false; // Window messages must only forward while backends are torn down.
    ++g_inputEpoch; // In-flight producers cannot publish into a replacement context.
    if (g_api == kD3D12) WaitGpu12();
    if (g_api == kD3D11) ImGui_ImplDX11_Shutdown();
    else if (g_api == kD3D12) ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    ReleaseRtv();
    if (g_list) { g_list->Release(); g_list = nullptr; }
    for (UINT i = 0; i < kMaxBuffers; ++i)
        if (g_alloc[i]) { g_alloc[i]->Release(); g_alloc[i] = nullptr; }
    if (g_fence) { g_fence->Release(); g_fence = nullptr; }
    if (g_fenceEvent) { ::CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
    if (g_rtvHeap) { g_rtvHeap->Release(); g_rtvHeap = nullptr; }
    if (g_srvHeap) { g_srvHeap->Release(); g_srvHeap = nullptr; }
    g_srvFree.clear();
    g_fenceVal = 0;
    for (auto& value : g_allocFence) value = 0;
    if (g_sc3) { g_sc3->Release(); g_sc3 = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    g_dev12 = nullptr;
    g_wantQueueOf = nullptr;
    g_queue = nullptr;
    g_lastScaledEvent = 0; // ImGui event IDs restart with the new context.
    g_wasOpen = false;
    g_inited = false;
}

// 这一帧是不是游戏的交换链在 Present。游戏重建交换链（切全屏、退出存档回标题）时
// 指针会变，甚至设备都换了；同一个窗口就认作新的游戏交换链，旧资源就地丢掉。
bool IsGameSwapChain(IDXGISwapChain* sc)
{
    if (sc == g_sc) return true;
    if (sc == g_foreign) return false;
    Log("overlay: 交换链接管 %p -> %p", (void*)g_sc, (void*)sc);
    DXGI_SWAP_CHAIN_DESC d{};
    if (!SUCCEEDED(sc->GetDesc(&d)) || d.OutputWindow != g_hwnd) { g_foreign = sc; return false; }

    if (g_api == kD3D11) {
        ID3D11Device* dev = nullptr;
        const bool same = SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) && dev == g_dev;
        if (dev) dev->Release();
        if (same) { g_sc = sc; ReleaseRtv(); return true; }
    } else {
        ID3D12Device* dev = nullptr;
        IDXGISwapChain3* sc3 = nullptr;
        const bool gotDev = SUCCEEDED(sc->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&dev))) && dev != nullptr;
        const bool gotSc3 = SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc3))) && sc3 != nullptr;
        if (gotDev && gotSc3) {
            if (dev == g_dev12 && d.BufferDesc.Format == g_bbFormat) {
                g_sc3->Release();
                g_sc3 = sc3;
                g_sc = sc;
                ReleaseRtv();
                dev->Release();
                return true;
            }
            DropRenderer();
            g_sc = sc;
            g_wantQueueOf = dev; // Capture the new device's queue before the next initialization attempt.
            g_openAt = ::GetTickCount64(); // Rebuilding gets its own initialization timeout.
            sc3->Release();
            dev->Release();
            Log("overlay: 新交换链等待重新初始化（格式 %d，%ux%u）", (int)d.BufferDesc.Format, d.BufferDesc.Width, d.BufferDesc.Height);
            return false; // This frame no longer owns an initialized ImGui backend.
        }
        if (dev) dev->Release();
        if (sc3) sc3->Release();
    }
    g_foreign = sc;
    return false;
}

void RenderPanel(IDXGISwapChain* sc)
{
    SetStage("render");
    if (g_dead.load()) return;
    if (g_depth > 1) return;                      // 别在渲染里再进渲染
    struct DepthGuard { DepthGuard() { ++g_depth; } ~DepthGuard() { --g_depth; } } dg;
    const unsigned long long openState = g_openState.load();
    const bool open = (openState & 1) != 0;
    const unsigned long long generation = openState >> 1;
    if (open && (!g_wasOpen || generation != g_seenOpenGeneration)) {
        g_seenOpenGeneration = generation;
        g_panelGamepadGate.Arm();
        if (g_imeEnabled.load()) ImeRequest(true, false);
        if (g_cb.opened) g_cb.opened();
    }
    if (!open && g_wasOpen) ImeRequest(false, false);
    g_wasOpen = open;
    if (!open) return;
    if (g_api == kD3D11 && !g_rtv && !CreateRtv(sc)) return;

    if (g_api == kD3D11) ImGui_ImplDX11_NewFrame();
    else ImGui_ImplDX12_NewFrame();
    DrainPanelInput({openState, g_inputEpoch.load()});
    ImGui_ImplWin32_NewFrame();
    // 后端已轮询真实手柄、ImGui 尚未消费事件：吞掉唤出时仍按住的 UI 副本。
    g_panelGamepadGate.Filter(*GImGui);
    ImGuiIO& io = ImGui::GetIO();
    ImGui::GetPlatformIO().Platform_SetImeDataFn = SetImeDataFn;   // 每帧设一次：后端初始化时会塞它自己的
    io.MouseDrawCursor = true;   // 游戏把系统光标藏了，面板开着时自己画一个
    // 直接按后台缓冲的真实分辨率排版和渲染。游戏没声明高 DPI，系统缩放 125% 时
    // 它拿到的窗口大小是缩小过的（2560x1440 的屏报成 2048x1152），按那个排版再放大就糊了。
    // 鼠标消息里的坐标是缩小后的窗口坐标，排队的鼠标事件按比例换算到缓冲坐标。
    DXGI_SWAP_CHAIN_DESC d{};
    if (SUCCEEDED(sc->GetDesc(&d)) && io.DisplaySize.x > 0 && io.DisplaySize.y > 0 && d.BufferDesc.Width && d.BufferDesc.Height) {
        const float sx = d.BufferDesc.Width / io.DisplaySize.x, sy = d.BufferDesc.Height / io.DisplaySize.y;
        if (sx != 1.0f || sy != 1.0f) {
            for (ImGuiInputEvent& e : GImGui->InputEventsQueue) {
                if (e.Type != ImGuiInputEventType_MousePos || e.EventId <= g_lastScaledEvent) continue;
                if (e.MousePos.PosX != -FLT_MAX) { e.MousePos.PosX *= sx; e.MousePos.PosY *= sy; }
                g_lastScaledEvent = e.EventId;   // 没处理完留到下一帧的事件不能再乘一遍
            }
        }
        io.DisplaySize = ImVec2((float)d.BufferDesc.Width, (float)d.BufferDesc.Height);
        io.DisplayFramebufferScale = ImVec2(1, 1);
    }
    ImGui::NewFrame();
    // 有输入框拿到焦点时才把输入法打开，免得平时按键被组字吞掉。
    // 这里只投递，真正的 IMM 调用在窗口线程上做（见 ImeOnWindowThread）。
    if (g_imeAttached) {
        const bool want = io.WantTextInput;
        if (!g_imeEnabled.load()) {
            // 排查模式：不接管输入法
        } else if (want != g_imeWant) {
            Log("ime: %s文字输入", want ? "开始" : "结束");
            ImeRequest(true, want);
        } else if (want && (++g_imeTick % 30) == 0) {
            ImeRequest(true, want);   // 每半秒确认一次，上下文被游戏换掉就挂回来
        }
    }
    if (g_cb.draw) g_cb.draw(io.DisplaySize.x, io.DisplaySize.y);
    ImGui::Render();
    hdr::Apply(ImGui::GetDrawData());   // HDR 交换链：把顶点颜色换成目标编码
    if (g_api == kD3D12) {
        Render12(sc, ImGui::GetDrawData());
        return;
    }
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

    if (g_debugAfterDraw) {
        ID3D11Texture2D* bb = nullptr;
        if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&bb))) && bb) {
            g_debugAfterDraw(bb);
            bb->Release();
        }
    }
}

// ---- 诊断：面板关着时，我们这一帧花了多少时间、游戏帧间隔多少（每 30 秒写一行日志）
std::atomic<unsigned long long> g_pCalls{0}, g_pTicks{0}, g_pMax{0};
unsigned long long g_pLastQpc = 0, g_fTicks = 0, g_fN = 0;
unsigned long long g_perfAt = 0;

double QpcUs0(unsigned long long ticks)
{
    static double perTick = 0;
    if (perTick == 0) {
        LARGE_INTEGER f;
        ::QueryPerformanceFrequency(&f);
        perTick = 1e6 / (double)f.QuadPart;
    }
    return (double)ticks * perTick;
}

void PerfTick(unsigned long long begin, unsigned long long end)
{
    const unsigned long long d = end - begin;
    ++g_pCalls;
    g_pTicks += d;
    if (d > g_pMax) g_pMax = d;
    if (g_pLastQpc) { g_fTicks += begin - g_pLastQpc; ++g_fN; }
    g_pLastQpc = begin;
    const unsigned long long now = ::GetTickCount64();
    if (!g_perfAt) { g_perfAt = now; return; }
    if (now - g_perfAt < 30000) return;
    const unsigned long long calls = g_pCalls.exchange(0);
    const unsigned long long ticks = g_pTicks.exchange(0);
    const unsigned long long mx = g_pMax.exchange(0);
    const unsigned long long fN = g_fN;
    const unsigned long long fT = g_fTicks;
    g_fN = 0;
    g_fTicks = 0;
    g_perfAt = now;
    if (!calls) return;
    Log("perf: Present 钩子 %llu 次，平均 %.2f µs、最大 %.2f µs；帧间隔 平均 %.1f ms", calls,
        QpcUs0(ticks) / (double)calls, QpcUs0(mx), fN ? QpcUs0(fT) / (double)fN / 1000.0 : 0.0);
}

HRESULT WINAPI HkPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    SetStage("present");
    if (g_dead.load()) return g_present(sc, sync, flags);   // 窗口都没了，纯转发
    if (g_depth > 3) {                            // Present 套娃：只转发，不再走我们的逻辑
        static std::atomic<int> warned{0};
        if (warned.fetch_add(1) < 3) Log("overlay: Present 重入 %d 层，跳过本帧的面板处理", g_depth);
        return g_present(sc, sync, flags);
    }
    {
    std::lock_guard<std::recursive_mutex> lock(g_rendererMutex);
    LARGE_INTEGER q0, q1;
    ::QueryPerformanceCounter(&q0);
    if (!g_dead.load() && !g_failed.load() && !(flags & DXGI_PRESENT_TEST)) {
        // 第一次打开面板才初始化（要读字体文件），从没打开过就完全不碰游戏的渲染
        if (!g_inited && PanelOpen()) {
            const int r = InitRenderer(sc);
            // D3D12 要等游戏提交一次命令才知道它的队列，多给点时间
            const unsigned long long limit = r == 2 ? 4000 : 1500;
            if (r != 1 && !g_failed.load() && ::GetTickCount64() - g_openAt.load() > limit) {
                Log(r == 2 ? "overlay: 等不到游戏的 D3D12 命令队列，面板不可用；重开快捷键不受影响"
                           : "overlay: 没找到能画面板的交换链（D3D11 / D3D12 都不是），面板不可用；重开快捷键不受影响");
                g_failed = true;
            }
        }
        if (g_inited && IsGameSwapChain(sc)) {
        // 栈用量哨兵：栈溢出前往往已经吃掉大半栈，先记一笔
        static std::atomic<unsigned long long> nextAt{0};
        const unsigned long long now = ::GetTickCount64();
        if (now >= nextAt.load()) {
            nextAt = now + 30000;
            char here = 0;
            PNT_TIB tib = reinterpret_cast<PNT_TIB>(::NtCurrentTeb());
            const unsigned long long used = static_cast<const char*>(tib->StackBase) - &here;
            if (used > 400 * 1024) Log("overlay: 栈已用 %llu KB（栈顶 %p）", used / 1024, tib->StackBase);
        }
        RenderPanel(sc);
    }
        ::QueryPerformanceCounter(&q1);
        if (!PanelOpen()) PerfTick((unsigned long long)q0.QuadPart, (unsigned long long)q1.QuadPart);
    }
    } // Never hold the renderer lock inside the game's Present.
    return g_present(sc, sync, flags);
}

// 游戏要换后台缓冲：D3D11 先放掉手上的 RTV；D3D12 等自己提交的命令跑完（缓冲本来就不长期持有）
void BeforeResize(IDXGISwapChain* sc)
{
    SetStage("resize");
    if (sc != g_sc) return;
    if (g_api == kD3D11) ReleaseRtv();
    else if (g_api == kD3D12) WaitGpu12();
}

HRESULT WINAPI HkResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags)
{
    std::lock_guard<std::recursive_mutex> lock(g_rendererMutex);
    if (g_dead.load()) return g_resize(sc, n, w, h, f, flags);
    BeforeResize(sc);
    const HRESULT r = g_resize(sc, n, w, h, f, flags);
    if (sc == g_sc) hdr::Detect(sc);
    return r;
}

HRESULT WINAPI HkResize1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags, const UINT* mask,
                         IUnknown* const* queues)
{
    std::lock_guard<std::recursive_mutex> lock(g_rendererMutex);
    if (g_dead.load()) return g_resize1(sc, n, w, h, f, flags, mask, queues);
    BeforeResize(sc);
    const HRESULT r = g_resize1(sc, n, w, h, f, flags, mask, queues);
    if (sc == g_sc) hdr::Detect(sc);
    return r;
}

// 建一个临时窗口和交换链，只为拿到交换链的虚表（D3D11 和 D3D12 的交换链是同一个类，共用这张表）
void** GetSwapChainVtable(std::string& err, void*** vt3)
{
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = ::DefWindowProcW;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BetterINQ_dummy";
    ::RegisterClassExW(&wc);
    HWND hw = ::CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr,
                                wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hw;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    HRESULT hr = ::D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                                 D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr))
        hr = ::D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                             D3D11_SDK_VERSION, &sd, &sc, &dev, nullptr, &ctx);
    void** vt = nullptr;
    *vt3 = nullptr;
    if (SUCCEEDED(hr) && sc) {
        vt = *reinterpret_cast<void***>(sc);
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc3))) && sc3) {
            *vt3 = *reinterpret_cast<void***>(sc3);
            sc3->Release();
        }
    } else {
        char code[16];
        std::snprintf(code, sizeof(code), "0x%X", static_cast<unsigned>(hr));
        err = i18n::Fmt(i18n::Tr("建临时交换链失败 hr=%s",
                                "creating a temporary swap chain failed, hr=%s",
                                "一時スワップチェーンの作成に失敗 hr=%s"), code);
    }
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    if (hw) ::DestroyWindow(hw);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return vt;
}


} // namespace

bool Install(const Callbacks& cb, std::string& err)
{
    g_cb = cb;
    void** vt3 = nullptr;
    void** vt = GetSwapChainVtable(err, &vt3);
    if (!vt) return false;
    g_swapVt = vt;
    g_swapVt3 = vt3;
    // IDXGISwapChain 虚表：8 = Present，13 = ResizeBuffers；IDXGISwapChain3：39 = ResizeBuffers1（DX12 游戏可能用它）
    if (!PatchSlot(&vt[8], reinterpret_cast<void*>(HkPresent), reinterpret_cast<void**>(&g_present)) ||
        !PatchSlot(&vt[13], reinterpret_cast<void*>(HkResize), reinterpret_cast<void**>(&g_resize))) {
        err = i18n::Tr("改交换链虚表失败", "patching the swap chain vtable failed", "スワップチェーンの仮想関数テーブルの書き換えに失敗");
        return false;
    }
    if (vt3 && !PatchSlot(&vt3[39], reinterpret_cast<void*>(HkResize1), reinterpret_cast<void**>(&g_resize1)))
        Log("overlay: ResizeBuffers1 没挂上（不影响 D3D11）");
    Log("overlay: Present 已挂（原 %p）", reinterpret_cast<void*>(g_present));
    return true;
}

void SetOpen(bool open)
{
    const bool wanted = open && !g_failed.load();
    unsigned long long previous = g_openState.load();
    for (;;) {
        const bool wasOpen = (previous & 1) != 0;
        if (wanted && !wasOpen) g_openAt = ::GetTickCount64();
        const unsigned long long next = wanted
            ? (wasOpen ? previous : ((previous & ~1ULL) + 2) | 1ULL)
            : previous & ~1ULL;
        if (g_openState.compare_exchange_weak(previous, next)) break;
    }
    // 关闭即时发布；窗口线程下一条消息先恢复，哪怕之后再没有 Present。
    ImeRequest(PanelOpen() && g_inited.load(), false);
}
bool IsOpen() { return PanelOpen() && !g_failed.load(); }
bool Failed() { return g_failed.load(); }
void SetDebugAfterDraw(void (*fn)(void*)) { g_debugAfterDraw = fn; }
void SetDebugAfterDraw12(void (*fn)(void*, void*)) { g_debugAfterDraw12 = fn; }

void Shutdown(bool processExiting)
{
    // DllMain/进程退出路径不等待窗口线程或渲染互斥。
    if (processExiting) { g_inited = false; ++g_inputEpoch; return; }
    std::lock_guard<std::recursive_mutex> rendererLock(g_rendererMutex);
    if (!g_inited) { ImeRequest(false, false); return; }
    g_inited = false;
    ++g_inputEpoch;
    g_openState.fetch_and(~1ULL);
    ImeRequest(false, false);
    SetStage("shutdown");
    Log("overlay: 收尾（api=%d，%s）", (int)g_api, processExiting ? "进程退出" : "正常卸载");
    if (g_api == kD3D11) ImGui_ImplDX11_Shutdown();
    else if (g_api == kD3D12) ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    ReleaseRtv();
    if (g_list) { g_list->Release(); g_list = nullptr; }
    for (UINT i = 0; i < kMaxBuffers; ++i)
        if (g_alloc[i]) { g_alloc[i]->Release(); g_alloc[i] = nullptr; }
    if (g_fence) { g_fence->Release(); g_fence = nullptr; }
    if (g_fenceEvent) { ::CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
    if (g_rtvHeap) { g_rtvHeap->Release(); g_rtvHeap = nullptr; }
    if (g_srvHeap) { g_srvHeap->Release(); g_srvHeap = nullptr; }
    if (g_sc3) { g_sc3->Release(); g_sc3 = nullptr; }
    g_dev12 = nullptr;   // 不是我们的引用，别释放
    // 队列同理：HkExecute 里是捡指针，没有 AddRef（见那里的注释）。
    // 这里绝对不能 Release —— 那是把游戏的引用还掉一个，游戏后面再用队列就是野指针。
    g_queue.store(nullptr);
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    // 非窗口线程只能异步归还；保留纯转发钩子让清理消息仍可抵达。
    // 不持有渲染锁跨线程 SendMessage，避免窗口/Present 互等。
    if (g_hwnd && ::GetCurrentThreadId() == ::GetWindowThreadProcessId(g_hwnd, nullptr)) {
        const WNDPROC previous = PreviousProc(g_hwnd);
        // Do not remove another overlay's outer callback. If wrapped, retain
        // our forwarding-only callback until WM_NCDESTROY removes its property.
        if (previous && ::GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(WndProc)) {
            ::SetLastError(0);
            const LONG_PTR replaced = ::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previous));
            if (replaced || ::GetLastError() == 0) {
                if (replaced != reinterpret_cast<LONG_PTR>(WndProc))
                    Log("overlay: WARNING: concurrent window subclass change detected during removal");
                ::RemovePropW(g_hwnd, kPreviousProc);
            }
        }
    }
    g_inited = false;
}

// 游戏退出（窗口已销毁）时收尾。
// 旧版为了躲开驱动崩溃，这里几乎什么都不释放，把 D3D 对象留给系统回收。
// 实测「进过存档 → 开过面板 → 回到标题 → 退出」会卡在「只关窗口、进程不退」，
// 于是改成：趁设备还活着（窗口刚销毁，游戏的设备析构还没跑）把自己建的东西
// 全部放掉；每步之间写一行日志 —— 万一还是卡住，日志最后一行就是卡住的那一步。
void ShutdownForExit()
{
    if (g_dead.exchange(true)) return;   // 只跑一次；Present 从下一帧起只转发
    g_openState.fetch_and(~1ULL);                     // 面板视为已关闭，游戏那侧不该再被我们挡输入
    std::lock_guard<std::recursive_mutex> rendererLock(g_rendererMutex);
    g_inited = false;
    ++g_inputEpoch;
    SetStage("exit");
    Log("overlay: 退出路径：开始收尾（窗口已销毁，设备还活着）");
    // 窗口过程转发链故意不动：
    //   SetWindowLongPtrW 是跨线程调用，窗口线程可能正卡在 DestroyWindow 里等我们；
    //   WM_NCDESTROY 那边已经 RemovePropW 并用保存下来的前驱把消息转给游戏了。
    // 输入法归还由窗口线程的 WM_NCDESTROY 完成；退出工作线程不调用 IMM。
    g_imeAttached = false;
    g_imeWant = false;
    ++g_imeGeneration;
    Log("overlay: 收尾 1/6 等 GPU 把已提交的活干完");
    WaitGpu12();                        // 有界（2 秒），不碰已经不在的队列
    Log("overlay: 收尾 2/6 放 ImGui 后端与上下文");
    if (g_api == kD3D11) ImGui_ImplDX11_Shutdown();
    else if (g_api == kD3D12) ImGui_ImplDX12_Shutdown();   // 根签名、PSO、字体纹理都在这
    ImGui_ImplWin32_Shutdown();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    Log("overlay: 收尾 3/6 放渲染目标、命令列表与分配器");
    ReleaseRtv();
    if (g_list) { g_list->Release(); g_list = nullptr; }
    for (UINT i = 0; i < kMaxBuffers; ++i)
        if (g_alloc[i]) { g_alloc[i]->Release(); g_alloc[i] = nullptr; }
    Log("overlay: 收尾 4/6 放围栏与描述符堆");
    if (g_fence) { g_fence->Release(); g_fence = nullptr; }
    if (g_fenceEvent) { ::CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
    if (g_srvHeap) { g_srvHeap->Release(); g_srvHeap = nullptr; }
    if (g_rtvHeap) { g_rtvHeap->Release(); g_rtvHeap = nullptr; }
    g_srvFree.clear();
    Log("overlay: 收尾 5/6 还原虚表钩子");
    // 先停止插件入口，再释放我们持有的对象引用。虚表通常是模块共享表，
    // 并非对象内存；此顺序不能证明此前的挂起一定由虚表悬空导致。
    if (g_swapVt) {
        UnpatchSlot(&g_swapVt[8], reinterpret_cast<void*>(HkPresent), reinterpret_cast<void*>(g_present));
        UnpatchSlot(&g_swapVt[13], reinterpret_cast<void*>(HkResize), reinterpret_cast<void*>(g_resize));
    }
    if (g_swapVt3) UnpatchSlot(&g_swapVt3[39], reinterpret_cast<void*>(HkResize1), reinterpret_cast<void*>(g_resize1));
    // 命令队列是 HkExecute 捡来的指针（没 AddRef），它的 vtable 说不定已经跟着
    // 游戏那边一起没了；槽里不是我们的钩子就不碰。
    if (g_queueVt) UnpatchSlot(&g_queueVt[10], reinterpret_cast<void*>(HkExecute), reinterpret_cast<void*>(g_execute));
    Log("overlay: 收尾 6/6 放开交换链与设备的引用");
    // 这几步都是纯 COM 引用计数：攥着它们游戏就销毁不掉交换链/设备。
    if (g_sc3) { g_sc3->Release(); g_sc3 = nullptr; }
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    if (g_dev) { g_dev->Release(); g_dev = nullptr; }
    g_dev12 = nullptr;                  // 借来的引用（游戏管），不释放
    g_queue.store(nullptr);             // HkExecute 捡的指针，没 AddRef，绝对不能 Release
    g_sc = nullptr;
    g_foreign = nullptr;
    g_wasOpen = false;
    Log("overlay: 退出收尾完成（Present/ResizeBuffers/ResizeBuffers1/ExecuteCommandLists 已还原）");
    g_inited = false;
}

const char* Stage() { return g_stage.load(); }

bool WindowGone()
{
    const HWND h = g_hwnd;
    return h != nullptr && ::IsWindow(h) == FALSE;
}
void SetImeEnabled(bool on) { g_imeEnabled = on; if (!on) ImeRequest(false, false); }
void SetWndHook(bool on) { g_wndHook = on; }

} // namespace overlay
