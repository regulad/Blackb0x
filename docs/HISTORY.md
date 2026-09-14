# HISTORY.md — Blackb0x portable port debugging log

This is the full narrative log of the macOS→Linux port (and, later, the reopened
Linux→portable work — see "Reopening macOS support" below): every bug hunt, every dead end,
every decision and the evidence behind it, in the order it happened. `AGENTS.md` (repo
root) is the short version — what an agent needs to navigate the repo and build it right
now. Come here when you need the *why* behind something AGENTS.md just states as fact, or
when picking up a specific unresolved thread (search for its name — every major
investigation has its own heading). Not a design doc — see `CMakeLists.txt`'s own inline
comments for terse rationale on each individual build fix.

## What Blackb0x is

A jailbreak tool for 2nd/3rd-gen Apple TV (A1378/A1427/A1469) via the checkm8/SHAtter
DFU-mode boot exploit, which then side-loads Cydia + Kodi. Originally a macOS
Cocoa/Objective-C app (Xcode project). This repo is being ported to a Linux CLI.

## Decisions already made (do not re-litigate without asking)

- **CLI-only.** No GTK/Qt GUI port — a plain command-line tool replaces AppDelegate/MainView.
- **Linux-only.** macOS/Xcode/AppKit support is being dropped entirely, not dual-maintained.
  (Superseded — see "Reopening macOS support" below: this was reopened by explicit
  request, scoped first to the `blackb0x`/`gaster` CLI path, not the ramdisk baker.)
- **Surgical conversion, not a rewrite.** Keep already-portable C libraries as-is; convert
  Objective-C/Foundation files to C/C++ as each is touched, in the phase order below.
- **CMake**, not Theos (Theos cross-compiles *for* Apple platforms; it can't produce a
  Linux binary or replace AppKit/IOKit, so it was ruled out early).
- **Git submodules** under `third_party/` for every externally-sourced dependency, per
  explicit user instruction — not FetchContent, not system packages where avoidable.
- **Version-pin every vendored library to what the original app actually used**, via OSINT
  on the original prebuilt binaries recovered from git history — not "whatever's current."
  See "Version pinning" below. This was explicitly requested and took significant effort;
  don't casually re-point a submodule at a different commit without redoing the legwork.

## Repo layout after Phase 0/1 restructuring

- `Blackb0x/Source/` — `main.cpp` + `Cli.hpp`/`.cpp` (Phase 6), `DeviceManager.hpp`/`.cpp`
  (Phase 3), `IPSW.hpp`/`.cpp` + `IPSWDownloader.hpp`/`.cpp` (Phase 4), and `Patcher.hpp`/
  `.cpp` (Phase 5) are all done C++ ports — see below for each phase's detail.
  `ResourcePath.hpp`/`.cpp` is a small shared utility (resolves paths under
  `Blackb0x/Files/`) used by Phases 4, 5, and 6. `AppDelegate.h/.m`, `MainView.h/.m`,
  `Patcher.h/.mm`, `TaskManager.h/.m`, `Blackb0x.h/.m`, `DeviceManager.h/.m`,
  `IPSW.h/.mm`/`IPSWDownloader.h/.mm` are the **original, now-fully-superseded
  Objective-C** — kept in-tree for reference/provenance only, not deleted, not built.
  `checkm8.h`/`SHAtter.h` (payload byte arrays) are `#include`d directly by
  `DeviceManager.cpp` instead of the old `.m`.
- `Blackb0x/Libraries/` — the already-portable C kept in-tree: `CBPatcher.c`,
  `libiboot32patcher.c`, `xpwntool.c` (+ their header subdirs), all built directly by the
  root `CMakeLists.txt`. `libbootkit/` is dead code (dead `#include` in `DeviceManager.m`,
  no symbols actually called) — left alone, not linked into anything.
- `Blackb0x/Files/` — payload data (Cydia tarball, keys, `setup.sh`). Unchanged; `setup.sh`
  runs *on* the jailbroken Apple TV post-install, not on the host, so it needs no porting.
- `third_party/` — everything vendored from scratch (see pin table below).
- Removed entirely: `Blackb0x.xcodeproj/`, `MainMenu.xib`, `Blackb0x.entitlements`,
  `Info.plist`, `Blackb0x.pch`, `Assets.xcassets/`, icon PNGs, `main.m`, and every prebuilt
  macOS `.dylib`/`.a` (all rebuilt from source now).

## Version pinning: how each library's version was determined

The user asked to pin every vendored library to the exact commit the original developer
used, not to whatever's current upstream — "this'll take some OSINT." Method:

1. Recovered the original prebuilt macOS binaries from git history (`git show
   85e2d8b:Blackb0x/Libraries/<file>` — the initial commit; `libcbpatcher.a`/
   `libxpwntool.a` were updated once more a week later in commit `21a2f65`, everything
   else was frozen from the start and never touched again).
2. Ran `strings`/`nm`/`file` on them via WSL (Windows Git Bash has no `strings`).
3. Found three kinds of hard evidence:
   - **Exact embedded version strings.** `libusbmuxd.dylib` contains the literal string
     `libusbmuxd 2.0.1`. `libfragmentzip.0.dylib`/`libfragmentzip.dylib` contain their own
     git-describe strings (`0.60-120447d0f4...` and `0.59-542a470d7b...` respectively —
     two different builds were bundled; the higher/newer one was taken as authoritative).
   - **An embedded debug path.** `libirecovery.a`/`libirecovery.3.dylib` contain
     `/Users/spiral/Desktop/Jailbreak/Tools/synackuk-libirecovery/...` — the developer
     built from **synackuk's fork** of libirecovery, not `libimobiledevice/libirecovery`.
   - **A toolchain fingerprint.** `libimobiledevice.a`/`libirecovery.a` both embed
     `Apple clang version 11.0.0 (clang-1100.0.33.16)` → Xcode 11.3.1 (released
     2020-01-28).
