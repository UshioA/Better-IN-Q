#include "hook.h"

#include <windows.h>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace hook {
namespace {

std::uintptr_t g_arena = 0;
std::size_t g_arenaSize = 0;
std::size_t g_arenaUsed = 0;
std::mutex g_mx;

bool InRel32(std::uintptr_t from, std::uintptr_t to)
{
    const std::int64_t d = (std::int64_t)to - (std::int64_t)from;
    return d >= INT32_MIN && d <= INT32_MAX;
}

std::uintptr_t TryAllocAt(std::uintptr_t addr, std::size_t size)
{
    void* p = ::VirtualAlloc(reinterpret_cast<void*>(addr), size,
                             MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    return reinterpret_cast<std::uintptr_t>(p);
}

int HexNibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

} // namespace

bool InitArena(std::uintptr_t lo, std::uintptr_t hi, std::size_t size)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_arena) return true;
    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    const std::uintptr_t gran = si.dwAllocationGranularity;   // 64KB

    // 先往模块下面找（CT 用的就是 0x13FF80000），再往上面找。整段都要在 rel32 内。
    // 2026-07: 有的机器上 ±1.75GB 里一个 64KB 的空洞都没有（4K + DX12 + Steam 壳），
    // 于是扫完再用「让系统随便给、自己验距离」兜一下（见函数末尾）。
    const std::uintptr_t span = 0x7F000000;   // 留余量，别贴着 2GB 边
    for (std::uintptr_t a = (lo - gran) & ~(gran - 1); a > gran && lo - a < span; a -= gran) {
        if (!InRel32(a, hi) || !InRel32(a + size, lo)) continue;
        if (std::uintptr_t p = TryAllocAt(a, size)) { g_arena = p; break; }
    }
    if (!g_arena) {
        for (std::uintptr_t a = (hi + gran) & ~(gran - 1); a - hi < span; a += gran) {
            if (!InRel32(lo, a + size)) break;
            if (std::uintptr_t p = TryAllocAt(a, size)) { g_arena = p; break; }
        }
    }
    if (!g_arena) {
        // 64KB 网格扫不到，不代表没有：地址空间里可能只剩不落在网格上的缝。
        // VirtualAlloc(nullptr) 让系统自己挑，挑到哪算哪，验一下 rel32，不在就还回去。
        for (int i = 0; i < 64 && !g_arena; ++i) {
            void* p = ::VirtualAlloc(nullptr, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (!p) break;
            const std::uintptr_t a = reinterpret_cast<std::uintptr_t>(p);
            if (InRel32(a, hi) && InRel32(a + size, lo)) g_arena = a;
            else ::VirtualFree(p, 0, MEM_RELEASE);
        }
    }
    if (!g_arena) return false;
    g_arenaSize = size;
    g_arenaUsed = 0;
    std::memset(reinterpret_cast<void*>(g_arena), 0xCC, size);
    return true;
}

std::uintptr_t ArenaBase() { return g_arena; }

std::uintptr_t Commit(const Emitter& e)
{
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_arena) return 0;
    const std::size_t at = (g_arenaUsed + 15) & ~std::size_t(15);
    if (at + e.size() > g_arenaSize) return 0;
    std::memcpy(reinterpret_cast<void*>(g_arena + at), e.bytes().data(), e.size());
    g_arenaUsed = at + e.size();
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(g_arena + at), e.size());
    return g_arena + at;
}

bool WriteMem(std::uintptr_t addr, const void* bytes, std::size_t n)
{
    DWORD old = 0;
    if (!::VirtualProtect(reinterpret_cast<void*>(addr), n, PAGE_EXECUTE_READWRITE, &old)) return false;
    std::memcpy(reinterpret_cast<void*>(addr), bytes, n);
    DWORD tmp = 0;
    ::VirtualProtect(reinterpret_cast<void*>(addr), n, old, &tmp);
    ::FlushInstructionCache(::GetCurrentProcess(), reinterpret_cast<void*>(addr), n);
    return true;
}

bool PatchJump(std::uintptr_t site, std::size_t len, std::uintptr_t stub)
{
    if (len < 5 || len > 16 || !stub || !InRel32(site + 5, stub)) return false;
    std::uint8_t p[16];
    p[0] = 0xE9;
    const std::int32_t rel = (std::int32_t)((std::int64_t)stub - (std::int64_t)(site + 5));
    std::memcpy(p + 1, &rel, 4);
    for (std::size_t i = 5; i < len; ++i) p[i] = 0x90;
    return WriteMem(site, p, len);
}

