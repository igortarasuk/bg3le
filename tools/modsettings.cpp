// Builds a modsettings.lsx load order from a directory of .pak files.
//
// The Windows backup this was written for has the paks but no usable load
// order -- its modsettings.lsx lists only the campaign module, so the mods
// were installed and never enabled. Rather than hand-write 58 entries, read
// each archive's own meta.lsx: it carries the UUID, folder, name and
// version the load order needs, and the dependencies that decide the order.
//
// Reports what it could not resolve rather than guessing: a mod whose
// dependency is not installed is listed on stderr and still written, since
// the engine tolerates it and the alternative is silently dropping a mod
// the user asked for.

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <dirent.h>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../src/pak.h"

namespace {

struct Mod {
    std::string Uuid;
    std::string Name;
    std::string Folder;
    std::string Version;
    std::string PublishHandle;
    std::string Md5;
    std::vector<std::string> Needs;
    std::string Pak;
};

// The modules the game itself provides, read from its own archives rather
// than listed here: a mod depending on Gustav or Shared is not missing
// anything, and hard-coding those UUIDs goes stale with every patch.
std::set<std::string>& base_modules() {
    static std::set<std::string> uuids;
    return uuids;
}

bool is_base(std::string const& uuid) {
    return base_modules().count(uuid) != 0;
}

// The value of one LSX attribute inside `block`, or an empty string.
std::string attribute(std::string const& block, char const* id) {
    const std::string needle = std::string("id=\"") + id + "\"";
    std::size_t at = 0;
    while ((at = block.find(needle, at)) != std::string::npos) {
        // The attribute has to be an <attribute>, not a <node id="...">.
        const std::size_t open = block.rfind('<', at);
        if (open == std::string::npos
            || block.compare(open, 10, "<attribute") != 0) {
            at += needle.size();
            continue;
        }
        const std::size_t value = block.find("value=\"", at);
        const std::size_t end = block.find('>', at);
        if (value == std::string::npos || end == std::string::npos
            || value > end) {
            at += needle.size();
            continue;
        }
        const std::size_t from = value + 7;
        const std::size_t to = block.find('"', from);
        if (to == std::string::npos) return std::string();
        return block.substr(from, to - from);
    }
    return std::string();
}

// The text of the node whose opening tag carries `id`, up to the end of its
// children. Good enough for meta.lsx, which nests one level.
std::string node(std::string const& text, char const* id) {
    const std::string needle = std::string("<node id=\"") + id + "\"";
    const std::size_t at = text.find(needle);
    if (at == std::string::npos) return std::string();

    // A node with children ends at its </children>; a leaf at its </node>.
    const std::size_t children = text.find("<children>", at);
    const std::size_t closes = text.find("</node>", at);
    if (children != std::string::npos && children < closes) {
        const std::size_t end = text.find("</children>", children);
        if (end == std::string::npos) return text.substr(at);
        return text.substr(at, end - at);
    }
    if (closes == std::string::npos) return text.substr(at);
    return text.substr(at, closes - at);
}

bool parse_meta(std::string const& meta, Mod* out) {
    const std::string info = node(meta, "ModuleInfo");
    if (info.empty()) return false;

    out->Uuid = attribute(info, "UUID");
    out->Name = attribute(info, "Name");
    out->Folder = attribute(info, "Folder");
    // Deliberately not meta.lsx's MD5. That is the hash of the archive as
    // published; the engine compares it with the archive on disk and drops
    // the mod when they differ, which is every mod repacked or updated
    // since. An empty MD5 means "do not check", which is what the mod
    // managers write.
    out->Md5.clear();
    out->PublishHandle = attribute(info, "PublishHandle");
    out->Version = attribute(info, "Version64");
    if (out->Version.empty()) out->Version = attribute(info, "Version");
    if (out->Version.empty()) out->Version = "36028797018963968";
    if (out->PublishHandle.empty()) out->PublishHandle = "0";
    if (out->Uuid.empty() || out->Folder.empty()) return false;
    if (out->Name.empty()) out->Name = out->Folder;

    const std::string deps = node(meta, "Dependencies");
    std::size_t at = 0;
    while ((at = deps.find("<node id=\"ModuleShortDesc\"", at))
           != std::string::npos) {
        const std::size_t end = deps.find("</node>", at);
        const std::string one = deps.substr(
            at, end == std::string::npos ? std::string::npos : end - at);
        const std::string uuid = attribute(one, "UUID");
        if (!uuid.empty()) out->Needs.push_back(uuid);
        if (end == std::string::npos) break;
        at = end + 1;
    }
    return true;
}

// Every module an archive declares, not just the first: the game's own
// Gustav.pak carries several, and taking one of them left the rest looking
// like missing dependencies.
std::vector<Mod> read_metas(std::string const& path) {
    std::vector<std::string> metas;
    const bool ok = bg3le::pak_read(
        path.c_str(),
        [](char const* name) {
            const std::size_t len = std::strlen(name);
            return len >= 8 && std::strcmp(name + len - 8, "meta.lsx") == 0;
        },
        [&](char const* name, char const* data, std::size_t size) {
            (void)name;
            metas.emplace_back(data, size);
        });

    std::vector<Mod> mods;
    if (!ok) return mods;
    for (std::string const& meta : metas) {
        Mod mod;
        if (parse_meta(meta, &mod)) mods.push_back(std::move(mod));
    }
    return mods;
}

// Dependencies before dependents, and alphabetical within a tier so the
// order is reproducible rather than filesystem-dependent.
std::vector<Mod> ordered(std::vector<Mod> mods) {
    std::map<std::string, Mod const*> byUuid;
    for (Mod const& mod : mods) byUuid[mod.Uuid] = &mod;

    std::sort(mods.begin(), mods.end(), [](Mod const& a, Mod const& b) {
        return a.Name < b.Name;
    });

    std::vector<Mod> out;
    std::set<std::string> placed;
    std::set<std::string> placing;

    std::function<void(Mod const&)> place = [&](Mod const& mod) {
        if (placed.count(mod.Uuid) != 0) return;
        // A dependency cycle is a mod-authoring bug, not ours to resolve:
        // break it and keep the order otherwise intact.
        if (!placing.insert(mod.Uuid).second) return;
        for (std::string const& need : mod.Needs) {
            auto it = byUuid.find(need);
            if (it != byUuid.end()) place(*it->second);
        }
        placing.erase(mod.Uuid);
        if (placed.insert(mod.Uuid).second) out.push_back(mod);
    };

    for (Mod const& mod : mods) place(mod);
    return out;
}

void write_entry(std::FILE* f, std::string const& folder,
                 std::string const& md5, std::string const& name,
                 std::string const& publish, std::string const& uuid,
                 std::string const& version) {
    std::fprintf(f,
                 "                        <node id=\"ModuleShortDesc\">\n"
                 "                            <attribute id=\"Folder\" type=\"LSString\" value=\"%s\"/>\n"
                 "                            <attribute id=\"MD5\" type=\"LSString\" value=\"%s\"/>\n"
                 "                            <attribute id=\"Name\" type=\"LSString\" value=\"%s\"/>\n"
                 "                            <attribute id=\"PublishHandle\" type=\"uint64\" value=\"%s\"/>\n"
                 "                            <attribute id=\"UUID\" type=\"guid\" value=\"%s\"/>\n"
                 "                            <attribute id=\"Version64\" type=\"int64\" value=\"%s\"/>\n"
                 "                        </node>\n",
                 folder.c_str(), md5.c_str(), name.c_str(), publish.c_str(),
                 uuid.c_str(), version.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    // Why a pak has no usable meta.lsx is worth being able to ask.
    if (argc == 3 && std::strcmp(argv[1], "--list") == 0) {
        const bool ok = bg3le::pak_read(
            argv[2], [](char const*) { return true; },
            [](char const* name, char const*, std::size_t size) {
                std::printf("%9zu  %s\n", size, name);
            });
        if (!ok) std::fprintf(stderr, "modsettings: cannot read %s\n", argv[2]);
        return ok ? 0 : 1;
    }

    if (argc == 4 && std::strcmp(argv[1], "--cat") == 0) {
        bool found = false;
        bg3le::pak_read(
            argv[2],
            [&](char const* name) { return std::strcmp(name, argv[3]) == 0; },
            [&](char const*, char const* data, std::size_t size) {
                std::fwrite(data, 1, size, stdout);
                found = true;
            });
        return found ? 0 : 1;
    }

    if (argc < 3) {
        std::fprintf(stderr, "usage: modsettings <pak-directory> "
                             "<out.lsx> [game-data-directory]\n");
        return 2;
    }

    // The game's own modules first, so a dependency on one of them is not
    // reported as missing. Its multi-part archives cannot be read and hold
    // no meta.lsx anyway.
    if (argc >= 4) {
        DIR* data = opendir(argv[3]);
        if (data == nullptr) {
            std::fprintf(stderr, "modsettings: cannot read %s\n", argv[3]);
            return 1;
        }
        while (dirent* entry = readdir(data)) {
            const std::string name = entry->d_name;
            if (name.size() < 5
                || name.compare(name.size() - 4, 4, ".pak") != 0) {
                continue;
            }
            for (Mod const& mod :
                 read_metas(std::string(argv[3]) + "/" + name)) {
                base_modules().insert(mod.Uuid);
            }
        }
        closedir(data);
        std::fprintf(stderr, "%zu modules provided by the game\n",
                     base_modules().size());
    }

    DIR* dir = opendir(argv[1]);
    if (dir == nullptr) {
        std::fprintf(stderr, "modsettings: cannot read %s\n", argv[1]);
        return 1;
    }

    std::vector<Mod> mods;
    std::vector<std::string> unreadable;
    while (dirent* entry = readdir(dir)) {
        const std::string name = entry->d_name;
        if (name.size() < 5
            || name.compare(name.size() - 4, 4, ".pak") != 0) {
            continue;
        }
        const std::string path = std::string(argv[1]) + "/" + name;
        std::vector<Mod> found = read_metas(path);
        if (found.empty()) {
            unreadable.push_back(name);
            continue;
        }
        for (Mod& mod : found) {
            mod.Pak = name;
            mods.push_back(std::move(mod));
        }
    }
    closedir(dir);

    std::vector<Mod> const order = ordered(mods);

    std::FILE* f = std::fopen(argv[2], "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "modsettings: cannot write %s\n", argv[2]);
        return 1;
    }
    std::fprintf(f,
                 "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                 "<save>\n"
                 "    <version major=\"4\" minor=\"8\" revision=\"0\" build=\"700\"/>\n"
                 "    <region id=\"ModuleSettings\">\n"
                 "        <node id=\"root\">\n"
                 "            <children>\n"
                 "                <node id=\"Mods\">\n"
                 "                    <children>\n");
    // The campaign module first, as the engine writes it.
    write_entry(f, "GustavX", "", "GustavX", "0",
                "cb555efe-2d9e-131f-8195-a89329d218ea",
                "36028797018963968");
    for (Mod const& mod : order) {
        write_entry(f, mod.Folder, mod.Md5, mod.Name, mod.PublishHandle,
                    mod.Uuid, mod.Version);
    }
    std::fprintf(f,
                 "                    </children>\n"
                 "                </node>\n"
                 "            </children>\n"
                 "        </node>\n"
                 "    </region>\n"
                 "</save>\n");
    std::fclose(f);

    std::set<std::string> have;
    for (Mod const& mod : order) have.insert(mod.Uuid);
    for (Mod const& mod : order) {
        for (std::string const& need : mod.Needs) {
            if (have.count(need) == 0 && !is_base(need)) {
                std::fprintf(stderr, "missing dependency: %s needs %s\n",
                             mod.Name.c_str(), need.c_str());
            }
        }
    }
    for (std::string const& pak : unreadable) {
        std::fprintf(stderr, "no readable meta.lsx: %s\n", pak.c_str());
    }
    std::fprintf(stderr, "%zu mods written, %zu unreadable\n", order.size(),
                 unreadable.size());
    return 0;
}
