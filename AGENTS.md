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

- `Blackb0x/Source/` — the real, ported C++ (`main.cpp`, `Cli.hpp`/`.cpp`,
  `DeviceManager.hpp`/`.cpp`, `IPSW.hpp`/`.cpp`, `IPSWDownloader.hpp`/`.cpp`,
  `Patcher.hpp`/`.cpp`, `ResourcePath.hpp`/`.cpp`) alongside the **original,
  now-superseded Objective-C** (`AppDelegate`, `MainView`, `Blackb0x`, `TaskManager`,
  and the `.m`/`.mm` counterparts of the files above) — kept for reference/provenance,
  not built. `checkm8.h`/`SHAtter.h` are exploit payload byte arrays, `#include`d
  directly by `DeviceManager.cpp`.
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
| `gaster` | verygenericname/gaster | No — used unmodified, shelled out to for the actual checkm8 exploit |

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

**Runtime requirements beyond the build** (not just build-time deps):
- `mkfs.hfsplus`/`fsck.hfsplus` (`hfsprogs` package) and a kernel with `hfsplus`
  support (`CONFIG_HFSPLUS_FS`) — needed by `patchRamdisk()`.
- The system `usbmuxd` **must run with `--no-preflight`** (a systemd drop-in —
  `/etc/systemd/system/usbmuxd.service.d/override.conf` — is the documented way; see
  `docs/HISTORY.md` for exactly why) or Normal-mode device discovery silently never
  fires. This has to ship in the end-user README.
- The invoking user's own `~/.ssh/authorized_keys` must exist — `patchRamdisk()`
  refuses to proceed without it (no shared default key is ever baked into the
  ramdisk).

## Current status

Builds clean; fully statically linked. Normal-mode discovery (real `usbmuxd`,
wolfSSL/SSLv3 lockdownd handshake, AFC), the full firmware download/decrypt/patch/
re-encrypt pipeline, and `--dry-run` are all verified working against real hardware.
**Only one physical unit has ever been available to test against: an AppleTV3,2** —
`AppleTV2,1`(SHAtter)/`AppleTV3,1` (external-hardware checkm8) paths are implemented
from protocol analysis only, unverified.

The actual `checkm8` exploit run is currently blocked — not by a software bug, but by
what looks like a host-controller-level USB limitation on the current dev machine
(direct connection, Thunderbolt routing, and two different USB hub topologies all hit
the identical device-reset/reconnect failure). Full investigation, including the six
real software bugs that were found and fixed getting here, is in `docs/HISTORY.md`.
Next step is trying different host hardware, not further changes to this codebase.
