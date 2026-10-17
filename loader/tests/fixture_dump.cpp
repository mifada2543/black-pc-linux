// Write the synthetic XBE to a file, so the program can be exercised by hand against it.
//
//   ./build/linux/fixture_dump /tmp/opencode/boot.xbe && \
//   ./build/linux/blacklinux --xbe /tmp/opencode/boot.xbe --probe --trace
//
// This is not a test: it produces the same bytes tests/xbe_fixture.h builds in memory, for the one job a
// unit test cannot do -- put a file on disk and hand it to the real program, which parses a path, maps
// sections, patches fs->gs, resolves 115 imports and runs the entry point on a thread. It also keeps the
// fixture out of /tmp hand-written generators, which is where the numbers used to live.
#include "xbe_fixture.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: %s <out.xbe>\n", argv[0]);
        return 2;
    }
    const std::vector<uint8_t> bytes = bootfix::build();
    FILE *f = fopen(argv[1], "wb");
    if (!f) {
        fprintf(stderr, "%s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    const size_t n = fwrite(bytes.data(), 1, bytes.size(), f);
    const int closed = fclose(f);
    if (n != bytes.size() || closed != 0) {
        fprintf(stderr, "%s: short write\n", argv[1]);
        return 1;
    }
    printf("%s: %zu bytes, entry 0x%08x, stub %zu bytes\n", argv[1], bytes.size(), bootfix::ENTRY_VA,
           sizeof bootfix::kBootCode);
    return 0;
}
