#pragma once

// Reads Larian's LSPK archives, enough to pull text files out of them.

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

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

// The archive's load priority, as the engine reads it: where two archives
// hold the same path, the higher priority wins. The game ships most of its
// own at zero and its patch archives above them.
//
// Returns false if `path` is not an archive this reader understands.
bool pak_priority(char const* path, unsigned* priority);

bool pak_read(char const* path,
              std::function<bool(char const* name)> const& accept,
              std::function<void(char const* name, char const* data,
                                 std::size_t size)> const& sink);

// Writes an LSPK v18 archive with the given files, stored uncompressed.
// For rebuilding an archive whose contents have been edited; the engine
// reads stored entries as readily as compressed ones.
bool pak_write(char const* path,
               std::vector<std::pair<std::string, std::string>> const& files);

}  // namespace bg3le
