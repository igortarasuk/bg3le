// Which mod defines each stat, which is what Ext.Stats reports as ModId and
// OriginalModId.
//
// Upstream does not read these off the stat object -- there is no such
// field. It hooks the engine as each Stats/Generated/*.txt is opened during
// load, notes which mod's directory the path names, and attributes every
// entry parsed afterwards to that mod. The Linux build carries no symbol for
// any engine function (`nm -C bg3 | grep " t .*ls::"` is empty), so there is
// nothing to hook by name.
//
// The information is in the files regardless: a stat lives in
// Public/<Directory>/Stats/Generated/**.txt, and <Directory> is a loaded
// module's Info.Directory. Reading the archives gives the same answer
// without depending on catching the load as it happens -- which also means
// it works when bg3le attaches to a game that has already loaded.
//
// Precedence follows load order rather than wall-clock order, which is what
// the engine's own ordering means: the last mod to define an entry wins
// (ModId), the first to define it is where it came from (OriginalModId).

#include <stdafx.h>

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "mods.h"

#include "../log.h"
#include "../pak.h"

namespace bg3le {

namespace {

// Where a stat entry begins in a .txt: `new entry "NAME"`.
constexpr char const kEntryPrefix[] = "new entry \"";
constexpr std::size_t kEntryPrefixLen = sizeof(kEntryPrefix) - 1;

struct Origin {
    int First{-1};
    int Last{-1};
};

struct Origins {
    bool Built{false};
    // Stat name to the load-order indices that define it.
    std::unordered_map<std::string, Origin> ByName;
    // Load-order index to module uuid, held so the map can hand out
    // stable strings.
    std::vector<std::string> Uuids;
};

Origins& state() {
    static Origins o;
    return o;
}

// The directory a stats path belongs to: Public/<Directory>/Stats/...
// Returns an empty string if the path is not one.
std::string stats_directory(char const* name) {
    constexpr char const kPublic[] = "Public/";
    constexpr std::size_t kPublicLen = sizeof(kPublic) - 1;
    if (std::strncmp(name, kPublic, kPublicLen) != 0) return {};

    char const* dir = name + kPublicLen;
    char const* slash = std::strchr(dir, '/');
    if (slash == nullptr) return {};
    if (std::strncmp(slash, "/Stats/Generated/", 17) != 0) return {};

    const std::size_t len = std::strlen(name);
    if (len < 4 || std::strcmp(name + len - 4, ".txt") != 0) return {};
    return std::string(dir, (std::size_t)(slash - dir));
}

void note(Origins* out, std::string const& name, int index) {
    Origin& o = out->ByName[name];
    if (o.First < 0 || index < o.First) o.First = index;
    if (index > o.Last) o.Last = index;
}

// Every `new entry "NAME"` in one file.
void scan_text(Origins* out, char const* data, std::size_t size, int index) {
    char const* end = data + size;
    for (char const* p = data; p + kEntryPrefixLen < end; ) {
        char const* hit = (char const*)memmem(
            p, (std::size_t)(end - p), kEntryPrefix, kEntryPrefixLen);
        if (hit == nullptr) return;

        char const* nameStart = hit + kEntryPrefixLen;
        char const* quote = (char const*)std::memchr(
            nameStart, '"', (std::size_t)(end - nameStart));
        if (quote == nullptr) return;

        note(out, std::string(nameStart, (std::size_t)(quote - nameStart)),
             index);
        p = quote + 1;
    }
}

// The game's Data directory, from the running executable: bin/bg3 sits one
// level below the install root.
std::string data_directory() {
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return {};
    exe[n] = '\0';

    std::string path(exe);
    const std::size_t bin = path.rfind("/bin/");
    if (bin == std::string::npos) return {};
    return path.substr(0, bin) + "/Data";
}

// Every .pak in a directory, in name order so a run is reproducible.
std::vector<std::string> paks_in(std::string const& dir) {
    std::vector<std::string> out;
    DIR* d = opendir(dir.c_str());
    if (d == nullptr) return out;

    while (dirent* e = readdir(d)) {
        std::string const name = e->d_name;
        if (name.size() < 5) continue;
        if (name.compare(name.size() - 4, 4, ".pak") != 0) continue;
        out.push_back(dir + "/" + name);
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

// Mods the player installed live beside the profile, not in the install.
std::string user_mods_directory() {
    char const* home = std::getenv("HOME");
    if (home == nullptr) return {};
    return std::string(home)
           + "/.local/share/Larian Studios/Baldur's Gate 3/Mods";
}

bool build() {
    Origins out;

    // The load order decides precedence, so it has to be known first.
    const std::size_t mods = bg3le_mods_count();
    if (mods == 0) return false;

    std::unordered_map<std::string, int> directoryToIndex;
    out.Uuids.resize(mods);
    for (std::size_t i = 0; i < mods; ++i) {
        void* module = bg3le_mods_at(i);
        ModInfo info{};
        if (module == nullptr || !bg3le_mod_info(module, &info)) continue;
        out.Uuids[i] = info.ModuleUUIDString;
        if (info.Directory[0] != '\0') {
            directoryToIndex[info.Directory] = (int)i;
        }
    }

    std::vector<std::string> archives = paks_in(data_directory());
    for (std::string const& pak : paks_in(user_mods_directory())) {
        archives.push_back(pak);
    }
    if (archives.empty()) {
        logf("origins: no archives found; ModId stays unavailable");
        return false;
    }

    std::size_t files = 0;
    for (std::string const& pak : archives) {
        int index = -1;
        pak_read(
            pak.c_str(),
            [&](char const* name) {
                std::string const dir = stats_directory(name);
                if (dir.empty()) return false;
                auto it = directoryToIndex.find(dir);
                // A mod that is installed but not in the load order does
                // not define anything the engine loaded.
                if (it == directoryToIndex.end()) return false;
                index = it->second;
                return true;
            },
            [&](char const*, char const* data, std::size_t size) {
                scan_text(&out, data, size, index);
                ++files;
            });
    }

    if (out.ByName.empty()) {
        logf("origins: read %zu archives but found no stat entries",
             archives.size());
        return false;
    }

    out.Built = true;
    state() = std::move(out);
    logf("origins: %zu stats attributed across %zu files in %zu archives",
         state().ByName.size(), files, archives.size());
    return true;
}

bool ready() {
    if (state().Built) return true;
    static int attempts = 0;
    if (attempts >= 40) return false;
    ++attempts;
    return build();
}

char const* uuid_at(int index) {
    Origins const& o = state();
    if (index < 0 || (std::size_t)index >= o.Uuids.size()) return nullptr;
    if (o.Uuids[(std::size_t)index].empty()) return nullptr;
    return o.Uuids[(std::size_t)index].c_str();
}

}  // namespace

// The mod whose definition of this stat the engine ended up with, and the
// one it first appeared in. Null when the stat is not attributed, which
// beats reporting a mod that did not define it.
extern "C" bool bg3le_stat_origin(char const* name, char const** modId,
                                  char const** originalModId) {
    if (name == nullptr || !ready()) return false;

    auto it = state().ByName.find(name);
    if (it == state().ByName.end()) return false;

    if (modId != nullptr) *modId = uuid_at(it->second.Last);
    if (originalModId != nullptr) {
        *originalModId = uuid_at(it->second.First);
    }
    return true;
}

// Warmed off the story thread alongside the other searches.
extern "C" bool bg3le_stat_origins_ready() {
    return ready();
}

}  // namespace bg3le