4. Cross-referenced Homebrew's `homebrew-core` formula git history (via `gh api
   repos/Homebrew/homebrew-core/commits?path=Formula/<name>.rb`) and found
   `libimobiledevice`, `libusbmuxd`, and `libplist` were all bumped to new versions by
   Homebrew **on the same day, 2020-06-15**. The bundled binaries predate that bump
   (`libusbmuxd 2.0.1` was superseded that day), landing the whole toolchain in a
   **Nov 2019 – 2020-06-15** window — consistent with the Xcode 11.3.1 fingerprint. This
   window is what led to the *initial* `libimobiledevice` pin (tag `1.2.0`) — later
   superseded by a much more precise method, see point 6.
5. For `xpwntool.c`/`libxpwntool/*.h` (already in-tree, not a submodule): the README
   itself credits **zzanehip** — "updated xpwntool (Created by planetbeing)". Found
   `zzanehip/xpwntool-swift` on GitHub and confirmed by direct content diff: `decrypt()`'s
   body and several headers are byte-identical to Blackb0x's copies (only include paths /
   a header comment / one debug printf differ). Pinned by evidence to commit `58b69c595c`
   (2020-09-13, the repo's last commit). No file changes were needed — this just confirms
   provenance of code already in-tree.
6. **Exact header content diffing beats date-proximity inference.** The original vendored
   headers (`Blackb0x/Libraries/libimobiledevice/*.h`, deleted in Phase 1 but recoverable
   via `git show 85e2d8b:...`) are the real ground truth for what API surface the app
   compiled against — independent of which binary got linked. When Phase 3's actual code
   (`DeviceManager.m`) turned out to call `idevice_new_with_options()`, which doesn't exist
   in tag `1.2.0`, the fix was: extract the original header from git history, then binary-
   search forward through `libimobiledevice`'s upstream commit log (using `git log -S
   '<newly-needed-symbol>'` to find candidate commits, then `diff` the original header
   against each) until finding a **byte-for-byte exact match**. This landed on commit
   `e52ef95` (2020-02-20) — a full four months more precise than the Homebrew-based guess,
   and the one actually used going forward. Apply this method first, before Homebrew/date
   inference, whenever a specific API call is in question.
7. **Not everything is recoverable — `libirecovery`'s upstream history was rewritten.**
   Once `DeviceManager.cpp` was written and needed `checkm8`'s
   `irecv_async_usb_control_transfer_with_cancel()`, the commit that (per current GitHub
   history) *introduces* that function in `synackuk/libirecovery` is dated **2025-11-27** —
   impossible, since `DeviceManager.m` (Sept 2020) already depends on it. The fork's git
   history was evidently squashed/rebased upstream at some point, discarding whatever
   commit actually introduced checkm8 support. There is no way to recover the true original
   commit from the current repo. Resolution: pinned to `e7aadfe`, the *oldest surviving*
   commit in current history that has the needed function — an honest "best available,"
   not a genuine date-accurate pin. Its API surface then cascaded: it needs
   `libimobiledevice-glue` (re-added, current HEAD — also not date-accurate, same reason),
   which itself needs `libplist >= 2.3.0` — well past the historically-pinned `libplist
   2.1.0`, which is genuinely correct for `libusbmuxd`/`libimobiledevice`. Rather than
   compromise the good pin, added a **second** `libplist` submodule
   (`third_party/libplist-modern`, current HEAD) used only to satisfy `glue`'s
   configure-time version check — see `CMakeLists.txt` for the mechanics.

### Final pins

| Submodule | Source | Pinned commit | Evidence |
|---|---|---|---|
| `third_party/libplist` | libimobiledevice/libplist | `3df02d4d...` (tag `2.1.0`) | Homebrew formula history, 2020-06-15 bump |
| `third_party/libusbmuxd` | libimobiledevice/libusbmuxd | `e97cebea...` (tag `2.0.1`) | Exact embedded version string |
| `third_party/libimobiledevice` | **regulad/libimobiledevice** (fork), `legacy` branch | `e52ef95...` (2020-02-20) is still the base commit — pin evidence unchanged (see point 6) | Forked later to add wolfSSL as an additional, `--with-ssl-implementation=wolfssl`-selectable SSL backend for `idevice.c`'s connection-layer TLS (real transport/`usbmuxd_*` calls untouched) — see the "libimobiledevice was forked" bullet further down for the full story. Not a re-pin: the `legacy` branch is that exact commit plus a small, additive patch (4 files, +237/-27). |
| `third_party/libirecovery` | **synackuk/libirecovery** (not upstream!) | `e7aadfe...` | Embedded debug path names this fork explicitly; exact commit is **unrecoverable** — upstream history was rewritten (see point 7). This is the oldest surviving commit with the checkm8 API Blackb0x needs, not a date-accurate pin. |
| `third_party/libimobiledevice-glue` | libimobiledevice/libimobiledevice-glue | current HEAD (**not historically pinned**) | Required transitively by the `libirecovery` pin above; didn't exist as a separate library in 2020 |
| `third_party/libplist-modern` | libimobiledevice/libplist | current HEAD (**not historically pinned**) | Exists ONLY to satisfy `libimobiledevice-glue`'s `libplist >= 2.3.0` configure check — not linked into anything itself |
| `third_party/libfragmentzip` | tihmstar/libfragmentzip | `120447d0...` (v0.60, 2020-01-04) | Exact embedded git-describe string |
| `third_party/libgeneral` | tihmstar/libgeneral | `e499458c...` (2020-01-04, 18 min before the libfragmentzip pin) | Re-added after initially (and wrongly) being judged unnecessary — see below |
| `third_party/xpwn` | **regulad/xpwn** (fork), `legacy` branch | `20c32e5...` (planetbeing/xpwn current HEAD at fork time) | No surviving evidence for the original pin; see rationale in `CMakeLists.txt` — this supplies capability the original app never had (DMG building), not a port of something it used. Forked later (still that same commit, two files' worth of changes) to fix a real wolfSSL AES buffer over-read in `img3.c` and to durably keep the pre-existing `pwnmetheus2`-disable edit — see the "first real end-to-end attempt" bullet further down for both |

`Blackb0x/Libraries/xpwntool.c` (in-tree, not a submodule): confirmed sourced from
`zzanehip/xpwntool-swift @ 58b69c595c` by content diff. No changes needed.

### Submodules added later for SSLv3 support / full static linking (not historically pinned)

None of these existed in the original macOS app — they supply capability it never needed
(a modern static-linking-friendly build, and a TLS library that still supports SSLv3 for
talking to this era of Apple TV lockdownd). Pinned to current HEAD or the latest stable
release tag, same as `xpwn`/`libimobiledevice-glue` above.

| Submodule | Source | Pinned commit | Why not a release tag |
|---|---|---|---|
| `third_party/wolfssl` | wolfSSL/wolfssl | current HEAD | Needs `--enable-sslv3`/`WOLFSSL_STATIC_RSA` build flags either way; no reason to lag |
| `third_party/curl` | curl/curl | current HEAD | Built against wolfSSL, not OpenSSL, specifically for this project |
| `third_party/libusb` | libusb/libusb | current HEAD | Vendored only because no static `libusb-1.0.a` exists anywhere on this system |
| `third_party/libzip` | nih-at/libzip | current HEAD | **Supersedes** the earlier "system package" decision noted above — vendored once static linking became the goal |
| `third_party/libpng` | pnggroup/libpng | `v1.6.58` (release tag) | `master` tracks an unstable `1.8`-dev series (`PNGLIB_ABI_VERSION` → `libpng18.a`, not `libpng16.a`) |
| `third_party/bzip2` | libarchive/bzip2 | current HEAD | Upstream `sourceware.org/bzip2` has no CMake support at all; this is the common CMake-buildable fork |
| `third_party/libfragmentzip` | **regulad/libfragmentzip** (fork) | `fix-cxx-stdbool-header` branch, off `120447d0` | Same commit as the historical pin above, plus one C++-compatibility fix that kept getting lost to `git reset --hard` as an in-tree patch — see the "`third_party/libfragmentzip` is forked" bullet below for the full story |

### Dependencies that were added, then removed, then one re-added

- `libimobiledevice-glue` and `libtatsu` were added early (the *current* upstream
  `libimobiledevice`/`libusbmuxd`/`libirecovery` configure scripts require them) but
  **removed** once pinned to the historical versions above — checked each pinned
  version's own `configure.ac` directly and confirmed neither dependency existed yet at
  that point in history.
- `libgeneral` was removed by the same reasoning (`libfragmentzip`'s pinned `configure.ac`
  has no `PKG_CHECK_MODULES` for it) — but this was **wrong** and had to be reverted:
  `libfragmentzip.c` at that exact commit already `#include <libgeneral/macros.h>` in the
  source even though the build system hadn't caught up yet. Lesson: check the `.c` source
  directly, not just `configure.ac`, before concluding a dependency is unneeded.
- `libzip` was added as a **system** package (`apt libzip-dev` + `pkg_check_modules`) once
  `libfragmentzip`'s pinned `configure.ac` revealed it as a real dependency. This also
  retroactively explains why `libzip.dylib` existed in the original app's bundled binaries
  — a loose end from the initial investigation, now closed.
- `libimobiledevice-glue` and `libtatsu` were removed a second time for the same reason as
  the first, then **`libimobiledevice-glue` had to be re-added** once `libirecovery` got
  re-pinned to `e7aadfe` (see "Version pinning" point 7) — that specific commit genuinely
  needs it. `libtatsu` stayed removed; nothing in the final pin set needs it.

## Build-system fixes needed to compile 2019–2020-era code on a 2025 toolchain

All of these are documented inline in `CMakeLists.txt` next to the relevant
`ExternalProject_Add`/`add_subdirectory` call; this is the summary:

- **CRLF line endings**: every submodule ships its own `.gitattributes` (`* text=auto`),
  which on Windows checkout used `core.eol=native` → CRLF, breaking every `#!/bin/sh`
  shebang. Fixed via **local, repo-scoped** git config on each submodule —
  `core.autocrlf false` + `core.eol lf` — followed by a forced clean re-checkout
  (`git clean -xfdq` then `reset --hard`, or delete-then-checkout for a fresh submodule
  add). This was an explicit user preference: no `.gitattributes` edits, no CMake-side
  normalization logic. **Apply this immediately after adding or re-pinning any submodule,
  before ever running its `autogen.sh`/`configure`.**
- **`getconf LFS_CFLAGS` returns empty** on this modern glibc (64-bit `off_t` is already
  the default), but `libimobiledevice`'s old `common/Makefile.am` unconditionally
  concatenates it into `-D_FILE_OFFSET_BITS=`, which is syntactically invalid and breaks
  `<features.h>`'s own version check. Fixed by overriding `LFS_CFLAGS=` at `make` time.
- **Broken vendored `autogen.sh`** in `libimobiledevice` and `libfragmentzip` at these old
  commits — both fail with `required file './ltmain.sh' not found` on a modern
  autoconf/libtool. Fixed by substituting `autoreconf -fi && ./configure` in their
  `CONFIGURE_COMMAND` instead of calling the vendored script.
- **`libfragmentzip` needs `libgeneral` with no `PKG_CHECK_MODULES` mechanism** to learn
  its location — passed `CPPFLAGS=-I${DEPS_INCLUDE} LDFLAGS=-L${DEPS_LIB}` explicitly.
- **`libfragmentzip.h`'s `bool`/`true`/`false` enum conflicts with `<stdbool.h>`** now
  being pulled in transitively by a newer system `curl.h` (didn't happen in 2016 when the
  header was written). This is the **one vendored-source patch** made this session
  (everywhere else uses compile flags) — verified upstream's own later fix was exactly
  this (`#include <stdbool.h>`, delete the manual enum) and applied the same fix.
- **`SSLv3_method()` was removed entirely in OpenSSL 3.x** (not just deprecated/hidden —
  genuinely gone from the linkable ABI on this distro). Patched
  `third_party/libimobiledevice/src/idevice.c` to call `TLS_method()` instead (the modern
  version-flexible replacement; matches the direction upstream itself eventually took,
  though not the same commit — that fix isn't even in this tag's ancestry, likely due to
  a git history rewrite/import upstream at some point).
- **`libimobiledevice`'s bundled CLI tools** (`idevicebackup2`, `ideviceinfo`, etc., under
  `tools/`) fail to link for reasons unrelated to the library itself and are not used by
  Blackb0x — skipped by overriding `make`'s `SUBDIRS=common src include`.
- **`xpwn`'s `pwnmetheus2` subdirectory** hard-requires the legacy libusb-0.1 API
  (`<usb.h>`) on non-Apple platforms — an unrelated GUI pairing tool. Disabled by
  commenting out its `add_subdirectory` line in `third_party/xpwn/CMakeLists.txt`.
- **`xpwn`'s vendored `minizip`** predates zlib's `z_crc_t` typedef, and **most of `xpwn`
  is circa-2010 C** that trips several diagnostics GCC ≥14 turned from warnings into
  hard errors by default (implicit-int, implicit-function-declaration,
  incompatible-pointer-types, int-conversion, return-mismatch) plus GCC ≥10's
  `-fno-common` default causing "multiple definition" link errors. Fixed with a blanket
  loop over every target `xpwn` defines, relaxing those specific diagnostics — not by
  patching 2010-era vendored source file-by-file.
- **`xpwn` itself declares `cmake_minimum_required(VERSION 2.6)`**, which CMake ≥4 refuses
  outright. Wrapped its `add_subdirectory` call in
  `set(CMAKE_POLICY_VERSION_MINIMUM 3.5)` / `unset(...)` rather than editing the vendored
  `CMakeLists.txt`.
- **Wrong imported-library filenames** were guessed more than once when wiring up
  `add_deps_imported_lib` (e.g. assumed `-2.0`/`-1.0` suffixes that these older library
  versions don't use, or vice versa). Ground truth is always `ls build/deps/lib/*.a`
  after a successful `ExternalProject_Add` install — don't guess, check.
- **`AC_CHECK_MEMBER`/`AC_TRY_COMPILE` probes run before `PKG_CHECK_MODULES` CFLAGS are
  folded into `CPPFLAGS`.** `libimobiledevice`'s `configure.ac` probes whether the
  installed `usbmuxd.h` is "new enough" using a raw compile test that only sees the
  *default* include path — `PKG_CHECK_MODULES(libusbmuxd, ...)` earlier in the same file
  does not itself add `-I` flags to the environment these probes run in. Without an
  explicit `CPPFLAGS=-I${DEPS_INCLUDE}`, the probe can't find our built `usbmuxd.h` at all
  and fails with a **misleading** message ("libusbmuxd is not up-to-date; missing
  conn_type member") that's actually "header not found" — don't take such messages at
  face value; check whether the header is even reachable first.
- **A `.pc` file's own transitive `Requires:` can cross a prefix split.**
  `libimobiledevice-glue-1.0.pc` (built against `libplist-modern`) declares `Requires:
  libplist-2.0`. Any *other* autotools project that does `pkg-config
  libimobiledevice-glue-1.0` (i.e. `libirecovery`) needs `libplist-2.0.pc` on its OWN
  `PKG_CONFIG_PATH` too, even though it never uses libplist directly — pkg-config resolves
  `Requires:` chains transitively. Whenever a dependency spans the `deps`/`deps-modern`
  prefix split, every consumer up the chain needs both prefixes on its `PKG_CONFIG_PATH`.
- **`extern "C"` linkage must match between a `friend` declaration and its definition.**
  When `DeviceManager.hpp` declared `friend void some_c_callback(...)` with implicit C++
  linkage, but `DeviceManager.cpp` defined the same function as `extern "C"`, GCC treats
  them as conflicting declarations of different functions — the friendship silently didn't
  apply, and calls to private members from that "friend" failed with "is private within
  this context". Fix: declare the function's `extern "C"` prototype once (e.g. right after
  the `extern "C" { #include ... }` block), then have the `friend` declaration reference
  that exact (already C-linkage) name.
- **Newly-linked static libraries can need transitively-implied system libraries you never
  needed before.** Adding `DeviceManager.cpp` (which pulls in `libimobiledevice.a`, which
  uses OpenSSL's `SSL_*` functions internally for device connection encryption) needed
  `OpenSSL::SSL` added to `blackb0x`'s link line — `OpenSSL::Crypto` alone (all that was
  needed for the stub) wasn't enough. `undefined reference to 'SSL_*'` at final link,
  not at compile time, is the signature of this class of problem.
- **Stale `ExternalProject_Add` stamp files**: these live under
  `build/<name>-prefix/src/<name>-stamp/`, separate from the submodule source directory.
  Cleaning a submodule's source tree (`git clean -xfdq`) does **not** invalidate a stale
  "configure already succeeded" stamp from a prior failed attempt, causing `make` to run
  against a source tree with no generated `Makefile`. **Rule: do a full `rm -rf build`
  any time you change a submodule's pinned commit, patch a vendored file, or edit an
  `ExternalProject_Add`'s configure/build commands — don't rely on incremental stamps.**

## Phase 5 — `Patcher.mm` → `.hpp`/`.cpp` (done, but with one unresolved bug)

`patchiBSS`/`patchiBEC`/`patchKernel` ported near-verbatim — thin wrappers around the
already-portable `decrypt()`/`iBootPatcher()`/`patch_kernel()`, unchanged behavior.
`patchRamdisk` (the one method built around `hdiutil`/`tar` shell-outs) was substantially
rewritten; see `Patcher.hpp`'s header comment for the pipeline summary. Notes beyond what's
in that comment:

- **`blackb0x_xpwntool` naming.** The new static library wrapping `xpwntool.c` had to be
  named `blackb0x_xpwntool`, not `xpwntool` — `third_party/xpwn`'s own `CMakeLists.txt`
  already defines an `add_executable(xpwntool xpwntool.c)` target, and CMake target names
  are global.
- **`CBPatcher.c`/`libiboot32patcher.c` had no implementations in-tree.** Same pattern as
  the DMG/HFS+ code in Phase 0: only headers were checked in (`CBPatch.h`, `finders.h`,
  `functions.h`, `patchers.h`, etc.) because the original app linked prebuilt `.a` files.
  This only surfaced once `Patcher.cpp` became the first code to actually call
  `iBootPatcher()`/`patch_kernel()`. Recovered the missing sources from **zzanehip's**
  forks — `zzanehip/CBPatcher` (`CBPatch.c`, `CBPUtils.c`) and `zzanehip/iBoot32Patcher`
  (`finders.c`, `functions.c`, `patchers.c`) — confirmed by near-exact header match against
  the headers already in-tree before pulling in anything, same provenance-first method used
  everywhere else in this port. The iBoot32Patcher sources needed a mechanical
  `sed -i 's|<include/|<libiboot32patcher/|g'` to match Blackb0x's own include-path
  convention; no other changes.
- **`CBPatch.c`'s Mach-O headers don't exist on Linux**, but the actual usage is pure
  static binary analysis of a downloaded ARMv7 kernelcache buffer (confirmed via grep: no
  `dlopen`/`mmap` calls anywhere in the file, despite including `<dlfcn.h>`/`<sys/mman.h>`).
  Since Mach-O is a stable, publicly documented file format (not a private macOS runtime
  API), the fix was a new header — `Blackb0x/Libraries/libcbpatcher/portable_macho.h` —
  defining just the classic 32-bit structs actually used (`mach_header`, `load_command`,
  `segment_command`, `section`, `symtab_command`, `nlist`), included via
  `#ifdef __APPLE__ ... #else #include "portable_macho.h" #endif` around the original
  `<mach-o/*.h>` includes. `mac_policy.h` additionally needed an explicit `#include
  <stdint.h>` it had previously gotten transitively from a removed Apple header.
- **UDIF-vs-raw-HFS+ format auto-detection** (added only after real-world testing, not
  planned upfront). The port plan and `third_party/xpwn`'s own reference tool
  (`ipsw-patch/main.c`) both assume every image `decrypt()` produces is UDIF-wrapped
  (trailing "koly" magic). Downloading and decrypting a real AppleTV2,1 build 11D258
  `RestoreRamdisk.dmg` and inspecting it with `xxd` showed it decrypts straight to a **raw**
  HFS+ partition — "H+" signature at offset `0x400`, no "koly" trailer anywhere in the file.
  Fixed by probing the last 512 bytes of the decrypted file for the "koly" magic at runtime
  and branching: only genuinely UDIF-wrapped images go through xpwn's `extractDmg()` to
  unwrap to a raw HFS+ image first; the common (older-hardware) case is read directly as
  raw bytes. The final write-back mirrors this: `buildDmg()` only if the input was UDIF,
  otherwise a plain raw write — matching whatever `decrypt()` actually produced.
- **Deliberately unimplemented: `AppleTV2,1_4.x` ramdisk recreation.** The original's
  `"AppleTV2,1_4."`-prefixed branch doesn't patch an existing ramdisk at all — it rebuilds
  one from scratch via `hdiutil create ... -format UDRW`, for the oldest ATV2 4.x-era
  firmware. This port explicitly does **not** implement that branch — `patchRamdisk` prints
  an error to stderr and returns `false` for that path prefix rather than silently doing
  the wrong thing. Building a fresh empty HFS+ volume from nothing is a materially
  different problem from growing/injecting into an existing one and was out of scope for
  this pass.
- **In-memory HFS+ manipulation, no mount/loopback anywhere — first design, later abandoned.**
  The raw (or UDIF-unwrapped) bytes were loaded into a heap buffer, wrapped as an
  `AbstractFile`/`io_func` via xpwn's `createAbstractFileFromMemoryFile()`, then opened
  directly as a `Volume*` via `openVolume()`. All injection used xpwn's own `add_hfs()`/
  `grow_hfs()`/`hfs_untar()` primitives against that in-memory volume. This matched the
  original port plan's explicit intent to avoid any OS-level mount — see "RESOLVED" below
  for why it was abandoned anyway, in favor of a real loop mount via the Linux kernel's own
  `hfsplus` driver.
- **`hfs_untar()` has no destination-prefix concept** — it always extracts each tar
  member to the literal path baked into the tar, with no way to redirect e.g. `ATV-Cydia.tgz`'s
  contents under `/files/cydia/`. Wrote a small wrapper, `hfsUntarWithPrefix()` (mirrors
  `hfs_untar()`'s own tar-parsing loop but prepends a caller-supplied prefix to every member
  path before calling `add_hfs()`/`newFolder()`), plus `ensureHfsDirExists()` (a `mkdir -p`
  equivalent: walks `getRecordFromPath3()` up the path components, calling `newFolder()` for
  any that don't already exist) used by both the wrapper and by `addLocalFileToVolume()`.
  **(Superseded — these helpers no longer exist; see "RESOLVED" below. Kept here for the
  historical record, since the compressed-file fix immediately below was real, hard-won
  debugging work independent of which design ultimately won.)**
- **The compressed-file crash and its two-attempt fix.** Real functional testing (not just
  compiling) against the downloaded 11D258 ramdisk hit a SIGSEGV partway through
  `hfsUntarWithPrefix()`. `gdb -batch -ex run -ex bt` (via WSL) showed the crash was in
  `free()` ← `closeHFSPlusCompressed()` ← `writeToHFSFile()` ← `add_hfs()` — xpwn's own code
  crashing while overwriting a file that already has the `UF_COMPRESSED` flag set (HFS+
  transparent decmpfs compression) from a prior extraction pass over the same path.
  - **First attempt (wrong): `clearHfsCompressionFlagIfSet()`.** Looked up the existing
    record via `getRecordFromPath3()`, cleared `UF_COMPRESSED` in
    `fileRecord->permissions.ownerFlags`, called `updateCatalog()` to persist the cleared
    flag, then let `add_hfs()` proceed to overwrite. This avoided the segfault, but running
    the same real-data test further revealed a **new** failure: `hfs_panic("BTree
    inconsistent!")`. Directly mutating a catalog record's flags out from under the B-tree
    without going through its normal record-removal path left something inconsistent.
  - **Second attempt (correct): `removeExistingFileIfPresent()`.** Instead of trying to
    salvage the existing record, look it up and — if it's a file record — call xpwn's own
    `removeFile()` on it before `add_hfs()` runs, exactly matching the "replacing %s" branch
    already present in xpwn's own `hfs_untar()` for this exact situation (confirmed by
    reading that function's source first). This is the precedent-following fix, not a novel
    one. Called from both `addLocalFileToVolume()` and `hfsUntarWithPrefix()`'s regular-file
    branch. **Confirmed working**: re-ran the real-data test and `ssh.tar`, `RamdiskBins.tar`,
    and the *entire* `ATV-Cydia.tgz` `bin/` directory (many compressed executables) all
    extracted without crashing.
- **RESOLVED: the "BTree inconsistent!" catalog-growth bug, and everything that followed
  from chasing it.** After the compressed-file fix above, the identical
  `hfs_panic("BTree inconsistent!")` error still occurred later in the same real-data test,
  partway through `Debs.tar` extraction into `/files/`. A second `gdb -batch -ex run -ex
  'break hfs_panic' -ex bt` backtrace showed this was unrelated to compression — it's inside
  xpwn's own catalog B-tree **growth** path: `growBTree` → `getNewNode` → `splitNode` →
  `addRecord` (recursing ~3 levels) → `addToBTree` → `newFile` → `add_hfs`, ultimately
  failing inside `allocate` → `writeExtents` → `removeExtents` → `search` → `searchNode` →
  `hfs_panic`, triggered when `lastRecordDataOffset == 0`. Reading `grow_hfs()`'s full
  implementation confirmed the root cause: it only resizes the **overall volume bitmap**,
  not the **catalog file's own B-tree extents** — so once enough new catalog entries have
  been injected that the catalog B-tree needs to grow past its originally-allocated extent,
  that organic-growth code path (not exercised by xpwn's own more modest reference use
  cases) has a genuine, pre-existing bug in this ~2010-era from-scratch HFS+ writer. Not a
  bug introduced by the Blackb0x port — a real limitation of `third_party/xpwn` itself, hit
  only because Blackb0x's ramdisk payload (six tarballs plus a dozen loose files) pushes the
  catalog B-tree further than xpwn's own tools ever have.

  This was reported to the user as a genuine open question rather than guessed at further,
  which led to a second research pass — evaluating alternative Linux-side HFS+ *write*
  implementations rather than trying to fix xpwn's B-tree code blind:
  - **libhfsp (Debian's `hfsplus`/`libhfsp-dev` package) — ruled out, worse than xpwn.**
    Direct testing (`hpmount`/`hpmkdir`/`hpcopy`, the package's own CLI tools) found a single
    `hpmkdir()` call — no file copy at all — on a freshly-`mkfs.hfsplus`'d volume already
    corrupted an extent entry into the reserved alternate-volume-header block, confirmed via
    `fsck.hfsplus` and by checking that the same volume was clean immediately after
    `mkfs.hfsplus` and before any libhfsp write. Root-caused directly in libhfsp's own
    source (`libhfsp/src/volume.c`, cloned from `deepin-community/hfsplus` for inspection):
    all three block-allocation functions (`volume_allocated`/`volume_allocate`/
    `volume_deallocate`) contain a bounds check against the volume's reserved region that
    exists in the source **only as commented-out dead code**. Corroborated by the project's
    own `ChangeLog`, which calls the 1.0.1 release (2000) "a stable, readonly implementation"
    and never claims the write path reached that bar through the final 1.0.4 release (2002).
  - **The Linux kernel's own, in-tree, actively-maintained `hfsplus` driver — this is what's
    actually used now.** Requires a real loop mount, which the original port plan explicitly
    wanted to avoid — but real functional testing (loop-mounting a blank `mkfs.hfsplus`
    volume and copying the *entire* real payload set onto it, including `Debs.tar`, via
    ordinary `tar`/`cp`) confirmed it handles exactly the catalog growth that broke both
    designs above, with a clean `fsck.hfsplus` verdict and a byte-for-byte content match
    against the source tarballs. (Testing this required a real Fedora/Bluefin Linux host —
    WSL2's own kernel ships with `CONFIG_HFSPLUS_FS` disabled and building an out-of-tree
    module for it turned out to not be a genuinely supported WSL2 configuration either; see
    "Environment notes" below.)
  - **Resizing an *existing* volume in place — investigated and confirmed unsupported on
    Linux, not just "no tool found."** `patchRamdisk()` needs to GROW an existing,
    already-populated Apple-built HFS+ image, not just write into a volume already sized
    correctly by `mkfs.hfsplus`. There is no `resize2fs`-equivalent CLI tool for HFS+ on
    Linux, but there IS a real library that implements HFS+ resize —
    `libparted-fs-resize` (what GParted's HFS+ support uses under the hood; split out of
    libparted core in parted-3.0 into this separate add-on library specifically because
    HFS+/FAT resize had no free alternative at the time). Built a throwaway C program
    directly against it (`ped_file_system_open`/`get_resize_constraint`/`resize`, via a
    Fedora toolbox container for the `parted-devel` headers, keeping the host's immutable
    base image untouched) and ran it against a real, populated HFS+ volume: the library
    reported its own resize constraint capping `max_size` at the volume's *current* size,
    and calling `ped_file_system_resize()` to grow past that returned, verbatim, **"No
    Implementation: Sorry, HFS+ cannot be resized that way yet."** Growing (as opposed to
    shrinking) HFS+ is an explicit, acknowledged non-implementation in the one library that
    does implement HFS+ resize on Linux — not an untested edge case, and not something worth
    waiting on upstream for.

  **The actual fix**, implemented in `Patcher.cpp`'s `patchRamdisk()`: don't grow the
  original volume at all. `mkfs.hfsplus` a brand-new volume at the final target size (same
  40MB/60MB sizing as before), copy the original volume's entire tree across via a real loop
  mount + `cp -a` (preserving permissions/ownership/setuid bits/symlinks — this needs
  `CAP_CHOWN`, so `patchRamdisk()` now requires the calling process to run as root, e.g.
  `sudo blackb0x ...`), copy a deliberately narrow set of the original's volume-header
  identity fields across too (`finderInfo`, `createDate`, `lastMountedVersion` — patched at
  both the primary and backup header offsets via plain fixed-size byte copies, no B-tree
  interaction at all; `attributes` and every size/count field are deliberately *not* copied,
  since blindly copying them risks importing stale state that doesn't match the
  freshly-built volume's actual, correct state), and only then inject the new payloads via
  ordinary `tar -x`/file copies into the *new*, already-correctly-sized volume. All of the
  bespoke xpwn-Volume helpers described above (`ensureHfsDirExists`, `parentOf`,
  `removeExistingFileIfPresent`, `addLocalFileToVolume`, `hfsUntarWithPrefix`,
  `untarFileIntoVolume`, `gunzipFile`, `untarGzipFileIntoVolume`) are gone, replaced by a
  handful of `fork`/`execvp` subprocess helpers (`runCommand`, `runCommandCapture`,
  `makeTempDir`, a `MountGuard` RAII type, `copyLocalFileIntoMount`, `readVolumeLabel`,
  `copyVolumeHeaderMetadata`) invoking `mount`/`umount`/`mkfs.hfsplus`/`blkid`/`cp`/`tar`
  directly (argv arrays, no shell). **Verified end-to-end against the same real, live-
  downloaded AppleTV2,1 11D258 `RestoreRamdisk.dmg` used throughout this investigation**:
  `patchRamdisk(ssh=false)` returns `true`, the resulting image passes `fsck.hfsplus` clean
  ("The volume ramdisk appears to be OK"), `file(1)` confirms the original's real 2014
  creation date survived the metadata copy, and mounting the result confirms all 8 spot-
  checked injected paths (`/ssh.tar`, `/sbin/launchd`, `/files/setup.sh`, `/files/cydia`,
  `/files/p0sixspwn`, `/files/etasonATV`, `/files/rtbuddyd.bin`, `/files/kodi.png`) are
  present, with 916 files under `/files` total. Phase 5 is done.
- **Real-data test methodology** (not part of the permanent build — written, run, and
  deleted each time, per this session's convention of leaving no scratch files behind):
  download a real AppleTV2,1 11D258 `RestoreRamdisk.dmg` + its firmware keys via the
  already-verified Phase 4 `IpswFetch`/`FragmentDownloader`, run it through
  `Patcher::patchRamdisk()`, and inspect/gdb the output. This is the only way Phase 5's
  HFS+ code has ever been exercised — there is no offline synthetic-fixture test for it —
  but by the final (kernel-mount) design it's a genuine pass/fail signal: `fsck.hfsplus` on
  the output plus a mount-and-spot-check of injected paths, not just "didn't crash."

## Environment notes

- **The dev environment moved partway through Phase 5, from Windows+WSL2 to a real Linux
  box (Fedora Silverblue/Bluefin, immutable/rpm-ostree-based, hostname `longitude`).** This
  wasn't incidental — WSL2's own kernel ships with `CONFIG_HFSPLUS_FS` disabled entirely
  (`# CONFIG_HFSPLUS_FS is not set` in `/proc/config.gz`), and building it as an
  out-of-tree module for WSL2 turned out to not be a clean, genuinely-supported path either:
  Microsoft's own `.wslconfig` `kernelModules` mechanism exists and is documented, but it's
  designed around pairing a full custom-built replacement kernel with a matching modules
  VHDX, not bolting one module onto the stock inbox kernel, and Microsoft's own support
  guidance calls building/loading kernel modules this way "not officially supported."
  Testing the kernel-mount approach for `patchRamdisk()` (see above) needed a real kernel
  with `hfsplus` support, which a real Fedora box has out of the box — this single
  constraint is what actually decided Phase 5's final design between the sessions.
- **This machine is accessed over SSH with no GUI available**, which shaped how privileged
  (root-requiring) test commands got authenticated:
  - `sudo` normally needs a TTY for the password prompt, which a non-interactive tool-call
    shell doesn't have — `sudo -v`/`sudo -S` from an agent's own shell will not work here.
  - `pkexec` (polkit's graphical auth agent) was tried next, since the D-Bus session bus
    address was reachable even without `DISPLAY`/`WAYLAND_DISPLAY` set in the calling
    shell — but this pops a GUI dialog on the machine's own physical screen, useless to a
    user who's only ever connected over SSH. Confirm what the user is actually looking at
    before assuming a GUI auth prompt is reachable.
  - **What actually worked**: a scoped `/etc/sudoers.d/` rule the *user* adds themselves via
    `sudo visudo -f /etc/sudoers.d/<name>` (never generated/written by the agent — sudoers
    edits are exactly the kind of hard-to-reverse, security-relevant action that needs the
    user's own hands), granting `NOPASSWD` for only the specific binaries/argument patterns
    a task needs (e.g. `/usr/bin/mount -t hfsplus *`, `/usr/bin/mkfs.hfsplus *`, `/usr/bin/
    umount /tmp/*`) — never a blanket `ALL=(ALL) NOPASSWD: ALL`. Confirmed scoping actually
    works by checking that `sudo -n <allowed-command>` succeeds while `sudo -n true`
    (something NOT in the rule) still demands a password. For anything not covered by such a
    rule (e.g. running an arbitrary one-off test binary that itself needs to run *as root*,
    rather than just needing to shell out to a couple of allowed commands), the simplest
    honest path is just asking the user to run that one command themselves via Claude Code's
    `!` prefix, rather than trying to route around a real authentication boundary.
- **Podman/toolbox on this host needed the system binary, not linuxbrew's.** `podman` was
  resolved from `/home/linuxbrew/.linuxbrew/bin/podman` by default (earlier in `$PATH`),
  which isn't configured for this host's rootless-container setup and fails with `configure
  storage: mkdir /run/containers: permission denied`. Fixed by explicitly using
  `/usr/bin/podman`/`/usr/bin/toolbox` (`PATH=/usr/bin:$PATH ...`, or a small shim directory
  with a symlink prepended to `PATH`, since `toolbox` itself shells out to a bare `podman`
  it resolves via its own inherited `$PATH`). A toolbox container's `/tmp` and `$HOME` are
  both bind-mounted from the host by default — confirmed by writing a marker file from one
  side and reading it from the other — which made it possible to build a throwaway C test
  program inside a toolbox (for `parted-devel` headers, without layering anything onto the
  host's immutable base image via `rpm-ostree`) while operating on image files created by
  the host's own already-working `mkfs.hfsplus`/`mount`.
- Before that move: dev machine was Windows; there was no local Linux toolchain outside
  WSL. Build/test
  everything through the `regulad-ubuntu` WSL distro (`wsl -d regulad-ubuntu -- bash -lc
  "..."`), which has cmake, gcc/g++, git, pkg-config, autoconf, automake, and linuxbrew's
  libtool/m4 already installed. The repo is reachable from WSL at
  `/mnt/d/repositories/Blackb0x`.
- Extra system packages installed in that WSL distro for this build:
  `libusb-1.0-0-dev`, `libzip-dev` (both via `apt-get install`).
- When a `git checkout <commit>` on a submodule aborts because a locally-modified tracked
  file (e.g. a source patch you made) would be overwritten, do **not** work around it with
  a `find ... -exec rm -rf` + re-checkout combo run unconditionally after the failed
  command — if the checkout aborted, HEAD didn't move, and the follow-up commands still
  execute (no `set -e`), leaving a confusing mixed-content working tree that matches
  neither commit. `git -C <submodule> reset --hard HEAD` first, *then* checkout cleanly.
- **PowerShell → `wsl.exe` argument passing is unreliable for anything with shell
  variables or complex quoting** — `$f` in a `for` loop got silently dropped/mangled more
  than once this session even inside single-quoted PowerShell strings. When a WSL
  one-liner needs real shell logic (loops, variable expansion), write it to a `.sh` file
  in the repo first and invoke `wsl -d regulad-ubuntu -- bash /mnt/d/repositories/Blackb0x/somefile.sh`
  instead of trying to inline it through PowerShell.
- `wsl -l -v`'s own output comes through mangled (UTF-16LE double-decoding, one space
  between every letter) when captured — read past the garbling rather than assuming the
  command failed.
- The build's true exit code is easy to lose: piping through `tail`/`tee` without
  `pipefail` reports the pipeline's last command's exit status, not the actual build's.
  Always `grep` the log for `Error 1\|Error 2\|error:\|configure: error` rather than
  trusting a reported exit code.
- **Known machine where a real end-to-end jailbreak attempt did NOT succeed**: this same
  dev machine (hostname `longitude`, see above) — Dell Latitude 7420 (2022), 11th Gen
  Core/Tiger Lake-LP, Iris Xe, two xHCI USB host controllers (one routed through the
  Thunderbolt 4 controller, one a plain USB 3.2 Gen 2x1 controller, **no legacy EHCI
  controller at all**), Bluefin 44, kernel `7.1.8-200.fc44.x86_64`. Full hardware probe:
  <https://linux-hardware.org/?probe=bd45f480d3>. Tried repeatedly against a real
  AppleTV3,2 in DFU mode. **Power-cycling/rebooting alone does not fix it** — this is the
  expected result, not a mystery: see the long "First real end-to-end attempts" bullet in
  the libimobiledevice-fork section above for the full investigation, but the short
  version is that six real software bugs surfaced and were fixed along the way (DFU
  remote instructions, curl/wolfSSL CA chain validation, the kernelcache version-string
  and patch-failure heap corruption, a wolfSSL AES-CBC buffer over-read, and a
  Linux/libusb vs. macOS/IOKit error-code translation gap in the hand-ported checkm8
  code) — and after all of them were fixed and the exploit backend was even swapped
  entirely for `gaster` (the same implementation `palera1n` itself uses, confirmed to
  support this exact chip/firmware), the exploit payload itself was confirmed to
  genuinely execute (`Stage: PATCH` / `ret: true`) on this machine — but the
  device-initiated USB reset that checkm8 triggers afterward unreliably hangs (a real
  uninterruptible-sleep kernel wait, confirmed via `ps`/`/proc/<pid>/status`) or fails to
  re-enumerate, at inconsistent points in the sequence, on two independent
  implementations. That, combined with this machine's Thunderbolt-only/no-EHCI USB
  topology, is the actual, current, well-evidenced conclusion: **a host-controller-level
  USB limitation for checkm8-class device-initiated resets, not a software bug in this
  project** — rebooting the host was never going to fix that.
  **The USB-hub mitigation was tried too, twice, and also did not help**:
  <https://linux-hardware.org/?probe=101c2e8278> (Microchip USB2807/USB5807 hub
  chipsets — actually a Dell docking station, given the RTL8153 Ethernet/webcam/audio
  devices riding alongside) and <https://linux-hardware.org/?probe=44f0d4852a>
  (Terminus Technology `1a40:0101` + VIA Labs `2109:2817` — genuine plain USB hub
  chipsets, the closer match to the "route through a dumb hub" workaround this was
  meant to test). Neither got any further than connecting directly. With the direct,
  Thunderbolt-routed, and two structurally different hub-routed paths all hitting the
  identical device-reset/reconnect failure, a topology-specific quirk (e.g. "needs a
  hub in the path") is no longer a plausible explanation — this reads as this specific
  machine's USB stack (kernel/xHCI driver/controller firmware) genuinely not handling
  checkm8's device-initiated reset reliably, full stop. **Next step is different
  hardware, not more mitigations on this one** — ideally something with a legacy EHCI
  controller (older/cheaper laptops, many desktop motherboards) or at minimum a
  non-Thunderbolt-routed xHCI implementation from a different vendor generation, since
  every avenue on this specific Tiger Lake-LP/Thunderbolt setup has now been tried.

## Current state / how to build

```
cmake -S . -B build && cmake --build build -j$(nproc)
./build/blackb0x
```

(The commands above assume a normal Linux dev box. If still working from the old
Windows+WSL2 setup for some reason, prefix each with
`wsl -d regulad-ubuntu -- bash -lc "cd /mnt/d/repositories/Blackb0x && ...`", but see
"Environment notes" above for why that setup was abandoned partway through Phase 5.)

This builds and runs the **real CLI** now — `blackb0x --help` prints usage and exits 0;
run without root it correctly refuses with a clear error; run as root (`sudo ./build/
blackb0x`) it prints the startup banner and begins waiting for a device. The entire pinned
dependency stack (libplist ×2, libusbmuxd, libimobiledevice, libimobiledevice-glue,
libirecovery, libgeneral, libfragmentzip, xpwn, cbpatcher, iboot32patcher) **and**
`DeviceManager.cpp`/`IPSW.cpp`/`IPSWDownloader.cpp`/`Patcher.cpp`/`Cli.cpp` (Phases 3–6,
all done) compile cleanly and link successfully. Phases 4 and 5 were both functionally
verified end-to-end with throwaway test programs (built, run, and deleted each time —
never part of the permanent build, per this session's convention): Phase 4 against a live
`api.ipsw.me` query, a real `.keys` file parse, and a full round-trip download + plist-parse
of a real `BuildManifest.plist` from a live IPSW on Apple's CDN; Phase 5 (and its SSH-key
security fix) against multiple full `patchRamdisk()` runs on a live-downloaded AppleTV2,1
11D258 `RestoreRamdisk.dmg`, verified via a clean `fsck.hfsplus` and a mount-and-spot-check
of the injected payload and SSH configuration (see the "RESOLVED" writeup and the Phase 6
entry below for exact detail). This is real, verified functionality, not aspirational.
Phase 6's device-selection/DFU-wait/exploit/upload flow itself is **not yet
hardware-verified** — see that phase's entry below. **`blackb0x` runs entirely as root**
(decided during Phase 6 — see that entry for why) — always invoke it via `sudo`.

**New runtime (not just build-time) dependency from Phase 5's final design**: the target
Linux system needs `mkfs.hfsplus`/`fsck.hfsplus` (the `hfsprogs` package on Fedora/Debian/
Ubuntu — a port of Apple's own `diskdev_cmds`) and a kernel with `hfsplus` support
(`CONFIG_HFSPLUS_FS`, either built-in or as a loadable module — standard on virtually every
mainstream distro kernel; see "Environment notes" for the one dev-environment exception,
WSL2, that doesn't have it). `mount`/`umount`/`blkid`/`cp`/`tar` are assumed present on any
Linux system as a matter of course and aren't called out as new dependencies.

## Remaining phases (see original plan for full detail; ordering matters)

1. ~~**Phase 3 — `DeviceManager.m` → `.hpp`/`.cpp`**~~ **Done.** The checkm8/SHAtter
   exploit byte-level USB control transfer logic was ported **verbatim** (same sequence,
   buffer sizes, sleep durations — exploit-critical hardware-timing code, not "improved"
   during conversion). `AppleTVIcon`'s Cocoa rendering became a plain `AppleTVDevice`
   struct; `dispatch_async`/`MainView` mutation became `DeviceEventSink` callbacks (direct,
   synchronous calls — there's no GUI event loop to marshal onto); `NSDictionary`-based
   plist handling became direct libplist C API calls extracting just the 5 fields actually
   used; the device list became a `std::deque<AppleTVDevice>` (deque specifically, not
   vector — push_back must not invalidate pointers a background jailbreak-check thread may
   be holding). `arrangeIcons()` (pure icon-grid layout) and the original `newDevice()`'s
   "is this the currently-GUI-selected device, should we auto-continue" logic were dropped
   entirely — the latter is a CLI-front-end decision to make in Phase 6, not
   `DeviceManager`'s job. **Not yet wired up** — `main.cpp` doesn't construct a
   `DeviceManager` or call anything in it; that's Phase 6.
2. ~~**Phase 4 — `IPSW`/`IPSWDownloader` → `.hpp`/`.cpp`**~~ **Done.** `IpswFetch` replaces
   `IPSW_Fetch`: `NSURLSession` → a small `httpGet()` via libcurl (the IPSW.me API
   response is plain text, not JSON, confirmed by how the original consumed it — no JSON
   parser needed); `NSDictionary`-based `.keys` parsing → direct libplist dict/array
   iteration into `std::map<std::string, FirmwareKeyPair>`. `FragmentDownloader` replaces
   the original's fixed 5-slot design (`dlone`..`dlfive`, which only existed to drive 5
   parallel `NSProgressIndicator` widgets) with one instance per in-flight download and a
   `std::function<void(unsigned int)>` progress callback — since
   `fragmentzip_download_file`'s callback is a bare C function pointer with no user-data
   parameter, routing to the callback uses a `thread_local` trampoline (safe because the
   call is synchronous/blocking on the calling thread); this scales to any number of
   concurrent downloads (each on its own thread) rather than being capped at 5. The
   original's hardcoded `/Users/<name>/Documents/Blackb0x` save path became
   `ipswDataRoot()` (`$XDG_DATA_HOME/blackb0x` or `~/.local/share/blackb0x`). Verified
   functionally, not just compiled — see "Current state" above.
3. ~~**Phase 5 — `Patcher.mm` → `.hpp`/`.cpp`**~~ **Done.** `patchiBSS`/`patchiBEC`/
   `patchKernel` ported verbatim. `patchRamdisk` replaces the `hdiutil`/`tar` shell-outs
   with: `mkfs.hfsplus` a new right-sized volume, loop-mount it via the real Linux kernel
   `hfsplus` driver alongside a read-only mount of the original, `cp -a` the original's
   entire tree across (plus a narrow, deliberate copy of its volume-header identity fields),
   then inject every payload with ordinary `tar -x`/file copies. Getting here took two
   earlier designs that both hit real, reproducible data-corrupting bugs under real firmware
   payloads (an in-memory `xpwn` `Volume` writer, then `libhfsp`), plus a confirmed dead end
   trying to resize the *existing* volume in place instead of building a right-sized
   replacement (`libparted-fs-resize` explicitly doesn't implement growing HFS+, only
   shrinking) — see the "RESOLVED" writeup above for the full evidence trail. **Verified
   end-to-end** against a real, live-downloaded AppleTV2,1 11D258 `RestoreRamdisk.dmg`:
   `patchRamdisk(ssh=false)` returns `true`, output passes `fsck.hfsplus` clean, and every
   spot-checked injected path is present after mounting the result. Needs the calling
   process to run as root (see "Current state" above). The `AppleTV2,1_4.x` from-scratch
   ramdisk-rebuild branch is deliberately unimplemented (a materially different problem —
   building an empty volume with no pre-existing content to base it on — and out of scope
   for this pass), not forgotten.
4. ~~**Phase 6 — CLI framework**~~ **Done.** `Cli.hpp`/`Cli.cpp` replace
   `AppDelegate`/`MainView`/`Blackb0x.h/.m`/`main.m` (the Objective-C originals are still
   in-tree for reference, per the repo-layout note above) — a single linear session:
   wait for + select a device (`--ecid`/`--udid` flags or an interactive numbered menu),
   wait for DFU mode if needed (an actual blocking poll loop, replacing `spawnDFUHelper`'s
   popup — a CLI has no button to re-click once the user notices it), run the
   model-appropriate exploit (`checkExploit`'s per-model branching ported verbatim,
   including the literal "plug in an Arduino" message for `AppleTV3,1`), download+patch
   firmware components for the right build (`10B329a` hardcoded for a fresh jailbreak
   install — verbatim from the original, a real firmware-compatibility constraint, not a
   placeholder), and send them to the device (`componentsReady:`'s jailbroken-vs-fresh
   `iBECDowngrade`/`iBECBoot` branching, also verbatim). `TaskManager.h/.m` is dropped
   entirely — it only ever drove `NSProgressIndicator` widgets, no logic of its own.
   Downloads are sequential, not parallel like the original's fire-and-forget
   `dispatch_async` — a deliberate simplification, see `Cli.cpp`'s header comment.
   **Decided**: `blackb0x` just runs as root, full stop — no scoped sudoers/polkit rule, no
   udev-group juggling for non-root USB access either, since the DFU/USB layer needs raw
   device access anyway and the user explicitly didn't want to maintain two separate
   privilege-elevation stories (one for USB, one for mount/mkfs) when "run the whole thing
   as root" already covers both. `patchRamdisk()` itself deliberately doesn't embed `sudo`
   calls (see its doc comment) precisely so this choice stays open for later reconsideration.
   **Not yet hardware-tested** — compiles and runs its non-hardware-dependent paths
   (`--help`, the root check, option parsing) cleanly; the actual device-selection/DFU-wait/
   exploit/upload flow needs a physical Apple TV 2/3, the primary hardware-gated checkpoint
   for the whole port (see below). One real device was available briefly during this phase,
   but only in Normal mode, and its lockdown pairing failed — root-caused to the *system's*
   `usbmuxd` (not this port's code: `go-ios`, an independent from-scratch reimplementation,
   failed identically) refusing the device's ancient SSL/TLS handshake during its own
   preflight step (`journalctl -u usbmuxd`: `lockdown error -5`, i.e. `LOCKDOWN_E_SSL_ERROR`)
   — a real environment incompatibility between 2010s-era embedded lockdownd and a modern
   OpenSSL 3.x-linked usbmuxd, not something to chase further here since it doesn't block
   DFU-mode operation at all (`checkm8`/`SHAtter`/every `send*` call talk raw USB via
   `libirecovery`, no lockdownd/SSL/usbmuxd involved).
   - **Security fix made during this phase**: `Blackb0x/Files/ssh.tar` — a static, checked-
     into-this-public-repo resource — shipped fixed SSH host keys (RSA, DSA, and the legacy
     SSH-1 format) identical on every device anyone has ever jailbroken with this tool, which
     defeats SSH host-key verification entirely (anyone with a copy of this repo can already
     impersonate any Blackb0x-jailbroken device). Fixed two ways: (1) the host keys were
     surgically removed from `ssh.tar` itself via `tar --delete` (confirmed byte-identical
     otherwise — no metadata drift on the remaining members), and `patchRamdisk()` now
     generates a fresh RSA host key per patching run via the host's own `ssh-keygen` (RSA
     only — this old sshd predates ed25519, and modern `ssh-keygen` can no longer generate
     the SSH-1/DSA formats it would otherwise also need); (2) `patchRamdisk()` requires (hard
     error if missing, not a soft warning) the invoking user's own `~/.ssh/authorized_keys` —
     resolved via `$SUDO_USER` since the tool runs as root, so plain `$HOME` would otherwise
     resolve to root's home — and copies it onto the device's root account, so the device is
     reachable with keys the user already controls rather than any shared default. This tool
     deliberately never generates an SSH *client* keypair on the user's behalf — if they don't
     have one, the error message tells them to run `ssh-keygen` themselves and add the result
     to `~/.ssh/authorized_keys`, two distinct steps. Verified end-to-end against real
     downloaded firmware: legacy/DSA host key files absent from the patched image, a fresh
     RSA host key with its own unique fingerprint present, `authorized_keys` byte-identical to
     the invoking user's own file with correct `0600`/`0700` permissions, and `fsck.hfsplus`
     still clean.
   - **Tried and abandoned: an in-process reimplementation of `usbmuxd`'s raw
     TCP-over-USB transport, entirely bypassing the system daemon.** The
     `lockdown error -5` finding above first looked like "usbmuxd's SSL is too
     modern," but direct testing showed the system usbmuxd doesn't expose this
     device to any client at all once its own preflight step fails — not even the
     raw device listing — so at the time this looked like the system daemon could
     never be used for this hardware, for anything. In response, `third_party/usbmuxd`
     was vendored as a submodule and its real, unmodified `src/usb.c`/`src/device.c`
     (a genuine mini-TCP stack: `struct mux_header` wrapping a real `struct tcphdr`,
     full SYN/SYN-ACK/ACK connection states) were built into an in-process transport
     (`Blackb0x/Source/InProcessMux/`), with `preflight.c` stubbed out and `client.c`
     replaced by a single-process mutex/condvar byte-buffer client instead of a
     Unix-socket-per-client model. This was fully debugged (including a real,
     non-obvious bug: inbound device data only becomes visible to a reader when
     `device_client_process()` is driven with `POLLOUT`, an event a real `poll()`
     loop over client sockets normally supplies and which the in-process design had
     to generate itself via a pump thread) and **verified end-to-end against real
     hardware** — a plaintext lockdownd `QueryType` round-trip matched the expected
     `com.apple.mobile.lockdown` response exactly. It was abandoned anyway once
     further investigation (below) found that the system usbmuxd's transport layer
     is actually protocol-agnostic and gates devices in exactly one place
     (`preflight.c`'s own internal SSL probe), which a documented `--no-preflight`
     daemon flag disables outright — making the entire vendored transport
     unnecessary. `third_party/usbmuxd` has been deregistered as a submodule and
     `Blackb0x/Source/InProcessMux/` deleted; see the "libimobiledevice forked"
     section below for the architecture that replaced it.
     - **wolfSSL vendored for the actual SSLv3 layer** (submodule, pinned to current
       HEAD — no historical pin applies, this supplies capability the original app never
       needed). mbedTLS and GnuTLS were ruled out first by direct research: mbedTLS
       dropped SSLv3 in 3.0.0 (so only an EOL 2.x release would have it), GnuTLS dropped
       it in 3.4.0 (2015) — wolfSSL is the one currently-maintained library that still
       supports it, via an explicit `--enable-sslv3` build flag, specifically because it
       targets embedded/legacy-compatibility use cases. Built via the same
       `ExternalProject_Add` pattern as the other autotools deps, with
       `--enable-sslv3 --enable-arc4 --enable-md5` (the latter two, both off by default,
       anticipating that a genuinely ancient SSLv3 peer will only offer legacy
       RC4-MD5-family cipher suites — not yet confirmed against the real device, since
       the handshake itself isn't implemented yet). Deliberately uses wolfSSL's own
       native `wolfSSL_*` API, never its OpenSSL-compatibility layer, so it cannot
       collide with the real OpenSSL 3.x symbols already linked elsewhere in this
       project.
     - **A from-scratch lockdownd+AFC client (`LegacyLockdownClient`/`AfcClient`/
       `CertGen`/`PairingStore`) was built, fully verified against real hardware, and
       then deliberately deleted** once a better architecture was found — see the
       "libimobiledevice forked instead of hand-maintaining a lockdownd/AFC client"
       section below for what replaced it and why. It's gone from the tree, but three
       real bugs it surfaced along the way are still load-bearing today (the fixes
       live on in the fork/wolfSSL build, not in deleted code), so the debugging trail
       is worth keeping:
       1. `wolfSSL_CTX_use_certificate_buffer` failed with `-463`
          (`WOLFSSL_BAD_FILE`, an unhelpfully generic code). wolfSSL's own
          `--enable-debug` trace pinpointed the actual cause: the cert generator gave
          *every* generated cert — including the host leaf cert loaded directly into
          wolfSSL — a serial number of `0`. wolfSSL enforces RFC 5280 §4.1.2.2
          (positive serials) and rejects serial `0` on any cert that isn't a
          self-signed CA (`wolfcrypt/src/asn.c`: "Error serial number of 0 for
          non-root certificate") — and real libimobiledevice's own
          `common/userpref.c` does the exact same thing on its OpenSSL code path
          (`ASN1_INTEGER_set(sn, 0)` on root/host/device certs alike), so this wasn't
          a bug specific to the from-scratch generator. Rather than patching
          `userpref.c` to diverge from upstream, the fix landed in wolfSSL's own
          build instead: `EXTRA_CFLAGS=-DWOLFSSL_ASN_ALLOW_0_SERIAL` (wolfSSL's own
          documented escape hatch for exactly this, confirmed directly in
          `wolfcrypt/src/asn.c`'s guard around the same check) — one line, and
          `userpref.c` needed no changes at all.
       2. Even after the cert loaded, the very first real handshake attempt got an
          immediate TCP RST from the device, ~2ms after a suspiciously tiny 48-byte
          ClientHello. Root cause: wolfSSL compiles out the entire classic
          static-RSA-key-exchange cipher suite family (`RC4-MD5`, `RC4-SHA`, etc.) by
          default as a security hardening measure — and those are the *only* suites a
          genuine SSLv3 peer from this era can offer (SSLv3 predates
          ephemeral DHE/ECDHE key exchange entirely). With none compiled in, the
          ClientHello's cipher-suite list was effectively empty, and the device
          correctly rejected it outright. Fixed by rebuilding wolfSSL with
          `EXTRA_CFLAGS=-DWOLFSSL_STATIC_RSA` (there's no dedicated
          `--enable-staticrsa` configure flag — the only place this macro appears in
          `configure.ac` is bundled into the unrelated `--enable-qt-test` path, which
          *also* forces `-DOPENSSL_NO_SSL3`, the opposite of what's needed) and
          explicitly calling `wolfSSL_CTX_set_cipher_list(ctx, "RC4-MD5")` at the SSL
          setup site rather than trusting wolfSSL's default suite list — both fixes
          are still exactly what the libimobiledevice fork's own
          `idevice_connection_enable_ssl()` does today.
       3. With the handshake itself finally succeeding, the *first* `GetValue` request
          worked, but every one after it failed or returned garbage ("implausible
          response length"). Root cause: the from-scratch client's 4-byte length-prefix
          read did a single non-looping `wolfSSL_read(..., 4)` call and treated any
          return other than exactly 4 as a hard failure — but a short read (`n=1`,
          `n=3`) is normal stream behavior, not an error, and the un-consumed remainder
          of that prefix stayed in the stream, permanently desyncing every request
          after the first. Fixed with a small loop-to-completion helper at the time;
          real libimobiledevice's own `property_list_service.c` already loops
          correctly for this exact reason, so the fork inherits correct behavior here
          for free.
       Also found by inspecting a real successful pairing response: **this device's
       `Pair` response omits `"Result": "Success"` entirely on success** (`Result` is
       only ever present to signal failure). Real libimobiledevice's own
       `lockdown.c` already handles this correctly and has for years — its
       `lockdownd_check_result()` has a comment reading `/* iOS 5: the 'Result' key is
       not present anymore. But we need to check for the 'Error' key. */` — so this
       needed no fix at all once the fork's transport could reach the device; it was
       only a real problem for the from-scratch client, which didn't know the quirk
       going in.
     - **Project-wide OpenSSL removal.** After the cert-serial bug above, and per an
       explicit decision to stop linking real OpenSSL at all: every OpenSSL use in
       `xpwn`/`dmg`'s vendored `.c` files (`filevault.c`, `8900.c`, `img3.c`,
       `pwnutil.c`) now resolves through wolfSSL's OpenSSL-compatibility shim headers
       (`wolfssl/openssl/*.h`, enabled via `--enable-opensslall`) instead of real
       OpenSSL, via `OPENSSL_INCLUDE_DIR`/`OPENSSL_LIBRARIES`/`CRYPTO_LIBRARIES`
       CACHE-variable overrides forced before `add_subdirectory(third_party/xpwn)`.
       This wasn't a drop-in swap — three real build problems had to be solved:
       - wolfSSL's shim headers aren't self-contained the way real OpenSSL's are; they
         need `wolfssl/options.h` processed first. Fixed with a per-target
         `-include .../wolfssl/options.h` force-include on every xpwn/dmg target, plus
         `${DEPS_INCLUDE}` on the include path for `options.h`'s own
         `#include <wolfssl/wolfcrypt/settings.h>`.
       - `--enable-opensslall`'s shim headers textually collide with real OpenSSL
         headers if both ever land in the same translation unit (both declare the same
         bare symbol names). This same constraint is why the libimobiledevice fork's
         `idevice.c` (native wolfSSL, for the actual TLS session — see below) never
         `#include`s any OpenSSL-shim header, while `common/userpref.c` in that same
         library (real OpenSSL/wolfSSL-shim, for cert generation) is left completely
         unpatched in its own separate translation unit — the same two-files-never-
         mixed pattern this project used earlier for its own now-deleted
         `CertGen.cpp`/`LegacyLockdownClient.cpp` split.
       - linuxbrew's own real OpenSSL headers happen to live under
         `/home/linuxbrew/.linuxbrew/include`, the same prefix PNG/BZip2's own
         `include_directories()` calls inside xpwn's vendored CMakeLists.txt pull in
         incidentally — and without `BEFORE`, that path won the `-I` order race ahead
         of wolfSSL's shim dir, so `#include <openssl/aes.h>` silently resolved to the
         real header, compiled fine, and only failed at *link* time with undefined
         references to bare `AES_cbc_encrypt`/`SHA1_Init` (confirmed via `nm` on the
         resulting `.o`). Fixed with `target_include_directories(... BEFORE PRIVATE
         ${DEPS_INCLUDE}/wolfssl)` on every affected target.
       Verified via `nm -D` on the final `blackb0x` binary: zero `SSL_*`/`EVP_*`/
       `X509_*` symbols anywhere, confirming no real OpenSSL linkage survives.
     - **Full static linking.** Per an explicit decision that every third-party
       dependency should be a static archive (only glibc/libstdc++/libgcc_s/libm stay
       dynamic — deliberately *not* full `-static`, since glibc's NSS/`getpwnam`
       is unreliable when statically linked, and was needed at the time for
       `$SUDO_USER` resolution in code since superseded — see the libimobiledevice
       fork section below), five more submodules were vendored specifically because no
       static archive for them exists anywhere on this system: `curl` (built against
       wolfSSL, not OpenSSL — `CURL_USE_WOLFSSL=ON`, `-DUSE_LIBIDN2=OFF` since the
       system's `libidn2` is only available as a `.so` and IDN support isn't needed for
       Apple's ASCII-only hostnames), `libusb` (`--disable-udev`, verified its
       netlink-based Linux backend, `linux_netlink.c`, is a real first-class
       non-udev path per `configure.ac`, not a degraded fallback), `libzip`,
       `libpng` (pinned to the `v1.6.58` release tag — its `master` branch tracks an
       unstable `1.8`-dev series that produces a differently-named `libpng18.a`), and
       `bzip2` (`libarchive/bzip2`, a common CMake-buildable fork — upstream bzip2 has
       no CMake support at all). Real build-system problems solved along the way:
       - `CMAKE_INSTALL_LIBDIR` defaults to `lib64` on this Fedora-based system for
         CMake-based `ExternalProject_Add` targets (`curl`, `libzip`), while every
         autotools-based one installs to plain `lib` — pinned
         `-DCMAKE_INSTALL_LIBDIR=lib` explicitly on both to match.
       - `libusb`'s public header installs to `include/libusb-1.0/libusb.h`, not
         directly under `include/`, but usbmuxd's own `usb.c` does a bare
         `#include <libusb.h>` — the imported `deps::usb` target's
         `INTERFACE_INCLUDE_DIRECTORIES` needed that subdirectory added explicitly
         (and the directory pre-created via `file(MAKE_DIRECTORY ...)` at configure
         time, since CMake validates an IMPORTED target's include paths before any
         `ExternalProject` has actually built anything).
       - Two separate vendored xpwn files (`minizip/CMakeLists.txt`,
         `dmg/CMakeLists.txt`) each hardcode a bare `target_link_libraries(... z)`
         *in addition to* their own correct `${ZLIB_LIBRARIES}` reference — a bare
         `-lz` makes the linker do a fresh library search that prefers the `.so` over
         a static `.a` sitting in the same `-L` directory, silently reintroducing a
         dynamic `libz.so` dependency. Fixed post-hoc from the top-level
         `CMakeLists.txt` (not by editing the vendored files) — overwriting
         `minizip`'s `LINK_LIBRARIES`/`INTERFACE_LINK_LIBRARIES` outright (since
         `target_link_libraries()` only appends, so calling it again can't undo the
         bad entry), and surgically removing just the `z` item from `dmg`'s list with
         `list(REMOVE_ITEM ...)`.
       - `bzip2_ext`/`libpng_ext` need to exist as CMake targets *before* the loop that
         calls `add_dependencies(${xpwn_executable} bzip2_ext libpng_ext)` on every
         executable xpwn's subdirectories define (needed because `xpwn`, the static
         library, re-exports `${BZIP2_LIBRARIES}`/`${PNG_LIBRARIES}` transitively to
         every consumer) — CMake silently drops an `add_dependencies()` call whose
         target doesn't exist yet instead of erroring, which produced a real "No rule
         to make target 'deps/lib/libpng16.a'" build failure until both
         `ExternalProject_Add` blocks were moved earlier in the file.
       Verified via `ldd` on the final `blackb0x`: only
       `libstdc++.so.6`/`libm.so.6`/`libgcc_s.so.1`/`libc.so.6`/`ld-linux-x86-64.so.2`
       remain — no `libssl`/`libcrypto`/`libcurl`/`libusb`/`libzip`/`libpng`/`libbz2`/
       `libudev` anywhere in the dynamic dependency list.
     - **`third_party/libfragmentzip` is forked**, not patched in-place: its
       `libfragmentzip.h` needs a small C++-compatibility fix (`#include <stdbool.h>`
       instead of a hand-rolled `typedef enum{false=0,true=1}bool;` that collides with
       C++'s built-in `bool` keyword) that a plain in-tree patch lost *twice* to
       `git reset --hard` on that submodule during this session. Fixed for good by
       forking to `github.com/regulad/libfragmentzip`, committing the fix on a
       `fix-cxx-stdbool-header` branch off the exact commit already in use, and
       pointing `.gitmodules`'s `url`/`branch` at the fork — the fix is now upstream of
       any future reset instead of living as a repeatedly-reapplied local patch.
     - **libimobiledevice was forked instead of hand-maintaining a lockdownd/AFC
       client, and `DeviceManager.cpp` is wired up to it through the real system
       `usbmuxd`.** The from-scratch `LegacyLockdownClient`/`AfcClient`/`CertGen`/
       `PairingStore` above were fully verified against real hardware first — full
       pairing, `StartSession`, the wolfSSL SSLv3 handshake, and `GetValue` all
       returned correct data end-to-end against a real Apple TV 3,2
       (`8.4.4`/`12H1006`) — but implementing AFC's full feature set (and every
       *other* libimobiledevice service: installation_proxy, notification_proxy,
       diagnostics_relay, mobile_image_mounter, ...) from scratch one operation at a
       time was never going to be a good use of time, when real libimobiledevice
       already implements all of it correctly.
       - **The question that changed the architecture**: given that
         `idevice_connection_enable_ssl()` (not `usbmuxd`) is what terminates SSL for
         a libimobiledevice client, could the system `usbmuxd` be used again instead
         of vendoring a whole transport? Reading `usbmuxd`'s own real source
         (`src/device.c`/`usb.c`/`client.c`, fetched directly from GitHub) confirmed
         those three files are a pure byte-shuttle mux-TCP-over-USB engine with zero
         SSL/TLS awareness anywhere in them (`grep -in "ssl\|tls"` on all three: no
         matches). The *only* place the daemon itself touches SSL is `preflight.c` —
         an internal, best-effort probe that opens its own lockdown session (using
         the daemon's own linked libimobiledevice) purely to warm a pairing-record
         cache, called *before* `client_device_add()` (the call that actually makes a
         device visible to any client's `usbmuxd_get_device_list()`). Several of
         `preflight.c`'s failure paths `goto leave` without ever reaching
         `client_device_add()` — that's the entire mechanism hiding this hardware
         from the system daemon, not anything in the daemon's real transport. And
         `main.c` has a real, documented `-p`/`--no-preflight` flag that skips this
         probe entirely and calls `client_device_add()` unconditionally. So the
         system usbmuxd works fine for this hardware's raw transport — it just needs
         to be told not to preflight it with a TLS stack that can't negotiate SSLv3
         either. **This is why the in-process transport above was abandoned**: once
         `--no-preflight` was confirmed to work, vendoring `usbmuxd`'s own engine was
         no longer buying anything, only extra code to maintain.
       - **The remaining problem is exactly the one the in-process attempt already
         solved**: libimobiledevice's own `idevice_connection_enable_ssl()` hardcodes
         real OpenSSL/GnuTLS with `SSL_CTX_set_min_proto_version(ssl_ctx,
         TLS1_VERSION)` — a TLS 1.0 floor that can never negotiate SSLv3 regardless
         of backing library, the same reason the system daemon's own preflight probe
         fails. Since libimobiledevice has no pluggable transport/TLS layer
         (`internal_connection_send`/`internal_connection_receive` call
         `usbmuxd_send`/`usbmuxd_recv` directly on a real fd, and SSL setup is
         inlined into `idevice_connection_enable_ssl()`), this still requires a fork
         — but a much smaller one than before, since the real `usbmuxd_*` transport
         calls no longer need replacing, only the SSL call site.
       - **Fork**: `github.com/regulad/libimobiledevice`, branch `legacy`, off the
         same `e52ef95` commit this project was already pinned to (see "Final pins"
         above) — not a new pin, that exact commit with `configure.ac`,
         `src/Makefile.am`, `src/idevice.h`, and `src/idevice.c` patched.
       - **The SSL backend is additive and configure-time selectable, not a hard
         replacement of OpenSSL/GnuTLS**: a new `--with-ssl-implementation=
         auto|openssl|gnutls|wolfssl|mbedtls` autoconf option (`configure.ac`,
         mirroring the existing `--with-cython` pattern) defines exactly one of
         `LIBIMOBILEDEVICE_SSL_IMPLEMENTATION_{OPENSSL,GNUTLS,WOLFSSL}` (naming
         matches this project's own `LIBIMOBILEDEVICE_*` config-macro convention),
         completely independent of the pre-existing `HAVE_OPENSSL`/`--disable-openssl`
         flag, which continues to govern only `common/userpref.c`'s cert generation,
         unchanged. `mbedtls` is a recognized value that errors cleanly at configure
         time — mbedTLS dropped SSLv3 support in 3.0.0, so it fundamentally can't do
         what this option exists for. This project's own build passes
         `--with-ssl-implementation=wolfssl` (see `CMakeLists.txt`'s
         `libimobiledevice_ext`), but the fork itself still builds and works as
         plain upstream against real OpenSSL/GnuTLS if configured that way.
       - **`src/idevice.h`**: `struct ssl_data_private` is now a 3-way branch on
         `LIBIMOBILEDEVICE_SSL_IMPLEMENTATION_{WOLFSSL,GNUTLS}`/plain-OpenSSL, holding
         `WOLFSSL*`/`WOLFSSL_CTX*` in the new branch and the original fields
         unchanged in the other two. `struct idevice_connection_private`/
         `struct idevice_private` are **completely unchanged** from upstream — `data`
         still holds the real fd `usbmuxd_connect()` returns, `mux_id` is still real
         — there is no opaque bridge type anywhere in this version of the patch.
       - **`src/idevice.c`**: every transport function (`idevice_new_with_options`,
         `idevice_connect`, `internal_connection_send`/`_receive`/`_receive_timeout`,
         `idevice_get_device_list[_extended]`, `idevice_event_subscribe`, ...) is
         **byte-for-byte unchanged from upstream** — real `usbmuxd_*` calls
         throughout, real push-based hotplug notification via
         `idevice_event_subscribe()`. Only `idevice_connection_enable_ssl()` (plus
         `internal_idevice_init`/`_deinit` for `wolfSSL_Init`/`Cleanup`, and cleanup
         in `internal_ssl_cleanup`) gained a new
         `#elif defined(LIBIMOBILEDEVICE_SSL_IMPLEMENTATION_WOLFSSL)` branch, using
         `wolfSSL_set_fd()` against the real fd (no custom I/O callbacks needed, since
         a real fd exists again), `wolfSSLv3_client_method()`,
         `wolfSSL_CTX_set_cipher_list(ctx, "RC4-MD5")`, and the pair record's **host**
         certificate/key (`USERPREF_HOST_CERTIFICATE_KEY`/`_HOST_PRIVATE_KEY_KEY`) —
         not the **root** cert/key upstream's OpenSSL branch uses, which targets
         iOS 5+/TLS1.0-era devices and was never verified to also work for HOST vs
         ROOT on hardware this old; HOST is what the from-scratch client verified
         correct above. `common/userpref.c` — real cert generation, using real
         OpenSSL/GnuTLS — is completely unpatched; its serial-0 certs work as-is
         because of the `WOLFSSL_ASN_ALLOW_0_SERIAL` wolfSSL build flag (see the
         bug-history bullet above), not because anything in this fork was changed
         for it. Verified via `nm build/deps/lib/libimobiledevice.a | grep -c
         wolfSSL_connect` (real wolfSSL symbols compiled in, not silently falling
         back) and by inspecting the generated `config.h` (`HAVE_OPENSSL` and
         `LIBIMOBILEDEVICE_SSL_IMPLEMENTATION_WOLFSSL` both set, independently, as
         designed). Diff against upstream `e52ef95`: 4 files, +237/-27 lines — kept
         deliberately small and clean (`git reset --soft` + a targeted re-patch onto
         a pristine checkout, then `git push --force origin legacy`, after an earlier,
         much larger version of this same fork branch — the one that also replaced
         the transport layer for the in-process mux attempt above — was abandoned).
       - **A real, known ecosystem conflict, still present and still fixed the same
         way**: `libusbmuxd.a` (needed transitively — `common/userpref.c`'s
         `usbmuxd_read_buid`/`usbmuxd_read_pair_record`/etc, a daemon-side
         pairing-record cache path separate from the direct-filesystem
         `/var/lib/lockdown` storage) and `libimobiledevice-glue-1.0.a` each vendor
         their own separate copy of `common/collection.c` with external linkage — a
         hard "multiple definition" link error once a real binary exercises both.
         Both copies implement the same generic collection ADT, so
         `-Wl,--allow-multiple-definition` on `blackb0x`'s own link flags is the
         standard practical workaround, not a correctness compromise.
       - **Requires the system `usbmuxd` to be run with `--no-preflight` (or `-p`)**
         — without it, this hardware's preflight probe still fails exactly as
         described above and the device is never exposed to `blackb0x` at all, fork
         or no fork. **This must ship as a systemd drop-in and be documented in the
         end-user README**: a unit override
         (`/etc/systemd/system/usbmuxd.service.d/override.conf`, or the
         distro-appropriate equivalent) adding `--no-preflight` to `usbmuxd`'s
         `ExecStart` — e.g.
         ```
         [Service]
         ExecStart=
         ExecStart=/usr/sbin/usbmuxd -U usbmuxd --no-preflight
         ```
         (the empty `ExecStart=` first is required to clear the original
         `ExecStart` before overriding it — systemd drop-ins append by default), then
         `systemctl daemon-reload && systemctl restart usbmuxd`. This is a real,
         permanent change to how the *system's* usbmuxd behaves for every device, not
         just this project's target hardware — worth calling out plainly in the
         README rather than leaving it as an implicit prerequisite, since without it
         normal-mode discovery silently never fires.
       - **Verified against real hardware, through the real, unmodified `afc.c`**
         (not the deleted from-scratch `AfcClient`), talking through the real system
         `usbmuxd` run with `--no-preflight`: a checkpoint program calling the real
         `idevice_new`/`lockdownd_client_new_with_handshake`/`lockdownd_get_value`/
         `lockdownd_start_service`/`afc_client_new`/`afc_read_directory` returned
         correct `ProductType`/`ProductVersion`/`BuildVersion`/`DeviceName`/
         `UniqueDeviceID`, correctly reported `com.apple.afc2` unreachable (device
         isn't jailbroken yet), and produced a genuine AFC directory listing
         (`. .. DCIM Downloads Photos iTunes_Control`) via libimobiledevice's actual
         AFC protocol implementation. Also confirmed directly via
         `ls -la /var/lib/lockdown/`: the pairing record landed at the genuine
         libimobiledevice storage path (`<udid>.plist`, owned `usbmuxd:usbmuxd`),
         not the `$XDG_CONFIG_HOME/blackb0x` location the now-deleted `PairingStore`
         used.
       - **Pairing-record persistence is entirely delegated to the daemon, on
         purpose — blackb0x never writes into that directory itself.**
         `common/userpref.c`'s `userpref_save_pair_record()`/`_read_pair_record()`/
         `_delete_pair_record()` are just thin wrappers around
         `usbmuxd_save_pair_record_with_device_id()`/`usbmuxd_read_pair_record()`/
         `usbmuxd_delete_pair_record()` (`libusbmuxd`) — confirmed by reading both
         files end to end — which only ever send a `SavePairRecord`/`ReadPairRecord`/
         `DeletePairRecord` message over the usbmuxd control socket; there is no
         fallback direct-filesystem write path anywhere in this fork or upstream.
         The actual `<udid>.plist` file is created by the daemon process itself, so
         even though blackb0x runs entirely as root, the file lands owned by
         whatever unprivileged user the daemon runs as (`usbmuxd:usbmuxd` here) with
         mode `644` — exactly the ownership/permissions the directory (`/var/lib/
         lockdown` on this system; some distros use `/var/lib/usbmuxd` instead, same
         idea) needs, for free, without blackb0x having to chown/chmod anything
         itself. **If any future code path ever needs to write a file into that
         directory directly** (bypassing the daemon's own IPC), it must explicitly
         `chown`/`chmod 644` it to match the directory's owner — root would
         otherwise leave the daemon unable to read its own pairing-record store.
       - **`LegacyLockdownClient`/`AfcClient`/`CertGen`/`PairingStore` are deleted**
         (all four files, entirely), per the explicit decision to not maintain two
         parallel implementations of the same thing. `DeviceManager.cpp`'s
         `plistInfoForDeviceUUID()`/`isJailbroken()`/`isJailbreakRunning()` are back
         to essentially their *original* pre-fork form (real `idevice_new`/
         `lockdownd_client_new_with_handshake`/etc calls), and its constructor is
         back to a single `idevice_event_subscribe(blackb0x_idevice_event_cb, ...)`
         call — real push-based hotplug discovery through the real daemon, no
         polling thread. None of this higher-level code needed to change at all;
         only the library underneath it did — the whole point of this approach is
         that it doesn't know or care that it's talking to hardware the system
         usbmuxd would otherwise refuse. `third_party/usbmuxd` (the vendored daemon
         submodule) and `Blackb0x/Source/InProcessMux/` (the whole in-process
         transport, bridge, and verbosity-scheme directory) are both gone from the
         tree — no protocol-level or transport-level code left to maintain outside
         the fork itself.
   - **Tried and abandoned: Fil-C (fil-c.org) as this project's whole-repo build
     toolchain.** Fil-C is a memory-safe C/C++ clang fork (fat pointers + a
     capability-checked runtime) — tried on the theory that a root-required
     binary this size, written entirely in C/C++, is worth hardening against
     its own memory-safety bugs. Fil-C requires the ENTIRE dependency graph to
     be compiled by its own compiler (no interop with normal "Yolo-C" objects
     at all), so every `ExternalProject_Add(..._ext)` needed `CC`/`CXX`
     (autotools) or `-DCMAKE_C_COMPILER`/`-DCMAKE_CXX_COMPILER` (CMake) pointed
     at Fil-C's `clang`/`clang++` explicitly, on top of setting
     `CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER` before `project()` for every
     native target this file defines directly.
     - **Most of the dependency graph genuinely compiled and linked clean**
       with nothing but the compiler swapped — no source changes: wolfSSL
       (real crypto/ASN.1/RSA/AES, ~70MB of object code), libplist/libplist++,
       libgeneral, libimobiledevice-glue, bzip2, libusb, libzip, curl,
       libimobiledevice, libusbmuxd, libirecovery, and xpwn's own
       `hfs`/`dmg`/`minizip`/`common` static libraries.
     - **zlib had to stop being the one system/Homebrew-provided, non-vendored
       dependency** (see the ZLIB comment near the top of `CMakeLists.txt`) —
       Fil-C's "no interop, full stop" rule doesn't make an exception for
       small/simple libraries, and pulling in the real system `zlib.h`
       (`ZLIB_INCLUDE_DIR=/usr/include`, found by the old `find_package(ZLIB)`)
       broke immediately with "unknown type name '__gnuc_va_list'" the moment
       any translation unit's `#include` chain reached `<stdio.h>` — glibc's
       header assumes gcc/glibc-internal builtin cooperation Fil-C's own
       clang+musl doesn't independently provide when parsing a foreign libc's
       headers directly. This was the single largest class of errors (~6000,
       nearly all identical) in the first whole-repo attempt. **`zlib_ext` is
       kept vendored even after reverting off Fil-C** — the user's explicit
       call, and also more consistent with this project's own "every
       third-party dependency is a static archive built from source" goal
       (see "Full static linking" above) than reintroducing the one exception
       would have been.
     - **The actual wall: a genuine Fil-C compiler crash, not a config
       problem.** `third_party/xpwn`'s own CLI tools (`ipsw-patch/libxpwn.c`,
       `dmg/dmg.c`, `hdutil/hdutil.c`, `hfs/hfs.c`) are circa-2010 C that
       declares the same globals in multiple translation units without
       `extern` — a legal, if deprecated, pre-C99 idiom, which is exactly why
       this project already passes `-fcommon` to every xpwn target (a
       pre-existing fix for an unrelated GCC≥14 diagnostics change). Fil-C's
       own instrumentation pass can't represent a "common linkage" global at
       all (its whole memory-safety model needs a global's size/type resolved
       unambiguously, which common linkage deliberately defers to link time)
       and hits a hard assertion instead of a clean diagnostic:
       ```
       clang: /fil-c/llvm/.../FilPizlonator.cpp:12459: Assertion
       `G.getLinkage() != GlobalValue::CommonLinkage' failed.
       ```
       (Fil-C version 0.684.) **Checked for an existing upstream issue before
       stopping**: none found for this exact assertion. The closest is
       [pizlonator/fil-c#251](https://github.com/pizlonator/fil-c/issues/251)
       ("Compiler crashes when building Protobuf"), which hits the same
       `lockDownLinkage()` function's assertions but for `AppendingLinkage`,
       not `CommonLinkage` — a related but genuinely different case. Worth
       filing upstream if this gets revisited, but wasn't filed as part of
       this session.
     - **Two smaller, separate build-script bugs found and fixed along the
       way** (both real, independent of Fil-C, but only surfaced by trying
       it): `ExternalProject_Add(... BUILD_COMMAND make -j SUBDIRS="common src
       include")` — writing the quotes literally into an *unquoted* CMake
       argument doesn't group it the way a shell would; CMake splits on the
       embedded spaces regardless, producing three broken argv tokens
       (confirmed directly: `/bin/sh: line 21: cd: "common: No such file or
       directory`). The fix is a single CMake-quoted token,
       `"SUBDIRS=common src include"`, matching the pre-existing
       `libimobiledevice_ext` pattern (`SUBDIRS=${LIBIMOBILEDEVICE_LIB_SUBDIRS}`,
       a variable substituted whole into one already-CMake-quoted argument).
       Also: `xpwn/hfs/hfscompress.c` does a bare `#include <zlib.h>` with no
       `include_directories()` call anywhere in `xpwn/hfs`'s own
       `CMakeLists.txt` (unlike `minizip`/`ipsw-patch`, which do call that) —
       invisible under a normal host compiler (always has `/usr/include` on
       its default search path regardless) but a hard failure once zlib.h
       wasn't implicitly reachable; fixed with an explicit
       `target_include_directories(hfs PRIVATE ${DEPS_INCLUDE})` +
       `add_dependencies(hfs zlib_ext)`, both kept after reverting off Fil-C
       since they're correct (and arguably more correct — using the vendored
       zlib.h/libz.a consistently) regardless of which compiler builds it.
     - **Reverted via `git diff` review, not a hard reset**: every
       Fil-C-specific piece (the `CMAKE_C_COMPILER`/`CMAKE_CXX_COMPILER`
       override and its propagation into every `_ext` sub-build, plus the
       `--disable-examples-build`/`--disable-tests-build`/`-DBUILD_OSSFUZZ=OFF`/
       `SUBDIRS=...`-restriction changes made solely to dodge Fil-C's
       CRT/`main`-symbol collisions in `libusb`/`libzip`/`libusbmuxd`/
       `libirecovery`'s own unused test/example/tool binaries) was removed;
       zlib vendoring and the two build-script fixes above were kept.
       Verified with a full clean rebuild afterward: gcc again (via this
       system's `ccache` wrapper), `ldd build/blackb0x` still shows only
       libc/libstdc++/libm/libgcc_s.
   - **First real end-to-end attempts against the user's actual AppleTV3,2,
     with a real DFU-mode remote handoff, surfaced five more real, unrelated
     bugs — all five now fixed — plus real, repetitive CLI noise worth
     cleaning up once actually watched scroll by on a live run:**
     - **The remote's two-stage DFU handoff isn't obvious and the CLI's own
       instructions didn't explain it.** Holding Menu+Down until the LED
       blinks fast only reaches Recovery Mode — which blinks identically to
       DFU mode, so it looks like it worked — DFU itself needs a *second*
       stage right after: release both buttons completely, then hold
       Menu+Play until it blinks fast again. `Cli.cpp`'s `runCli()` DFU-wait
       message now spells out both stages explicitly (plus a note that a bad
       micro-USB cable or a missed IR button-edge during the handoff are the
       two most common silent failure modes), instead of the original
       single-stage "hold DOWN and MENU" instruction that was accurate for
       Recovery Mode but not DFU.
     - **`IPSW.cpp`'s `httpGet()` (used to resolve firmware URLs from
       `api.ipsw.me`) failed with curl's `CURLE_SSL_CACERT` — "Problem with
       the SSL CA cert."** Root-caused with a throwaway `wolfSSL_connect()`
       test program (`wolfSSL_Debugging_ON()` made this fast) rather than
       guessing: the CA bundle itself
       (`/etc/ssl/certs/ca-certificates.crt`) loaded fine and genuinely
       contains the right root (Google Trust Services' `GTS Root R4`,
       confirmed present, which is what `api.ipsw.me`'s real chain —
       `CN=ipsw.me` -> `Google Trust Services WE1` -> `GTS Root R4` —
       chains to) — the actual failure was `ASN_NO_SIGNER_E` (-188) during
       chain validation itself: wolfSSL's default chain verifier requires
       each certificate's issuer to exactly match the next certificate's
       subject in a single, exact path all the way to a trusted root, and
       gives up rather than trying alternate paths the way OpenSSL does.
       This is common enough with real-world CAs (cross-signing, multiple
       valid paths to different roots) that wolfSSL ships a dedicated,
       documented macro for it: `WOLFSSL_ALT_CERT_CHAINS` ("Allow
       non-validated intermediate CAs", `src/internal.c`) — added to
       `wolfssl_ext`'s `EXTRA_CFLAGS` alongside `WOLFSSL_STATIC_RSA`/
       `WOLFSSL_ASN_ALLOW_0_SERIAL` (no dedicated configure flag for this one
       either). Verified fixed with the same throwaway test program
       (`wolfSSL_connect()` now succeeds against `api.ipsw.me`) and directly
       through `blackb0x`'s own static curl+wolfSSL stack (a minimal
       `curl_easy_perform()` against the real `api.ipsw.me` URL blackb0x
       itself calls returns `CURLE_OK`). This is unrelated to the Apple
       TV/`idevice.c` SSLv3 story entirely — it's curl's own modern
       TLS 1.2/1.3 connection to a normal HTTPS API, not the ancient
       lockdownd handshake.
     - **`Patcher::patchKernel()`'s version string was always empty, silently,
       on every single run — a real bug present since this method was first
       written, never previously exercised on real hardware.** The deleted
       `versionString(path)` helper derived a device/version pair by
       splitting `path`'s *parent directory name* on `_` — an assumption
       matching a single flat `"<device>_<version>"` cache-directory layout
       that was never actually true for this port: `Cli.cpp`'s
       `downloadAndPatchComponents()` builds `workDir` as
       `ipswDataRoot()/deviceModel/buildID` — two nested segments, neither of
       which contains a version string at all (confirmed directly: real
       output was `Device: 10B329a - Version:` — the "device" slot actually
       held the *build ID*, and "version" was always empty). Fixed by
       sourcing the version from where it actually lives —
       BuildManifest.plist's own top-level `ProductVersion` key (confirmed
       present via a direct manual fetch+parse of a real manifest:
       `ProductVersion` = `"6.1.3"` for this exact 10B329a build) — added to
       `Cli.cpp`'s `ManifestInfo`/`parseManifest()` and threaded through as
       an explicit parameter,
       `Patcher::patchKernel(path, productVersion)`, replacing the deleted
       path-based guess entirely.
     - **That empty version, plus a second bug — `patchKernel()` never
       checked `patch_kernel()`'s return value — combined into real, live
       heap corruption (`malloc(): corrupted top size`, a crash) on every
       kernel-patch attempt, not just a wrong result.** `CBPatcher.c`'s
       `patch_kernel()` correctly rejects an unsupported/unparseable version
       ("This version of CBPatcher does not support iOS 0 kernels") and
       returns nonzero *without ever writing its output file* — but the
       calling code proceeded to `decrypt()` that nonexistent file anyway.
       Confirmed by direct, isolated reproduction (a throwaway harness
       linking this project's own built `libcbpatcher.a`/`libxpwn.a`/
       `libwolfssl.a` against a real kernelcache fetched from the same real
       IPSW, replaying the exact key/iv from a live run): with `version=""`,
       `patch_kernel()` fails cleanly as designed, and the *following*
       `decrypt()` call — third_party/xpwn's own `xpwntool.c`, given a
       missing input file — is what actually corrupts the heap (`error:
       cannot open infile` immediately followed by `malloc(): corrupted top
       size`). Fixed by checking `patch_kernel()`'s return value and
       aborting `patchKernel()` cleanly (no second `decrypt()`, no
       `outputs_.kernel` set) on failure — a real robustness gap independent
       of the version bug above, since patch_kernel() can fail for other
       reasons on other builds too.
     - **A third, deeper bug — this one survives both fixes above and is
       specific to the kernelcache, never hit by iBSS/iBEC — turned out to
       be a real buffer over-read/over-write inside wolfSSL's own optimized
       AES-CBC path, not xpwn's control flow.** With the *correct* version
       (`"6.1.3"` -> `getRealVersion()` -> `"6.0"`), `patch_kernel()` now
       genuinely succeeds (`[CBPatch] Found ...` signature matches, "Kernel
       patched successfully") — but the *second* `decrypt()` call
       (re-encrypting the patched kernel back into the original IMG3
       container, using the original file as a template — the same
       operation iBSS/iBEC also do, successfully, every run) hit a
       *different* corruption: `double free or corruption (!prev)`.
       Hand-instrumenting copies of `third_party/xpwn/ipsw-patch/{xpwntool.c,
       img3.c,lzssfile.c}` with `fprintf(stderr, ...)` tracing first
       narrowed it to `info->root->free(info->root)` inside
       `duplicateAbstractFile2()`/`duplicateAbstractFile()`'s recursive
       signature-sniffing of the template's nested container structure
       (kernelcache payloads are IMG3-wrapping-a-Comp/LZSS-container,
       confirmed via the "match: ..." trace already visible during the
       *first* `decrypt()` call — iBSS/iBEC are plain, uncompressed IMG3
       with no nested Comp layer, exactly why they never exercise this
       path) — but pinning the *exact* line needed a real debugger.
       `valgrind` (installed by the user partway through this
       investigation, specifically for this) nailed it precisely on the
       first run: `Invalid write of size 4/8` inside wolfSSL's
       `AesDecrypt_C`/`AesDecrypt_preFetchOpt` (`wolfcrypt/src/aes.c`),
       called via `wolfSSL_AES_cbc_encrypt` -> `setKeyImg3`, reading/writing
       up to ~11 bytes **past the end of** `readImg3Element()`'s
       `malloc(header->dataSize)` for the kernelcache's DATA tag
       (`dataSize` = 6,004,801 — an odd, non-16-aligned length). This code
       was written against real OpenSSL's `AES_cbc_encrypt`, which touches
       exactly the requested length; wolfSSL's optimized decrypt path
       (`AesDecrypt_preFetchOpt`, used because this project links wolfSSL's
       OpenSSL-compat shim instead of real OpenSSL — see the "Project-wide
       OpenSSL removal" bullet above) does a small fixed-size lookahead past
       the last full block for performance. iBSS/iBEC never hit this
       because they're smaller/laid out differently, so the same overrun
       happened to land inside other live heap data instead of triggering a
       glibc-detected corruption — not because the underlying bug wasn't
       there.
       - **Fixed in a fork, not a local patch**: `third_party/xpwn`
         (planetbeing/xpwn) had no fork yet, unlike libimobiledevice/
         libfragmentzip — forked to `github.com/regulad/xpwn`, branch
         `legacy`, off the exact pinned commit (`20c32e5`). Two changes on
         that branch: (1) a pre-existing *uncommitted* local edit this repo
         was already carrying (disabling `pwnmetheus2`, which hard-requires
         the unavailable legacy libusb-0.1 API) — folded into the fork so it
         stops being a silently-droppable local diff, same reasoning as the
         other two forks; (2) the actual fix — every buffer that gets
         AES-CBC'd in place (`readImg3Element`'s DATA/KBAG allocations,
         `writeImg3`'s realloc-grown write buffer) now gets one extra,
         explicitly-zeroed AES block (`IMG3_AES_OVERREAD_PAD = 16`,
         `calloc`/`memset` instead of `malloc`/bare `realloc`) past its
         logical length. This only pads the underlying allocation —
         `dataSize`/`size` and everything derived from them, and therefore
         the actual on-disk IMG3 format this code produces, are completely
         unaffected; only how much slack exists in memory after the
         logical end changes. Zeroing (not just padding) the extra bytes
         matters too, confirmed by valgrind's *next* complaint once the
         overrun itself was fixed: "Use of uninitialised value" from the
         same AES lookahead reading real-but-never-written pad bytes —
         harmless to correctness (never incorporated into output within
         `dataSize`) but worth silencing properly rather than leaving a
         valgrind-dirty result.
       - **Verified valgrind-clean end to end**, not just "doesn't crash":
         same isolated repro harness (this project's own built
         `libcbpatcher.a`/the fixed `libxpwn.a`/`libwolfssl.a`, a real
         kernelcache fetched from the same real IPSW, the exact key/iv from
         a live run) under `valgrind --error-exitcode=99`: `ERROR SUMMARY: 0
         errors from 0 contexts`, `All heap blocks were freed -- no leaks
         are possible`, exit 0.
     - **A fifth bug, found on the next real `--dry-run` attempt (device in
       DFU, all four bugs above already fixed): `getRealVersion()`'s
       "real firmware version -> iOS-equivalent kernel-signature family"
       bucketing — ported byte-for-byte from the original `Patcher.mm`
       (confirmed directly: identical switch/case, still present, dead code,
       there today) — maps this tool's actual target (10B329a / real
       `ProductVersion` `"6.1.3"`) to `"7.0"`, and `patch_kernel()` finds
       **zero** matching signatures for `"7.0"` against a real AppleTV3,2
       6.1.3 kernelcache (`"[CBPatch] One or more patches not found"`,
       hard failure — this is the return-value check from the previous bug
       correctly catching a real failure, not a regression). Passing the
       raw `productVersion` straight through instead — relying on
       `kernPat()`'s/`kernPatOld()`'s own internal major-version truncation
       (`versionInt < 8 -> versionFloat = (float)versionInt`, `CBPatcher.c`)
       — finds and applies every expected patch (tfp0, the AMFI/memcmp
       bypass, the sandbox policy patch, ...) and reports success, against
       this exact real kernelcache. Most likely explanation, not a claim
       the original bucketing logic was conceptually wrong: this project's
       `libcbpatcher.a` was lost and rebuilt from a third-party source fork
       (see the libcbpatcher/iboot32patcher recovery story elsewhere in this
       file), whose `"7.0"`-family signatures apparently don't exactly match
       what the original 2020 binary's signatures did. `Patcher::
       patchKernel()` now passes `productVersion` directly and no longer
       calls `getRealVersion()` — which is kept, unused, as a preserved
       reference in case the signature drift for the `"7.0"` bucket ever
       gets fixed upstream, not deleted, since it may still be correct for
       firmware families (4.x/5.x/7.x+) this tool has no way to test against.
     - **Also cleaned up real, repetitive CLI noise surfaced by actually
       watching a full real run scroll by**: `Patcher.cpp`'s
       `patchiBSS`/`patchiBEC`/`patchKernel`/`patchRamdisk` each printed a
       raw file-path + hex AES key/IV dump on every single call (no value
       once the wolfSSL/decrypt path is trusted; useful during the original
       key/IV debugging, not for normal operation) — replaced with one
       `"Patching iBSS/iBEC/kernelcache/ramdisk..."` line each. The download
       progress callback (`Cli.cpp`'s `downloadAndPatch`) printed every
       `"NN%"` line as many times as the underlying transfer happened to
       report that same rounded percentage (chunk-driven, not
       percentage-change-driven) — confirmed directly in a real run's
       output (`"KernelCache: 0%"` five times in a row) — fixed by tracking
       the last percentage actually printed. `Blackb0x/Libraries/
       xpwntool.c`'s `decrypt()` (this project's own copy, see its
       provenance note elsewhere in this file) printed a raw `input_path`
       and a decrypt-flag debug line, with a **missing trailing newline**
       on the first one that visibly ran the two together in real output
       (`"input_path kernelcache.release.j33idecrypt 0"`) — both removed.
       `third_party/xpwn` itself has a real leveled-logging facility
       (`XLOG(level, ...)` -> `Log()`, `libxpwn.c`) that `init_libxpwn()`
       defaults to "suppress nothing" (`GlobalLogLevel = 0xFF`, and `Log()`
       suppresses only when `level >= GlobalLogLevel` — a value that high
       passes everything through, not the reverse) — of ~130 `XLOG()` call
       sites across the whole vendored tree, only 3 use level 4 or 5, and
       both of the specific lines cluttering every real run
       (`img3.c`'s raw payload-hash dump, `lzssfile.c`'s LZSS
       compressed/uncompressed-length "match" line) are among them — added
       `libxpwn_loglevel(4)` right after `init_libxpwn()` in `xpwntool.c`'s
       `decrypt()` to silence just those two, leaving the ~127 level-0-3
       calls (genuinely useful progress/status output) untouched. The
       `iBootPatcher`/`CBPatcher` per-instruction trace
       (`patch_boot_args: Entering...`, `find_*: Found ... at 0x...`, etc,
       ~186 plain `printf()` calls across `Blackb0x/Libraries/
       libiboot32patcher/*.c`, no leveled-logging mechanism to hook)
       deliberately left alone — unlike the other noise, each line there
       appears exactly once per patch, not repeated, and is a genuine audit
       trail of what got changed in the boot chain, not debug spam.
   - **The next `--dry-run` (device in DFU, all five bugs above fixed) completed
     fully clean end to end for the first time** — iBSS/iBEC/kernelcache all
     patched successfully, ramdisk built and SSH-granted, no crashes, no
     errors, quiet output. This was the first real confirmation the whole
     download -> decrypt -> patch -> re-encrypt pipeline genuinely works
     against real hardware, not just against isolated repro harnesses.
   - **A sixth bug — and the actual, final blocker — surfaced on the first
     genuine non-`--dry-run` attempt: `checkm8()` itself failed immediately**
     (`"Failed to stall pipe -9."`, right at its very first exploit-setup
     step). checkm8 *deliberately* induces a USB pipe stall as the first
     step of its exploit sequence — a stall here is the expected, correct
     signal, not a failure — so `DeviceManager.cpp`'s
     `usb_req_stall()`/`usb_req_leak()`/`usb_req_no_leak()` call sites check
     their result against `IRECV_E_PIPE`/`IRECV_E_TIMEOUT`
     (libirecovery's own `irecv_error_t` enum, -10/-11). But those three
     helpers — and the handful of raw `irecv_usb_control_transfer()` calls
     later in the same function — all funnel through
     `irecv_usb_control_transfer()`, which is platform-conditional
     (`third_party/libirecovery/src/libirecovery.c`, `#ifdef HAVE_IOKIT`):
     on macOS it calls `iokit_usb_control_transfer()`, which explicitly
     translates IOKit's own status codes into `IRECV_E_*`
     (`kIOUSBPipeStalled -> IRECV_E_PIPE`,
     `kIOReturnTimeout`/`kIOUSBTransactionTimeout -> IRECV_E_TIMEOUT`,
     confirmed by reading that translation table directly) — but on Linux
     it's a bare pass-through to `libusb_control_transfer()`, returning
     libusb's own raw `LIBUSB_ERROR_*` codes completely untranslated
     (`LIBUSB_ERROR_PIPE = -9`, `LIBUSB_ERROR_TIMEOUT = -7` — different
     numbers in a completely unrelated enum). The original `DeviceManager.m`
     checks were correct for the platform it was ever run on (macOS/IOKit);
     porting straight to Linux/libusb silently broke every one of these
     checks, since a real, successful stall now reports as -9 instead of
     -10. Confirmed directly against the real error text in the log above.
     Fixed by changing all six comparisons in `checkm8()`
     (two `usb_req_stall()` sites, three `usb_req_leak()`/`usb_req_no_leak()`
     sites, one raw payload-upload `irecv_usb_control_transfer()` call) from
     `IRECV_E_PIPE`/`IRECV_E_TIMEOUT` to `LIBUSB_ERROR_PIPE`/
     `LIBUSB_ERROR_TIMEOUT` (`<libusb.h>`, already available transitively via
     `deps::usb`) — this is the exact same platform-conditional-translation
     gap, applied consistently everywhere the exploit relies on it, not just
     the one call site that happened to fail first. **Not yet re-verified
     against real hardware** — this fix hasn't had a live retry yet as of
     this writing; the previous five bugs were each confirmed with a real
     device before moving on, and this one should be too before treating it
     as done.
   - **Before that retry: a full pass removing raw debug `printf()` noise
     from `DeviceManager.cpp`, and unifying every remaining user-facing
     message in `Cli.cpp` into one consistent style.** Watching real runs
     scroll by (the five-bug session above, then the real checkm8 attempt)
     surfaced something the isolated repro harnesses never would: the same
     "device connected"/"exploit status" information printed two or three
     times in three different formats (a raw `DeviceManager.cpp` printf, a
     `[bracket-tag]`-prefixed `Cli.cpp` rendering of the *same* event via
     the `DeviceEventSink` callback, sometimes both). `DeviceManager.cpp`
     turned out to carry ~50 leftover `printf()`/ANSI-escape-colored debug
     lines from the original app's own development (`"trying to send
     ibss"`, `"no upload client m8"`, `"sendiBSS_ATV32(%s, %llu)"`, raw
     ECID/CPID/nonce hex dumps, `"\x1b[36mUploading soft DFU\x1b[39m"`,
     `"0x8947 configuration"`, ...) — none of them gated behind any
     verbosity flag, all firing unconditionally alongside the *already
     existing*, clean `DeviceEventSink` (`sink_.onStatus`/`onProgress`/
     `onDeviceAdded`/`onDeviceUpdated`/`onDeviceRemoved`) channel that
     `Cli.cpp` was built to render. Fixed by making `DeviceManager.cpp`
     itself silent on success — every routine status message now flows
     through exactly one path, `sink_`, with real failures going to stderr
     instead of stdout — and having `Cli.cpp` alone own the rendering, in
     one unbracketed style throughout (`"Connected to AppleTV3,2 (2685...)
     in DFU"`, `"Trying checkm8..."`, `"(dry run) Would try checkm8"` — no
     more `[status]`/`[connected]`/`[disconnected]`/`[device]`/`[dry-run]`
     tags, no more restating the same device identity on consecutive
     lines). Also removed while touching this: two genuinely dead
     functions (`print_device_info()`/`print_hex()`, declared and defined
     but never called from anywhere), `AppleTVDevice::printDeviceInfo()`/
     `humanDeviceName()` (both existed only to feed the now-removed
     redundant `"Selected: "` line — `Cli.cpp` had already printed the
     exact same device identity one event earlier via `onDeviceAdded`), and
     a couple of `send*()` functions whose own `sink_.onStatus("Sending
     X...")` calls duplicated `Cli.cpp`'s `sendComponentsToDevice()`
     printing the same thing itself (which also already knows and reports
     the actual Sent/Error outcome, something the removed calls didn't).
     One real, if minor, correctness fix fell out of this along the way:
     `sendiBEC()` previously always returned `0` regardless of whether
     `irecv_send_file()` actually succeeded, so a real transfer failure
     would still print `"Sent"` — now it returns the real result.
   - **`selectDevice()` (`Cli.cpp`) now prints a one-time reminder if
     nothing shows up within 5 seconds of waiting**: `"Still searching...
     Please power-cycle your Apple TV after a failed exploit attempt."` —
     added after a real run needed exactly that (see below): several of
     `checkm8()`'s own early-return paths never call `irecv_reset()`/
     `irecv_close()` before bailing, which can leave a partially-exploited
     device's USB stack wedged until it's fully power-cycled, not just
     re-DFU'd. Fires once per wait (a `std::chrono::steady_clock`
     timestamp + one-shot flag, not a per-poll check), regardless of
     whether waiting on `--ecid`/`--udid`, the sole device, or an
     interactive multi-device pick.
   - **The next real (non-dry-run) attempt — after the power-cycle above —
     got further than ever before: all the way through checkm8's
     "Executing payload" stage** (the actual pwn) **before segfaulting.**
     Root cause, found by reading the exact code path: after running the
     payload, `checkm8()` reconnects to the device to verify the pwn took
     (`client = get_tv(ecid); info = irecv_get_device_info(client); ...
     info->serial_string...`) — but `get_tv(ecid)` can return `nullptr`
     (it does its own ~6-attempt/1s-apart retry internally, then gives up
     and logs `"ERROR: Unable to find device"` to stderr), and neither this
     port nor the original `DeviceManager.m` it was ported verbatim from
     ever checked for that before dereferencing the result — turning "the
     device took a little longer to re-enumerate after running injected
     SecureROM-level code" into a straight null-pointer segfault instead of
     a clean `"Checkm8 unsuccessful"`. Confirmed directly: a real run got
     exactly this far (`"Disconnected (...)"` printed right after
     `"Executing payload"`, then `"ERROR: Unable to find device"`, then
     `segmentation fault`) — this specific reconnect is also the single
     most likely one in the whole function to need extra patience, since
     it's the only one checking on a device that just finished executing
     injected code rather than reconnecting to one already sitting quietly
     in a known DFU/Recovery state. Fixed two ways: (1) every `get_tv()`
     result in `checkm8()` (the initial connect, both mid-sequence
     reconnects, and this final one) is now null-checked before use,
     failing cleanly instead of crashing; (2) this specific final reconnect
     also gets its own extra retry loop (5 more attempts, 1s apart) on top
     of `get_tv()`'s own internal one, since it's the one place in the
     function where "not found yet" genuinely might just mean "give it
     another few seconds," not "the device is gone."
   - **That retry did fix the segfault, but the device genuinely never came
     back this time — not even in `lsusb` after a physical unplug/replug**,
     a strictly worse symptom than the earlier runs (which at least got
     that far and just needed a power-cycle). Prompted a direct
     cross-reference against `gaster`
     (github.com/verygenericname/gaster) — the actual checkm8
     implementation `palera1n` (a real, current, widely-used jailbreak
     tool covering this same device class) delegates to today, not a
     from-scratch implementation of its own. First confirmed the low-level
     exploit itself isn't in question: `gaster`'s own per-chip config table
     (`checkm8_check_usb_device()`) has an entry for `"SRTG:[iBoot-1458.2]"`
     (this exact AppleTV3,2 firmware string) with `cpid=0x8947`,
     `config_large_leak=626`, `config_overwrite_pad=0x660` — byte-identical
     to this file's own `get_exploit_configuration()` for the same cpid.
     What *is* different, and well-evidenced enough to act on: `gaster`'s
     own USB-wait helper (`wait_usb_handle()`) never gives up at all — an
     unbounded loop that just keeps polling until the device reappears —
     and its top-level state machine (`gaster_checkm8()`: RESET -> SETUP ->
     SPRAY -> PATCH) resets and retries the *entire* exploit from scratch,
     also in an unbounded loop, on any stage failure, rather than a
     single-shot linear attempt giving up on the first hiccup. Adopted both
     patterns, in bounded form (the CLI still needs to eventually report a
     real failure rather than hang forever): a new `get_tv_patient()`
     helper (up to 30 one-second attempts, used for every reconnect inside
     the exploit, not just the post-payload one) and a new
     `checkm8Attempt()`/`checkm8()` split — `checkm8Attempt()` is the
     existing single-pass sequence, completely unchanged at the USB
     request/payload/timing level; `checkm8()` now retries it up to 3 times
     on failure instead of giving up on the first one. **Deliberately did
     not touch the actual low-level request sequence itself** — unlike the
     connection-management architecture, that's confirmed correct (see the
     config match above), and rewriting exploit-critical, hardware-timing
     code (this file's own header comment: "ported VERBATIM... not
     'improved' during conversion") on a guess, without being able to test
     it, is a real way to make things worse, not better.
     - **Still unresolved and likely a separate, hardware/environment-level
       problem, not something more software patience can fix**: a device
       that won't even show up in `lsusb` after a physical unplug/replug is
       gone at the kernel/host-controller level, not just slow to
       re-enumerate. This machine has no legacy EHCI controller at all —
       `lspci` shows only a Tiger Lake-LP Thunderbolt 4 controller and a
       500-series xHCI 3.2 controller — and `gaster`/`palera1n`'s own
       documented hardware caveats (AMD hosts having a "very low success
       rate" with checkm8; Apple-Silicon-Mac USB-C ports sometimes needing
       a plain USB hub in between to work around DFU-mode enumeration
       issues) are exactly this class of host-controller-specific quirk,
       just for a different vendor/OS combination — plausible that
       Thunderbolt/xHCI has a similar one here. If a full power-cycle of
       the Apple TV itself (not just a USB replug), a different cable, and
       a different physical port don't help, routing through a plain
       (non-Thunderbolt) USB hub is the next thing to try, per that same
       precedent.
   - **Neither the retry/patience fix above nor this cable/hub/power-cycle
     angle has been re-verified against real hardware yet.**
   - The retry/patience fix was tried against real hardware and **did not
     fix it** — the device still didn't reappear. Prompted a look at
     switching the whole exploit backend to shell out to a proven external
     binary instead, the way `palera1n` itself does, rather than continuing
     to harden this file's own USB state machine blind. Two real
     candidates, and they are *not* the same tool:
     - `palera1n`'s `legacy` branch (the shell-script one, targeting iOS
       15-16 on `A8`-`A11`) shells out to a separate `gaster` binary
       (`"$dir"/gaster pwn`) for the actual DFU pwn step — this is the one
       compared against above, and it's a small, MIT-licensed, fully open
       C source file.
     - `palera1n`'s current `main` branch (the C rewrite everything ships
       from today, including third-party wrappers like
       `roothide/Palera1n-roothide`) does **not** use `gaster` at all —
       `src/exec_checkra1n.c` downloads the real, official, **closed-source**
       `checkra1n` release binary from `assets.checkra.in` at build time
       (`src/Makefile`: `curl -Lfo $@
       https://assets.checkra.in/downloads/preview/$(CHECKRA1N_VERSION)/...`),
       embeds it as a resource blob, and at runtime extracts it to a temp
       file and `posix_spawn`s it directly. The "exploit" in this path is
       the actual checkra1n team's own binary, not something portable or
       auditable — you can shell out to it, but you can't read or adapt its
       exploit logic the way `gaster`'s source allows.
     - **checkra1n cannot be used here at all, for either branch:
       confirmed no A5/`AppleTV3,2` support exists anywhere in that
       tool.** checkra1n's officially supported floor has always been `A7`
       (iPhone 5s) — `A5`/`A6` support (e.g. iPhone 4S) was requested as
       far back as December 2019 (`checkra1n/BugTracker#628`) and was
       never added. `palera1n`'s own README (both branches) states its
       device requirement as `A8`-`A11` on iOS/tvOS 15+; its documented
       Apple TV support is explicitly Apple TV HD (`A8`) and Apple TV 4K
       (`A10X`) only. `Palera1n-roothide` specifically scopes itself even
       further, to `A9`-`A11`/iOS 15-18 only (it drives the roothide
       Dopamine2/TrollStore chain, which needs an iOS15+-era kernel).
       None of this is a missing flag or config option — even if
       checkra1n's raw bootrom-exploit stage could technically still pwn
       an `A5` chip (the SecureROM bug it's built on does span `A5`-`A11`),
       its binary almost certainly hard-rejects unrecognized `cpid`s before
       trying, and its entire downstream chain (PongoOS payload, KPF,
       ramdisk) is compiled against iOS15+/tvOS12+ kernel offsets that
       don't exist for tvOS 7.x/8.4.x. Whatever reliability was observed
       running a `Palera1n-roothide` build against real hardware on this
       same machine, it cannot have exercised this device's exploit path —
       checkra1n would have refused the `AppleTV3,2` outright — so it's not
       usable evidence about this project's specific failure.
     - `gaster`, by contrast, **is confirmed to support this exact chip and
       firmware** — not just "the general `A5` family" by name, but this
       device's literal SRTG build string. `gaster.c`'s
       `checkm8_check_usb_device()` detects the target purely by matching
       the USB serial number's SRTG string against a table, with no
       device-name/product-type allowlist involved at all; the
       `" SRTG:[iBoot-1458.2]"` branch (this exact AppleTV3,2 iBoot) sets
       `cpid = 0x8947` plus a full set of real, non-placeholder gadget
       addresses (`dfu_handle_request`, `usb_core_do_transfer`,
       `payload_dest_armv7`, etc. — the `_armv7` naming, and the fact that
       upstream `0x7ff/gaster` ships dedicated
       `payload_handle_checkm8_request_armv7.S`/`.bin` files alongside the
       `A9`+ ones, both confirm `gaster` has real, deliberate 32-bit/`A5`-
       `A6`-class chip support, not just accidental overlap from an
       ARM64-focused tool). This is the strongest form of confirmation
       available short of running it: source-level, chip-specific, and
       independent of any device-support marketing list.
     - Conclusion: `gaster` remains the only viable "switch to a proven
       external implementation" path for this project. Shelling out to
       official `checkra1n` (either by hand-building it like `palera1n`
       does, or bundling a release) is not an option — it would simply
       refuse the device.
   - **Actually switched over.** The retry/patience fix (`get_tv_patient()`/
     `checkm8Attempt()`/`checkm8()` above) was tried against real hardware
     and did not fix the "device did not reappear" failure — confirming the
     problem was never about this port's own patience/retry architecture.
     `DeviceManager.cpp`'s `checkm8Attempt()` no longer drives the low-level
     USB request sequence in-process at all: it now shells out to the
     vendored `gaster` binary (`third_party/gaster`, a git submodule
     pointing at `verygenericname/gaster` directly — the exact fork
     `palera1n`'s `legacy` branch downloads, unmodified, no fork of our own
     needed) via a new `runGaster()` helper (fork/exec, combined
     stdout+stderr captured and relayed to the user on failure, since
     gaster never writes to stderr itself — everything comes out on
     stdout). `gaster pwn` replaces everything from "Configuring checkm8
     exploit" through "Executing payload" in one call; `checkm8Attempt()`
     still independently reconnects and checks for `"PWND:["` in the serial
     string afterward rather than trusting gaster's exit status alone. On
     failure, `gaster reset` is run for cleanup (matching palera1n's own
     `_pwn()`). `runGaster()` enforces its own timeout (180s for `pwn`, 15s
     for `reset`; SIGTERM then SIGKILL) — necessary because gaster's own
     wait-for-device and pwn-retry loops are genuinely unbounded, and this
     project's CLI still needs to eventually report a real failure instead
     of hanging forever if the device is truly gone. `checkm8()`'s own
     3-attempt outer retry is kept as a secondary safety net, though it's
     expected to matter much less now that gaster's own internal state
     machine already retries unboundedly within a single `pwn` call.
     The old hand-ported exploit code (`usb_req_stall`/`usb_req_leak`/
     `usb_req_no_leak`, `get_exploit_configuration()`/
     `get_payload_configuration()`, the `checkm8_config_t` struct and
     `S518947X_OVERWRITE` macro in `DeviceManager.hpp`, and the
     `#include "checkm8.h"` payload-data header) is deleted outright, not
     kept-but-unused — its exploit parameters were already confirmed
     byte-identical to gaster's own equivalents before this switch, so
     there's nothing left to reference it for.
     - **Build**: `third_party/gaster` is compiled directly by this
       project's own `CMakeLists.txt` (a new `add_executable(gaster ...)`
       target, landing in the same output directory as `blackb0x` itself,
       built automatically as part of the default `all` target), not via
       gaster's own Makefile. The one snag: gaster's `libusb` Makefile
       target links real OpenSSL (`-lcrypto`) purely for the one `EVP_*`
       AES-256-CBC call `gaster_decrypt_file()`/`gaster_decrypt_kbag()` use
       (neither of which `checkm8Attempt()` ever invokes — it only runs
       `pwn`/`reset` — but the symbols still have to resolve at link time
       regardless, single translation unit). Rather than vendor real
       OpenSSL just for that, `gaster.c`'s bare `#include <openssl/evp.h>`
       is redirected through this project's existing wolfSSL
       OpenSSL-compatibility shim instead — the exact same
       `OPENSSL_INCLUDE_DIR`/`-include .../wolfssl/options.h` treatment
       `xpwn`/`dmg` already get, confirmed to work with zero source
       changes to `gaster.c` at all (wolfSSL was already built with
       `--enable-opensslall`). One real bug hit and fixed along the way:
       `xxd -i`'s emitted C variable name is derived from its *input path*,
       not the output filename — passing the full absolute
       `third_party/gaster/payload_A9.bin` path (as originally written)
       produced a mangled name like
       `_var_home_..._third_party_gaster_payload_A9_bin` instead of the
       bare `payload_A9_bin` `gaster.c`'s own `#include "payload_A9.h"`
       expects, which compiled fine but failed at link time with six
       `undefined reference to 'payload_*_bin'` errors. Fixed by running
       `xxd` with `WORKING_DIRECTORY` set to `third_party/gaster` and a
       bare relative filename, matching how gaster's own Makefile invokes
       it.
     - **Not yet verified against real hardware.** Confirmed cleanly:
       `gaster` itself builds, links statically against the same vendored
       libusb/wolfSSL as the rest of this project (`ldd build/gaster` shows
       only libc/libm), and runs (`--help`-equivalent usage banner prints
       correctly with no device attached). `blackb0x` itself still builds
       clean against the rewritten `DeviceManager.cpp` with no new
       warnings, `ldd build/blackb0x` is unchanged, and `--dry-run` still
       works. None of this exercises the actual exploit path, though — the
       real test is still an actual checkm8 attempt against the real
       AppleTV3,2.
   - **First real-hardware test: `gaster pwn` got stuck in an
     uninterruptible kernel USB wait (`D` state) after checkm8 ran and the
     device rebooted.** Investigated live, on the actual stuck process,
     rather than guessing: `ps` showed the `gaster pwn` child in state `Dl+`
     (`D` = uninterruptible sleep — the one process state SIGKILL cannot
     terminate; the kernel only wakes it when whatever blocking call it's
     in returns on its own). `lsusb`/`lsusb -v` run fresh, independently,
     against the exact same device at the exact same time worked fine and
     returned real descriptor data (`idVendor 0x05ac`, `iSerial "Apple
     Mobile Device (DFU Mode)"`) — so this was never "the bus/device is
     dead," specifically gaster's own already-open handle/session was
     stuck on something a brand new libusb session wasn't. This pointed at
     a real, separate bug in `runGaster()` itself, found by inspection
     right after: its post-SIGKILL cleanup called a *blocking*
     `waitpid(pid, &status, 0)`, meaning as soon as a child ever landed in
     `D` state, the intended timeout-and-move-on behavior didn't actually
     work — it just relocated the hang from `gaster` into `blackb0x`
     itself, silently, with no diagnostic at all. Fixed: `runGaster()` now
     gives `SIGTERM`/`SIGKILL` a short *bounded* grace period (polled via
     non-blocking `waitpid(..., WNOHANG)`, never a blocking wait), and if
     the child still hasn't been reaped, checks `/proc/<pid>/status` for
     `D (disk sleep)` and reports that specifically — "stuck in an
     uninterruptible kernel USB wait, can't be killed by software, try
     physically unplugging the Apple TV" — instead of a generic timeout
     message, then moves on regardless (the orphaned child is left to be
     reaped whenever/if its blocking syscall ever actually returns).
   - **`runGaster()` now streams gaster's stdout/stderr straight through to
     blackb0x's own stdout/stderr live, verbatim**, instead of buffering
     silently and only dumping on failure — genuinely useful for exactly
     the class of problem above: a stuck run is something worth watching
     happen in real time, not reconstructing after the fact. Implemented
     with two separate pipes (not one merged one, so gaster's stdout and
     stderr — currently identical, since gaster itself never writes to
     stderr, but not guaranteed to stay that way — map onto blackb0x's own
     correctly) and `poll()` instead of a fixed busy-sleep, since there are
     now two file descriptors to watch instead of one.
   - **`checkm8Attempt()` now checks whether the device is already in
     pwned DFU before ever invoking gaster at all** (a quick, one-shot
     `get_tv()`/`"PWND:["` check, same as the post-pwn verification, just
     run first). Directly motivated by the `D`-state incident above: if
     gaster's pwn actually succeeds and the device reboots into pwned DFU,
     but gaster's own reconnect-and-verify step is what gets stuck
     afterward, the exploit itself already worked — retrying it from
     scratch (this project's own 3-attempt outer retry, or a fresh CLI
     invocation after killing a stuck one) would otherwise re-run the whole
     spray/patch sequence pointlessly against an already-pwned device
     instead of just noticing it's done and moving on.
   - **Checked whether gaster's cpid 0x8947 configuration actually matches
     the original (`DeviceManager.m`) checkm8 implementation this whole
     project is ported from** — it does not, but not because of a mistake:
     they're two independently-written, fundamentally different
     *techniques* for exploiting the same checkm8 bootrom bug. The
     original's `case 0x8947:` (`DeviceManager.m` ~line 554) sets exactly
     two numbers — `large_leak`/`overwrite_offset` — for the classic
     "spray heap holes, overwrite a fixed byte pattern at a computed
     offset, execute an uploaded payload blob" approach (this is what
     `checkm8Attempt()` itself did before the switch to gaster, and those
     two numbers were already confirmed byte-identical to gaster's own
     `config_large_leak`/`config_overwrite_pad` for this same cpid).
     gaster's own `checkm8_check_usb_device()` entry for `0x8947` sets a
     completely different kind of data instead — hardcoded *gadget
     addresses within this specific iBoot build* (`dfu_handle_request`,
     `usb_core_do_transfer`, `aes_crypto_cmd`, `memcpy_addr`,
     `payload_dest_armv7`, etc.) — because gaster's technique for
     non-A9-class armv7 chips (confirmed via `gaster.c`'s own dispatch
     logic: cpid `0x8947` falls through to the generic
     `payload_notA9_armv7_bin` + `payload_handle_checkm8_request_armv7_bin`
     path, not a per-chip special case) is to directly patch live iBoot
     function pointers at those addresses, not spray-and-overwrite. Neither
     approach is "the config"; they're not directly comparable
     number-for-number beyond the two that already matched.
     **More relevant than the config comparison**: this project's own
     original port of the classic spray-and-overwrite approach — before
     gaster ever entered the picture — hit the *exact same class of
     symptom* (device not reappearing cleanly after "Executing payload")
     that prompted the whole gaster investigation in the first place. That
     was never resolved by patience/retry logic either. Combined with the
     live `D`-state evidence (a kernel-level USB syscall not returning,
     not a userspace logic bug in either implementation), the more likely
     explanation is a Linux kernel/host-controller-level issue with how
     this specific machine's USB stack handles a device that resets itself
     off the bus mid-transaction (exactly what SecureROM payload execution
     does) — independent of which checkm8 implementation triggers it. This
     doesn't rule out a gaster-specific bug, but it means switching
     exploit backends again would not obviously fix this class of failure
     if it's truly host-controller-level.
     **Still unresolved, still not confirmed working end-to-end.**
   - **The "live streaming" fix above was actually blind the whole time —
     found on the very next real attempt.** A fresh run with all the fixes
     above showed `"Exploiting with checkm8"` → `"Disconnected (...)"` →
     `"checkm8: gaster pwn timed out waiting for the device."` with *zero*
     gaster output in between, timeout to timeout, despite `runGaster()`
     streaming its pipes live. Root cause: glibc's stdio only line-buffers
     stdout/stderr when attached to a terminal (`isatty()`); attached to a
     pipe — exactly what `runGaster()` does — it silently switches to full
     block buffering instead, and `gaster.c` never calls
     `setvbuf()`/`fflush()` itself. A short-lived invocation (no args,
     prints usage, exits) looks fine either way since `exit()` flushes
     stdio regardless — confirmed directly, this is exactly why it wasn't
     caught in the earlier "does gaster run" sanity check — but a `gaster
     pwn` that runs a long time before ever exiting (precisely the "stuck"
     case this whole timeout/D-state mechanism exists for) never flushes
     its `"Stage: RESET"`/`"Stage: SETUP"`/etc progress lines to the pipe
     at all. Fixed by prefixing the exec with `stdbuf -oL -eL` (GNU
     coreutils; `LD_PRELOAD`s a constructor that calls `setvbuf()` before
     `gaster`'s own `main()` runs — confirmed directly: without it, a test
     run's output only appeared after the process exited; with it, output
     — including a `"[libusb] Waiting for the USB handle with VID: 0x5AC,
     PID: 0x1227"` line never seen before — appeared within 0.5s), with a
     fallback to exec'ing `gaster` directly if `stdbuf` isn't installed.
     This means every previous real-hardware attempt in this file's own
     history was diagnosed without ever actually seeing which of
     `gaster_checkm8()`'s four stages (RESET/SETUP/SPRAY/PATCH) it was
     stuck in — the D-state hang could have been happening before the
     exploit payload ever ran, not just after, and there was no way to
     tell. The next real attempt is the first one that will actually show
     this.
   - **Conclusion, with the buffering fix's real visibility: this is a
     host-machine/USB-controller-level limitation, not an exploit bug in
     either implementation, and not fixable by more software patience.**
     A real run's live stage output (`RESET`/`SETUP`/`SPRAY`/`PATCH`)
     showed the exploit payload itself genuinely executing successfully
     (`Stage: PATCH` / `ret: true` — the furthest ever confirmed, on either
     implementation) — but the *reconnect* immediately after read the
     device as not-yet-pwned, then the next stage (a mundane
     "re-prime the device into a known DFU state" step, not exploit code)
     failed outright, and reconnecting after *that* hung again.
     Confirmed directly on repeat attempts: which stage it hangs after is
     inconsistent — sometimes `SETUP` (before any exploit code has run at
     all), sometimes `PATCH` — meaning the failure isn't tied to anything
     the exploit payload does; it's this host's USB stack unreliably
     handling the reset-close-reopen cycle gaster performs after *every*
     stage transition, regardless of which stage triggered it. Combined
     with the earlier confirmed genuine kernel-level `D`-state hang (not a
     userspace logic bug) and this machine's all-Thunderbolt/xHCI USB
     topology (no legacy EHCI at all, confirmed via `lspci`), this is
     conclusive enough: two independent, differently-implemented checkm8
     exploits both hit the identical failure class at the identical
     conceptual boundary (device-initiated USB reset → reconnect), at
     genuinely random points in the sequence. No further software fix
     inside this project — more retry patience, a different exploit
     backend, deeper protocol changes — is expected to help; the retries
     already happen (gaster's own loop is unbounded and self-healing) and
     still don't reliably converge. **Tried the one cheap mitigation this
     pointed at — routing through a plain, non-Thunderbolt USB hub — twice,
     with two structurally different hubs (see the "Known machine" entry in
     "Environment notes" above for both hardware probes: a Microchip
     USB2807/USB5807-based Dell dock, and a Terminus/VIA Labs plain hub) —
     and it did not help either time.** With direct connection, Thunderbolt
     routing, and two different hub topologies all hitting the identical
     device-reset/reconnect failure, this is conclusively a real limitation
     of this specific machine's USB controller/driver stack for
     checkm8-class work, not a topology quirk a hub can route around. The
     practical fix is running `blackb0x` from different hardware entirely —
     ideally one with a legacy EHCI controller or a known-good xHCI
     implementation for this kind of workload — not further changes to this
     codebase.
   - **SUPERSEDED — the above was a single-machine conclusion; a real,
     fixable software cause was found once the same hang reproduced
     identically across multiple, unrelated Linux machines.** Running the
     vendored `gaster` binary directly (`sudo ./build/gaster pwn`, bypassing
     `blackb0x`/`runGaster()` entirely) hung again — this time captured live
     with `dmesg -w` running in a second terminal at the exact moment it
     froze, right after `Stage: SETUP` / `ret: true`, before the next
     stage's own `[libusb] Waiting for the USB handle...` line ever
     printed. The kernel log showed the real cause directly, not inferred:
     ```
     apple-mfi-fastcharge 3-3: usbfs: process 402495 (gaster) did not claim interface 0 before use
     apple-mfi-fastcharge 3-3: reset high-speed USB device number 29 using xhci_hcd
     ```
     followed by a disconnect/reconnect storm with garbled descriptors
     (`Product: Љ`) and `-71`/`-110`/`-75` USB errors. `apple_mfi_fastcharge`
     is a real, in-tree, commonly-autoloaded Linux driver (`drivers/usb/misc/
     apple-mfi-fastcharge.c`, upstream author Bastien Nocera) for Lightning
     fast-charge negotiation — confirmed present and loaded
     (`modinfo apple_mfi_fastcharge`) with alias
     `usb:v05ACp*d*dc*dsc*dp*ic*isc*ip*in*`. A real upstream fix,
     [`USB: apple-mfi-fastcharge: don't probe unhandled devices`](https://lkml.iu.edu/hypermail/linux/kernel/2011.0/03254.html),
     added a `mfi_fc_match()` check restricting probing to product IDs
     `0x1200`-`0x12ff` — but this device's real DFU-mode PID is `0x1227`
     and its normal-mode PID is `0x12a7`, both squarely inside that
     "fixed" range. So even a fully up-to-date, upstream-patched kernel
     still auto-binds this driver to the Apple TV whether it's in DFU mode
     or not. `gaster` never calls `libusb_claim_interface()` (or any
     kernel-driver-detach equivalent) — it talks straight to the default
     control pipe via raw `libusb_control_transfer()` calls — so this
     driver stays attached the whole time and independently calls its own
     `usb_reset_device()` whenever gaster's raw, exploit-timing-sensitive
     transfers confuse it. Two independent actors (gaster's own
     `reset_usb_handle()` calls after every stage, and this driver's own
     recovery-triggered resets) issuing USB resets against the same device
     at once is exactly the kind of race that corrupts enumeration and can
     wedge the xHCI command ring hard enough to produce the previously
     observed D-state hangs — this is very likely the same underlying
     mechanism as the "device-initiated reset" theory above, just with the
     actual second actor identified instead of blamed on the host
     controller itself. Explains the multi-machine reproducibility
     directly: this module ships in essentially every mainstream
     distro kernel, unrelated to any one machine's chipset/topology —
     the earlier single-machine hub/Thunderbolt-routing investigation
     never had a chance to rule this out, since nothing on that machine's
     side varied it.
     - **Not yet re-verified against real hardware.** The fastest
       available manual confirmation
       (`sudo modprobe -r apple_mfi_fastcharge && sudo ./build/gaster pwn`,
       safe since the module's refcnt was 0 — nothing else on the test
       machine was actively using it) had not been reported back as of this
       writing.
     - **Two code-based fixes were tried in `DeviceManager.cpp` and then
       both abandoned, per explicit user direction, in favor of a
       documented manual step instead — the same pattern already used for
       `usbmuxd --no-preflight`.** Worth keeping the trail since the
       reasoning that ruled each one out is real, even though none of this
       code exists in the tree anymore:
       - **First attempt (wrong): a bare `modprobe -r` up front, nothing
         else, in a small RAII guard scoped around `checkm8Attempt()`.**
         Tried against real hardware — the module came right back on its
         own before the exploit got anywhere. Root cause: every one of
         gaster's stage-transition reconnects fires the kernel's own
         module-autoload path (`request_module()`, driven by the
         re-enumerating device's USB modalias — the same mechanism whether
         the kernel invokes `modprobe` directly via a usermode helper or
         udev does it on the kernel's behalf), completely independently of
         anything either blackb0x or gaster does in userspace. Removing an
         already-loaded instance once does nothing to stop the very next
         reconnect from loading it straight back in — a single sysfs
         interface-unbind *before* invoking gaster was also considered and
         rejected for the identical reason (gaster's own internal
         RESET→SETUP→SPRAY→PATCH state machine reconnects several times
         *inside one `gaster pwn` call*, with no way for code outside that
         subprocess to repeat a per-stage unbind in between).
       - **Second attempt (also abandoned): the RAII guard writing a
         temporary `/etc/modprobe.d` `blacklist` entry itself** (removed
         on construction along with an already-loaded, currently-unused
         instance; both restored in the destructor on every exit path). A
         `blacklist` directive is the right *mechanism* — `modprobe`/
         `libkmod` honor it specifically for *automatic*, alias-triggered
         loads while still allowing an *explicit* `modprobe
         apple_mfi_fastcharge` to work — but having `blackb0x` itself
         silently write and delete a system-wide `/etc/modprobe.d` file at
         runtime was the wrong place to put that mechanism: not
         signal-safe (a hard `SIGKILL`, or an uncaught `SIGINT`, skips the
         destructor and can leave the blacklist file and/or the removed
         module behind), and inconsistent with how this exact class of
         problem is already handled elsewhere in this project.
       - **Final decision: document it as a one-time manual setup step,
         exactly like `usbmuxd --no-preflight`, and add no code at all.**
         `blackb0x` never checks for or manages the daemon flag that
         `usbmuxd --no-preflight` requires either — that's a documented
         README/AGENTS.md prerequisite the user sets up once, not
         something enforced at runtime — and this driver conflict is the
         same shape of problem: a system-level configuration issue, not
         something a single exploit run should be silently patching system
         state to work around. README.md gained a new "One-time system
         setup: blacklist `apple_mfi_fastcharge`" section (mirroring the
         `usbmuxd` one immediately below it) with the exact
         `/etc/modprobe.d/blacklist-apple-mfi-fastcharge.conf` +
         `modprobe -r` commands; AGENTS.md's runtime-requirements list and
         "Current status" section were updated to match.
     - **Tested against real hardware with the module actually blacklisted
       — confirmed real, confirmed insufficient on its own.** `lsmod`
       during the test showed `apple_mfi_fastcharge` genuinely not
       loaded (the blacklist worked as designed — no reload, unlike the
       bare-`modprobe -r` attempt above), and no `gaster` process was left
       hung afterward. **The identical corruption still happened anyway**:
       a fresh `sudo journalctl -k` capture during the same `sudo
       ./build/gaster pwn` run showed the exact same
       "usbfs: process ... (gaster) did not claim interface 0 before use"
       → "reset high-speed USB device ... using xhci_hcd" pair (twice,
       once per completed stage), followed by the same disconnect/garbled-
       re-enumeration storm (`error -90`, `config 1 has 0 interfaces`,
       `can't set config #1, error -110`) — except this time every one of
       those kernel log lines is attributed to the generic `usb` bus name
       instead of `apple-mfi-fastcharge`, direct confirmation that no
       competing driver is involved anymore. `gaster` hung again at the
       same point (`Stage: SETUP` / `ret: true`, then `wait_usb_handle()`
       for `SPRAY` never finding the device again), and the device was
       left sitting in the same corrupted state observed live afterward
       (`lsusb -v -d 05ac:1227`: `iManufacturer`/`iProduct` still read
       garbled two-byte strings, `Couldn't open device`) — it never
       actually recovered on its own, matching the "needs a physical
       unplug/replug" recovery already documented above.
       **Conclusion: the `apple_mfi_fastcharge` conflict was real, and the
       blacklist genuinely fixes that specific conflict, but it was never
       the (sole) root cause of this hang.** With it conclusively ruled
       out, the corruption is happening purely between `gaster`'s own
       control-transfer usage (interface-recipient requests without ever
       claiming the interface — triggering the "did not claim" warning on
       its own, independent of any driver) and its own unconditional
       post-stage `reset_usb_handle()`/`libusb_reset_device()` call
       colliding with however this host's USB core/xHCI driver
       reinitializes the device afterward. This is genuinely the same
       failure class the very first (pre-`apple_mfi_fastcharge`,
       single-machine) investigation described — that conclusion was
       correct as far as it went; the `apple_mfi_fastcharge` finding was a
       real, additive discovery (worth keeping — it's one less variable in
       the way) but not the fix for the underlying problem.
     - **`gaster` forked to test the host-side-reset theory directly.**
       Per this project's own convention (fork a vendored dependency rather
       than carry a local patch — see "Vendored dependencies" in
       `AGENTS.md`), forked to **regulad/gaster**, `linux-reset-race`
       branch, off the exact commit already pinned
       (`d4b98423ee92935ba04646ea41a1ed74a27202a1`). `.gitmodules`'s
       `third_party/gaster` entry now points at that fork/branch instead of
       `verygenericname/gaster` directly.
       - **The actual change**: `gaster_checkm8()`'s state machine
         (`RESET → SETUP → SPRAY → PATCH`) calls the same
         `reset_usb_handle()` (a host-triggered `libusb_reset_device()`)
         unconditionally after every single stage, win or lose. Reading
         each stage's own ending sequence shows this isn't uniformly
         necessary: `checkm8_stage_reset()` and `checkm8_stage_patch()`
         both explicitly put the device into `DFU_STATE_MANIFEST_WAIT_RESET`
         first (via `dfu_set_state_wait_reset()`/`dfu_check_status()`), a
         real DFU-protocol state that genuinely requires a bus reset to
         advance out of — for those two, the reset is doing necessary work.
         `checkm8_stage_setup()` and `checkm8_stage_spray()` are pure
         host-side control-transfer races (heap-hole timing tricks) whose
         own last request is a plain STALL probe / `DFU_CLR_STATUS` — no
         protocol-mandated reset-wait state at all. The corruption observed
         above happened at exactly this boundary: right after
         `Stage: SETUP` / `ret: true`, before `SPRAY`'s own
         `wait_usb_handle()` ever found the device again. The fork adds a
         `completed_stage` variable (capturing which stage just ran, before
         the existing code advances `stage` to the next one) and skips the
         `reset_usb_handle()` call specifically when the just-completed
         stage was `SETUP` or `SPRAY` *and* it succeeded — a failed stage
         still falls back to `STAGE_RESET` and still gets the real reset,
         unchanged from upstream, since that recovery path genuinely wants
         a clean device state before retrying from scratch. A 26-line diff
         against upstream, touching only the state machine's control flow —
         no change to any of the actual exploit request sequence, payload,
         or timing values themselves, per this file's own repeated caution
         about not touching verified-correct exploit-critical code.
       - **Compiles clean**: built standalone with the exact `xxd -iC`
         payload-header generation `CMakeLists.txt` uses (same working
         directory + bare relative filename convention, to get the same
         `payload_*_bin` variable names `gaster.c` expects) and `gcc -c`
         against the already-built `build/deps/include` tree, matching the
         real `add_executable(gaster ...)` target's own include/define
         flags (`-DHAVE_LIBUSB`, wolfSSL's `options.h` force-included, the
         same `libusb-1.0` include path).
     - **Second fix on the same fork/branch, same real root cause the
       `apple_mfi_fastcharge` investigation surfaced but only partly
       addressed: `gaster` never actually claims the USB interface.**
       Every DFU class request this file sends (`bmRequestType` `0x21`,
       recipient = interface) went out over usbfs with interface 0
       unclaimed — this, not any specific competing driver, is what the
       kernel's own "usbfs: process ... (gaster) did not claim interface 0
       before use" warning was reporting, on every single real-hardware
       capture in this whole investigation, `apple_mfi_fastcharge` bound
       or not. `wait_usb_handle()` now calls
       `libusb_set_auto_detach_kernel_driver(handle->device, 1)` right
       after opening the device (Linux-only, a documented no-op on other
       platforms) and then `libusb_claim_interface(handle->device, 0)`
       before running `usb_check_cb`/returning the handle; the interface
       is released again on both the "wrong device" retry path and in
       `close_usb_handle()`. Two effects: proper libusb usage (interface
       claimed for every request that needs it, matching normal libusb
       API contract instead of relying on usbfs's permissive-but-warned
       unclaimed-request fallback), and — via
       `libusb_set_auto_detach_kernel_driver()` — automatic detach/
       reattach of any kernel driver bound to that interface (exactly
       `apple_mfi_fastcharge`'s case) scoped to precisely the claim/
       release window, at the code level, rather than depending solely on
       the driver being blacklisted system-wide ahead of time. The
       existing README/AGENTS.md blacklist documentation is left in place
       rather than removed on the strength of this alone — auto-detach
       should make it redundant in theory, but that's exactly the kind of
       claim this file's own history says not to trust without a real
       test.
     - **Compiles and links clean** (rebuilt standalone the same way as
       the previous fix, same flags), still fully statically linked
       (`ldd`: only `libc`/`libm`/`ld-linux`).
     - **Tested against real hardware with both fixes together — the
       `SETUP`/`SPRAY` reset-skip is confirmed wrong, and reverted.** This
       run showed neither the earlier corruption nor any kernel log noise
       at all after `Stage: SETUP` / `ret: true` — just silence, and
       `gaster` hanging in the same place. Direct proof it was a genuine
       kernel-level hang, not a userspace retry loop: `ps` showed `Dl+`
       (confirmed `D`, uninterruptible), and — since I had shell access to
       the same physical test machine for this investigation —
       `sudo cat /proc/<pid>/stack` gave an exact kernel stack instead of
       another guess:
       ```
       usb_start_wait_urb
       usb_control_msg
       usb_reset_configuration
       usbdev_do_ioctl
       __x64_sys_ioctl
       ```
       `usb_reset_configuration()` is the kernel-side handler for
       `USBDEVFS_SETCONFIGURATION` — i.e. this is `libusb_set_configuration()`
       inside the *next* `wait_usb_handle()` call (for `SPRAY`), blocked
       forever on a `SET_CONFIGURATION` control URB that never completes.
       `lsusb -v` at the same moment showed clean, uncorrupted descriptors
       this time (unlike the earlier `apple_mfi_fastcharge`-era runs) —
       the device is sitting there looking fine at the descriptor-cache
       level, but its actual USB peripheral controller has stopped
       responding to this specific control request.
       **Conclusion: the post-`SETUP` reset was never merely
       precautionary DFU-protocol cleanup — it's load-bearing at the
       hardware level.** `checkm8_stage_setup()`'s technique (aborting
       async control transfers mid-flight via `libusb_cancel_transfer()`
       to manipulate heap timing) appears to leave the device's own USB
       peripheral hardware unresponsive to further standard requests
       until a real bus reset clears it — independent of any DFU protocol
       state, which is exactly the angle the original reasoning for this
       fix missed. Reverted the reset-skip entirely (`gaster_checkm8()`
       back to upstream's unconditional post-stage `reset_usb_handle()`
       call for every stage); the interface-claim/auto-detach fix from
       the previous commit stays, since it's independently justified.
     - **Tested against real hardware in this reverted (claim-fix-only)
       form — one real, permanent improvement confirmed, the core
       reconnect corruption still not fixed.** The improvement:
       `/proc/<pid>/stack` this time showed
       `hrtimer_nanosleep`/`common_nsleep`/`__x64_sys_clock_nanosleep` —
       the ordinary `sleep_ms()` between `wait_usb_handle()`'s poll
       attempts, not a kernel-level wedge. The interface-claim fix
       genuinely eliminated the unkillable `D`-state hang; this run
       livelocked in an ordinary retry loop instead, which at least means
       the process itself stays responsive. The core problem persisted
       anyway: `dmesg` showed the identical corruption class as the very
       first (pre-any-fix) capture — two resets against the same device
       number right after `RESET` and `SETUP` complete, then 10 seconds
       later a real `"device firmware changed"` + disconnect, then a
       cascade of `error -110`/`error -75`/"config 1 has 0 interfaces"
       re-enumeration failures. **Confirmed it never recovers on its
       own** — left to run, it stayed in this state rather than
       eventually settling into a clean reconnect.
     - **Conclusion: three independent, real, confirmed software causes
       have now each been ruled out one at a time
       (`apple_mfi_fastcharge`, the SETUP/SPRAY reset-skip theory, and
       unclaimed interfaces), and the identical corrupted-reconnect
       symptom survived the elimination of every one of them.** Two of
       the three fixes are real, permanent improvements worth keeping
       regardless (no more competing kernel driver, no more unkillable
       `D`-state hang, no more "did not claim interface" warnings) — but
       none of them, together or separately, fixed the actual exploit
       hang. This converges strongly on the same conclusion the very
       first single-machine investigation reached, now corroborated by
       controlled elimination of every specific software mechanism this
       session could identify and test, on top of the original report
       that this exact symptom reproduces across multiple different
       Linux machines: **this looks like a genuine host-side (kernel/
       xHCI) limitation reinitializing this specific device after a
       reset performed mid-exploit, not a userspace software bug in
       either `gaster` or this project.** Further blind changes to
       `checkm8_stage_setup()`/`checkm8_stage_spray()`'s own request
       timing are not recommended without a new, specific mechanism to
       test — this file's own repeated lesson about not guessing at
       verified-correct exploit-critical code applies in full now that
       three good-faith guesses have each been individually disproven
       rather than confirmed.
     - **A real bug in the interface-claim fix itself, found by directly
       validating the two things actually in question rather than
       accepting the "host limitation" conclusion at face value**: (1)
       whether libusb's Linux sysfs-based device matching (this project
       builds libusb with `--disable-udev`, falling back to the
       `linux_netlink.c` backend for hotplug — see `CMakeLists.txt`'s own
       comment) actually finds this exact VID/PID pair, and (2) whether
       the device genuinely still exists on the bus at the moment
       `gaster` is stuck. Verified both directly: wrote a standalone probe
       program (built, run, and deleted — not part of the permanent
       tree), linked against the exact same static `libusb-1.0.a` this
       project already builds, calling the identical
       `libusb_open_device_with_vid_pid()` sequence `wait_usb_handle()`
       uses. Run live, with shell access to the same physical test
       machine, **while `gaster` was actively stuck waiting**: the probe
       found and opened the device on the very first call, and
       `libusb_get_configuration()`/`libusb_set_configuration(1)` (the
       exact call previously seen wedged in `D` state) both succeeded
       immediately. This directly confirms sysfs-based matching works
       correctly and the device is genuinely present and at least
       partially responsive — ruling out "libusb can't find/open the
       device" as an explanation.
       - `libusb_claim_interface(handle->device, 0)`, however, failed
         with `LIBUSB_ERROR_INVALID_PARAM` — libusb only returns that
         when the interface number doesn't exist in the device's cached
         config descriptor. `lsusb -v -d 05ac:1227` at the same moment
         confirmed why: a genuinely truncated configuration descriptor
         (`bLength 9`, `wTotalLength 0x0019`, `bNumInterfaces 0` —
         missing its interface descriptor entirely), stable and
         unchanged across several minutes of no exploit activity at all,
         plus empty/unreadable string descriptors. Real, persistent
         device-side descriptor corruption following `SETUP`'s heap-race
         technique, not a momentary glitch.
       - **The actual bug**: the previous commit's `wait_usb_handle()`
         *gated* success on `libusb_claim_interface()` succeeding — but
         upstream `gaster` never claims the interface at all, so it never
         had this dependency. With the config descriptor genuinely
         showing zero interfaces, the claim fails every single time,
         which meant `wait_usb_handle()` closed the handle and retried
         *without ever reaching `usb_check_cb()`* (i.e.
         `checkm8_check_usb_device()`'s own serial-string/cpid check) at
         all — turning a possibly-transient, exploit-related descriptor
         state into an unconditional, permanent "device not found," on
         top of whatever the string-descriptor corruption would have
         caused on its own.
       - **Fixed**: `wait_usb_handle()` now calls
         `libusb_get_active_config_descriptor()` and only attempts
         `libusb_claim_interface()` when `bNumInterfaces > 0`; either way
         it proceeds to `usb_check_cb()` regardless of whether the claim
         happened, matching upstream's original permissiveness for
         exactly the case where claiming isn't possible. Every DFU class
         request this file sends was, is, and remains interface-recipient
         (`bmRequestType` `0x21`) regardless of whether the claim
         succeeds — claiming when possible only fixes the kernel's own
         "did not claim interface 0" warning and enables auto-detach for
         a conflicting kernel driver; it was never required for those
         requests to actually go out. `usb_handle_t` gained an
         `interface_claimed` flag so `close_usb_handle()` only releases
         what was actually claimed (calling
         `libusb_release_interface()` on an unclaimed interface is a
         guaranteed error, not a harmless no-op).
       - **Compiles and links clean**, still fully statically linked.
         **Not yet re-verified against real hardware** — the actual test
         is whether `checkm8_check_usb_device()` now gets a chance to run
         against the still-corrupted-descriptor device, and if so,
         whether its own string-descriptor read succeeds or fails for
         the same underlying reason. If it fails too, that would show
         this exact corruption (not just the interface-claim gate) is
         what's actually blocking progress in every variant tried so
         far, upstream included — a materially different, narrower
         conclusion than "host-controller limitation."
5. **Phase 7 — packaging.** The end goal is deliberately minimal: clone the repo
   (with binary assets), build the static executable, run it. Resource-path
   resolution for `Blackb0x/Files/*` and a README rewrite (the CLI's
   self-identification banner is `"blackb0x (regulad's linux port)"` — update the
   README to match, don't reintroduce the old `"Blackb0x (Linux port)"` phrasing)
   are the only real remaining work. **No udev rules, no install rules** — the tool
   runs entirely as root by design (see "Decisions already made" above), so there's
   no non-root USB permission story to build, and no reason to install it anywhere
   other than wherever it was built. **The README must also document the
   `usbmuxd --no-preflight` systemd drop-in** (see the "libimobiledevice was
   forked" bullet above for why it's required) as an explicit setup step — without
   it, this hardware's normal-mode discovery silently never fires, with no error
   surfaced anywhere in `blackb0x` itself to point a user at the actual cause.

**Hardware-gated checkpoints** (cannot be verified without a physical Apple TV 2/3 in
DFU/Normal mode over real USB): device enumeration via `libirecovery`, the actual
`checkm8`/`SHAtter`/`send*` exploit functions, and the full end-to-end jailbreak/
tether-boot flow. Everything else — dependency builds, networking, plist/DMG parsing,
CLI plumbing — can and should be verified on the Linux dev box with no device attached.
**The only physical unit available for any of this is an AppleTV3,2** — every
"verified against real hardware" claim elsewhere in this file is against that one
model (`checkm8`, the wolfSSL/SSLv3 lockdownd handshake, the real-usbmuxd/
`--no-preflight` path, AFC). `AppleTV2,1` (SHAtter) and `AppleTV3,1` (checkm8 via
external Arduino hardware, see `checkExploit()` in `Cli.cpp`) are implemented from
source/protocol analysis and the original app's own logic, not from a real device —
treat those two paths as unverified until someone tests on the actual hardware.

## Entrypoint injection point: backstepped from `/etc/rc.boot` to `/sbin/launchd`

The original (pre-rewrite) tool always spliced its PID-1 replacement into
`/sbin/launchd` (see `.claude/LEGACY_FLOW.md`'s Stage 1). Mid-rewrite, real
disassembly of an AppleTV2,1 10B809 `RestoreRamdisk` showed `/etc/rc.boot`
itself is LC_MAIN-entered directly by the kernel on that firmware — not
merely a preamble before `launchd` — so `bakeRamdisk()`'s
`spliceFileContentInPlace()` call was switched to target `/etc/rc.boot`
instead, on the theory that injecting at the true first entry point is
strictly better than injecting at `launchd`.

That assumption didn't generalize. A real bake against AppleTV3,1/AppleTV3,2
12H606 failed with `/etc/rc.boot does not exist on the mounted volume`.
Mounting that firmware's actual ramdisk and inspecting it directly (not
guessing) showed `/etc/` on that build is nearly empty (just empty `group`
and `master.passwd` files) — no `rc.boot` at all — while `/sbin/launchd`
does exist. So the 10B809 finding was real but firmware-specific, not a
property of these restore ramdisks in general: at least one later firmware
generation dropped `rc.boot` as a kernel-invoked stage entirely.

Reverted to always targeting `/sbin/launchd` universally — the same choice
the original tool made, and the one thing guaranteed to exist and be real
PID-1 across every firmware generation, rather than trying to detect or
special-case which injection point a given firmware uses. The ad-hoc
signing identity changed to match: `com.apple.launchd` (was `com.apple.rc`,
matching `/etc/rc.boot`'s own CodeDirectory identifier — no longer
applicable now that the splice target is `launchd` again).

## Reopening macOS support: root cause on `--allow-multiple-definition`, and gaster's own IOKit path

A long real-hardware session against AppleTV3,2 (ECID 2685369898254) kept
surfacing timing-dependent libusb/DFU-state races in `DeviceManager.cpp`'s
`boot_client()` (the soft-DFU iBSS uploader) — most recently, an
intermittent "boots into the real OS instead of continuing the exploit
chain" failure where neither always resetting the USB bus after the final
control transfer nor never resetting it were consistently correct (see
`boot_client()`'s own inline comment for the full account; the
state-conditional fix landed — reset only when the device's own GETSTATUS
reports DFU state 8/`dfuMANIFEST-WAIT-RESET` — is a real, spec-grounded fix
but wasn't confirmed to fully close out the intermittent failure by the time
this section was written). Rather than keep chasing what may be inherent
libusb-on-Linux timing behavior, `.claude/TODO.md`'s item 4 (macOS support,
previously scoped-but-parked) was reopened as the more promising direction:
run the real chain against gaster's and libimobiledevice's native,
first-party macOS frameworks instead of a Linux libusb path that's already
shown itself to be fragile against this exact device.

Two pieces of real, verified work came out of picking this back up (both
build clean on Linux — this session had no Mac to actually compile/run the
Apple-only branches on):

1. **The `-Wl,--allow-multiple-definition` link flag was a workaround for a
   real, understood conflict, not an unavoidable one.** `libusbmuxd`'s
   `common/collection.c` and `libimobiledevice-glue`'s `src/collection.c`
   both vendor an externally-linked copy of the same generic collection
   ADT. Diffed directly (not assumed): identical `struct collection`
   layout, byte-identical function bodies for every symbol libusbmuxd
   defines; glue's copy is a strict superset, adding one extra function
   (`collection_copy()`) that libusbmuxd's own code never calls. Since
   nothing depends on libusbmuxd's specific copy surviving, `libusbmuxd_ext`
   (`CMakeLists.txt`) now runs `ar d <installed libusbmuxd.a> collection.o`
   as an extra `INSTALL_COMMAND` step right after `make install`, so only
   glue's copy of the object ever reaches final link. This removes the
   duplicate-definition conflict at its source instead of suppressing the
   linker's complaint about it — confirmed the Linux build still links
   clean with the GNU-ld-only flag removed entirely. That flag had no
   Apple-linker equivalent as of Xcode 15+ (the old `-multiply_defined
   suppress` synonym is gone from the new linker), so this was a real
   blocker for macOS, not just untidy — now it's simply gone, on every
   platform.
2. **gaster already ships a real, upstream-maintained IOKit implementation
   — the build just never selected it.** `third_party/gaster/gaster.c`'s
   top-of-file includes are `#ifdef HAVE_LIBUSB` / `#else`: the `#else`
   branch pulls in `<CommonCrypto/CommonCrypto.h>` and
   `<IOKit/usb/IOUSBLib.h>` directly, no vendored dependency needed at all
   (both are always-present system frameworks on any Mac). This project's
   `CMakeLists.txt` was defining `HAVE_LIBUSB` unconditionally for the
   `gaster` target regardless of host OS, which — per the still-open TODO
   item this reopening started from — meant even a macOS build would have
   inherited whatever libusb's Darwin backend does, not gaster upstream's
   own native path. Now gated `if(APPLE) ... else() ... endif()`: Apple
   builds skip `HAVE_LIBUSB`/libusb/wolfSSL entirely and link `-framework
   CoreFoundation -framework IOKit`; Linux keeps the existing libusb+wolfSSL
   build byte-for-byte. Scoped deliberately narrow — this only changes
   which implementation gaster's own standalone `pwn`/`reset` exploit step
   uses; it does not touch libirecovery's own USB transport, which the rest
   of `DeviceManager.cpp` (`sendiBSS`/`sendiBEC`/etc.) still goes through on
   every platform, and whose Darwin behavior remains genuinely unverified
   (see `.claude/TODO.md` item 4's "needs real macOS hardware" list).

`ResourcePath.cpp`'s `resolveGasterPath()` (now `_NSGetExecutablePath()` +
`realpath()` behind `#ifdef __APPLE__`) and `DeviceManager.cpp`'s
`isUninterruptible()` (now a `ps -o state=`-based fork/exec+pipe check on
Apple, matching the file's existing no-`popen()`/no-`system()` convention,
instead of reading `/proc/<pid>/status`) and `runGaster()`'s `stdbuf`
requirement (now probes for `gstdbuf` — Homebrew coreutils' default
prefixed name — as a fallback when plain `stdbuf` isn't on `PATH`) were the
other three blockers already named in `.claude/TODO.md` item 4; all three
are mechanical platform bridging with no design decision of their own
worth recording here beyond what that TODO entry already says.
