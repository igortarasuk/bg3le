// Checks the /proc/self/maps line filter used by the string table search.
//
// The first version skipped two fields before looking for the pathname, but
// the format is
//
//   address perms offset dev inode pathname
//
// so it landed on the inode, saw a digit, decided every region was
// file-backed, and scanned nothing at all -- then reported "not found" for a
// table it had never looked for. The only reason that was noticeable is that
// the log prints how much it scanned.
//
// So the filter is tested against real lines. Every case below is copied from
// an actual /proc/self/maps.
//
//   cc -o /tmp/mapcheck tools/check-maps.c -ldl && /tmp/mapcheck build/libbg3le.so

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int (*scannable)(char const*, unsigned long long*, unsigned long long*);

static int failures = 0;

static void expect(char const* line, int wantScannable,
                   unsigned long long wantFrom, unsigned long long wantTo,
                   char const* why) {
    unsigned long long from = 0;
    unsigned long long to = 0;
    const int got = scannable(line, &from, &to) ? 1 : 0;

    if (got != wantScannable) {
        printf("  FAIL %s: %s, expected %s\n", why,
               got ? "scannable" : "skipped",
               wantScannable ? "scannable" : "skipped");
        failures++;
        return;
    }
    if (wantScannable && (from != wantFrom || to != wantTo)) {
        printf("  FAIL %s: bounds %llx-%llx, expected %llx-%llx\n", why, from,
               to, wantFrom, wantTo);
        failures++;
        return;
    }
    printf("  ok   %s\n", why);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <path to libbg3le.so>\n", argv[0]);
        return 2;
    }

    void* h = dlopen(argv[1], RTLD_NOW);
    if (h == NULL) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    scannable = dlsym(h, "bg3le_scannable_region");
    if (scannable == NULL) {
        fprintf(stderr, "missing symbol: bg3le_scannable_region\n");
        return 1;
    }

    // The heap, and an anonymous region: both wanted.
    expect("562cf9c9b000-562cfa1b4000 rw-p 00000000 00:00 0 "
           "                         [heap]\n",
           1, 0x562cf9c9b000ULL, 0x562cfa1b4000ULL, "the heap is scanned");
    expect("7f2c40000000-7f2c44000000 rw-p 00000000 00:00 0 \n",
           1, 0x7f2c40000000ULL, 0x7f2c44000000ULL,
           "an anonymous mapping is scanned");

    // A file-backed writable region is skipped, and deliberately: the table is
    // a heap allocation, the scan reads directly because safe_read is a
    // syscall, and a direct read of a file-backed page can SIGBUS where an
    // anonymous one cannot.
    expect("562cf9000000-562cf9c9b000 rw-p 01a3b000 fd:01 1234567 "
           "         /home/lenon/bg3mods/bg3-linux-native/bin/bg3\n",
           0, 0, 0, "a writable file-backed region is skipped");

    // Not writable: nothing of ours lives there.
    expect("562cf7000000-562cf9000000 r-xp 00000000 fd:01 1234567 "
           "         /home/lenon/bg3mods/bg3-linux-native/bin/bg3\n",
           0, 0, 0, "an executable mapping is skipped");
    expect("7f2c50000000-7f2c50001000 ---p 00000000 00:00 0 \n",
           0, 0, 0, "a guard page is skipped");
    expect("7f2c60000000-7f2c60001000 r--p 00000000 00:00 0 \n",
           0, 0, 0, "a read-only mapping is skipped");

    // Reading a device mapping is not always free of consequence.
    expect("7f2c70000000-7f2c78000000 rw-s 00000000 00:05 4096 "
           "           /dev/nvidiactl\n",
           0, 0, 0, "a device mapping is skipped");
    expect("7f2c80000000-7f2c80800000 rw-s 00000000 00:01 98765 "
           "          /memfd:something (deleted)\n",
           0, 0, 0, "a memfd mapping is skipped");
    expect("7ffd0f7f9000-7ffd0f7fd000 r--p 00000000 00:00 0 "
           "                  [vvar]\n",
           0, 0, 0, "vvar is skipped");

    // The stack is writable and anonymous-ish, and scanning it is harmless.
    expect("7ffd0f7fd000-7ffd0f81e000 rw-p 00000000 00:00 0 "
           "                  [stack]\n",
           1, 0x7ffd0f7fd000ULL, 0x7ffd0f81e000ULL, "the stack is scanned");

    // Malformed input must be refused rather than producing bounds.
    expect("this is not a maps line\n", 0, 0, 0, "a malformed line is skipped");
    expect("562cf9c9b000-562cf9c9b000 rw-p 00000000 00:00 0 \n", 0, 0, 0,
           "an empty range is skipped");

    if (failures != 0) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("maps filter behaves\n");
    return 0;
}
