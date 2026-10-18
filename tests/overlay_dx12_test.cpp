// 游戏开着 DX12 时面板也要能画出来。用 WARP 建 D3D12 设备和交换链，模拟游戏每帧：
// 自己清屏 → ExecuteCommandLists → Present。打开面板后截图看效果，中途 ResizeBuffers 一次。
//   overlay_dx12_test.exe <输出目录>
#define WIDL_EXPLICIT_AGGREGATE_RETURNS   // 和 src/overlay.cpp 一样，见那里的说明
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <cstdio>
#include <string>

#include "../src/config.h"
#include "../src/overlay.h"
#include "../src/ui.h"
#include "../src/i18n.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { if (cond) std::printf("  ok   " __VA_ARGS__); \
    else { std::printf("  FAIL " __VA_ARGS__); ++g_fail; } std::printf("\n"); } while (0)

static Config g_cfg;
static ui::Model g_model;
static int g_draws = 0;
static void OnInit(float scale) { ui::Init(scale); }
static void OnOpened() { ui::OnOpen(g_model); }
static void OnDraw(float w, float h) { ui::Draw(g_model, w, h); ++g_draws; }

static ID3D12Device* dev = nullptr;
static ID3D12Fence* fence = nullptr;
static UINT64 fenceVal = 0;
static HANDLE fenceEv = nullptr;
static std::wstring g_shot;
static bool g_shotOk = false;
static int g_litPixels = 0;

static void WaitQueue(ID3D12CommandQueue* q)
{
    q->Signal(fence, ++fenceVal);
    if (fence->GetCompletedValue() < fenceVal) {
        fence->SetEventOnCompletion(fenceVal, fenceEv);
        WaitForSingleObject(fenceEv, 5000);
    }
}

// 面板画完、交给游戏 Present 之前：把后台缓冲拷到回读缓冲存 PNG，顺便数一数「金色」的像素
static void Save12(void* res, void* queue)
{
    if (g_shot.empty()) return;
    ID3D12Resource* bb = static_cast<ID3D12Resource*>(res);
    ID3D12CommandQueue* q = static_cast<ID3D12CommandQueue*>(queue);
    const D3D12_RESOURCE_DESC rd = bb->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows = 0;
    UINT64 rowSize = 0, total = 0;
    dev->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &rowSize, &total);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = nullptr;
    dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                 __uuidof(ID3D12Resource), reinterpret_cast<void**>(&rb));
    ID3D12CommandAllocator* al = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&al));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, __uuidof(ID3D12GraphicsCommandList),
                           reinterpret_cast<void**>(&cl));
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
    src.pResource = bb; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    cl->ResourceBarrier(1, &b);
    cl->Close();
    ID3D12CommandList* lists[] = {cl};
    q->ExecuteCommandLists(1, lists);
    WaitQueue(q);

    BYTE* p = nullptr;
    D3D12_RANGE rr{0, (SIZE_T)total};
    rb->Map(0, &rr, reinterpret_cast<void**>(&p));
    const UINT w = (UINT)rd.Width, h = rd.Height, pitch = fp.Footprint.RowPitch;
    g_litPixels = 0;
    for (UINT y = 0; y < h; y += 2)
        for (UINT x = 0; x < w; x += 2) {
            const BYTE* px = p + y * pitch + x * 4;   // R8G8B8A8：面板的金色是 R 高、B 低
            if (px[0] > 180 && px[1] > 140 && px[2] < 140) ++g_litPixels;
        }
    IWICImagingFactory* f = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    IWICStream* s = nullptr; f->CreateStream(&s); s->InitializeFromFilename(g_shot.c_str(), GENERIC_WRITE);
    IWICBitmapEncoder* enc = nullptr; f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    enc->Initialize(s, WICBitmapEncoderNoCache);
    IWICBitmapFrameEncode* fr = nullptr; enc->CreateNewFrame(&fr, nullptr); fr->Initialize(nullptr);
    fr->SetSize(w, h);
    // PNG 编码器只认 BGRA（给 RGBA 它会悄悄改成 BGRA，红蓝对调），先换成 BGRA 再写
    std::string bgra(reinterpret_cast<const char*>(p), (size_t)pitch * h);
    for (UINT y = 0; y < h; ++y)
        for (UINT x = 0; x < w; ++x) std::swap(bgra[(size_t)y * pitch + x * 4], bgra[(size_t)y * pitch + x * 4 + 2]);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    fr->SetPixelFormat(&fmt);
    fr->WritePixels(h, pitch, pitch * h, reinterpret_cast<BYTE*>(&bgra[0]));
    fr->Commit(); enc->Commit();
    fr->Release(); enc->Release(); s->Release(); f->Release();
    rb->Unmap(0, nullptr);
    rb->Release(); cl->Release(); al->Release();
    std::wprintf(L"  截图 %ls（金色像素 %d）\n", g_shot.c_str(), g_litPixels);
    g_shotOk = true;
    g_shot.clear();
}

