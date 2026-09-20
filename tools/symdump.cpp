// Offline check of the symbol parser against a given ELF.
#include <cstdio>
#include "../src/elf_symbols.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: symdump <elf> [symbol ...]\n");
        return 2;
    }
    bg3le::SymbolTable st;
    if (!st.load_file(argv[1], 0)) {
        std::fprintf(stderr, "no .symtab in %s\n", argv[1]);
        return 1;
    }
    std::printf("symbols: %zu\n", st.count());
    for (int i = 2; i < argc; ++i) {
        void* a = st.find(argv[i]);
        std::printf("  %-56s %s\n", argv[i], a ? "resolved" : "MISSING");
    }
    return 0;
}
