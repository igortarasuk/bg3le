// Bridges bg3le's captured ECS pointers to bg3se's ECS implementation.
//
// bg3le supplies what only it can: the component type indices, read from the
// named TypeId statics in the symbol table, and the EntityStorageContainer,
// captured from a hooked GetEntityStorage call. bg3se supplies what it already
// reverse engineered: the layout of the container, of EntityStorageData, and
// the page-map walk that turns an EntityHandle plus a component index into a
// pointer.
//
// Its layout has been checked against this build twice over -- the component
// mask is 0x110 bytes, matching ComponentMapSize = 0x880, and the container
// begins with Array<EntityStorageData*>, which is where the disassembled
// lookup reads its buffer pointer from.
//
// The bg3se code this calls is by Norbyte and the bg3se contributors
// (https://github.com/Norbyte/bg3se); only the bridge is ours.

// Utils.h first: upstream relies on its own translation units pulling the
// logging macros in before the Lua headers.
#include <Extender/Shared/Utils.h>

#include <GameDefinitions/EntitySystem.h>
#include <GameDefinitions/Components/All.h>

namespace bg3le {

// Returns the component of the given type for an entity, or nullptr if the
// entity has no such component. componentSize is what bg3se uses to stride the
// component page, so it has to match the engine's real component size.
extern "C" void* bg3le_entity_component(void* container, std::uint64_t handle,
                                        std::uint16_t componentIndex,
                                        std::size_t componentSize) {
    if (container == nullptr) return nullptr;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto entity = bg3se::EntityHandle(handle);

    const auto storageIndex = storages->GetEntityStorageIndex(entity);
    if (!storageIndex.has_value()) return nullptr;

    auto* storage = storages->GetEntityStorage(*storageIndex);
    if (storage == nullptr) return nullptr;

    return storage->GetComponent(entity,
                                 bg3se::ecs::ComponentTypeIndex(componentIndex),
                                 componentSize);
}

// End-to-end proof: reads Health off an entity. Keeps every bg3se type inside
// this translation unit, so the rest of bg3le needs none of its headers.
//
// The component size matters: bg3se strides the component page with it, so it
// has to match the engine's. Taking it from bg3se's own struct is exactly the
// earlier finding that its component definitions lay out correctly here,
// because they are plain structs the compiler arranges rather than hardcoded
// offsets.
extern "C" bool bg3le_entity_health(void* container, std::uint64_t handle,
                                    std::uint16_t componentIndex,
                                    std::int32_t* hp, std::int32_t* maxHp) {
    auto* component = static_cast<bg3se::HealthComponent*>(bg3le_entity_component(
        container, handle, componentIndex, sizeof(bg3se::HealthComponent)));
    if (component == nullptr) return false;
    *hp = component->Hp;
    *maxHp = component->MaxHp;
    return true;
}

}  // namespace bg3le
