// Detouring a function's entry inside a running process, without tearing a thread that is in it.
// The function's first ``prologue`` bytes (whole instructions, none RIP-relative, at least 14) move to a
// trampoline that jumps back into the rest of it; the entry then jumps to ``hook``. The entry is changed in
// three steps: it first jumps to itself (one atomic 8-byte write), the rest is written while nothing can
// reach it, then the jump to the hook lands (a second atomic write).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdint.h>
#include <string.h>

namespace wave_detour {

inline bool install(uint8_t *target, size_t prologue, void *hook, void **next) {
    if (reinterpret_cast<uintptr_t>(target) % 8 || prologue < 14 || prologue > 16) return false;
    auto *trampoline = static_cast<uint8_t *>(VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!trampoline) return false;
    memcpy(trampoline, target, prologue);
    trampoline[prologue] = 0xFF;  // jmp qword ptr [rip+0], then where to go on
    trampoline[prologue + 1] = 0x25;
    memset(trampoline + prologue + 2, 0, 4);
    const uint64_t resume = reinterpret_cast<uint64_t>(target + prologue);
    memcpy(trampoline + prologue + 6, &resume, 8);
    DWORD was = 0;
    if (!VirtualProtect(trampoline, 64, PAGE_EXECUTE_READ, &was)) return false;
    FlushInstructionCache(GetCurrentProcess(), trampoline, 64);

    uint8_t patch[16];
    memcpy(patch, target, sizeof patch);
    patch[0] = 0xFF;  // jmp qword ptr [rip+0], then the hook's address
    patch[1] = 0x25;
    memset(patch + 2, 0, 4);
    const uint64_t to = reinterpret_cast<uint64_t>(hook);
    memcpy(patch + 6, &to, 8);
    for (size_t i = 14; i < prologue; ++i) patch[i] = 0xCC;  // never reached: the jump leaves first
    if (!VirtualProtect(target, 16, PAGE_EXECUTE_READWRITE, &was)) return false;
    *static_cast<void *volatile *>(next) = trampoline;  // before anything can reach the hook
    MemoryBarrier();
    auto *head = reinterpret_cast<volatile LONG64 *>(target);
    LONG64 spin;
    memcpy(&spin, target, 8);
    reinterpret_cast<uint8_t *>(&spin)[0] = 0xEB;  // jmp $
    reinterpret_cast<uint8_t *>(&spin)[1] = 0xFE;
    InterlockedExchange64(head, spin);
    FlushInstructionCache(GetCurrentProcess(), target, 16);
    Sleep(100);  // whoever was past the entry is through the moved instructions by now
    memcpy(target + 8, patch + 8, prologue - 8);
    FlushInstructionCache(GetCurrentProcess(), target, 16);
    LONG64 jump;
    memcpy(&jump, patch, 8);
    InterlockedExchange64(head, jump);
    FlushInstructionCache(GetCurrentProcess(), target, 16);
    VirtualProtect(target, 16, was, &was);
    return true;
}

}  // namespace wave_detour
