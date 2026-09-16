//
//  main.c
//  blackb0x-pwn
//
//  Standalone CLI for Blackb0x's own original checkm8/SHAtter exploit
//  implementations (see Checkm8Pwn.c) -- deliberately independent of the
//  main `blackb0x` binary's gaster-based checkm8Attempt() path. Builds on
//  every platform: nothing here is Apple-specific, only libirecovery's own
//  backend-independent irecv_* API, which resolves to IOKit on Darwin and
//  libusb elsewhere. See this target's own block in CMakeLists.txt.
//

#include "Checkm8Pwn.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void printUsage(const char* argv0) {
    fprintf(stderr,
        "Usage: %s <checkm8|shatter> [--ecid <hex-or-decimal>]\n"
        "\n"
        "  checkm8  Apple TV 3,1/3,2 (S5L8947X) SecureROM DFU exploit\n"
        "  shatter  Apple TV 2,1 SecureROM DFU exploit\n"
        "\n"
        "Runs Blackb0x's own original, hand-ported exploit implementations\n"
        "directly over libirecovery (its native IOKit backend on macOS, libusb\n"
        "elsewhere) -- no gaster. Waits for a single already-connected DFU-mode\n"
        "device; if --ecid is omitted, the first one found is used.\n",
        argv0);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    const char* exploit = argv[1];
    uint64_t ecid = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--ecid") == 0 && i + 1 < argc) {
            ecid = strtoull(argv[++i], NULL, 0);
        } else {
            fprintf(stderr, "Unknown argument: %s\n\n", argv[i]);
            printUsage(argv[0]);
            return 1;
        }
    }

    int ok;
    if (strcmp(exploit, "checkm8") == 0) {
        ok = runCheckm8(ecid);
    } else if (strcmp(exploit, "shatter") == 0) {
        ok = runSHAtter(ecid);
    } else {
        printUsage(argv[0]);
        return 1;
    }

    return ok ? 0 : 1;
}
