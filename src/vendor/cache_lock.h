// Locks for bg3le's derived caches.
//
// The server and client Lua contexts run on different engine threads, and
// every cache bg3le builds is process-wide: the string-table map, the text
// index, the stats indexes, the resource bank index, the Osiris name maps.
// Nothing synchronised them. Two threads inserting into the same
// unordered_map put a cycle in a bucket's forward list, and the server thread
// then spun in __do_rehash for eight minutes with a level half loaded -- the
// same stack twice, 460 seconds apart, with no other thread in sight, because
// the damage had already been done by then.
//
// One mutex per group rather than one for everything, because the groups form
// a fixed order and holding a single lock across a rebuild would serialise the
// two contexts for as long as an index takes to build. The order is acyclic:
// stats, static data and the component metadata all reach the string table,
// and it reaches none of them.
//
// Recursive because the stats indexes are built from each other -- a name is
// filed under a modifier list only when the by-name index agrees -- and
// because an entry point may be re-entered through a Lua callback.
//
// The lock has to span the *use* of a cache, not just the lookup: every
// accessor hands back a reference, and a rebuild on another thread would
// invalidate it. That is why these are taken at the exported entry points.
#pragma once

#include <mutex>

namespace bg3le {

using CacheLock = std::lock_guard<std::recursive_mutex>;

// src/vendor/fixed_string.cpp: the id-to-text map, the text-to-id index and
// the set of strings bg3le interned itself. The innermost lock.
inline std::recursive_mutex& string_cache_lock() {
    static std::recursive_mutex lock;
    return lock;
}

// src/vendor/stats.cpp: stats by name, names by modifier list, an object's
// properties, a modifier's metadata, the attribute-name and condition caches,
// and the shared read buffer.
inline std::recursive_mutex& stats_cache_lock() {
    static std::recursive_mutex lock;
    return lock;
}

// src/vendor/static_data.cpp: each resource bank's GUID index.
inline std::recursive_mutex& resource_cache_lock() {
    static std::recursive_mutex lock;
    return lock;
}

// src/osi.cpp: the database entries, the node class names and the parameter
// list vtable.
inline std::recursive_mutex& osiris_cache_lock() {
    static std::recursive_mutex lock;
    return lock;
}

}  // namespace bg3le
