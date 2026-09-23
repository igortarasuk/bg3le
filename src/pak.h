#pragma once

// Reads Larian's LSPK archives, enough to pull text files out of them.

#include <cstddef>
#include <functional>

namespace bg3le {

// Calls `sink` with the decompressed contents of every file in `path` whose
// name `accept` returns true for. Names use forward slashes and are relative
// to the archive root, e.g. "Public/Shared/Stats/Generated/Data/Weapon.txt".
//
// Returns false if the archive could not be read at all; a file that fails
// to decompress is skipped, not fatal.
// Every file name in `path`, without reading any of their contents. The
// list is decoded once per archive and kept, so this is cheap to repeat.
bool pak_list(char const* path,
              std::function<void(char const* name)> const& sink);

bool pak_read(char const* path,
              std::function<bool(char const* name)> const& accept,
              std::function<void(char const* name, char const* data,
                                 std::size_t size)> const& sink);

}  // namespace bg3le
