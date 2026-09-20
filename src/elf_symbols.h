// Runtime symbol resolution against the (unstripped) bg3 executable.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace bg3le {

class SymbolTable {
public:
    // Parses .symtab from the running executable on disk and records its
    // load bias.  Returns false if the binary has been stripped.  Never throws.
    bool load();

    // Testable form: parse an arbitrary ELF with an explicit load bias.
    bool load_file(const std::string& path, std::uintptr_t bias);

    // Runtime address of a mangled symbol, or nullptr.
    void* find(const std::string& mangled) const;

    template <typename T>
    T find_as(const std::string& mangled) const {
        return reinterpret_cast<T>(find(mangled));
    }

    std::size_t count() const { return symbols_.size(); }
    std::uintptr_t bias() const { return bias_; }
    const std::string& path() const { return path_; }

private:
    std::unordered_map<std::string, std::uintptr_t> symbols_;  // st_value, unbiased
    std::uintptr_t bias_ = 0;
    std::string path_;
};

}  // namespace bg3le
