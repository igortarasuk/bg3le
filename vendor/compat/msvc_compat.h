#pragma once
//
// Compatibility glue for building the BG3 Script Extender
// (https://github.com/Norbyte/bg3se) with clang on Linux instead of MSVC on
// Windows. The code it supports is by Norbyte and the bg3se contributors,
// MIT + Commons Clause; only this shim is ours. With thanks to them.
//
//
// Force-included (clang -include) so upstream headers compile unmodified.
// MSVC defines SAL annotations and the SRWLOCK types implicitly; clang does
// not, and the headers that use them do not include anything that would.
//

// SAL annotations carry no codegen meaning; discard them.
#define _Post_ptr_invalid_
#define _Pre_valid_
#define _Post_writable_byte_size_(size)
#define _Post_writable_size_(size)
#define _Pre_writable_size_(size)
#define _Pre_readable_size_(size)
#define _In_
#define _In_opt_
#define _Out_
#define _Inout_

// A slim-reader-writer lock is one pointer wide. This is enough to compile
// declarations; the Linux engine uses pthread primitives, so any *layout*
// that embeds one is suspect and must be checked before it is trusted.
extern "C" {
typedef struct _RTL_SRWLOCK { void* Ptr; } SRWLOCK, *PSRWLOCK;
}

typedef void* HANDLE;

// MSVC layout, for declarations only -- nothing here should end up in a
// structure whose layout must match the engine.
typedef struct _RTL_CRITICAL_SECTION {
    void* DebugInfo;
    long LockCount;
    long RecursionCount;
    void* OwningThread;
    void* LockSemaphore;
    unsigned long long SpinCount;
} CRITICAL_SECTION, *PCRITICAL_SECTION;

// Declared, not defined. These guard state belonging to bg3se rather than to
// the engine, so a futex-backed one-pointer implementation can stand in; that
// is only needed to link, not to compile the definitions.
extern "C" {
void InitializeSRWLock(PSRWLOCK);
void AcquireSRWLockExclusive(PSRWLOCK);
void ReleaseSRWLockExclusive(PSRWLOCK);
void AcquireSRWLockShared(PSRWLOCK);
void ReleaseSRWLockShared(PSRWLOCK);
}

#define _In_z_
#define _In_reads_(n)
#define _Out_writes_(n)
#define _Printf_format_string_

typedef unsigned int DWORD;   // 32-bit on Windows; unsigned long is 64-bit on LP64
typedef long long LONG64;
typedef unsigned long long ULONG64;
typedef void* HMODULE;

// MSVC declares these in the global namespace as well as in std.
#include <exception>
using std::terminate;

typedef int LONG;             // 32-bit on Windows
typedef struct _EXCEPTION_POINTERS {
    void* ExceptionRecord;
    void* ContextRecord;
} EXCEPTION_POINTERS, *PEXCEPTION_POINTERS;

// MSVC pulls these in transitively; libc++ does not.
#include <variant>
#include <optional>
#include <string>

#define WINBASEAPI
#define WINAPI
#define CALLBACK

// Win32 file-enumeration types, needed only so the hook typedefs in
// GameHooks/EngineHooksFwdDecl.h parse. The Linux build does not hook these.
typedef int BOOL;
typedef wchar_t* LPWSTR;
typedef const wchar_t* LPCWSTR;
typedef struct _WIN32_FIND_DATAW { unsigned char opaque[592]; }
    WIN32_FIND_DATAW, *LPWIN32_FIND_DATAW;

typedef unsigned char BYTE;   // one byte: it appears in engine layouts
typedef unsigned short WORD;

// Upstream uses the Win32 pointer probe to reject garbage pointers before
// dereferencing them during validation. bg3le implements it over
// process_vm_readv; see src/win_compat.cpp.
extern "C" BOOL IsBadReadPtr(void const* p, unsigned long long size);

// MSVC bit-scan intrinsics. Their out-parameter is unsigned long, which is
// 32-bit on Windows and 64-bit here, so clang builtins of the same name do
// not accept the upstream call sites. Macros take precedence over builtins.
namespace bg3le_compat {
template <class TIndex>
inline unsigned char bsf64(TIndex* index, unsigned long long mask) {
    if (mask == 0) return 0;
    *index = static_cast<TIndex>(__builtin_ctzll(mask));
    return 1;
}
template <class TIndex>
inline unsigned char bsf32(TIndex* index, unsigned int mask) {
    if (mask == 0) return 0;
    *index = static_cast<TIndex>(__builtin_ctz(mask));
    return 1;
}
template <class TIndex>
inline unsigned char bsr64(TIndex* index, unsigned long long mask) {
    if (mask == 0) return 0;
    *index = static_cast<TIndex>(63 - __builtin_clzll(mask));
    return 1;
}
}  // namespace bg3le_compat