bool InstallCtxHook(std::uintptr_t site, std::size_t len, Handler h,
                    const std::uint8_t* displaced, std::size_t dlen)
{
    Emitter e(256);
    // ---- 保存现场 ----
    e.b({0x9C});                                        // pushfq
    e.b({0x50, 0x51, 0x52, 0x53, 0x55, 0x56, 0x57});    // push rax rcx rdx rbx rbp rsi rdi
    for (std::uint8_t r = 0; r < 8; ++r) e.b({0x41, std::uint8_t(0x50 + r)});   // push r8..r15
    e.b({0x48, 0x89, 0xE5});                            // mov rbp, rsp      (rbp = &Ctx)
    e.b({0x48, 0x83, 0xEC, 0x60});                      // sub rsp, 0x60
    e.b({0x48, 0x83, 0xE4, 0xF0});                      // and rsp, -16
    for (std::uint8_t n = 0; n < 6; ++n)                // movdqa [rsp+n*16], xmmN
        e.b({0x66, 0x0F, 0x7F, std::uint8_t(0x44 | (n << 3)), 0x24, std::uint8_t(n * 16)});
    e.b({0x48, 0x83, 0xEC, 0x20});                      // sub rsp, 0x20     (shadow space)
    e.b({0x48, 0x89, 0xE9});                            // mov rcx, rbp
    e.b({0x48, 0xB8}).u64(reinterpret_cast<std::uint64_t>(h));   // mov rax, handler
    e.b({0xFF, 0xD0});                                  // call rax
    e.b({0x48, 0x83, 0xC4, 0x20});                      // add rsp, 0x20
    for (std::uint8_t n = 0; n < 6; ++n)                // movdqa xmmN, [rsp+n*16]
        e.b({0x66, 0x0F, 0x6F, std::uint8_t(0x44 | (n << 3)), 0x24, std::uint8_t(n * 16)});
    e.b({0x48, 0x89, 0xEC});                            // mov rsp, rbp
    for (int r = 7; r >= 0; --r) e.b({0x41, std::uint8_t(0x58 + r)});   // pop r15..r8
    e.b({0x5F, 0x5E, 0x5D, 0x5B, 0x5A, 0x59, 0x58});    // pop rdi rsi rbp rbx rdx rcx rax
    e.b({0x9D});                                        // popfq
    // ---- 被覆盖的原指令，然后回去 ----
    e.raw(displaced, dlen);
    e.jmpAbs(site + len);

    const std::uintptr_t stub = Commit(e);
    return stub && PatchJump(site, len, stub);
}

bool InstallCallGate(std::uintptr_t site, volatile std::int32_t* flag,
                     std::int32_t skipWhen, std::int32_t setOnSkip,
                     volatile std::int32_t* enabled, volatile std::int32_t* inflight)
{
    const std::uint8_t* s = reinterpret_cast<const std::uint8_t*>(site);
    if (s[0] != 0xE8) return false;
    if ((enabled == nullptr) != (inflight == nullptr)) return false;
    std::int32_t rel = 0;
    std::memcpy(&rel, s + 1, 4);
    const std::uintptr_t target = site + 5 + (std::intptr_t)rel;
    if (skipWhen < -128 || skipWhen > 127) return false;

    Emitter e(160);
    e.b({0x50});                                                         // push rax
    if (enabled) {
        e.b({0x48, 0xB8}).u64(reinterpret_cast<std::uint64_t>(inflight)); // mov rax, inflight
        e.b({0xF0, 0xFF, 0x00});                                        // lock inc dword [rax]
        e.b({0x48, 0xB8}).u64(reinterpret_cast<std::uint64_t>(enabled));  // mov rax, enabled
        e.b({0x83, 0x38, 0x00});                                        // cmp dword [rax], 0
        e.b({0x74, 0x00});                                               // je inactive（稍后补 rel8）
    }
    e.b({0x48, 0xB8}).u64(reinterpret_cast<std::uint64_t>(flag));        // mov rax, flag
    e.b({0x83, 0x38, std::uint8_t(skipWhen)});                           // cmp dword [rax], skipWhen
    if (setOnSkip >= 0) {
        e.b({0x75, 0x06});                                               // jne +6
        e.b({0xC7, 0x00}).u32((std::uint32_t)setOnSkip);                 // mov dword [flag], setOnSkip（不改 flags）
    }
    e.b({0x0F, 0x94, 0xC0});                                            // sete al（写回后才可复用 rax 低字节）
    if (inflight) {
        e.b({0x50});                                                     // push rax（保存 0/1 结果）
        e.b({0x48, 0xB8}).u64(reinterpret_cast<std::uint64_t>(inflight)); // mov rax, inflight
        e.b({0xF0, 0xFF, 0x08});                                        // lock dec dword [rax]
        e.b({0x58});                                                     // pop rax
    }
    e.b({0x84, 0xC0});                                                   // test al,al
    e.b({0x58});                                                         // pop rax
    e.b({0x75, 0x10});                                                   // jne skip（跳过下面 16 字节）
    e.callAbs(target);                                                   // call 原目标
    e.jmpAbs(site + 5);                                                  // skip: 回去

    if (enabled) {
        // inactive: 已登记 inflight，但 enabled 为 0；退出登记后一定调用游戏原函数。
        Emitter tail(48);
        tail.b({0x48, 0xB8}).u64(reinterpret_cast<std::uint64_t>(inflight)); // mov rax, inflight
        tail.b({0xF0, 0xFF, 0x08});                                     // lock dec dword [rax]
        tail.b({0x58});                                                  // pop rax
        tail.callAbs(target);
        tail.jmpAbs(site + 5);

        std::vector<std::uint8_t> bytes = e.bytes();
        const std::size_t inactive = bytes.size();
        std::size_t jeDisp = bytes.size();
        for (std::size_t i = 0; i + 1 < bytes.size(); ++i) {
            if (bytes[i] == 0x74 && bytes[i + 1] == 0x00) { jeDisp = i + 1; break; }
        }
        if (jeDisp == bytes.size()) return false;
        const std::size_t afterJe = jeDisp + 1;
        if (inactive - afterJe > 127) return false;
        bytes[jeDisp] = static_cast<std::uint8_t>(inactive - afterJe);
        bytes.insert(bytes.end(), tail.bytes().begin(), tail.bytes().end());
        Emitter joined(bytes.size());
        joined.raw(bytes.data(), bytes.size());
        const std::uintptr_t stub = Commit(joined);
        return stub && PatchJump(site, 5, stub);
    }

    const std::uintptr_t stub = Commit(e);
    return stub && PatchJump(site, 5, stub);
}

