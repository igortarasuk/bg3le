#pragma once
//
// Compatibility glue for building the BG3 Script Extender
// (https://github.com/Norbyte/bg3se) with clang on Linux instead of MSVC on
// Windows. The code it supports is by Norbyte and the bg3se contributors,
// MIT + Commons Clause; only this shim is ours. With thanks to them.
//
// The Osiris debugger interface is written against Winsock. Berkeley sockets
// are close enough that the handful of names it uses map directly, so the
// upstream code compiles and behaves unchanged.
//

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>

typedef int SOCKET;

#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)

inline int closesocket(SOCKET s) { return ::close(s); }

// Winsock keeps its error separately from errno; on POSIX they are the same.
inline int WSAGetLastError() { return errno; }

// Winsock needs explicit library startup and teardown. POSIX does not.
struct WSADATA {
    unsigned short wVersion;
    unsigned short wHighVersion;
};

inline int WSAStartup(unsigned short /*versionRequested*/, WSADATA* data) {
    if (data != nullptr) {
        data->wVersion = 0x0202;
        data->wHighVersion = 0x0202;
    }
    return 0;
}

inline int WSACleanup() { return 0; }
