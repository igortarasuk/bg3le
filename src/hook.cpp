#include "hook.h"

#include <link.h>
#include <sys/mman.h>
#include <unistd.h>

#include "log.h"

namespace bg3le {
namespace {

int main_object(struct dl_phdr_info* info, std::size_t, void* data) {
    if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') return 0;
    *static_cast<std::uintptr_t*>(data) = info->dlpi_addr;
    return 1;
}

std::uintptr_t load_bias() {
    std::uintptr_t bias = 0;
    ::dl_iterate_phdr(main_object, &bias);
    return bias;
}

}  // namespace

bool hook_slot(std::uintptr_t slot_offset, std::uintptr_t expected_offset,
               void* replacement, void** original) {
    const std::uintptr_t bias = load_bias();
    auto* slot = reinterpret_cast<void**>(bias + slot_offset);
    void* expected = reinterpret_cast<void*>(bias + expected_offset);

    if (*slot != expected) {
        logf("hook: slot %#lx holds %p, expected %p -- refusing to patch",
             (unsigned long)slot_offset, *slot, expected);
        return false;
    }

    // The table lives in .data.rel.ro, which is read-only once relocated.
    const long page = ::sysconf(_SC_PAGESIZE);
    auto addr = reinterpret_cast<std::uintptr_t>(slot);
    auto* page_start = reinterpret_cast<void*>(addr & ~(std::uintptr_t)(page - 1));
    const std::size_t span = (addr + sizeof(void*)) - (std::uintptr_t)page_start;
    if (::mprotect(page_start, span, PROT_READ | PROT_WRITE) != 0) {
        logf("hook: mprotect failed for slot %#lx", (unsigned long)slot_offset);
        return false;
    }

    if (original != nullptr) *original = *slot;
    *slot = replacement;
    ::mprotect(page_start, span, PROT_READ);

    logf("hook: slot %#lx -> %p (was %p)", (unsigned long)slot_offset, replacement,
         original != nullptr ? *original : nullptr);
    return true;
}

}  // namespace bg3le
