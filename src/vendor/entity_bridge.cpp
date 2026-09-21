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

// Reports each step of the walk separately, because a single null cannot
// distinguish "the entity genuinely has no such component" from "the storage
// lookup failed". Returns the storage index (-1 if none), the storage pointer,
// and the component pointer.
extern "C" void bg3le_entity_probe(void* container, std::uint64_t handle,
                                   std::uint16_t componentIndex,
                                   std::int32_t* storageIndex,
                                   void** storage, void** component) {
    *storageIndex = -1;
    *storage = nullptr;
    *component = nullptr;
    if (container == nullptr) return;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto entity = bg3se::EntityHandle(handle);

    const auto index = storages->GetEntityStorageIndex(entity);
    if (!index.has_value()) return;
    *storageIndex = *index;

    auto* data = storages->GetEntityStorage(*index);
    if (data == nullptr) return;
    *storage = data;

    *component = data->GetComponent(entity,
                                    bg3se::ecs::ComponentTypeIndex(componentIndex),
                                    sizeof(bg3se::HealthComponent));
}

// Walks the container's storages looking for one whose component set includes
// the given type, and returns an entity out of it. Needed because the handle
// the thunk captures is whatever the engine touched last, which is usually
// something with no Health at all.
extern "C" std::uint64_t bg3le_find_entity_with(void* container,
                                                std::uint16_t componentIndex,
                                                std::uint32_t* storagesSeen) {
    *storagesSeen = 0;
    if (container == nullptr) return 0;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto type = bg3se::ecs::ComponentTypeIndex(componentIndex);

    for (auto* storage : storages->Storages) {
        if (storage == nullptr) continue;
        ++*storagesSeen;
        if (!storage->ComponentTypeToIndex.try_get(type)) continue;

        // First live entity in this storage. HashMap keeps its keys in an
        // array, so they can be read without walking buckets.
        for (auto const& key : storage->InstanceToPageMap.keys()) {
            if (key.Handle != bg3se::EntityHandle::NullHandle) return key.Handle;
        }
    }
    return 0;
}

// Finds the component of a given type on whichever entity carries it, by
// scanning the container's storages. Used for the singleton components, which
// bg3se normally reaches through EntityWorld -- and the world is the one thing
// with no symbol and no capture point, so this route avoids needing it.
static void* find_any_component(void* container, std::uint16_t componentIndex,
                                std::size_t componentSize) {
    if (container == nullptr) return nullptr;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto type = bg3se::ecs::ComponentTypeIndex(componentIndex);

    for (auto* storage : storages->Storages) {
        if (storage == nullptr) continue;
        if (!storage->ComponentTypeToIndex.try_get(type)) continue;

        for (auto const& key : storage->InstanceToPageMap.keys()) {
            if (key.Handle == bg3se::EntityHandle::NullHandle) continue;
            auto* component = storage->GetComponent(key, type, componentSize);
            if (component != nullptr) return component;
        }
    }
    return nullptr;
}

// UUID string -> EntityHandle, through ls::uuid::ToHandleMappingComponent.
// Returns 0 if the mapping component cannot be found or the UUID is unknown.
extern "C" std::uint64_t bg3le_uuid_to_handle(void* container,
                                              std::uint16_t mappingIndex,
                                              char const* uuid) {
    auto* mapping = static_cast<bg3se::UuidToHandleMappingComponent*>(
        find_any_component(container, mappingIndex,
                           sizeof(bg3se::UuidToHandleMappingComponent)));
    if (mapping == nullptr || uuid == nullptr) return 0;

    const auto guid = bg3se::Guid::ParseGuidString(uuid);
    if (!guid.has_value()) return 0;

    auto* handle = mapping->Mappings.try_get(*guid);
    return handle != nullptr ? handle->Handle : 0;
}

// Marks a component dirty so the change is picked up and replicated.
//
// bg3se routes this through EntityWorld::MarkComponentAsChanged, but that
// function only touches the storage and the container's UsedFrameDataStorages
// bitset -- and EntityWorld::Storage *is* the container we captured. So it
// needs no world either.
//
// This is MarkComponentAsChanged, not bg3se's full Replicate: that one also
// goes through ReplicateComponent to set per-field replication flags. Marking
// the component dirty is what makes the server-side change take effect; if a
// field turns out not to reach the client, that difference is the first place
// to look.
extern "C" bool bg3le_mark_component_changed(void* container,
                                             std::uint64_t handle,
                                             std::uint16_t componentIndex) {
    if (container == nullptr) return false;

    auto* storages = reinterpret_cast<bg3se::ecs::EntityStorageContainer*>(container);
    const auto entity = bg3se::EntityHandle(handle);
    const auto type = bg3se::ecs::ComponentTypeIndex(componentIndex);

    const auto index = storages->GetEntityStorageIndex(entity);
    if (!index.has_value()) return false;

    auto* storage = storages->GetEntityStorage(*index);
    if (storage == nullptr) return false;

    if (!storage->MarkComponentAsChanged(entity, type)) return false;

    if (!storages->UsedFrameDataStorages[storage->StorageIndex]) {
        storages->UsedFrameDataStorages.Set(storage->StorageIndex);
    }
    return true;
}

// Health is read and written through typed accessors rather than raw offsets,
// so the field layout comes from bg3se's struct rather than being restated.
extern "C" bool bg3le_set_health(void* container, std::uint64_t handle,
                                 std::uint16_t componentIndex,
                                 std::int32_t hp, bool setMax) {
    auto* component = static_cast<bg3se::HealthComponent*>(bg3le_entity_component(
        container, handle, componentIndex, sizeof(bg3se::HealthComponent)));
    if (component == nullptr) return false;
    component->Hp = hp;
    if (setMax) component->MaxHp = hp;
    return true;
}

}  // namespace bg3le
