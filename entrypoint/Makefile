# Plain Makefile — no Theos (see README.md for why: this binary is
# freestanding, Theos's tool.mk assumes dynamic linking against libSystem).
# Expects `arm-apple-darwin11-clang` (built by cctools-port's
# usage_examples/ios_toolchain/build.sh — see README.md step 3) on $PATH.

CC := arm-apple-darwin11-clang
LDID := ldid

CFLAGS := -arch armv6 -ffreestanding -nostdlib -fno-builtin -fno-stack-protector -Wall -Wextra -O2
LDFLAGS := -static -Wl,-e,_entry -nostartfiles

# entrypoint replaces /sbin/launchd's content directly (see
# BakeRamdisk.cpp's spliceFileContentInPlace() call), so it's signed under
# launchd's own well-known identifier rather than ldid's default (the
# binary's own filename). A mid-rewrite detour signed this as `com.apple.rc`
# and targeted /etc/rc.boot instead, on the strength of real disassembly
# showing that file was LC_MAIN-entered directly by the kernel on an
# AppleTV2,1 10B809 RestoreRamdisk — but that turned out to be firmware-
# specific: AppleTV3,1/3,2 12H606 has no /etc/rc.boot at all. Backstepped to
# always targeting /sbin/launchd, the one thing guaranteed present as real
# PID-1 across every firmware generation — see docs/HISTORY.md.
LAUNCHD_IDENTIFIER := com.apple.launchd

TARGET := entrypoint

.PHONY: all clean

all: $(TARGET)

$(TARGET): entrypoint.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $<
	$(LDID) -S -I$(LAUNCHD_IDENTIFIER) $@

clean:
	rm -f $(TARGET)
