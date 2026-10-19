// MinGW 的 d3d12.h 默认按 GCC 的规矩返回结构体（GetDesc、GetCPUDescriptorHandleForHeapStart 之类），
// 和 COM 实际的调用约定（隐藏的返回指针）对不上，会读到错的值。打开这个开关就按 COM 的来。
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include "overlay.h"

#include "hdr.h"
#include "i18n.h"
#include "log.h"

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <imm.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include <cfloat>

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_dx12.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace overlay {
namespace {

typedef HRESULT(WINAPI* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(WINAPI* ResizeFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(WINAPI* Resize1Fn)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
typedef void(WINAPI* ExecuteFn)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

enum Api { kNone, kD3D11, kD3D12 };

Callbacks g_cb;
PresentFn g_present = nullptr;
ResizeFn g_resize = nullptr;
Resize1Fn g_resize1 = nullptr;
std::atomic<bool> g_open{false};
bool g_wasOpen = false;
bool g_inited = false;
Api g_api = kNone;
std::atomic<bool> g_failed{false};
std::atomic<unsigned long long> g_openAt{0};   // 这次打开面板的时间（还没初始化成功时，用来判断要不要放弃）
IDXGISwapChain* g_sc = nullptr;                // 游戏的交换链（只比指针，不持有引用）
IDXGISwapChain* g_foreign = nullptr;           // 上一个确认过不是游戏的交换链，免得每帧再查
HWND g_hwnd = nullptr;
WNDPROC g_wndProc = nullptr;
thread_local int g_wndDepth = 0;   // 窗口过程的重入层数，见 WndProc 顶部
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
// 游戏窗口本来没有输入法上下文（MHW 没有文字输入），面板要打字就得自己挂一个：
// 挂上之后 IME 的组字消息才会送进这个窗口，WndProc 再把它们交给 ImGui 后端。
HIMC g_imc = nullptr;
HIMC g_imcPrev = nullptr;
bool g_imeWant = false;
int g_imeStart = 0, g_imeComp = 0, g_imeResult = 0, g_imeChar = 0, g_imeFix = 0;   // 诊断用

// 输入法上下文（HIMC）属于线程，而且只有窗口自己的线程去挂才有用：
// MHW 的主线程建窗口，Present 却在渲染线程上，所以这里必须把 IMM 调用
// 投递到窗口线程去执行，不能顺手在 Present 里干。
constexpr UINT kImeSync = WM_APP + 0x51;   // 让窗口线程去碰输入法上下文
constexpr UINT kImePos = WM_APP + 0x52;    // 让窗口线程去摆组字窗/候选窗的位置
bool g_imeAttached = false;
std::atomic<bool> g_imeEnabled{true};
// 游戏窗口已经没了（要退出了）：面板逻辑一律停用，Present 只做转发。
// 实测窗口销毁时如果面板还开着，我们的 Present 会带着失效的交换链继续画，
// 游戏的退出流程就再也走不完（画面没了但手柄还能动游戏）。
std::atomic<bool> g_dead{false};
unsigned g_imeTick = 0;

void ImeOnWindowThread()
{
    if (!g_hwnd) return;
    if (!g_imeEnabled.load()) return;
    if (g_imeAttached && !g_imc) {
        g_imc = ::ImmCreateContext();
        if (!g_imc) { Log("ime: 建不了输入法上下文"); return; }
        g_imcPrev = ::ImmAssociateContext(g_hwnd, g_imc);
        ::ImmSetOpenStatus(g_imc, FALSE);   // 光标进输入框时才打开，平时不挡键盘
        const DWORD wt = ::GetWindowThreadProcessId(g_hwnd, nullptr);
        const HKL hkl = ::GetKeyboardLayout(wt);
        wchar_t file[260] = L"";
        ::ImmGetIMEFileNameW(hkl, file, 259);
        Log("ime: 挂上输入法上下文（窗口线程 %lu，当前线程 %lu，布局 %p，是输入法 %d，%ls）",
            wt, ::GetCurrentThreadId(), (void*)hkl, ::ImmIsIME(hkl) ? 1 : 0, file);
    }
    if (!g_imc) return;
    // 游戏自己也在管 IME（聊天输入），可能把窗口的上下文换掉或摘掉：被换了就挂回来
    HIMC cur = ::ImmGetContext(g_hwnd);
    if (cur) ::ImmReleaseContext(g_hwnd, cur);
    if (cur != g_imc) {
        ::ImmAssociateContext(g_hwnd, g_imc);
        if (++g_imeFix <= 3) Log("ime: 上下文被别人换掉了（%p），重新挂回来", (void*)cur);
    }
    ::ImmSetOpenStatus(g_imc, g_imeWant ? TRUE : FALSE);
}

void ImeOffWindowThread()
{
    if (!g_imc) return;
    ::ImmAssociateContext(g_hwnd, g_imcPrev);
    ::ImmDestroyContext(g_imc);
    g_imc = nullptr;
    g_imcPrev = nullptr;
    g_imeWant = false;
    Log("ime: 卸掉输入法上下文（消息 START=%d COMPOSITION=%d 结果串=%d CHAR=%d 重挂=%d）",
        g_imeStart, g_imeComp, g_imeResult, g_imeChar, g_imeFix);
    g_imeStart = g_imeComp = g_imeResult = g_imeChar = g_imeFix = 0;
}

// 组字窗和候选窗要摆在输入框底下。ImGui 给的 InputPos 是「后台缓冲」坐标（我们改过
// io.DisplaySize），输入法要的是窗口客户区坐标；只有 1:1 时才一样，系统缩放 125% 时
// 缓冲比客户区大，不换算的话这个点会被判成「在窗口外面」，输入法就把窗口丢到屏幕角上。
void ImeSetPosOnWindowThread(int x, int y)
{
    HIMC c = ::ImmGetContext(g_hwnd);
    if (!c) return;
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
    ::ImmReleaseContext(g_hwnd, c);
}

void ImeSetPos(int x, int y)
{
    if (!g_hwnd) return;
    if (::GetWindowThreadProcessId(g_hwnd, nullptr) == ::GetCurrentThreadId()) ImeSetPosOnWindowThread(x, y);
    else ::PostMessageW(g_hwnd, kImePos, (WPARAM)x, (LPARAM)y);
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
    if (!g_hwnd) return;
    g_imeAttached = attach;
    g_imeWant = want;
    if (::GetWindowThreadProcessId(g_hwnd, nullptr) == ::GetCurrentThreadId()) {
        if (attach) ImeOnWindowThread();
        else ImeOffWindowThread();
        return;
    }
    if (!::PostMessageW(g_hwnd, kImeSync, attach ? 1 : 0, want ? 1 : 0))
        Log("ime: 投递同步消息失败 %lu", ::GetLastError());
}

// 面板开着时把键鼠消息吃掉，游戏收不到（手柄另由 padfilter 挡）
LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    // 万一窗口过程被装了两层（游戏退出到标题会重建窗口），消息会在我们和
    // CallWindowProc 之间原地打转，最后栈溢出（0xC00000FD 在 USER32）。
    // 这里加个数上限，超了就直接交回系统，宁可不画面板也别把游戏带崩。
    ++g_wndDepth;
    struct DepthGuard {
        ~DepthGuard() { --g_wndDepth; }
    } depthGuard;
    if (g_wndDepth > 8) return ::DefWindowProcW(h, msg, w, l);
    if (msg == kImeSync) {
        if (w) ImeOnWindowThread();
        else ImeOffWindowThread();
        return 0;
    }
    if (msg == kImePos) {
        ImeSetPosOnWindowThread((int)w, (int)l);
        return 0;
    }
    if (g_open.load()) {
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
                    if (bytes > 0) {
                        std::wstring text((size_t)bytes / sizeof(wchar_t), L'\0');
                        ::ImmGetCompositionStringW(imc, GCS_RESULTSTR, &text[0], bytes);
                        ImGuiIO& mio = ImGui::GetIO();
                        for (wchar_t ch : text) mio.AddInputCharacterUTF16((ImWchar16)ch);
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
        ImGui_ImplWin32_WndProcHandler(h, msg, w, l);
        switch (msg) {
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR:
        case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_LBUTTONDBLCLK: case WM_RBUTTONDBLCLK: case WM_XBUTTONDOWN: case WM_XBUTTONUP:
            return 0;
        case WM_INPUT:
            return ::DefWindowProcW(h, msg, w, l);   // 原始输入：交给系统收尾，不给游戏
        }
    }
    if (!g_wndProc || g_wndProc == WndProc) return ::DefWindowProcW(h, msg, w, l);
    return ::CallWindowProcW(g_wndProc, h, msg, w, l);
}

// 记下我们改过的虚表，退出时好还原（诊断用）
void** g_swapVt = nullptr;
void** g_swapVt3 = nullptr;
void** g_queueVt = nullptr;

void UnpatchSlot(void** slot, void* orig)
{
    if (!slot || !orig) return;
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
    ID3D12Device* want = g_wantQueueOf.load();
    if (want && !g_queue.load() && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ID3D12Device* d = nullptr;
        if (SUCCEEDED(q->GetDevice(__uuidof(ID3D12Device), reinterpret_cast<void**>(&d))) && d) {
            if (d == want) {
                ID3D12CommandQueue* expected = nullptr;
                if (g_queue.compare_exchange_strong(expected, q)) {
                    // 不加引用：退出到标题时游戏会放掉队列，我们攥着会让它死不掉   // 游戏退出到标题时会把队列放掉，我们自己得攥着
                    Log("overlay: 记下游戏的 D3D12 命令队列 %p（不持引用）", (void*)q);
                }
            }
            d->Release();
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
    {
        const LONG_PTR prev = ::GetWindowLongPtrW(g_hwnd, GWLP_WNDPROC);
        if (prev == reinterpret_cast<LONG_PTR>(WndProc)) {
            Log("overlay: 窗口过程已经是我们，跳过重复接管");
        } else {
            g_wndProc = reinterpret_cast<WNDPROC>(prev);
            ::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc));
            Log("overlay: 接管窗口过程 %p -> %p", (void*)prev, (void*)&WndProc);
        }
    }
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
    if (g_api == kD3D11) ImGui_ImplDX11_Shutdown();
    else if (g_api == kD3D12) ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    ReleaseRtv();
    if (g_list) { g_list->Release(); g_list = nullptr; }
    for (UINT i = 0; i < kMaxBuffers; ++i)
        if (g_alloc[i]) { g_alloc[i]->Release(); g_alloc[i] = nullptr; }
    if (g_fence) { g_fence->Release(); g_fence = nullptr; }
    if (g_sc3) { g_sc3->Release(); g_sc3 = nullptr; }
    if (g_dev) g_dev = nullptr;
    g_dev12 = nullptr;
    g_queue = nullptr;
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
                return true;
            }
            DropRenderer();
            g_sc = sc;
            g_sc3 = sc3;
            g_dev12 = dev;                       // 不持引用
            g_bbFormat = d.BufferDesc.Format;
            Log("overlay: 新交换链接管完成（格式 %d，%ux%u）", (int)g_bbFormat, d.BufferDesc.Width, d.BufferDesc.Height);
            return true;
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
    const bool open = g_open.load();
    if (open && !g_wasOpen) {
        if (g_imeEnabled.load()) ImeRequest(true, false);
        if (g_cb.opened) g_cb.opened();
    }
    if (!open && g_wasOpen) ImeRequest(false, false);
    g_wasOpen = open;
    if (!open) return;
    if (g_api == kD3D11 && !g_rtv && !CreateRtv(sc)) return;

    if (g_api == kD3D11) ImGui_ImplDX11_NewFrame();
    else ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
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
    LARGE_INTEGER q0, q1;
    ::QueryPerformanceCounter(&q0);
    if (!g_failed.load() && !(flags & DXGI_PRESENT_TEST)) {
        // 第一次打开面板才初始化（要读字体文件），从没打开过就完全不碰游戏的渲染
        if (!g_inited && g_open.load()) {
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
        if (!g_open.load()) PerfTick((unsigned long long)q0.QuadPart, (unsigned long long)q1.QuadPart);
    }
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
    BeforeResize(sc);
    const HRESULT r = g_resize(sc, n, w, h, f, flags);
    if (sc == g_sc) hdr::Detect(sc);
    return r;
}

HRESULT WINAPI HkResize1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags, const UINT* mask,
                         IUnknown* const* queues)
{
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
    if (open && !g_open.load()) g_openAt = ::GetTickCount64();
    g_open = open && !g_failed.load();
}
bool IsOpen() { return g_open.load() && !g_failed.load(); }
bool Failed() { return g_failed.load(); }
void SetDebugAfterDraw(void (*fn)(void*)) { g_debugAfterDraw = fn; }
void SetDebugAfterDraw12(void (*fn)(void*, void*)) { g_debugAfterDraw12 = fn; }

void Shutdown(bool processExiting)
{
    if (!g_inited) return;
    SetStage("shutdown");
    Log("overlay: 收尾（api=%d，%s）", (int)g_api, processExiting ? "进程退出" : "正常卸载");
    if (processExiting) {
        // 进程正在退出：这一段什么都不做，一行日志之后就走人。
        // 教训（0.7.23~0.7.27 一直卡在这里）：
        //   SetWindowLongPtrW 是跨线程调用，窗口已销毁时它会等窗口线程；
        //   ImeRequest 会 PostMessage 到那个窗口；
        //   两者都能让 DllMain 在 loader lock 里一直等下去 —— 表现就是
        //   画面没了、BGM 还在响、进程永远不退出。
        // 进程马上就结束了，窗口过程、输入法、D3D 对象都交给系统回收。
        Log("overlay: 进程退出：不做任何收尾");
        g_inited = false;
        return;
    }
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
    ID3D12CommandQueue* q = g_queue.exchange(nullptr);
    if (q) q->Release();
    if (g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
    g_dev = nullptr;   // 同上
    if (g_imeAttached && g_hwnd && ::GetCurrentThreadId() == ::GetWindowThreadProcessId(g_hwnd, nullptr)) {
        ImeOffWindowThread();
        g_imeAttached = false;
    }
    if (g_hwnd && g_wndProc) {
        ::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_wndProc));
        g_wndProc = nullptr;
    }
    g_inited = false;
}

// 游戏退出时只做这一件事。D3D 资源的释放会走驱动（实测 amdxc64 崩），
// 而进程马上就要结束，让系统回收更安全 —— 退出路径上我们一个驱动调用都不做。
void ShutdownForExit()
{
    // 只写一行日志，然后什么都不做。特别是不去 SetWindowLongPtrW：
    // 那是跨线程调用，窗口正在销毁时它会等窗口线程，而窗口线程可能正卡在
    // DestroyWindow 里等我们 —— 实测这种情况下进程会一直不退出（BGM 还在响）。
    Log("overlay: 退出路径：停面板、放开交换链引用");
    g_dead = true;         // Present 从下一帧起只转发，不再画面板
    g_open = false;        // 面板视为已关闭（游戏那侧再也不该被我们挡输入了）
    g_wndProc = nullptr;   // 窗口马上就没了，不去动它
    // 这里只放开我们对「游戏对象」的引用，这几步都是纯 COM 引用计数，不会进驱动：
    //   g_sc3  是我们 QueryInterface 拿到的交换链引用，攥着它游戏就销毁不掉交换链；
    //   g_alloc 里的包装各自持有后备缓冲的 ID3D12Resource 引用，同理。
    // 我们自己建的堆/PSO/围栏一律不动（那些释放会进驱动，进程马上结束，交给系统）。
    for (UINT i = 0; i < kMaxBuffers; ++i)
        if (g_alloc[i]) { g_alloc[i]->Release(); g_alloc[i] = nullptr; }
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
    if (g_sc3) { g_sc3->Release(); g_sc3 = nullptr; }
    Log("overlay: 已放开交换链/后备缓冲引用");
    // 把打进游戏虚表的钩子还原成原函数：验证「退出卡住」是不是 DXGI 销毁流程中
    // 走到我们的 Present/Resize 钩子引起的。还原之后游戏退出路径上完全看不到我们。
    UnpatchSlot(&g_swapVt[8], reinterpret_cast<void*>(g_present));
    UnpatchSlot(&g_swapVt[13], reinterpret_cast<void*>(g_resize));
    if (g_swapVt3) UnpatchSlot(&g_swapVt3[39], reinterpret_cast<void*>(g_resize1));
    if (g_queueVt) UnpatchSlot(&g_queueVt[10], reinterpret_cast<void*>(g_execute));
    Log("overlay: 虚表钩子已还原（Present/ResizeBuffers/ResizeBuffers1/ExecuteCommandLists）");
    g_inited = false;
}

const char* Stage() { return g_stage.load(); }

bool WindowGone()
{
    const HWND h = g_hwnd;
    return h != nullptr && ::IsWindow(h) == FALSE;
}
void SetImeEnabled(bool on) { g_imeEnabled = on; if (!on) ImeRequest(false, false); }

} // namespace overlay
