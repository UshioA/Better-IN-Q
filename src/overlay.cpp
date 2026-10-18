#include "overlay.h"

#include "log.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <atomic>

#include <cfloat>

#include "imgui.h"
#include "imgui_internal.h"
#include "backends/imgui_impl_dx11.h"
#include "backends/imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace overlay {
namespace {

typedef HRESULT(WINAPI* PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(WINAPI* ResizeFn)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

Callbacks g_cb;
PresentFn g_present = nullptr;
ResizeFn g_resize = nullptr;
std::atomic<bool> g_open{false};
bool g_wasOpen = false;
bool g_inited = false;
std::atomic<bool> g_failed{false};
std::atomic<unsigned long long> g_openAt{0};   // 这次打开面板的时间（还没初始化成功时，用来判断要不要放弃）
IDXGISwapChain* g_sc = nullptr;                // 游戏的交换链（只比指针，不持有引用）
IDXGISwapChain* g_foreign = nullptr;           // 上一个确认过不是游戏的交换链，免得每帧再查
HWND g_hwnd = nullptr;
WNDPROC g_wndProc = nullptr;
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
void (*g_debugAfterDraw)(void*) = nullptr;
ImU32 g_lastScaledEvent = 0;

// 面板开着时把键鼠消息吃掉，游戏收不到（手柄另由 padfilter 挡）
LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l)
{
    if (g_open.load()) {
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
    return ::CallWindowProcW(g_wndProc, h, msg, w, l);
}

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

// 用这个交换链初始化。它不是 D3D11 的（游戏开着 DX12，或者是别的模组自己建的 DX12 交换链）就返回 false，
// 不算失败：进程里所有交换链共用一张虚表，别人的 Present 也会走到这里。
bool InitRenderer(IDXGISwapChain* sc)
{
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_dev))) || !g_dev) {
        g_dev = nullptr;
        if (sc != g_foreign) {
            g_foreign = sc;
            DXGI_SWAP_CHAIN_DESC d{};
            sc->GetDesc(&d);
            Log("overlay: 跳过一个不是 D3D11 的交换链（缓冲 %ux%u）", d.BufferDesc.Width, d.BufferDesc.Height);
        }
        return false;
    }
    g_dev->GetImmediateContext(&g_ctx);
    DXGI_SWAP_CHAIN_DESC d{};
    sc->GetDesc(&d);
    g_hwnd = d.OutputWindow;
    g_sc = sc;
    RECT rc{};
    ::GetClientRect(g_hwnd, &rc);
    const float h = (float)d.BufferDesc.Height;   // 字号按真实分辨率定，不按缩小后的窗口

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;
    if (!ImGui_ImplWin32_Init(g_hwnd) || !ImGui_ImplDX11_Init(g_dev, g_ctx)) {
        Log("overlay: ImGui 后端初始化失败");
        g_failed = true;
        return false;
    }
    if (g_cb.init) g_cb.init(h > 0 ? h / 1080.0f : 1.0f);
    g_wndProc = reinterpret_cast<WNDPROC>(::SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(WndProc)));
    g_inited = true;
    Log("overlay: 就绪（窗口 %ldx%ld，缓冲 %ux%u，格式 %d）", rc.right - rc.left, rc.bottom - rc.top,
        d.BufferDesc.Width, d.BufferDesc.Height, (int)d.BufferDesc.Format);
    return true;
}

// 这一帧是不是游戏的交换链在 Present。游戏重建交换链（切全屏之类）时指针会变，
// 同一个窗口、同一个设备的就认作新的游戏交换链。
bool IsGameSwapChain(IDXGISwapChain* sc)
{
    if (sc == g_sc) return true;
    if (sc == g_foreign) return false;
    DXGI_SWAP_CHAIN_DESC d{};
    ID3D11Device* dev = nullptr;
    const bool same = SUCCEEDED(sc->GetDesc(&d)) && d.OutputWindow == g_hwnd &&
                      SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&dev))) && dev == g_dev;
    if (dev) dev->Release();
    if (!same) { g_foreign = sc; return false; }
    g_sc = sc;
    ReleaseRtv();
    return true;
}

void RenderPanel(IDXGISwapChain* sc)
{
    const bool open = g_open.load();
    if (open && !g_wasOpen && g_cb.opened) g_cb.opened();
    g_wasOpen = open;
    if (!open) return;
    if (!g_rtv && !CreateRtv(sc)) return;

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGuiIO& io = ImGui::GetIO();
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
    if (g_cb.draw) g_cb.draw(io.DisplaySize.x, io.DisplaySize.y);
    ImGui::Render();
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

HRESULT WINAPI HkPresent(IDXGISwapChain* sc, UINT sync, UINT flags)
{
    if (!g_failed.load() && !(flags & DXGI_PRESENT_TEST)) {
        // 第一次打开面板才初始化（要读字体文件），从没打开过就完全不碰游戏的渲染
        if (!g_inited && g_open.load() && !InitRenderer(sc) && !g_failed.load() &&
            ::GetTickCount64() - g_openAt.load() > 1500) {
            // 开着面板 1.5 秒都没等到 D3D11 的交换链：游戏是 DX12 模式
            Log("overlay: 游戏的交换链不是 D3D11 的（游戏开了 DX12？），面板不可用；重开快捷键不受影响");
            g_failed = true;
        }
        if (g_inited && IsGameSwapChain(sc)) RenderPanel(sc);
    }
    return g_present(sc, sync, flags);
}

HRESULT WINAPI HkResize(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT flags)
{
    if (sc == g_sc) ReleaseRtv();   // 后台缓冲要被换掉了，先放手；下次画的时候再建
    return g_resize(sc, n, w, h, f, flags);
}

// 建一个临时窗口和交换链，只为拿到交换链的虚表
void** GetSwapChainVtable(std::string& err)
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
    if (SUCCEEDED(hr) && sc) vt = *reinterpret_cast<void***>(sc);
    else err = "建临时交换链失败 hr=" + std::to_string((long)hr);
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    if (hw) ::DestroyWindow(hw);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return vt;
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

} // namespace

bool Install(const Callbacks& cb, std::string& err)
{
    g_cb = cb;
    void** vt = GetSwapChainVtable(err);
    if (!vt) return false;
    // IDXGISwapChain 虚表：8 = Present，13 = ResizeBuffers
    if (!PatchSlot(&vt[8], reinterpret_cast<void*>(HkPresent), reinterpret_cast<void**>(&g_present)) ||
        !PatchSlot(&vt[13], reinterpret_cast<void*>(HkResize), reinterpret_cast<void**>(&g_resize))) {
        err = "改交换链虚表失败";
        return false;
    }
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

} // namespace overlay
