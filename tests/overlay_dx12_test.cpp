// 游戏开着 DX12 时：面板画不了，要明确认定失败（不能卡在「开着但看不见」，那样手柄会一直被挡住）。
// 用 WARP 建一个 D3D12 的交换链，模拟 DX12 模式下游戏每帧 Present。
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <cstdio>
#include <string>

#include "../src/overlay.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) std::printf("  ok   " __VA_ARGS__); \
    else { std::printf("  FAIL " __VA_ARGS__); ++g_fail; } std::printf("\n"); } while (0)

static int g_draws = 0;
static void OnInit(float) {}
static void OnDraw(float, float) { ++g_draws; }

int main()
{
    overlay::Callbacks cb;
    cb.init = OnInit;
    cb.draw = OnDraw;
    std::string err;
    CHECK(overlay::Install(cb, err), "挂 Present / ResizeBuffers %s", err.c_str());

    IDXGIFactory4* fac = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&fac));
    IDXGIAdapter* warp = nullptr;
    if (fac) fac->EnumWarpAdapter(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&warp));
    ID3D12Device* dev = nullptr;
    if (warp) D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&dev));
    CHECK(dev != nullptr, "建 D3D12 设备（WARP）");
    if (!dev) return 1;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"binq_dx12_test";
    RegisterClassExW(&wc);
    HWND hw = CreateWindowExW(0, wc.lpszClassName, L"MHW DX12", WS_OVERLAPPEDWINDOW, 0, 0, 1296, 759, nullptr, nullptr,
                              wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 1280; sd.Height = 720;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = fac->CreateSwapChainForHwnd(queue, hw, &sd, nullptr, nullptr, &sc1);
    CHECK(SUCCEEDED(hr) && sc1, "建 D3D12 交换链（hr=%08lx）", (unsigned long)hr);
    if (!sc1) return 1;

    for (int i = 0; i < 3; ++i) sc1->Present(0, 0);
    CHECK(!overlay::Failed() && !overlay::IsOpen(), "面板没开：什么都不做");

    overlay::SetOpen(true);
    CHECK(overlay::IsOpen(), "刚按下快捷键：先当作打开（还不知道画不画得了）");
    const ULONGLONG t0 = GetTickCount64();
    while (GetTickCount64() - t0 < 2200) { sc1->Present(0, 0); Sleep(16); }
    CHECK(overlay::Failed(), "开着面板等了 2 秒都只有 D3D12 的交换链：认定画不了");
    CHECK(!overlay::IsOpen(), "认定失败后 IsOpen() 是 false（主循环据此放开手柄和鼠标）");
    CHECK(g_draws == 0, "一次都没往 D3D12 的画面上画（draws=%d）", g_draws);
    overlay::SetOpen(true);
    CHECK(!overlay::IsOpen(), "失败后再按快捷键也不会打开");

    sc1->Release(); queue->Release(); dev->Release(); warp->Release(); fac->Release();
    DestroyWindow(hw);
    std::printf("\n%s（%d 项失败）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
