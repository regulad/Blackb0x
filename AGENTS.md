# AGENTS.md — Blackb0x Linux port

Short and operational: what an agent needs to move around this repo and build it
correctly, right now. For the *why* — every bug hunt, dead end, and decision's
evidence — see **[`docs/HISTORY.md`](docs/HISTORY.md)**. If something here seems to
need more justification than it gives, it's almost certainly explained there.

## What Blackb0x is

A jailbreak tool for 2nd/3rd-gen Apple TV (A1378/A1427/A1469) via the checkm8/SHAtter
DFU-mode boot exploit, which then side-loads Cydia + Kodi. A Linux CLI port of an
original macOS Cocoa/Objective-C app (the `.m`/`.mm`/`.h` files still in
`Blackb0x/Source/` are that original — reference-only, not built).

## Conventions (don't re-litigate without asking)

- **CLI-only, Linux-only.** No GUI, no macOS/Xcode support — dropped, not
  dual-maintained.
- **Every third-party dependency is a git submodule under `third_party/`, built from
  source, statically linked.** Not FetchContent, not system packages, no exceptions —
  see the "vendored dependencies" table below and `CMakeLists.txt`'s
  `ExternalProject_Add` blocks. `zlib` used to be the one system-provided exception;
  it no longer is.
- **The only dynamic dependencies in the final binary are libc/libstdc++/libm/
  libgcc_s.** Verify with `ldd build/blackb0x` after any dependency change — it should
  never grow beyond those five lines.
- **If a vendored library needs a code change, fork it to its own branch — never leave
  an uncommitted local patch sitting in a submodule's working tree.** Push the fork,
  point `.gitmodules`'s `url`/`branch` at it, keep the diff against the pinned base
  small and clean (`git reset --soft` + re-commit + `git push --force` on that branch
  if it needs cleaning up, not a growing pile of fixup commits). See "Vendored
  dependencies" below for which submodules are already forks and why.
- **Prefer adapting an existing, battle-tested library over hand-rolling the
  equivalent.** This has been the deciding factor twice already: a from-scratch
  lockdownd/AFC client was fully verified working, then deleted in favor of forking
  real `libimobiledevice` once it became clear that was strictly less code to
  maintain; a hand-ported `checkm8` USB exploit sequence was replaced with shelling
  out to `gaster` (the same implementation `palera1n` itself uses) once it became the
  more proven path. Default to this; don't reach for a rewrite first.
- **Terse, single-source CLI output.** Every user-facing message flows through
  `Cli.cpp`'s rendering of `DeviceManager`'s `DeviceEventSink` callbacks
  (`onStatus`/`onProgress`/`onDeviceAdded`/`onDeviceUpdated`/`onDeviceRemoved`) in one
  consistent, unbracketed style — not raw `printf()` scattered through
  `DeviceManager.cpp`/`Patcher.cpp`. Real errors go to `stderr`. Don't reintroduce
  debug-dump printfs (hex keys/IVs, raw status codes) outside of an explicit verbosity
  flag.
- **Version-pin every historically-relevant vendored library to what the original app
  actually used**, not "whatever's current" — already done for every submodule that
  existed in the original app; don't re-point one of those at a different commit
  without redoing the OSINT (see `docs/HISTORY.md`'s "Version pinning" section for the
  method, if a pin ever needs revisiting).

## Repo layout

- `Blackb0x/Source/` — the ported C++, and nothing else: `main.cpp`, `Cli.hpp`/`.cpp`,
  `DeviceManager.hpp`/`.cpp`, `IPSW.hpp`/`.cpp`, `IPSWDownloader.hpp`/`.cpp`,
  `Patcher.hpp`/`.cpp`, `ResourcePath.hpp`/`.cpp`. The original Objective-C
  (`AppDelegate`, `MainView`, `Blackb0x.h`/`.m`, `TaskManager`, and the old
  `.h`/`.m`/`.mm` counterparts of the files above) has been fully ported and deleted —
  check `docs/HISTORY.md`/git history if you need to see what it looked like.
  `checkm8.h`/`SHAtter.h` are exploit payload byte arrays, `#include`d directly by
  `DeviceManager.cpp` — not leftover Cocoa, keep these.
- `Blackb0x/Libraries/` — already-portable C kept in-tree and built directly by the
  root `CMakeLists.txt`: `CBPatcher.c`/`libcbpatcher/`, `libiboot32patcher.c`/
  `libiboot32patcher/`, `xpwntool.c`. `libbootkit/` is dead code, linked into nothing.
