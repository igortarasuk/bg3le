// Linux stand-ins for the few Win32 entry points the vendored bg3se code
// calls. The callers are by Norbyte and the bg3se contributors
// (https://github.com/Norbyte/bg3se); these implementations are ours.

#include <sys/uio.h>
#include <unistd.h>


namespace {

// Probing every page of a large range would cost more than the validation it
// guards, so check the first and last byte. That catches the null and
// wild-pointer cases the callers actually care about.
bool readable(void const* p) {
    unsigned char sink;
    iovec local{&sink, 1};
    iovec remote{const_cast<void*>(p), 1};
    return ::process_vm_readv(::getpid(), &local, 1, &remote, 1, 0) == 1;
}

}  // namespace

// Win32 returns nonzero when the range is *not* readable.
extern "C" int IsBadReadPtr(void const* p, unsigned long long size) {
    if (p == nullptr || size == 0) return 1;
    if (!readable(p)) return 1;
    auto const last = static_cast<unsigned char const*>(p) + (size - 1);
    return readable(last) ? 0 : 1;
}

// Declared by vendor/compat/detours.h. See that header for why these refuse
// rather than hook: the only upstream user is CoreLib/Wrappers.h, whose
// callers bg3le replaces with PLT interposition, and Wrap() already treats a
// non-zero return as "not wrapped".
extern "C" long DetourAttachEx(void** /*ppPointer*/, void* /*pDetour*/,
                               void** /*ppRealTrampoline*/, void** /*ppRealTarget*/,
                               void** /*ppRealDetour*/) {
    return 50;  // ERROR_NOT_SUPPORTED
}

extern "C" long DetourDetach(void** /*ppPointer*/, void* /*pDetour*/) {
    return 50;
}
