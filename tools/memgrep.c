#define _GNU_SOURCE
// Counts occurrences of a literal string in another process's memory.
//
// For questions that have to be answered with bg3le absent: the shim is
// the only instrument inside the game, so anything it might itself be
// causing has to be observed from outside.
//
//   memgrep <pid> <string>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: memgrep <pid> <string>\n");
        return 2;
    }

    const long pid = strtol(argv[1], NULL, 10);
    const char* needle = argv[2];
    const size_t want = strlen(needle);

    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/maps", pid);
    FILE* maps = fopen(path, "r");
    if (maps == NULL) {
        fprintf(stderr, "memgrep: cannot read %s\n", path);
        return 1;
    }

    const size_t chunk = 1u << 20;
    char* block = malloc(chunk + want);
    unsigned long long scanned = 0;
    long hits = 0;

    char line[1024];
    while (fgets(line, sizeof(line), maps) != NULL) {
        unsigned long long from = 0;
        unsigned long long to = 0;
        char perms[8] = {0};
        if (sscanf(line, "%llx-%llx %7s", &from, &to, perms) != 3) continue;
        if (perms[0] != 'r') continue;
        // The archives are mapped into the process, and a hit inside one
        // says nothing about what the game parsed -- but everything else
        // file-backed is fair game, and skipping all of it left nothing
        // but a few megabytes to search.
        if (strstr(line, ".pak") != NULL) continue;
        if (strstr(line, "/dev/") != NULL) continue;

        for (unsigned long long at = from; at < to; at += chunk) {
            size_t size = (size_t)(to - at);
            if (size > chunk + want) size = chunk + want;

            struct iovec local = {block, size};
            struct iovec remote = {(void*)(size_t)at, size};
            const ssize_t got = process_vm_readv(pid, &local, 1, &remote, 1, 0);
            if (got <= 0) continue;
            scanned += (unsigned long long)got;

            for (char* p = block;
                 (p = memmem(p, (size_t)got - (size_t)(p - block), needle,
                             want)) != NULL;
                 p += want) {
                ++hits;
            }
        }
    }
    fclose(maps);
    free(block);

    printf("%ld occurrences of \"%s\" in %.1f MB\n", hits, needle,
           scanned / 1048576.0);
    return hits > 0 ? 0 : 1;
}
