#include "hangdump.h"
#include "log.h"

#include <windows.h>
#include <dbghelp.h>   // 只用它的结构体定义（STACKFRAME64）；函数一律运行时 GetProcAddress
#include <tlhelp32.h>
#include <atomic>
#include <cstdint>

namespace hangdump {
namespace {

typedef BOOL(WINAPI* StackWalk64Fn)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID,
                                    PREAD_PROCESS_MEMORY_ROUTINE64, PFUNCTION_TABLE_ACCESS_ROUTINE64,
                                    PGET_MODULE_BASE_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64);
typedef PVOID(WINAPI* SymFunctionTableAccess64Fn)(HANDLE, DWORD64);
typedef DWORD64(WINAPI* SymGetModuleBase64Fn)(HANDLE, DWORD64);
typedef BOOL(WINAPI* SymInitializeFn)(HANDLE, PCSTR, BOOL);
typedef DWORD(WINAPI* SymSetOptionsFn)(DWORD);

StackWalk64Fn g_stackWalk = nullptr;
SymFunctionTableAccess64Fn g_fnTable = nullptr;
SymGetModuleBase64Fn g_modBase = nullptr;
std::atomic<int> g_armed{0};
int g_first = 6000;
int g_second = 0;

// 写一行「模块+偏移」：地址落在哪个模块里、距模块基址多远。
// 有偏移就能拿同一台机器上的 dll 去对照（RVA 不随 ASLR 变），不用符号也够定位。
void Describe(const char* tag, unsigned i, DWORD64 addr)
{
    char name[128] = "?";
    unsigned long long off = 0;
    HMODULE mod = nullptr;
    if (addr &&
        ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(addr)), &mod) &&
        mod) {
        wchar_t path[MAX_PATH] = L"";
        if (::GetModuleFileNameW(mod, path, MAX_PATH)) {
            const wchar_t* base = path;
            for (const wchar_t* p = path; *p; ++p)
                if (*p == L'\\') base = p + 1;
            ::WideCharToMultiByte(CP_UTF8, 0, base, -1, name, (int)sizeof(name), nullptr, nullptr);
        }
        off = addr - static_cast<DWORD64>(reinterpret_cast<uintptr_t>(mod));
    }
    Log("%s #%u %p（%s+0x%llX）", tag, i, reinterpret_cast<void*>(static_cast<uintptr_t>(addr)), name, off);
}

void DumpSelf()
{
    void* frames[32] = {};
    const USHORT n = ::RtlCaptureStackBackTrace(1, 32, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) Describe("hangdump:  ", i, static_cast<DWORD64>(reinterpret_cast<uintptr_t>(frames[i])));
}

// 逐个线程抓栈。要点：先把地址抄进本地数组、马上 ResumeThread，再回来写日志 ——
// 别的线程被挂着的每一毫秒都可能正好卡住堆锁/loader lock，写文件必须等放开以后再干。
void DumpOne(DWORD tid, bool self)
{
    Log("hangdump: --- 线程 %lu%s ---", tid, self ? "（诊断线程自己）" : "");
    if (self) {
        DumpSelf();
        return;
    }
    HANDLE t = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!t) {
        Log("hangdump:   打不开这个线程（%lu）", ::GetLastError());
        return;
    }
    if (::SuspendThread(t) == (DWORD)-1) {
        ::CloseHandle(t);
        Log("hangdump:   挂不起这个线程（%lu）", ::GetLastError());
        return;
    }
    uintptr_t addrs[26] = {};
    int n = 0;
    bool got = false;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (::GetThreadContext(t, &ctx)) {
        got = true;
        addrs[n++] = static_cast<uintptr_t>(ctx.Rip);
        if (g_stackWalk && g_fnTable && g_modBase) {
            STACKFRAME64 f{};
            f.AddrPC.Offset = ctx.Rip;
            f.AddrPC.Mode = AddrModeFlat;
            f.AddrStack.Offset = ctx.Rsp;
            f.AddrStack.Mode = AddrModeFlat;
            // x64 的 unwind 信息里没有 frame pointer，约定用 RIP 当 frame
            f.AddrFrame.Offset = ctx.Rip;
            f.AddrFrame.Mode = AddrModeFlat;
            while (n < (int)(sizeof(addrs) / sizeof(addrs[0]))) {
                if (!g_stackWalk(IMAGE_FILE_MACHINE_AMD64, ::GetCurrentProcess(), t, &f, &ctx, nullptr, g_fnTable,
                                 g_modBase, nullptr))
                    break;
                if (!f.AddrPC.Offset) break;
                // AddrFrame 用 RIP 顶替，第一帧会和 #0 重复，跳过
                if (n > 0 && addrs[n - 1] == static_cast<uintptr_t>(f.AddrPC.Offset)) continue;
                addrs[n++] = static_cast<uintptr_t>(f.AddrPC.Offset);
                if (n > 3 && f.AddrPC.Offset == f.AddrReturn.Offset) break;   // 栈顶自环，别死循环
            }
        }
    }
    const DWORD err = got ? 0 : ::GetLastError();
    ::ResumeThread(t);
    ::CloseHandle(t);
    if (!got) {
        Log("hangdump:   读不到上下文（%lu）", err);
        return;
    }
    for (int i = 0; i < n; ++i)
        Describe(i == 0 ? "hangdump:   rip" : "hangdump:     ", (unsigned)i, static_cast<DWORD64>(addrs[i]));
}