- `Blackb0x/Files/` — payload data (Cydia tarball, keys, `setup.sh`) shipped to the
  jailbroken Apple TV itself; needs no porting.
- `third_party/` — every vendored dependency (see table below).
- `docs/HISTORY.md` — the full debugging/decision log.

## Vendored dependencies

All built from source via `ExternalProject_Add`/`add_subdirectory` in `CMakeLists.txt`,
statically linked. **Forked** means: patched on our own branch, pushed, pointed to from
`.gitmodules` — not a local working-tree diff.

| Submodule | Source | Forked? |
|---|---|---|
| `libplist`, `libusbmuxd` | libimobiledevice/* | No — historically pinned |
| `libimobiledevice` | **regulad/libimobiledevice**@`legacy` | Yes — additive, `--with-ssl-implementation=wolfssl`-selectable SSL backend for `idevice.c` (real OpenSSL/GnuTLS untouched, still selectable) |
| `libirecovery` | synackuk/libirecovery | No |
| `libimobiledevice-glue`, `libplist-modern` | libimobiledevice/* | No — current HEAD, not historically pinned (see HISTORY for why two `libplist`s) |
| `libfragmentzip` | **regulad/libfragmentzip**@`fix-cxx-stdbool-header` | Yes — one header fix (C++/`<stdbool.h>` collision) |
| `libgeneral` | tihmstar/libgeneral | No |
| `xpwn` | **regulad/xpwn**@`legacy` | Yes — a wolfSSL AES-CBC buffer over-read fix in `img3.c`, plus disabling the legacy-libusb-0.1-only `pwnmetheus2` subdirectory |
| `wolfssl`, `curl`, `libusb`, `libzip`, `libpng`, `bzip2`, `zlib` | upstream | No — current HEAD or latest stable tag; none of these existed in the original app |
| `gaster` | **regulad/gaster** (fork), `linux-reset-race` branch, off verygenericname/gaster | Yes — skips the post-`SETUP`/`SPRAY`-stage host-triggered `libusb_reset_device()` call (kept for `RESET`/`PATCH`, which genuinely need it) after real-hardware evidence that this Linux-specific reset was corrupting the device's own re-enumeration; see `docs/HISTORY.md` |

`Blackb0x/Libraries/xpwntool.c` (in-tree, not a submodule) is confirmed sourced from
`zzanehip/xpwntool-swift`, unchanged.

## Build & run

```
cmake -S . -B build && cmake --build build -j$(nproc)
sudo ./build/blackb0x [--ecid <id> | --udid <id>] [--tether-boot] [--dry-run]
```

`blackb0x` **must run as root** — DFU-mode USB access and `patchRamdisk()`'s loop-mount
(`CAP_SYS_ADMIN`/`CAP_CHOWN`) both need it. No udev rules, no install step — it runs
from wherever it's built.

**Build-time system dependencies, verified against a real fresh clone + build (see
README for the full list)**: a C/C++ toolchain, GNU make, CMake ≥3.16,
autoconf/automake/libtool/pkg-config (most of the tree is autotools-based), and
`xxd` — genuinely required, easy to miss, since it's only used once: embedding
`gaster`'s exploit payload binaries as C arrays at build time
(`add_custom_command(... COMMAND xxd -iC ...)` in `CMakeLists.txt`).

**Runtime requirements beyond the build** (not just build-time deps):
- `mkfs.hfsplus`/`fsck.hfsplus` (`hfsprogs` package) and a kernel with `hfsplus`
  support (`CONFIG_HFSPLUS_FS`) — needed by `patchRamdisk()`.
- `mount`/`umount`/`blkid`/`cp`/`tar` (invoked directly as subprocesses, no shell) —
  also `patchRamdisk()`. Assumed present on any mainstream distro, not called out as
  a separate install step.
- `usbmuxd` itself must be installed (a separate requirement from the point below —
  most distros package it separately, e.g. `usbmuxd`), and **must run with
  `--no-preflight`** (a systemd drop-in — `/etc/systemd/system/usbmuxd.service.d/
  override.conf` — is the documented way; see `docs/HISTORY.md` for exactly why) or
  Normal-mode device discovery silently never fires. Both of these have to ship in
  the end-user README.
- The invoking user's own `~/.ssh/authorized_keys` must exist — `patchRamdisk()`
  refuses to proceed without it (no shared default key is ever baked into the
  ramdisk).
- `stdbuf` (GNU coreutils) — **required**, not optional: `runGaster()`
  (`DeviceManager.cpp`) checks for it on `PATH` before ever forking `gaster` and
  refuses to run the exploit at all if it's missing, rather than silently falling
  back to unbuffered output. An earlier version of this code did fall back silently
  — that's exactly the "blind the whole time" bug documented in `docs/HISTORY.md`,
  reintroduced by treating this as optional. Don't re-add that fallback.
- The in-tree `apple_mfi_fastcharge` kernel driver **must be blacklisted** (a
  `/etc/modprobe.d` drop-in — see the README's own setup section) or it fights
  `gaster` for the DFU-mode device mid-exploit; see `docs/HISTORY.md` for exactly
  why. Same pattern as `usbmuxd --no-preflight` above: a documented one-time manual
  step, not something `blackb0x` checks or fixes for you at runtime.

## Current status

Builds clean; fully statically linked. Normal-mode discovery (real `usbmuxd`,
wolfSSL/SSLv3 lockdownd handshake, AFC), the full firmware download/decrypt/patch/
re-encrypt pipeline, and `--dry-run` are all verified working against real hardware.
**Only one physical unit has ever been available to test against: an AppleTV3,2** —
`AppleTV2,1`(SHAtter)/`AppleTV3,1` (external-hardware checkm8) paths are implemented
from protocol analysis only, unverified.

The actual `checkm8` exploit run has repeatedly hung/frozen the USB stack across
*multiple different Linux machines* — initially misdiagnosed (on a single machine) as
a host-controller-level USB limitation, since the symptoms (D-state hangs, corrupted
enumeration) looked hardware-specific. A live `dmesg` capture during a real hang found
the actual cause: the in-tree Linux `apple_mfi_fastcharge` driver auto-binds to the
Apple TV even in DFU mode (its product-ID match range, `0x1200`-`0x12ff`, includes this
device's real DFU PID `0x1227`) and independently issues its own `usb_reset_device()`
calls while `gaster`'s own raw, timing-sensitive control transfers are in flight — two
actors resetting the same device at once, which explains both the corruption and why it
reproduces on any Linux box with this common, usually-autoloaded kernel module present.
Fixed the same way as the `usbmuxd --no-preflight` requirement above — a one-time
manual system setup step documented in the README, not code in `blackb0x` itself: the
module needs to be blacklisted via `/etc/modprobe.d`, since a bare `modprobe -r` alone
was tried first and confirmed insufficient on real hardware (the kernel reloads it on
its own via `request_module()` on every one of gaster's stage-transition reconnects,
independent of anything either binary does in userspace); see the README's own setup
section for the exact commands.

**Tested against real hardware with the module actually blacklisted — confirmed
working as designed, but confirmed *not* the fix for the hang.** `apple_mfi_fastcharge`
genuinely stayed unloaded through the whole run (no competing driver anymore, confirmed
via `lsmod` and the kernel log's driver attribution), but the exact same corruption and
hang happened anyway — `gaster` still got stuck at the same point, and the device was
left in the same descriptor-corrupted state (`lsusb -v`: garbled `iManufacturer`/
`iProduct`, `Couldn't open device`) that only clears on a physical unplug/replug. So
`apple_mfi_fastcharge` was a real, additive conflict worth fixing, but not the (sole)
root cause — this pointed back at a genuine host-side (kernel/xHCI) limitation
reinitializing this device after `gaster`'s own reset, independent of any competing
driver.

`gaster` is now forked (**regulad/gaster**, `linux-reset-race` branch — see the vendored
dependencies table above) to test that theory directly: `gaster_checkm8()`'s
unconditional post-stage `reset_usb_handle()` call is only protocol-required after
`RESET`/`PATCH` (both explicitly put the device into `DFU_STATE_MANIFEST_WAIT_RESET`
first) — `SETUP`/`SPRAY` have no such requirement, and the corruption above happened
exactly at the `SETUP`→`SPRAY` boundary. The fork skips that specific host-triggered
reset on a successful `SETUP`/`SPRAY` transition, letting the next stage's own
`wait_usb_handle()` reconnect to the still-present device instead of forcing an
unnecessary bus reset. **Not yet tested against real hardware.** See `docs/HISTORY.md`'s
checkm8/gaster section for the full evidence trail and the six earlier real software
bugs found and fixed getting here. **Still unresolved.**
