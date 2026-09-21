// Pre-flight check: dlopen the built library with RTLD_NOW, so every symbol
// has to resolve. Catches the case where the library builds and links but the
// dynamic linker refuses it at load time -- which is what happens when a
// reference is left undefined, and is not visible from a successful build.
//
//   clang tools/dlopen-check.c -o /tmp/dlopen-check -ldl && /tmp/dlopen-check
//
#include <dlfcn.h>
#include <stdio.h>
int main(void) {
    void* h = dlopen("/home/lenon/bg3mods/bg3le/build/libbg3le.so", RTLD_NOW);
    if (h == NULL) { printf("FAILED: %s\n", dlerror()); return 1; }
    printf("loaded OK (RTLD_NOW: every symbol resolved)\n");
    return 0;
}
