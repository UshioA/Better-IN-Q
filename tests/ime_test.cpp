// Isolated real Win32/IMM regression. Never loads or modifies the game.
#include "../src/overlay.cpp"
#include <cstdio>
#include <thread>

static int failures = 0;
#define CHECK(x) do { const bool ok = (x); std::printf("%s %s\n", ok ? "ok" : "FAIL", #x); if (!ok) ++failures; } while (0)
static LRESULT CALLBACK GameProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcW(h, m, w, l);
}
static void Drain()
{
    MSG m{};
    while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}
static void Activate(HWND h)
{
    overlay::g_hwnd = h;
    overlay::g_inited = true;
    overlay::g_dead = false;
    overlay::g_failed = false;
    overlay::g_imeEnabled = true;
    overlay::g_wndHook = true;
    overlay::SetOpen(true);
    overlay::ImeRequest(true, false);
}
int main()
{
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = GameProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"BetterINQ_ime_test";
    CHECK(RegisterClassExW(&wc) != 0);
    HWND h = CreateWindowExW(0, wc.lpszClassName, L"isolated IME regression", WS_OVERLAPPEDWINDOW,
        0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
    CHECK(h != nullptr);
    if (!h) return 2;
    HIMC original = overlay::ImeContext(h);
    HIMC a = ImmCreateContext();
    HIMC b = ImmCreateContext();
    CHECK(a && b);
    if (!a || !b) return 2;
    ImmAssociateContext(h, a);
    ImGui::CreateContext();
    CHECK(ImGui_ImplWin32_Init(h));
    SetPropW(h, overlay::kPreviousProc, reinterpret_cast<HANDLE>(GameProc));
    SetWindowLongPtrW(h, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(overlay::WndProc));

    Activate(h);
    CHECK(overlay::g_imeOwner == h && overlay::g_imc != nullptr);
    CHECK(overlay::ImeContext(h) == overlay::g_imc);
    CHECK(overlay::g_imcPrev == a);
    overlay::SetOpen(false);
    CHECK(overlay::ImeContext(h) == a);
    CHECK(!overlay::g_imc && !overlay::g_imeOwner);

    // Preserve the game's latest replacement, not the first saved context.
    Activate(h);
    ImmAssociateContext(h, b);
    SendMessageW(h, overlay::kImeSync, 0, 0);
    CHECK(overlay::g_imcPrev == b);
    overlay::SetOpen(false);
    CHECK(overlay::ImeContext(h) == b);
    Activate(h);
    ImmAssociateContext(h, nullptr);
    SendMessageW(h, overlay::kImeSync, 0, 0);
    CHECK(overlay::g_imcPrev == nullptr);
    overlay::SetOpen(false);
    CHECK(overlay::ImeContext(h) == nullptr);

    // If the game already took ownership at close, do not overwrite it.
    ImmAssociateContext(h, a);
    Activate(h);
    ImmAssociateContext(h, b);
    overlay::SetOpen(false);
    CHECK(overlay::ImeContext(h) == b);

    // A renderer/worker publishes close, but no later Present is needed.
    Activate(h);
    std::thread closer([] { overlay::SetOpen(false); });
    closer.join();
    CHECK(overlay::g_imc != nullptr); // UI queue intentionally not pumped yet.
    SendMessageW(h, WM_NULL, 0, 0);   // First game message performs restoration.
    CHECK(!overlay::g_imc && overlay::ImeContext(h) == b);
    Drain();
    CHECK(overlay::ImeContext(h) == b);

    // Cleanup messages remain effective even when the renderer is inactive.
    Activate(h);
    overlay::g_inited = false;
    overlay::g_wasOpen = false;
    std::thread inactiveClose([] { overlay::SetOpen(false); });
    inactiveClose.join();
    Drain();
    CHECK(!overlay::g_imc && overlay::ImeContext(h) == b);

    // Stale position messages may not modify a restored game HIMC.
    COMPOSITIONFORM before{};
    before.dwStyle = CFS_POINT;
    before.ptCurrentPos.x = 17;
    before.ptCurrentPos.y = 23;
    ImmSetCompositionWindow(b, &before);
    COMPOSITIONFORM saved{};
    const bool readable = ImmGetCompositionWindow(b, &saved) != FALSE;
    Activate(h);
    const unsigned oldGeneration = overlay::g_imeGeneration.load();
    overlay::SetOpen(false);
    SendMessageW(h, overlay::kImePos, oldGeneration, MAKELPARAM(200, 210));
    COMPOSITIONFORM after{};
    if (readable) {
        CHECK(ImmGetCompositionWindow(b, &after));
        CHECK(after.dwStyle == saved.dwStyle && after.ptCurrentPos.x == saved.ptCurrentPos.x &&
            after.ptCurrentPos.y == saved.ptCurrentPos.y);
    }
    CHECK(overlay::ImeContext(h) == b);

    // Old sync/position messages cannot resurrect a closed ownership generation.
    Activate(h);
    overlay::SetOpen(false);
    Activate(h);
    HIMC current = overlay::g_imc;
    SendMessageW(h, overlay::kImeSync, 0, 0);
    SendMessageW(h, overlay::kImePos, oldGeneration, MAKELPARAM(300, 310));
    CHECK(overlay::g_imc == current && overlay::ImeContext(h) == current);
    overlay::SetOpen(false);

    // Cleanup is scoped to the owner HWND; unrelated windows are untouched.
    HWND other = CreateWindowExW(0, wc.lpszClassName, L"other", WS_OVERLAPPEDWINDOW,
        0, 0, 80, 80, nullptr, nullptr, wc.hInstance, nullptr);
    Activate(h);
    current = overlay::g_imc;
    overlay::ImeOffWindowThread(other);
    CHECK(overlay::g_imeOwner == h && overlay::g_imc == current);
    DestroyWindow(other);
    // Destruction cleans ownership before the HWND becomes invalid.
    DestroyWindow(h);
    CHECK(!overlay::g_imc && !overlay::g_imeOwner);
    CHECK(!overlay::PreviousProc(h));
    overlay::g_inited = false;
    overlay::g_hwnd = nullptr;
    overlay::g_openState.fetch_and(~1ull, std::memory_order_acq_rel);
    overlay::g_imeAttached = false;
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    (void)original; // Default window context is owned by the system.
    ImmDestroyContext(a);
    ImmDestroyContext(b);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    std::printf("IME failures: %d\n", failures);
    return failures ? 1 : 0;
}