#define _BitScanForward64(Index, Mask) ::bg3le_compat::bsf64((Index), (Mask))
#define _BitScanForward(Index, Mask)   ::bg3le_compat::bsf32((Index), (Mask))
#define _BitScanReverse64(Index, Mask) ::bg3le_compat::bsr64((Index), (Mask))

typedef void* LPSECURITY_ATTRIBUTES;

// Win32 spellings the upstream sources use directly.
#include <strings.h>
#include <sys/syscall.h>
#include <unistd.h>
inline int _stricmp(const char* a, const char* b) { return ::strcasecmp(a, b); }
inline unsigned long GetCurrentThreadId() {
    return static_cast<unsigned long>(::syscall(SYS_gettid));
}

// Win32 generic function pointer, used by the Osiris DLL wrappers.
typedef void (*FARPROC)();

// Win32 memory protection, used by the PE symbol mapper. bg3le does not use
// that mapper -- it reads the ELF symbol table instead -- but the header still
// has to compile.
#include <sys/mman.h>
typedef void* LPVOID;
#define PAGE_NOACCESS          0x01
#define PAGE_READONLY          0x02
#define PAGE_READWRITE         0x04
#define PAGE_EXECUTE           0x10
#define PAGE_EXECUTE_READ      0x20
#define PAGE_EXECUTE_READWRITE 0x40

inline int bg3le_page_prot(unsigned long win) {
    switch (win) {
        case PAGE_READONLY:          return PROT_READ;
        case PAGE_READWRITE:         return PROT_READ | PROT_WRITE;
        case PAGE_EXECUTE:           return PROT_EXEC;
        case PAGE_EXECUTE_READ:      return PROT_READ | PROT_EXEC;
        case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
        default:                     return PROT_NONE;
    }
}

inline BOOL VirtualProtect(LPVOID addr, std::size_t size,
                           DWORD newProtect, DWORD* oldProtect) {
    const long page = 4096;
    auto start = reinterpret_cast<unsigned long long>(addr) & ~(unsigned long long)(page - 1);
    const unsigned long long span =
        (reinterpret_cast<unsigned long long>(addr) + size) - start;
    if (oldProtect != nullptr) *oldProtect = PAGE_EXECUTE_READ;  // not queryable
    return ::mprotect(reinterpret_cast<void*>(start), span,
                      bg3le_page_prot(newProtect)) == 0;
}

// MSVC secure-CRT string formatting. Upstream uses the array-reference
// overloads, where the bound is deduced. _snprintf_s takes a maximum
// character count excluding the terminator; snprintf takes a buffer size
// including it.
#include <cstdio>
#include <cstddef>

template <std::size_t N, class... Args>
int _snprintf_s(char (&buf)[N], std::size_t count, const char* fmt, Args... args) {
    const std::size_t size = (count + 1 < N) ? count + 1 : N;
    return std::snprintf(buf, size, fmt, args...);
}

template <std::size_t N, class... Args>
int sprintf_s(char (&buf)[N], const char* fmt, Args... args) {
    return std::snprintf(buf, N, fmt, args...);
}

template <std::size_t N>
int strcpy_s(char (&buf)[N], const char* src) {
    std::snprintf(buf, N, "%s", src);
    return 0;
}

typedef unsigned long long ULONGLONG;

// Win32 high-resolution timing, used by Ext.Timer. CLOCK_MONOTONIC with a
// fixed 1 GHz frequency gives the nanosecond resolution the callers expect.
#include <ctime>

typedef union _LARGE_INTEGER {
    struct { DWORD LowPart; LONG HighPart; };
    long long QuadPart;
} LARGE_INTEGER;

inline BOOL QueryPerformanceCounter(LARGE_INTEGER* count) {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    count->QuadPart = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    return 1;
}

inline BOOL QueryPerformanceFrequency(LARGE_INTEGER* freq) {
    freq->QuadPart = 1000000000LL;
    return 1;
}

inline ULONGLONG GetTickCount64() {
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ULONGLONG)ts.tv_sec * 1000ULL + (ULONGLONG)(ts.tv_nsec / 1000000);
}