std::vector<std::uintptr_t> FindAll(std::uintptr_t begin, std::size_t len, const char* pattern)
{
    std::vector<int> pat;
    for (const char* p = pattern; *p;) {
        if (*p == ' ') { ++p; continue; }
        if (p[0] == '?') { pat.push_back(-1); p += (p[1] == '?') ? 2 : 1; continue; }
        const int hi = HexNibble(p[0]), lo = HexNibble(p[1]);
        if (hi < 0 || lo < 0) return {};
        pat.push_back(hi * 16 + lo);
        p += 2;
    }
    std::vector<std::uintptr_t> hits;
    if (pat.empty() || pat[0] < 0 || len < pat.size()) return hits;
    const std::uint8_t* base = reinterpret_cast<const std::uint8_t*>(begin);
    const std::uint8_t* end = base + len - pat.size() + 1;
    for (const std::uint8_t* q = base; q < end;) {
        q = static_cast<const std::uint8_t*>(std::memchr(q, pat[0], end - q));
        if (!q) break;
        bool ok = true;
        for (std::size_t i = 1; i < pat.size(); ++i)
            if (pat[i] >= 0 && q[i] != pat[i]) { ok = false; break; }
        if (ok) hits.push_back(reinterpret_cast<std::uintptr_t>(q));
        ++q;
    }
    return hits;
}

int ReplaceModulePointers(std::uintptr_t base, void* oldPtr, void* newPtr, const void* skipLo, const void* skipHi,
                          std::string* where)
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    int n = 0;
    for (WORD k = 0; k < nt->FileHeader.NumberOfSections; ++k) {
        if (sec[k].Characteristics & IMAGE_SCN_MEM_EXECUTE) continue;
        const std::uintptr_t a = base + sec[k].VirtualAddress;
        const std::size_t len = sec[k].Misc.VirtualSize & ~std::size_t(7);
        for (std::size_t o = 0; o < len; o += 8) {
            void** slot = reinterpret_cast<void**>(a + o);
            if (*slot != oldPtr) continue;
            if (skipLo && reinterpret_cast<const void*>(slot) >= skipLo && reinterpret_cast<const void*>(slot) < skipHi)
                continue;
            WriteMem(reinterpret_cast<std::uintptr_t>(slot), &newPtr, sizeof(newPtr));
            if (where) {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.8s+%zx ", reinterpret_cast<const char*>(sec[k].Name), o);
                *where += buf;
            }
            ++n;
        }
    }
    return n;
}

bool TextRange(std::uintptr_t moduleBase, std::uintptr_t& begin, std::size_t& len)
{
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(moduleBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(moduleBase + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        if (std::memcmp(sec[i].Name, ".text", 5) == 0) {
            begin = moduleBase + sec[i].VirtualAddress;
            len = sec[i].Misc.VirtualSize;
            return true;
        }
    }
    return false;
}

} // namespace hook