int wmain(int argc, wchar_t** argv)
{
    const std::wstring out = argc > 1 ? argv[1] : L".";
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::string err;
    LoadConfig(L"config\\BetterINQ.ini", g_cfg, err);
    i18n::Set(i18n::FromString(g_cfg.language));   // 面板语言跟着配置走，和 DLL 里一样
    g_model.cfg = &g_cfg;
    g_model.ready = true;
    g_model.version = "dx12";

    overlay::Callbacks cb;
    cb.init = OnInit;
    cb.opened = OnOpened;
    cb.draw = OnDraw;
    CHECK(overlay::Install(cb, err), "挂 Present / ResizeBuffers %s", err.c_str());
    overlay::SetDebugAfterDraw12(Save12);

    IDXGIFactory4* fac = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&fac));
    IDXGIAdapter* warp = nullptr;
    if (fac) fac->EnumWarpAdapter(__uuidof(IDXGIAdapter), reinterpret_cast<void**>(&warp));
    if (warp) D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), reinterpret_cast<void**>(&dev));
    CHECK(dev != nullptr, "建 D3D12 设备（WARP）");
    if (!dev) return 1;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
    fenceEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"binq_dx12_test";
    RegisterClassExW(&wc);
    HWND hw = CreateWindowExW(0, wc.lpszClassName, L"MHW DX12", WS_OVERLAPPEDWINDOW, 0, 0, 1936, 1119, nullptr, nullptr,
                              wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = 1920; sd.Height = 1080;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = fac->CreateSwapChainForHwnd(queue, hw, &sd, nullptr, nullptr, &sc1);
    CHECK(SUCCEEDED(hr) && sc1, "建 D3D12 交换链（hr=%08lx）", (unsigned long)hr);
    if (!sc1) return 1;
    IDXGISwapChain3* sc = nullptr;
    sc1->QueryInterface(__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&sc));

    // 「游戏」自己的渲染：每帧清屏成暗绿色
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 3;
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&rtvHeap));
    const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12CommandAllocator* al[3] = {};
    for (auto& a : al)
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&a));
    ID3D12GraphicsCommandList* cl = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al[0], nullptr, __uuidof(ID3D12GraphicsCommandList),
                           reinterpret_cast<void**>(&cl));
    cl->Close();
    auto frame = [&]() {
        WaitQueue(queue);   // 简单起见每帧等完（测的是面板，不是游戏的帧率）
        const UINT i = sc->GetCurrentBackBufferIndex();
        ID3D12Resource* bb = nullptr;
        sc->GetBuffer(i, __uuidof(ID3D12Resource), reinterpret_cast<void**>(&bb));
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += (SIZE_T)i * inc;
        dev->CreateRenderTargetView(bb, nullptr, rtv);
        al[i]->Reset();
        cl->Reset(al[i], nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bb;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cl->ResourceBarrier(1, &b);
        const float col[4] = {0.22f, 0.32f, 0.22f, 1};
        cl->ClearRenderTargetView(rtv, col, 0, nullptr);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        cl->ResourceBarrier(1, &b);
        cl->Close();
        ID3D12CommandList* lists[] = {cl};
        queue->ExecuteCommandLists(1, lists);
        bb->Release();
        sc->Present(0, 0);
    };

    for (int i = 0; i < 3; ++i) frame();
    CHECK(!overlay::Failed() && g_draws == 0, "面板没开：什么都不画");

    overlay::SetOpen(true);
    for (int i = 0; i < 40; ++i) {
        if (i == 39) g_shot = out + L"\\overlay_dx12_1080p.png";
        frame();
    }
    CHECK(!overlay::Failed() && overlay::IsOpen(), "DX12 下面板打开了，没有判失败");
    CHECK(g_draws > 30, "每帧都画了（draws=%d）", g_draws);
    CHECK(g_shotOk && g_litPixels > 500, "截图里有面板（金色像素 %d）", g_litPixels);

    // 游戏改分辨率：插件不能拿着后台缓冲不放，否则 ResizeBuffers 会失败
    WaitQueue(queue);
    hr = sc->ResizeBuffers(0, 1280, 720, DXGI_FORMAT_UNKNOWN, 0);
    CHECK(SUCCEEDED(hr), "开着面板 ResizeBuffers 成功（hr=%08lx）", (unsigned long)hr);
    g_shotOk = false;
    const int before = g_draws;
    for (int i = 0; i < 20; ++i) {
        if (i == 19) g_shot = out + L"\\overlay_dx12_720p.png";
        frame();
    }
    CHECK(g_draws == before + 20 && g_shotOk && g_litPixels > 200, "改分辨率后照常画（draws +%d，金色像素 %d）",
          g_draws - before, g_litPixels);

    overlay::SetOpen(false);
    const int closed = g_draws;
    for (int i = 0; i < 5; ++i) frame();
    CHECK(g_draws == closed, "关掉后不再画");

    WaitQueue(queue);
    cl->Release();
    for (auto& a : al) a->Release();
    rtvHeap->Release();
    sc->Release(); sc1->Release(); fence->Release(); queue->Release(); dev->Release(); warp->Release(); fac->Release();
    DestroyWindow(hw);
    std::printf("\n%s（%d 项失败）\n", g_fail ? "有失败" : "全部通过", g_fail);
    return g_fail ? 1 : 0;
}
