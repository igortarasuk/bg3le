#include "elf_symbols.h"

#include <elf.h>
#include <fcntl.h>
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>
#include <cerrno>
#include <exception>

#include "log.h"

namespace bg3le {
namespace {

// The main executable is the one entry with an empty name.
int find_main_object(struct dl_phdr_info* info, std::size_t, void* data) {
    if (info->dlpi_name != nullptr && info->dlpi_name[0] != '\0') return 0;
    *static_cast<std::uintptr_t*>(data) = info->dlpi_addr;
    return 1;
}

class MappedFile {
public:
    explicit MappedFile(const char* path) {
        fd_ = ::open(path, O_RDONLY | O_CLOEXEC);
        if (fd_ < 0) return;
        struct stat st {};
        if (::fstat(fd_, &st) != 0 || st.st_size <= 0) return;
        size_ = static_cast<std::size_t>(st.st_size);
        void* p = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
        if (p != MAP_FAILED) base_ = static_cast<const unsigned char*>(p);
    }

    ~MappedFile() {
        if (base_ != nullptr) ::munmap(const_cast<unsigned char*>(base_), size_);
        if (fd_ >= 0) ::close(fd_);
    }

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    bool ok() const { return base_ != nullptr; }
    const unsigned char* data() const { return base_; }
    std::size_t size() const { return size_; }

private:
    int fd_ = -1;
    const unsigned char* base_ = nullptr;
    std::size_t size_ = 0;
};

}  // namespace

bool SymbolTable::load() {
    char exe[4096];
    ssize_t n = ::readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return false;
    exe[n] = '\0';

    std::uintptr_t bias = 0;
    ::dl_iterate_phdr(find_main_object, &bias);

    // Open /proc/self/exe directly: inside the Steam runtime's bwrap
    // container the resolved path is often not visible in our mount
    // namespace, but the magic link always is.
    try {
        if (!load_file("/proc/self/exe", bias)) return false;
        path_ = exe;  // report the real path, not the magic link
        return true;
    } catch (const std::exception& e) {
        logf("elf: exception during parse: %s", e.what());
        return false;
    } catch (...) {
        logf("elf: unknown exception during parse");
        return false;
    }
}

bool SymbolTable::load_file(const std::string& path, std::uintptr_t bias) {
    path_ = path;
    bias_ = bias;
    symbols_.clear();

    MappedFile f(path.c_str());
    if (!f.ok() || f.size() < sizeof(Elf64_Ehdr)) {
        logf("elf: cannot map %s (errno %d)", path.c_str(), errno);
        return false;
    }

    const auto* ehdr = reinterpret_cast<const Elf64_Ehdr*>(f.data());
    if (std::memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0) return false;
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) return false;
    if (ehdr->e_shoff == 0 || ehdr->e_shentsize != sizeof(Elf64_Shdr)) return false;
    if (ehdr->e_shoff + std::size_t(ehdr->e_shnum) * sizeof(Elf64_Shdr) > f.size()) return false;

    const auto* shdrs = reinterpret_cast<const Elf64_Shdr*>(f.data() + ehdr->e_shoff);

    for (unsigned i = 0; i < ehdr->e_shnum; ++i) {
        if (shdrs[i].sh_type != SHT_SYMTAB) continue;  // .dynsym alone is not enough
        if (shdrs[i].sh_link >= ehdr->e_shnum) continue;

        const Elf64_Shdr& sym_sh = shdrs[i];
        const Elf64_Shdr& str_sh = shdrs[sym_sh.sh_link];
        if (sym_sh.sh_offset + sym_sh.sh_size > f.size()) continue;
        if (str_sh.sh_offset + str_sh.sh_size > f.size()) continue;
        if (sym_sh.sh_entsize != sizeof(Elf64_Sym)) continue;

        const auto* syms = reinterpret_cast<const Elf64_Sym*>(f.data() + sym_sh.sh_offset);
        const char* strs = reinterpret_cast<const char*>(f.data() + str_sh.sh_offset);
        const std::size_t total = sym_sh.sh_size / sizeof(Elf64_Sym);

        symbols_.reserve(total);
        for (std::size_t s = 0; s < total; ++s) {
            const Elf64_Sym& sym = syms[s];
            if (sym.st_name == 0 || sym.st_value == 0) continue;
            if (sym.st_name >= str_sh.sh_size) continue;
            const unsigned char type = ELF64_ST_TYPE(sym.st_info);
            if (type != STT_FUNC && type != STT_OBJECT) continue;

            // strtab may not be NUL-terminated at its end; bound the length
            // rather than letting std::string scan past the mapping.
            const char* name = strs + sym.st_name;
            const std::size_t avail = str_sh.sh_size - sym.st_name;
            const void* nul = std::memchr(name, '\0', avail);
            if (nul == nullptr) continue;
            const std::size_t len = static_cast<const char*>(nul) - name;
            if (len == 0) continue;
            symbols_.emplace(std::string(name, len), sym.st_value);
        }
    }

    logf("elf: parsed %zu symbols", symbols_.size());
    return !symbols_.empty();
}

void* SymbolTable::find(const std::string& mangled) const {
    auto it = symbols_.find(mangled);
    if (it == symbols_.end()) return nullptr;
    return reinterpret_cast<void*>(bias_ + it->second);
}

void SymbolTable::for_each(
    const std::function<void(const std::string&, std::uintptr_t)>& fn) const {
    for (const auto& [name, value] : symbols_) {
        fn(name, bias_ + value);
    }
}

}  // namespace bg3le