void DumpAll(const char* why)
{
    Log("hangdump: %s —— 进程还在，抓所有线程的调用栈", why);
    const DWORD self = ::GetCurrentThreadId();
    const DWORD pid = ::GetCurrentProcessId();
    HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        Log("hangdump: 拿不到线程列表（%lu），只抓自己", ::GetLastError());
        DumpOne(self, true);
        return;
    }
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    int count = 0;
    if (::Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            ++count;
            DumpOne(te.th32ThreadID, te.th32ThreadID == self);
        } while (::Thread32Next(snap, &te));
    }
    ::CloseHandle(snap);
    Log("hangdump: 一共 %d 个线程", count);
}

DWORD WINAPI Watchdog(LPVOID)
{
    ::Sleep((DWORD)(g_first > 0 ? g_first : 6000));
    DumpAll("窗口销毁之后进程仍在运行");
    if (g_second > g_first) {
        ::Sleep((DWORD)(g_second - g_first));
        DumpAll("又等了一会儿进程仍然在（和上一次对比：栈一样就是真卡住，不一样可能是在慢慢退）");
    }
    Log("hangdump: 诊断线程结束，进程依旧没有退出");
    return 0;
}

} // namespace

void Init()
{
    // 启动时就把 dbghelp 拿好：等到卡在 loader lock 的那天再 LoadLibrary 只会一起卡住
    HMODULE m = ::LoadLibraryW(L"dbghelp.dll");
    if (!m) {
        Log("hangdump: 没有 dbghelp.dll，只能记 RIP（%lu）", ::GetLastError());
        return;
    }
    g_stackWalk = (StackWalk64Fn)::GetProcAddress(m, "StackWalk64");
    g_fnTable = (SymFunctionTableAccess64Fn)::GetProcAddress(m, "SymFunctionTableAccess64");
    g_modBase = (SymGetModuleBase64Fn)::GetProcAddress(m, "SymGetModuleBase64");
    auto symInit = (SymInitializeFn)::GetProcAddress(m, "SymInitialize");
    auto symOptions = (SymSetOptionsFn)::GetProcAddress(m, "SymSetOptions");
    if (symOptions) symOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_NO_PROMPTS);
    // 第三个参数要 TRUE：dbghelp 得先把进程里已加载的模块登记进自己的表，
    // 否则 SymFunctionTableAccess64 查不到 .pdata，栈走到第二三帧就断了（实测只出 3 帧）。
    // 符号按需加载（DEFERRED_LOADS），启动时不会去读 pdb。
    if (symInit) symInit(::GetCurrentProcess(), nullptr, TRUE);
    if (!g_stackWalk || !g_fnTable || !g_modBase) Log("hangdump: dbghelp 里没找齐 StackWalk64 那几个函数");
}

void Arm(int firstDelayMs, int secondDelayMs)
{
    if (g_armed.exchange(1)) return;
    g_first = firstDelayMs > 0 ? firstDelayMs : 6000;
    g_second = secondDelayMs > g_first ? secondDelayMs : 0;
    HANDLE t = ::CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr);
    if (t) ::CloseHandle(t);
    else Log("hangdump: 诊断线程起不来（%lu）", ::GetLastError());
}

} // namespace hangdump
