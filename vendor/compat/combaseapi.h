#pragma once
//
// Compatibility glue for building the BG3 Script Extender
// (https://github.com/Norbyte/bg3se) with clang on Linux instead of MSVC on
// Windows. The code it supports is by Norbyte and the bg3se contributors,
// MIT + Commons Clause; only this shim is ours. With thanks to them.
//
// Upstream uses the Windows RPC UUID functions to parse and generate GUIDs.
// The layout matters: a Windows GUID stores Data1/Data2/Data3 in native byte
// order and Data4 as bytes, so the textual form is not a straight hex dump.
// These follow that layout exactly, because Guid values round-trip through
// Osiris and the ECS.
//

#include <cstdint>
#include <cstdio>

typedef struct _GUID {
    std::uint32_t Data1;
    std::uint16_t Data2;
    std::uint16_t Data3;
    std::uint8_t Data4[8];
} GUID, UUID;

typedef unsigned char* RPC_CSTR;
typedef long RPC_STATUS;

#define RPC_S_OK 0
#define RPC_S_INVALID_STRING_UUID 1700

extern "C" {
RPC_STATUS UuidFromStringA(RPC_CSTR stringUuid, UUID* uuid);
RPC_STATUS UuidCreate(UUID* uuid);
}
