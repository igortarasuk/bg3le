// Linux stand-ins for the bg3se components whose upstream implementations are
// Windows-only, and which bg3le therefore does not build. See
// tools/vendor-exclude.txt for the list and the reasons.
//
// Where a component has no meaning here, it refuses loudly rather than
// pretending to work: a symbol mapper that silently reports success would
// leave GetStaticSymbols() full of null pointers and fail much later, in a
// much less obvious way.
//
// The interfaces are by Norbyte and the bg3se contributors
// (https://github.com/Norbyte/bg3se); these implementations are ours.

#include <Extender/Client/SDLManager.h>
#include <GameDefinitions/AllSpark.h>
#include <GameHooks/DataLibraries.h>
#include <GameHooks/EngineHooks.h>
#include <GameHooks/OsirisWrappers.h>

#include "../log.h"

BEGIN_SE()

// ---------------------------------------------------------------------------
// Crash reporting
//
// bg3se installs a Windows unhandled-exception filter and a minidump writer.
// bg3le reports faults from a signal handler instead (src/stackdump.cpp), so
// these only have to exist.
// ---------------------------------------------------------------------------

std::atomic<uint32_t> gDisableCrashReportingCount{0};
std::unordered_set<void const*> gRegisteredTrampolines;

void InitCrashReporting() {}
void ShutdownCrashReporting() {}

// ---------------------------------------------------------------------------
// LibraryManager
//
// Upstream scans the PE image for byte patterns to populate
// GetStaticSymbols(). bg3le reads the ELF symbol table instead
// (src/elf_symbols.cpp), but that mapping is not wired into GetStaticSymbols()
// yet, so this reports failure rather than handing back null pointers.
// ---------------------------------------------------------------------------

// symbolMapper_ takes the mappings by reference and has no default
// constructor, so it has to be initialised here as upstream does.
LibraryManager::LibraryManager()
    : symbolMapper_(mappings_)
{}

bool LibraryManager::FindLibraries(uint32_t gameRevision)
{
    bg3le::logf("LibraryManager: symbol mapping is not implemented on Linux "
                "(game revision %u); engine symbols are unavailable",
                gameRevision);
    return false;
}

bool LibraryManager::PostStartupFindLibraries()
{
    return false;
}

void LibraryManager::ApplyCodePatches()
{
    // The upstream patches are byte edits keyed to Windows builds.
}

bool LibraryManager::GetGameVersion(GameVersionInfo& version)
{
    version = {};
    return false;
}

void LibraryManager::ShowStartupError(STDString const& msg, bool wait, bool exitGame)
{
    bg3le::logf("bg3se startup error: %s", msg.c_str());
    (void)wait;
    (void)exitGame;
}

void LibraryManager::ShowStartupError(STDString const& msg, bool exitGame)
{
    ShowStartupError(msg, false, exitGame);
}

// ---------------------------------------------------------------------------
// OsirisWrappers
//
// Upstream inline-hooks the Osiris exports through Detours. bg3le already
// interposes the same functions by mangled name via the PLT (src/preload.cpp),
// so installing a second set of hooks over the same functions would be a
// conflict, not a feature. These exist so the extender core links; Osiris
// integration runs through bg3le's own interposition.
// ---------------------------------------------------------------------------

// The hook statics live in the excluded .cpp files, so they are defined here
// instead. Upstream spells these decltype(X)* decltype(X)::gHook, which is not
// legal in a declarative nested name specifier, so an alias names the type --
// and an explicit specialisation of a static data member needs an initialiser
// to be a definition rather than a declaration.
#define STATIC_HOOK(name) \
    using name##HookType = decltype(OsirisWrappers::name); \
    template<> name##HookType* name##HookType::gHook = nullptr;

STATIC_HOOK(RegisterDivFunctions)
STATIC_HOOK(InitGame)
STATIC_HOOK(DeleteAllData)
STATIC_HOOK(GetFunctionMappings)
STATIC_HOOK(OpenLogFile)
STATIC_HOOK(CloseLogFile)
STATIC_HOOK(Compile)
STATIC_HOOK(Load)
STATIC_HOOK(Merge)
STATIC_HOOK(Event)
STATIC_HOOK(RuleActionCall)
STATIC_HOOK(Call)
STATIC_HOOK(Query)
STATIC_HOOK(Error)
STATIC_HOOK(Assert)
STATIC_HOOK(CreateFileW)
STATIC_HOOK(CloseHandle)
#undef STATIC_HOOK

#define HOOK_DEFN(name, sym, defn) \
    using Engine_##name##HookType = decltype(EngineHooks::name); \
    template<> Engine_##name##HookType* Engine_##name##HookType::gHook = nullptr;
#include <GameHooks/EngineHooks.inl>
#undef HOOK_DEFN

OsirisWrappers::OsirisWrappers() = default;

void OsirisWrappers::Initialize()
{
    bg3le::logf("OsirisWrappers: not installing Detours hooks; bg3le "
                "interposes Osiris through the PLT instead");
}

void OsirisWrappers::Shutdown() {}

bool OsirisWrappers::ResolveNodeVMTs()
{
    return false;
}

void OsirisWrappers::RegisterDIVFunctionsPreHook(void* Osiris, DivFunctions* Functions)
{
    (void)Osiris;
    (void)Functions;
}

// ---------------------------------------------------------------------------
// EngineHooks
//
// Every hook here is installed by WrappableFunction, which needs the inline
// hooker bg3le does not provide. Hooking these with our own primitives is
// possible -- they are ordinary functions with call sites -- but each one has
// to be looked up by symbol first.
// ---------------------------------------------------------------------------

void EngineHooks::HookAll()
{
    bg3le::logf("EngineHooks: inline hooking is unavailable on Linux; no "
                "engine hooks installed");
}

void EngineHooks::UnhookAll() {}

// ---------------------------------------------------------------------------
// SDLManager
//
// Hooks SDL_PollEvent and friends to drive the ImGui overlay. The game does
// link SDL2, so this is implementable later; it needs the hooks that
// EngineHooks does.
// ---------------------------------------------------------------------------

SDLManager::SDLManager() = default;
SDLManager::~SDLManager() = default;

void SDLManager::EnableHooks()
{
    bg3le::logf("SDLManager: SDL hooks are not installed on Linux");
}

void SDLManager::DisableHooks() {}
void SDLManager::InitializeUI() {}
void SDLManager::DestroyUI() {}
void SDLManager::NewFrame() {}

void SDLManager::InjectEvent(SDL_Event const& evt)
{
    (void)evt;
}

// ---------------------------------------------------------------------------
// aspk::Component
//
// Declared with a pure virtual destructor, which still needs a definition:
// every derived destructor calls it and the vtables reference it. bg3se never
// constructs one -- the type only describes engine memory -- so MSVC never
// demanded the symbol.
// ---------------------------------------------------------------------------

namespace aspk {
Component::~Component() {}
}

END_SE()
