#pragma once

// What Ext.Mod reads off one engine Module. See mods.cpp for how the layout
// was established.

#include <cstddef>
#include <cstdint>

namespace bg3le {

struct ModInfo {
    // Strings point into per-field storage owned by the reader and stay
    // valid until the next call on the same thread.
    char const* ModuleUUIDString;
    char const* Name;
    char const* Directory;
    char const* Hash;
    char const* Author;
    char const* Description;

    char const* StartLevelName;
    char const* MenuLevelName;
    char const* LobbyLevelName;
    char const* CharacterCreationLevelName;
    char const* PhotoBoothLevelName;

    // Major, minor, revision, build -- the order upstream reports them in.
    std::uint32_t ModVersion[4];
    std::uint32_t PublishVersion[4];

    std::uint8_t NumPlayers;
    std::uint64_t FileSize;
    std::uint64_t PublishHandle;
};

// One entry of a module's Dependencies, ModConflicts or Addons.
struct ModShortDesc {
    char const* ModuleUUIDString;
    char const* Name;
    char const* Folder;
    char const* Hash;
    std::uint32_t ModVersion[4];
    std::uint32_t PublishVersion[4];
    std::uint64_t PublishHandle;
};

}  // namespace bg3le

extern "C" {

std::size_t bg3le_mods_count();
char const* bg3le_mods_uuid_at(std::size_t index);
void* bg3le_mods_at(std::size_t index);
void* bg3le_mods_find(char const* uuid);

// ModManager::BaseModule -- the campaign, not the first module in load
// order.
void* bg3le_mods_base();

// ModManager::AvailableMods, the superset the load order is drawn from.
std::size_t bg3le_mods_available_count();
void* bg3le_mods_available_at(std::size_t index);

bool bg3le_mod_info(void const* module, bg3le::ModInfo* out);

// Which of a module's three ModuleShortDesc arrays to read.
enum Bg3leModList {
    kBg3leModDependencies = 0,
    kBg3leModConflicts = 1,
    kBg3leModAddons = 2,
};

std::size_t bg3le_mod_list_count(void const* module, int list);
bool bg3le_mod_list_at(void const* module, int list, std::size_t index,
                       bg3le::ModShortDesc* out);
}
