// Windows test for detour.h, run by CI: a function that starts exactly as Rekordbox 7.2.19's MIDI callback
// does is detoured while another thread calls it nonstop, as WinMM would. Every call must reach the hook, the
// hook must be able to keep a message from the original, and the rest must reach the original intact.
#include "detour.h"

#include <stdio.h>
#include <atomic>
#include <thread>

using Proc = void (*)(void *, unsigned, uintptr_t, uintptr_t, uintptr_t);
static std::atomic<uint64_t> hooked_calls{0}, kept_calls{0};
static void *volatile next_proc = nullptr;
static volatile uint64_t seen = 0;  // the original stores the message (its fourth argument) here

static void hook(void *in, unsigned message, uintptr_t instance, uintptr_t param1, uintptr_t param2) {
    hooked_calls++;
    if ((param1 & 0xFF) == 0xB6) { kept_calls++; return; }  // the browse knob: Wave keeps it
    reinterpret_cast<Proc>(next_proc)(in, message, instance, param1, param2);
}

int main() {
    // Rekordbox's three prologue instructions, then: mov rax,[rsp+0x20]; mov rcx,&seen; mov [rcx],rax; mov rbx,
    // [rsp+8]; mov rbp,[rsp+0x10]; ret. (Nothing else touches the stack, so no frame to unwind.)
    const uint8_t prologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x4c, 0x89, 0x4c, 0x24, 0x20};
    auto *code = static_cast<uint8_t *>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    uint8_t *p = code + 16;  // 8-aligned, as Rekordbox's is
    memcpy(p, prologue, sizeof prologue);
    uint8_t *q = p + sizeof prologue;
    const uint8_t load[] = {0x48, 0x8b, 0x44, 0x24, 0x20, 0x48, 0xb9};
    memcpy(q, load, sizeof load); q += sizeof load;
    const uint64_t where = reinterpret_cast<uint64_t>(&seen);
    memcpy(q, &where, 8); q += 8;
    const uint8_t tail[] = {0x48, 0x89, 0x01, 0x48, 0x8b, 0x5c, 0x24, 0x08, 0x48, 0x8b, 0x6c, 0x24, 0x10, 0xc3};
    memcpy(q, tail, sizeof tail);
    FlushInstructionCache(GetCurrentProcess(), code, 4096);
    const Proc target = reinterpret_cast<Proc>(p);

    target(nullptr, 0x3C3, 0, 0x7F4096, 0);
    if (seen != 0x7F4096) { printf("FAIL: the test function itself\n"); return 1; }

    std::atomic<bool> stop{false}, hooked{false};
    std::atomic<uint64_t> before{0}, after{0};
    std::thread caller([&] {
        uint64_t n = 0;
        while (!stop) {
            const uintptr_t message = (n % 3 == 0) ? 0x7F40B6 : 0x7F4696;  // knob turns and pushes
            target(nullptr, 0x3C3, 0, message, n);
            (hooked ? after : before)++;
            ++n;
        }
    });
    Sleep(50);
    if (!wave_detour::install(p, sizeof prologue, reinterpret_cast<void *>(&hook), const_cast<void **>(&next_proc))) {
        stop = true; caller.join();
        printf("FAIL: install\n");
        return 1;
    }
    hooked = true;
    Sleep(200);
    stop = true;
    caller.join();
    // Calls after the install all went through the hook; the knob was kept, pushes reached the original.
    const uint64_t calls = hooked_calls.load(), kept = kept_calls.load();
    target(nullptr, 0x3C3, 0, 0x7F4696, 0);
    const bool pushed = seen == 0x7F4696;
    target(nullptr, 0x3C3, 0, 0x7F40B6, 0);
    const bool knobKept = seen == 0x7F4696;  // unchanged: the hook kept it
    printf("calls before %llu, after %llu, through the hook %llu, kept %llu\n", (unsigned long long)before.load(),
           (unsigned long long)after.load(), (unsigned long long)calls, (unsigned long long)kept);
    if (before == 0 || after == 0 || calls < after || kept == 0 || !pushed || !knobKept) { printf("FAIL\n"); return 1; }
    printf("PASS\n");
    return 0;
}
