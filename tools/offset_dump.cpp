// Throwaway: print field offsets from bg3se's vendored struct definitions
// via a deliberate template-instantiation error (avoids linking Noesis,
// which GameState.h drags in transitively).
#include <GameDefinitions/GameState.h>
#include <GameDefinitions/Module.h>
#include <cstddef>

template <std::size_t N> struct Show;

using namespace bg3se;

Show<sizeof(esv::EoCServer)> s_sizeof_EoCServer;
Show<offsetof(esv::EoCServer, GameServer)> s_off_GameServer;
Show<offsetof(esv::EoCServer, GameStateMachine)> s_off_GameStateMachine;
Show<offsetof(esv::EoCServer, ModManager)> s_off_ModManager;
Show<offsetof(esv::EoCServer, EntityWorld)> s_off_EntityWorld;
Show<sizeof(ModManager)> s_sizeof_ModManager;
Show<offsetof(ModManager, Settings)> s_off_Settings;
Show<offsetof(ModManager, BaseModule)> s_off_BaseModule;
Show<sizeof(ModuleSettings)> s_sizeof_ModuleSettings;
Show<offsetof(ModuleSettings, Mods)> s_off_Mods;
Show<sizeof(ModuleShortDesc)> s_sizeof_ModuleShortDesc;
Show<offsetof(ModuleShortDesc, ModuleUUID)> s_off_ModuleUUID;
