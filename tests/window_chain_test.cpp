// Real Win32 + WARP regression: rebuilding a renderer must not re-subclass
// above another overlay that already forwards to us. Never touches the game.
#include "../src/overlay.cpp"
#include "../src/crashfilter.h"
#include "../src/hangdump.h"
#include <cstdio>
#include <fstream>
#include <future>
#include <chrono>
#include <iterator>
#include <string>
#include <vector>
static int failures=0, closeCalls=0, destroyCalls=0, ncCalls=0, wrapperCalls=0, probes=0;
static WNDPROC wrapperPrevious=nullptr;
static bool wrapperBounces=false;   // 模拟「前驱把消息再派回窗口」的 WinMM 式窗口过程
constexpr UINT kProbe=WM_APP+0x71;
constexpr UINT kNestBase=WM_APP+0x180;
static LRESULT CALLBACK GameProc(HWND,UINT,WPARAM,LPARAM);
static HWND nestOther=nullptr;
static int nestWrapperCalls=0, nestClassCalls=0, ncBounceCalls=0;
static bool nestDifferent=false, ncBounce=false;
static LRESULT CALLBACK NestPrevious(HWND h,UINT m,WPARAM w,LPARAM l) {
    if(m>=kNestBase && m<kNestBase+64) {
        ++nestWrapperCalls;
        if(m==kNestBase+32) {
            if(nestDifferent) return SendMessageW(h,kNestBase+33,0,0);
            if(nestOther && h!=nestOther) return SendMessageW(nestOther,m,0,0);
        }
        return 456;
    }
    return CallWindowProcW(GameProc,h,m,w,l);
}
#define CHECK(x) do { const bool ok=(x); std::printf("%s %s\n", ok?"ok":"FAIL", #x); if(!ok) ++failures; } while(0)
static LRESULT CALLBACK GameProc(HWND h,UINT m,WPARAM w,LPARAM l) {
    if(m>=kNestBase && m<kNestBase+64) { ++nestClassCalls; return 789; }
    if(m==WM_NCDESTROY && ncBounce) { ++ncBounceCalls; return SendMessageW(h,m,w,l); }
    if(m==kProbe) { ++probes; return 123; }
    if(m==WM_CLOSE) { ++closeCalls; DestroyWindow(h); return 0; }
    if(m==WM_DESTROY) ++destroyCalls;
    if(m==WM_NCDESTROY) ++ncCalls;
    return DefWindowProcW(h,m,w,l);
}
static LRESULT CALLBACK Wrapper(HWND h,UINT m,WPARAM w,LPARAM l) {
    if(m==kProbe) ++wrapperCalls;
    if(wrapperBounces) return SendMessageW(h,m,w,l);   // 原地把消息派回窗口顶端的窗口过程
    return CallWindowProcW(wrapperPrevious,h,m,w,l);
}
struct Window {
    HWND h=nullptr; IDXGISwapChain* sc=nullptr; ID3D11Device* dev=nullptr; ID3D11DeviceContext* ctx=nullptr;
    bool Create() {
        h=CreateWindowExW(0,L"BetterINQ_window_chain_test",L"isolated exit regression",WS_OVERLAPPEDWINDOW,0,0,320,240,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
        DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount=2; sd.BufferDesc.Width=320; sd.BufferDesc.Height=240;
        sd.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow=h; sd.SampleDesc.Count=1; sd.Windowed=TRUE; sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        return SUCCEEDED(D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&sd,&sc,&dev,nullptr,&ctx));
    }
    ~Window() { if(IsWindow(h)) DestroyWindow(h); if(sc)sc->Release(); if(ctx)ctx->Release(); if(dev)dev->Release(); }
};
int main() {
    WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc=GameProc; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"BetterINQ_window_chain_test"; RegisterClassExW(&wc);
    std::string err; overlay::Callbacks cb; CHECK(overlay::Install(cb,err));
    Window a; if(!a.Create()) return 2;
    CHECK(overlay::InitRenderer(a.sc)==1);
    CHECK(overlay::PreviousProc(a.h)==GameProc);
    // Real contention: the window thread must return while a different thread
    // owns the renderer lock, retain both edges, and never touch ImGui itself.
    {
        overlay::SetOpen(true);
        a.sc->Present(0,0);
        SendMessageW(a.h,WM_KEYDOWN,'W',1 | (0x11 << 16));
        a.sc->Present(0,0);
        CHECK(ImGui::GetKeyData(ImGuiKey_W)->Down);
        std::promise<void> locked, release;
        auto ready=locked.get_future(); auto go=release.get_future();
        auto renderer=std::async(std::launch::async,[&] {
            std::lock_guard<std::recursive_mutex> lock(overlay::g_rendererMutex);
            locked.set_value(); go.wait();
        });
        ready.wait();
        const int queuedBefore=GImGui->InputEventsQueue.Size;
        // A blocking WndProc implementation fails this test by deadlocking here.
        CHECK(SendMessageW(a.h,WM_KEYUP,'W',1 | (0x11 << 16) | (1LL << 31))==0);
        CHECK(SendMessageW(a.h,WM_KEYDOWN,'S',1 | (0x1f << 16))==0);
        CHECK(SendMessageW(a.h,WM_KEYUP,'S',1 | (0x1f << 16) | (1LL << 31))==0);
        CHECK(GImGui->InputEventsQueue.Size==queuedBefore);
        CHECK(ImGui::GetKeyData(ImGuiKey_W)->Down);
        CHECK(overlay::g_inputCount>0);
        release.set_value(); renderer.get();
        for(int i=0;i<5;++i) a.sc->Present(0,0);
        CHECK(!ImGui::GetKeyData(ImGuiKey_W)->Down && !ImGui::GetKeyData(ImGuiKey_S)->Down);

        // Closing between down/up, even without a closed Present, must purge
        // both already-consumed held keys and not-yet-consumed old characters.
        SendMessageW(a.h,WM_KEYDOWN,'W',1 | (0x11 << 16));
        a.sc->Present(0,0);
        CHECK(ImGui::GetKeyData(ImGuiKey_W)->Down);
        SendMessageW(a.h,WM_CHAR,'x',0);
        const auto stale=overlay::CurrentInputTicket();
        overlay::SetOpen(false);
        SendMessageW(a.h,WM_KEYUP,'W',1 | (1LL << 31));
        overlay::SetOpen(true);
        overlay::QueuePanelInput(stale,{overlay::InputKind::Character,'z'});
        a.sc->Present(0,0);
        CHECK(!ImGui::GetKeyData(ImGuiKey_W)->Down);
        CHECK(ImGui::GetIO().InputQueueCharacters.empty());

        // Boundedness, including overflow after a previously consumed down.
        SendMessageW(a.h,WM_KEYDOWN,'W',1 | (0x11 << 16));
        a.sc->Present(0,0);
        for(size_t i=0;i<overlay::kInputCapacity+10;++i) SendMessageW(a.h,WM_CHAR,'q',0);
        SendMessageW(a.h,WM_KEYUP,'W',1 | (1LL << 31));
        CHECK(overlay::g_inputCount<=overlay::kInputCapacity && overlay::g_inputOverflow);
        a.sc->Present(0,0);
        CHECK(!ImGui::GetKeyData(ImGuiKey_W)->Down);
        CHECK(ImGui::GetIO().InputQueueCharacters.empty());

        // Old renderer producers cannot deliver into a new ImGui context.
        const auto oldRenderer=overlay::CurrentInputTicket();
        SendMessageW(a.h,WM_KEYDOWN,'S',1 | (0x1f << 16));
        overlay::DropRenderer(); CHECK(overlay::InitRenderer(a.sc)==1);
        overlay::QueuePanelInput(oldRenderer,{overlay::InputKind::Key,ImGuiKey_W,0,0,true});
        a.sc->Present(0,0);
        CHECK(!ImGui::GetKeyData(ImGuiKey_W)->Down && !ImGui::GetKeyData(ImGuiKey_S)->Down);
        const size_t beforeDevice=overlay::g_inputCount;
        SendMessageW(a.h,WM_DEVICECHANGE,0x0007,0);
        CHECK(overlay::g_inputCount==beforeDevice+1);
        CHECK(overlay::g_inputs[beforeDevice].kind==overlay::InputKind::DeviceChange);
        overlay::DrainPanelInput(overlay::CurrentInputTicket());
        CHECK(overlay::g_inputCount==0);
        // Capture bookkeeping is per window thread, including a recreated HWND.
        auto otherWindowThread=std::async(std::launch::async,[] {
            const bool clean=overlay::g_inputCapture==nullptr && overlay::g_mouseButtons==0;
            overlay::g_mouseButtons=31;
            return clean;
        });
        CHECK(otherWindowThread.get() && overlay::g_mouseButtons==0);
        SendMessageW(a.h,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(20,20));
        CHECK(GetCapture()==a.h);
        overlay::SetOpen(false);
        SendMessageW(a.h,overlay::kImeSync,0,0);
        CHECK(GetCapture()!=a.h);
    }
    wrapperPrevious=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(a.h,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(Wrapper)));
    for(int i=0;i<3;++i) {
        overlay::DropRenderer(); CHECK(overlay::InitRenderer(a.sc)==1);
        CHECK(GetWindowLongPtrW(a.h,GWLP_WNDPROC)==reinterpret_cast<LONG_PTR>(Wrapper));
        CHECK(overlay::PreviousProc(a.h)==GameProc);
        CHECK(SendMessageW(a.h,kProbe,0,0)==123);
    }
    CHECK(probes==3 && wrapperCalls==3);
    // Active wrapped chain must deliver actual close/destruction, not just probes.
    SendMessageW(a.h,WM_CLOSE,0,0);
    CHECK(closeCalls==1 && destroyCalls==1 && ncCalls==1 && !IsWindow(a.h));
    CHECK(!overlay::PreviousProc(a.h));
    overlay::Shutdown();
    // Recreate the wrapped chain for shutdown/old-window forwarding checks.
    Window old; if(!old.Create()) return 2;
    CHECK(overlay::InitRenderer(old.sc)==1);
    wrapperPrevious=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(old.h,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(Wrapper)));
    overlay::Shutdown();
    CHECK(GetWindowLongPtrW(old.h,GWLP_WNDPROC)==reinterpret_cast<LONG_PTR>(Wrapper));
    CHECK(SendMessageW(old.h,kProbe,0,0)==123);
    // The old, wrapped HWND must keep forwarding after a new HWND becomes active.
    Window b; if(!b.Create()) return 2;
    CHECK(overlay::InitRenderer(b.sc)==1);
    CHECK(SendMessageW(old.h,kProbe,0,0)==123);
    SendMessageW(old.h,WM_CLOSE,0,0);
    CHECK(closeCalls==2 && destroyCalls==2 && ncCalls==2 && !IsWindow(old.h));
    CHECK(!overlay::PreviousProc(old.h));
    CHECK(overlay::g_inited && overlay::g_hwnd==b.h);
    overlay::Shutdown();
    CHECK(GetWindowLongPtrW(b.h,GWLP_WNDPROC)==reinterpret_cast<LONG_PTR>(GameProc));
    CHECK(!overlay::PreviousProc(b.h));
    CHECK(overlay::InitRenderer(b.sc)==1);
    {
        std::unique_lock<std::recursive_mutex> renderLock(overlay::g_rendererMutex);
        ImGuiContext* const activeContext=ImGui::GetCurrentContext();
        CHECK(activeContext!=nullptr);
        std::promise<void> started;
        auto startedFuture=started.get_future();
        auto shutdown=std::async(std::launch::async,[&started] {
            // This signals entry to the test thread, not arrival at the lock.
            started.set_value();
            overlay::ShutdownForExit();
        });
        startedFuture.wait();
        const bool waiting=shutdown.wait_for(std::chrono::milliseconds(100))==std::future_status::timeout;
        CHECK(waiting);
        // Read context only while holding the renderer lock and while teardown
        // is blocked. A failed pre-fix run must not race this inspection.
        if(waiting) CHECK(ImGui::GetCurrentContext()==activeContext);
        renderLock.unlock();
        shutdown.get();
        CHECK(ImGui::GetCurrentContext()==nullptr);
        CHECK(overlay::g_dead.load());
    }
    CHECK(SendMessageW(b.h,kProbe,0,0)==123);
    SendMessageW(b.h,WM_CLOSE,0,0);
    CHECK(closeCalls==3 && destroyCalls==3 && ncCalls==3 && !IsWindow(b.h));
    CHECK(!overlay::PreviousProc(b.h));

    // 前驱记录丢失时的兜底：别的插件 RemovePropW、或者我们自己装过又拆过，
    // property 就没了。这时候 WM_CLOSE 绝不能再交给 DefWindowProcW —— 那等于
    // 让系统直接 DestroyWindow，游戏收不到关闭通知，进程会留在后台。
    // 正确做法是退到窗口类过程（也就是游戏的窗口过程）。
    Window c; if(!c.Create()) return 2;
    CHECK(overlay::InitRenderer(c.sc)==1);
    ::RemovePropW(c.h, L"BetterINQ.PreviousWndProc.8c6f10a");
    CHECK(!overlay::PreviousProc(c.h));
    SendMessageW(c.h,WM_CLOSE,0,0);
    std::printf("  c: close=%d destroy=%d nc=%d alive=%d\n", closeCalls, destroyCalls, ncCalls, (int)IsWindow(c.h));
    CHECK(closeCalls==4 && destroyCalls==4 && ncCalls==4 && !IsWindow(c.h));

    // 重入自环：别的叠加层在我们外面，而它记的前驱又指回我们（O → B → O）。
    // 这条链条本来没有出口，必须硬着陆 —— 否则就是 0.7.1 实测过的
    // 0xC00000FD（栈溢出，闪退在 USER32 里）。这里手工把链条做成环来验闸门。
    {
        overlay::Shutdown();      // c 那次初始化还占着 ImGui，先放掉
        Window d; if(!d.Create()) return 2;
        CHECK(overlay::InitRenderer(d.sc)==1);
        wrapperPrevious = reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(d.h, GWLP_WNDPROC));
        CHECK(wrapperPrevious == overlay::WndProc);
        ::SetPropW(d.h, L"BetterINQ.PreviousWndProc.8c6f10a", reinterpret_cast<HANDLE>(Wrapper));
        ::SetWindowLongPtrW(d.h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(overlay::WndProc));
        const int before = wrapperCalls;
        SendMessageW(d.h, kProbe, 0, 0);          // 修好之前这里直接栈溢出
        const int turns = wrapperCalls - before;
        std::printf("  self-cycle turns: %d\n", turns);
        CHECK(turns > 0 && turns < 40);
        ::RemovePropW(d.h, L"BetterINQ.PreviousWndProc.8c6f10a");
        ::SetWindowLongPtrW(d.h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(GameProc));
    }

    // 「前驱把消息再派回窗口」这种情况（游戏切场景时窗口过程会临时换成 WinMM 里的
    // 函数，实测前驱就是 WINMM+0x4A5F0）：同一条消息回到手里时必须改投窗口类过程，
    // 消息要真的送到游戏，而不是被我们丢掉。
    {
        overlay::Shutdown();
        Window e; if(!e.Create()) return 2;
        CHECK(overlay::InitRenderer(e.sc)==1);
        wrapperPrevious = reinterpret_cast<WNDPROC>(::GetWindowLongPtrW(e.h, GWLP_WNDPROC));
        wrapperBounces = true;
        ::SetPropW(e.h, L"BetterINQ.PreviousWndProc.8c6f10a", reinterpret_cast<HANDLE>(Wrapper));
        ::SetWindowLongPtrW(e.h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(overlay::WndProc));
        probes = 0;
        SendMessageW(e.h, kProbe, 0, 0);
        std::printf("  bouncing predecessor: probes=%d\n", probes);
        CHECK(probes >= 1);              // 消息送到游戏（没被 DefWindowProcW 吃掉）
        wrapperBounces = false;
        ::RemovePropW(e.h, L"BetterINQ.PreviousWndProc.8c6f10a");
        ::SetWindowLongPtrW(e.h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(GameProc));
    }

    // Message history must not consume a permanent eight-entry table. After
    // many distinct messages, legitimate different-message nesting still uses
    // the predecessor; the same message on another HWND is not recursion.
    {
        Window f, g; if(!f.Create() || !g.Create()) return 2;
        SetPropW(f.h, L"BetterINQ.PreviousWndProc.8c6f10a", reinterpret_cast<HANDLE>(NestPrevious));
        SetPropW(g.h, L"BetterINQ.PreviousWndProc.8c6f10a", reinterpret_cast<HANDLE>(NestPrevious));
        SetWindowLongPtrW(f.h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(overlay::WndProc));
        SetWindowLongPtrW(g.h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(overlay::WndProc));
        nestWrapperCalls=nestClassCalls=0;
        for(UINT i=0;i<24;++i) CHECK(SendMessageW(f.h,kNestBase+i,0,0)==456);
        CHECK(overlay::g_msgNest==nullptr);
        nestDifferent=true;
        CHECK(SendMessageW(f.h,kNestBase+32,0,0)==456);
        nestDifferent=false;
        CHECK(nestWrapperCalls==26 && nestClassCalls==0);
        nestOther=g.h;
        CHECK(SendMessageW(f.h,kNestBase+32,0,0)==456);
        CHECK(nestWrapperCalls==28 && nestClassCalls==0);
        nestOther=nullptr;
        CHECK(overlay::g_msgNest==nullptr);

        // Even NCDESTROY must hit the same-message recursion gate. Here the
        // class procedure itself bounces, so class fallback alone cannot end it.
        ncBounceCalls=0; ncBounce=true;
        SendMessageW(f.h,WM_NCDESTROY,0,0);
        ncBounce=false;
        CHECK(ncBounceCalls>0 && ncBounceCalls<=3);
        CHECK(!overlay::PreviousProc(f.h));
        CHECK(overlay::g_msgNest==nullptr && overlay::g_wndDepth==0);
        SetWindowLongPtrW(f.h,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(GameProc));
        SetWindowLongPtrW(g.h,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(GameProc));
        RemovePropW(g.h,L"BetterINQ.PreviousWndProc.8c6f10a");
    }

    // 退出路径上的异常过滤：别的模组的 CLR/C++ 异常不该被我们记进日志，
    // 更不该在「窗口已经销毁」那一刻去做文件 IO（那是把游戏卡死的常见写法）。
    CHECK(!crashfilter::Recordable(0xE0434352u));   // CLR 异常
    CHECK(!crashfilter::Recordable(0xE06D7363u));   // C++ throw
    CHECK(!crashfilter::Recordable(0x40010006u));   // OutputDebugString
    CHECK(!crashfilter::Recordable(0x406D1388u));   // SetThreadName
    CHECK(!crashfilter::Recordable(0xC0000194u));   // POSSIBLE_DEADLOCK：只是警告
    CHECK(crashfilter::Recordable(0xC0000005u));    // 真访问违规还是要记
    CHECK(crashfilter::Recordable(0xC00000FDu));    // 栈溢出还是要记

    // 卡死诊断：真起一个诊断线程、真抓一遍本进程所有线程的栈，确认既不崩也不空转。
    // 只抓一轮（second=0），免得测试等太久。
    {
        const wchar_t* logPath = L"out\\window_chain_hangdump.log";   // 别把日志丢在工作树根上
        ::CreateDirectoryW(L"out", nullptr);
        ::DeleteFileW(logPath);
        LogInit(logPath);
        hangdump::Init();
        hangdump::Arm(200, 0);
        ::Sleep(1200);
        std::ifstream f(logPath, std::ios::binary);
        const std::string all((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        CHECK(all.find("hangdump: 窗口销毁之后进程仍在运行") != std::string::npos);
        CHECK(all.find("hangdump: --- 线程") != std::string::npos);
        CHECK(all.find("hangdump:   rip #0") != std::string::npos);
        CHECK(all.find("hangdump: 一共") != std::string::npos);
        std::printf("  hangdump 日志 %zu 字节\n", all.size());
    }
    std::printf("window chain failures: %d\n", failures);
    return failures?1:0;
}
