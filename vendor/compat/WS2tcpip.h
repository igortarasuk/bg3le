#pragma once
//
// Compatibility glue for building the BG3 Script Extender
// (https://github.com/Norbyte/bg3se) with clang on Linux instead of MSVC on
// Windows. The code it supports is by Norbyte and the bg3se contributors,
// MIT + Commons Clause; only this shim is ours. With thanks to them.
//
// The TCP/IP half of Winsock. Everything used is in the POSIX headers.
//
#include "WinSock2.h"

#include <netdb.h>
#include <netinet/tcp.h>
