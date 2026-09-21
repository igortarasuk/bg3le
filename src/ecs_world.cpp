// Captures the ECS storage pointer, which cannot be found by name.
//
// The engine has no EntityWorld symbol: of the ecs:: namespace only the
// type-id contexts survive in .symtab, because those appear as template
// arguments of the named TypeId statics. bg3se reaches the world through
// GetEoCServer()->EntityWorld, and both are anonymous file-scope statics here.
//
// What we do have is a component lookup function, found by disassembling the
// code that reads a component's type-index static:
//
//   mov  r15d, [rip+...]     ; ls::TypeId<eoc::HealthComponent, ...>::m_TypeIndex
//   mov  rdi,  [rbp+0x348]   ; the object we want
//   and  edx,  0x7fff        ; index masked to 15 bits
//   call <lookup>            ; (object, entity, index)
//
// Reading the whole function shows what its first argument is:
//
//   and r10d, 0x7fc0      ; index & ~63
//   shr r10d, 6           ; word = index / 64
//   mov rax, [rdi+r10*8]  ; a bitmask word at offset 0 of rdi
//   shl rbx, cl           ; bit = 1 << (index & 63)
//   test rax, rbx         ; presence test
//
// So rdi begins with a component-presence bitmask, and the pointer at +0x110
// puts that mask at 0x110 bytes -- 34 qwords, 2176 bits. bg3se independently
// has ComponentMapSize = 0x880, which is the same 2176, so its reverse
// engineered layout describes this build too.
//
// Rather than guess the prototype, the hook is a naked thunk: it records rdi
// and jumps to the original with every register untouched, so the real
// signature does not matter and there is no ABI risk.

#include "ecs_world.h"

#include "hook.h"
#include "log.h"

namespace bg3le {
namespace ecs {
namespace {

// Offset of the component lookup in 4.8.400.7143220, from the disassembly
// above. hook_call_sites verifies each site really calls it before patching.
constexpr std::uintptr_t kLookupFunc = 0x218e050;

}  // namespace

// Hidden, so the thunk can reach them with a PC-relative access. A default
// visibility symbol in a shared object is preemptible, and the linker will not
// accept R_X86_64_PC32 against one.
extern "C" {
__attribute__((visibility("hidden"))) void* bg3le_ecs_lookup_original = nullptr;
__attribute__((visibility("hidden"))) void* bg3le_ecs_storage = nullptr;
}

namespace {

// Records the first argument once, then jumps to the original. No prologue, no
// clobbers, nothing touched but rax -- which is caller-saved and about to hold
// the return value anyway.
extern "C" void bg3le_ecs_capture_thunk();
__asm__(
    ".text\n"
    ".globl bg3le_ecs_capture_thunk\n"
    ".hidden bg3le_ecs_capture_thunk\n"
    "bg3le_ecs_capture_thunk:\n"
    "  mov bg3le_ecs_storage(%rip), %rax\n"
    "  test %rax, %rax\n"
    "  jne 1f\n"
    // Only record an object whose component mask has something in it. The
    // first call comes early, while the mask is still all zeroes, and an empty
    // one tells us nothing.
    "  mov (%rdi), %rax\n"
    "  test %rax, %rax\n"
    "  je 1f\n"
    "  mov %rdi, bg3le_ecs_storage(%rip)\n"
    "1:\n"
    "  jmp *bg3le_ecs_lookup_original(%rip)\n");

}  // namespace

bool install_storage_capture() {
    void* original = nullptr;
    const std::size_t patched = hook_call_sites(
        kLookupFunc, reinterpret_cast<void*>(&bg3le_ecs_capture_thunk), &original);
    if (patched == 0) {
        logf("ecs: no call sites patched for the component lookup at %#lx; "
             "the storage pointer will stay unknown",
             (unsigned long)kLookupFunc);
        return false;
    }
    bg3le_ecs_lookup_original = original;
    logf("ecs: watching %zu call sites of the component lookup", patched);
    return true;
}

void* storage() { return bg3le_ecs_storage; }

}  // namespace ecs
}  // namespace bg3le
