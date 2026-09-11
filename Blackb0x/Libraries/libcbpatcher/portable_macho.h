//
//  portable_macho.h
//  Blackb0x
//
//  Minimal, portable (non-Apple-header-dependent) definitions of the classic
//  32-bit Mach-O structures CBPatch.c needs to statically analyze a
//  downloaded ARMv7 kernelcache buffer. This is pure binary-format parsing
//  of a stable, publicly documented file format — not a proprietary macOS
//  runtime API — so these struct layouts are safe to redefine outside of
//  <mach-o/*.h>/<mach/vm_types.h>, which don't exist on Linux.
//

#ifndef BLACKB0X_PORTABLE_MACHO_H
#define BLACKB0X_PORTABLE_MACHO_H

#include <stdint.h>
#include <stddef.h> /* offsetof() — CBPatch.c relies on this being pulled in
                     * transitively by the Apple headers this file replaces */

typedef uint32_t vm_address_t;

#define LC_SEGMENT 0x1
#define LC_SYMTAB  0x2

struct mach_header {
    uint32_t magic;
    int32_t cputype;
    int32_t cpusubtype;
    uint32_t filetype;
    uint32_t ncmds;
    uint32_t sizeofcmds;
    uint32_t flags;
};

struct load_command {
    uint32_t cmd;
    uint32_t cmdsize;
};

struct segment_command {
    uint32_t cmd;
    uint32_t cmdsize;
    char segname[16];
    uint32_t vmaddr;
    uint32_t vmsize;
    uint32_t fileoff;
    uint32_t filesize;
    int32_t maxprot;
    int32_t initprot;
    uint32_t nsects;
    uint32_t flags;
};

struct section {
    char sectname[16];
    char segname[16];
    uint32_t addr;
    uint32_t size;
    uint32_t offset;
    uint32_t align;
    uint32_t reloff;
    uint32_t nreloc;
    uint32_t flags;
    uint32_t reserved1;
    uint32_t reserved2;
};

struct symtab_command {
    uint32_t cmd;
    uint32_t cmdsize;
    uint32_t symoff;
    uint32_t nsyms;
    uint32_t stroff;
    uint32_t strsize;
};

struct nlist {
    union {
        uint32_t n_strx;
    } n_un;
    uint8_t n_type;
    uint8_t n_sect;
    int16_t n_desc;
    uint32_t n_value;
};

#endif /* BLACKB0X_PORTABLE_MACHO_H */
