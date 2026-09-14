# TODO

Open items not yet resolved. See `AGENTS.md` for repo conventions and
`docs/HISTORY.md`/`NEO_FLOW.md` for how we got here.

## 1. Pin down `untether.bin`'s exact build (mostly resolved)

`Blackb0x/Misc/tihmstar-untether.tar` — the last original tarball ever
checked into this repo — is gone now: the real, long-lost
`net.tihmstar.etasonuntether-1.3.1.deb` itself turned up (now in
`Blackb0x/Debs/`), and `BakeRamdisk.cpp`'s `stageEtasonatv()` extracts the
8.4 untether payload straight out of that `.deb` at bake time instead of
the tarball. Direct provenance was already confirmed before this switch —
see `Blackb0x/Misc/README.md`'s `## etasonATV / tihmstar-untether
provenance` section, `### Direct provenance, found via the "Home Depot"
lead`: tihmstar's own Cydia repo (`repo.tihmstar.net`) is still live, and
`untether/expl.js`, `usr/bin/orphan_commander`, and `etc/rc.d/daemonload`
are all byte-identical (MD5-verified) to files inside his real, current
`net.tihmstar.etasonuntether-1.3.1.deb` — so this switch changes the
*source* of those files from a checked-in tarball to the real package,
not their content.

What's still open — narrower than before, but not closed:

- `untether/untether.bin` (43,680 bytes, now kept standalone at
  `Blackb0x/Misc/untether.bin` — see that file's own README section) still
  doesn't match either currently published build: not tihmstar's real
  `etasonuntether-1.3.1`'s `untether.bin` (35,677 bytes, dated 2021-04-04
  in that package), nor `untetherhomedepot`'s (33,600 bytes, 2017). **The
  "intermediate official release" hypothesis is now ruled out**: Wayback
  Machine's crawl of `repo.tihmstar.net` shows only two
  `net.tihmstar.etasonuntether` releases ever existed (`1.3.0`, first
  crawled 2020-09-16 but internally dated 2017-09-25, and `1.3.1`,
  2021-04-04) — their `untether.bin`s are byte-identical (`sha256` match)
  to each other, so there was never a third, differently-built official
  release. Also newly characterized: our build's embedded kernel-banner
  table is all SoC `S5L8947X` (the real Apple TV 3 SoC) across four
  sequential tvOS 8.4.x point releases, while the officially-packaged
  `1.3.1` deb's own table never once mentions `S5L8947X` at all — it's
  five entries for five *other* SoCs (iPhone4S/iPad2/iPad3/iPod5/iPhone5-
  class), matching that deb's own control file describing it as a generic
  multi-device payload (`Description: Untether for 8.4.1 32bit`), not
  something ever actually verified against a real Apple TV 3 kernel. It
  shares the harness's distinctive strings/log format and the exact
  `"Marijuan"` watermark from tihmstar's own public `jelbrekTime` source
  (`jailbreak.m`, internally codenamed **"v0rtex"**) — and that watermark
  turns out to be a standing signature technique across his toolchain more
  broadly, not a one-off: his currently-maintained `tihmstar/libpatchfinder`
  has a reusable `get_MarijuanARM_patch()`, including a 32-bit iOS 8
  kernel-patchfinder file dated "13.08.21" (about 3.5 months after this
  tarball's own timestamps) — circumstantial support that this is
  tihmstar's own private 2021-era dev tooling, never packaged, rather than
  a third party's build, but not proof. **Still genuinely unknown**: the
  actual origin of the base build itself — no second copy of this exact
  binary turned up anywhere on the web, GitHub, or Wayback.
- Our own `untether.bin` vs. our own `orig_untether.bin`'s 15-byte diff is
  now fully characterized (previous entry here was wrong — these are NOT
  Thumb branch immediates). 14 of the 15 bytes are an in-place ASCII
  string edit: one of the binary's four embedded `Darwin Kernel Version`
  banner strings changes from `...xnu-2784.40.6~50/RELEASE_ARM` (Nov 2016)
  to `...xnu-2784.40.6~93/RELEASE_ARM` (Jan 2021) — same length, hence
  patchable in place without relinking; the other three banner slots
  (`~86`/`~87`/`~92`) are untouched. Net effect: drop the oldest
  recognized kernel build, add the newest, keeping the table fixed at 4
  entries. The remaining 1 byte (`ADDS r6, #0x28` → `#0x45`, offset
  `0x3301`) is a real, separate code change elsewhere, still unexplained.
  Timestamps (`orig_` 2021-04-17, patched 2021-04-28) sit just 8-19 days
  after Apple's real `12H923` tvOS 8.4.x update (2021-04-09) — whoever
  patched this was tracking Apple's still-ongoing point releases in close
  to real time. *Who* did it (tihmstar himself vs. a third party) is still
  unknown.
- Whether tihmstar's own `v0rtex`-family source (beyond the `jelbrekTime`
  copy, which targets watchOS/armv7k, not this binary's iOS/tvOS armv7,
  and `libpatchfinder`, whose visible git history only goes back to a June
  2023 squash commit) was ever published anywhere closer to this exact
  target — checked `tihmstar/v1ntex`, `v3ntex` (unrelated, 64-bit
  iOS11/12-era) and `tihmstar/jbinit` (unrelated, no AppleTV/8.4.1/
  S5L8947X references) directly; both dead ends. No further leads found
  yet.

## 2. Document & implement pushing SSH access to the device (resolved)

Previously: `DeviceManager::pushAuthorizedKeys()` hand-rolled an AFC2 write
of the invoking user's `~/.ssh/authorized_keys` to
`/private/var/root/.ssh/authorized_keys`, wired into `Cli.cpp` to run
automatically once the jailbreak was confirmed running. This piled up
exactly the "difficult flow" gaps this TODO originally listed (retry/timeout
semantics, `.ssh/` directory creation, permission bits, host-key
reconciliation, fatal-vs-warning), all needing to be gotten right inside
`main.cpp`'s dependency graph.

Replaced with `scripts/push_authorized_keys.sh`, run by hand, outside the
main binary entirely: it forwards a local TCP port to the device's real
sshd (Cydia's own openssh package, already running post-boot) via
`iproxy` (already built as part of the vendored `libusbmuxd`, no new
dependency), then pushes the keys file over that tunnel like a normal
`ssh-copy-id`. This sidesteps every gap above instead of solving it in C++:
- Mechanism is now just "read `scripts/push_authorized_keys.sh`" — plain
  `ssh`/`iproxy`, no bespoke AFC2 protocol code to document.
- Retry/timeout is a simple TCP-reachability poll loop in the script, not
  hand-rolled `waitForAFC2` state in `DeviceManager.cpp`.
- `.ssh/` creation and `chmod 700`/`600` permissions are one `ssh` command
  (`mkdir -p && chmod && cat > ... && chmod`) — the old AFC2 path never set
  permissions at all, a latent bug now fixed as a side effect.
- Host-key reconciliation is solved by not needing it: the script uses
  `UserKnownHostsFile=/dev/null` + `StrictHostKeyChecking=accept-new`, so a
  throwaway `127.0.0.1:<port>` tunnel never pollutes the user's real
  `~/.ssh/known_hosts`, and normal `ssh`/password-prompt UX (default Cydia
  openssh password `alpine`) handles first connect.
- Fatal-vs-warning is moot: it's an optional, separate, user-run step with
  its own exit code, not something `blackb0x`'s own run can fail on.
- Device/tether-combination coverage is now uniform by construction — the
  script only depends on Cydia's openssh already running post-boot, not on
  anything exploit-path-specific, so there's no separate path per device to
  validate.

`blackb0x` itself now only prints a one-line pointer to the script once
`jailbreakRunning` first flips to 1 (`Cli.cpp`'s `onDeviceUpdated`); see
`AGENTS.md`'s "Build & run" and "Runtime requirements" sections and the
top-level `README.md`'s "Steps to jailbreak" step 6 for the user-facing
writeup. Not yet re-verified against real hardware end-to-end (no unit
available this session) — the mechanism (usbmuxd TCP forwarding + a real
sshd behind it) is standard and low-risk, but flag if a real run surfaces
anything `iproxy`-specific (e.g. AppleTV 2,1's older tvOS/openssh build
behaving differently) worth recording here.

## 3. Reimplement p0sixspwn's postinst in `entrypoint.c`

Not attempted yet, and not worth chasing right now: nobody on this project
currently has the AppleTV2,1 hardware on 6.1.3-6.1.6 firmware this branch
targets to verify against, and `docs/HISTORY.md`'s own gaster/checkm8
history shows this kind of change can look correct on paper and still be
wrong in a way only real hardware would catch. See `BakeRamdisk.cpp`'s
`stageP0sixspwn()` for the specifics of what's currently staged vs. what
the real package's postinst does that isn't replicated anywhere yet.

## 4. Bring back macOS support for `blackb0x` (and, eventually, ramdisk baking too)

`AGENTS.md` currently states "CLI-only, Linux-only... dropped, not
dual-maintained" as a "don't re-litigate without asking" convention — this
entry reopens that by explicit request. Reopened a second time, more
urgently, after a long real-hardware debugging session on Linux
(AppleTV3,2, ECID 2685369898254) kept surfacing libusb/DFU-state races in
`DeviceManager.cpp`'s `boot_client()` — an intermittent "boots into the
real OS instead of continuing the exploit chain" failure that a conditional
reset (only reset when the device actually reports DFU state 8) mitigates
but hasn't been confirmed to fully resolve. Rather than keep chasing
timing-dependent libusb behavior on Linux, the plan is to get the real
chain running against gaster's and libimobiledevice's native, first-party
IOKit paths on macOS instead, where they're the actively-maintained
reference implementations rather than something inherited secondhand via
libusb's Darwin backend.

First cut is scoped to the `blackb0x`/`gaster` CLI path only — ramdisk
baking (`bake-all-ramdisks`, `BakeRamdisk.cpp`) can stay Linux-only in the
interim (real loop-mounted HFS+, `CAP_SYS_ADMIN`), with a macOS build of
`blackb0x` consuming `dist/` output baked elsewhere in the meantime. But
macOS support for the ramdisk baker itself is also wanted eventually, not
permanently deferred — see its own subsection below.

Real blockers, in the code today — **all four now addressed** (code
written and cross-checked to still build clean on Linux; none of it has
been compiled or run on actual macOS/Xcode yet, since no Mac was available
this session):

- ~~`target_link_options(blackb0x PRIVATE -Wl,--allow-multiple-definition)`~~
  — root-caused and fixed properly instead of worked around: diffed
  libusbmuxd's `common/collection.c` against libimobiledevice-glue's
  `src/collection.c` directly (byte-for-byte identical `struct collection`
  layout and function bodies; glue's is a strict superset, adding
  `collection_copy()`, which nothing in libusbmuxd calls). `libusbmuxd_ext`'s
  `INSTALL_COMMAND` (`CMakeLists.txt`) now runs `${CMAKE_AR} d
  ${DEPS_LIB}/libusbmuxd.a collection.o` right after `make install`,
  stripping libusbmuxd's copy of the object out of its installed archive so
  only libimobiledevice-glue's survives to link time. No more duplicate
  symbols to paper over, on any platform — the GNU-ld-only flag is gone
  entirely, not just made conditional. Confirmed: Linux build still links
  clean with it removed.
- ~~`ResourcePath.cpp`'s `resolveGasterPath()`~~ — now branches on
  `#if defined(__APPLE__)`: uses `_NSGetExecutablePath()`
  (`<mach-o/dyld.h>`) + `realpath()` to resolve symlinks (matching what
  `readlink("/proc/self/exe", ...)` already does implicitly on Linux),
  falling back to the existing `readlink`-based path otherwise.
- ~~`DeviceManager.cpp`'s `isUninterruptible()`~~ — now has a `#if
  defined(__APPLE__)` branch that shells out to `ps -o state= -p <pid>` via
  fork/exec+pipe (no `popen()`/`system()`, matching this file's existing
  no-shell convention) and checks for `U` (BSD ps's uninterruptible-wait
  code, same meaning as Linux's D-state), instead of reading
  `/proc/<pid>/status`.
- ~~`runGaster()`'s hard requirement on GNU `stdbuf`~~ — new
  `resolveStdbufBinary()` helper tries plain `stdbuf` first (works
  identically on Linux, and on macOS if the user has opted into Homebrew
  coreutils' "gnubin" PATH shim), then falls back to `gstdbuf` (Homebrew's
  default prefixed name) on Apple platforms. Went with the
  probe-for-both-names fix rather than the alternative
  `forkpty()`-backed rewrite floated here previously — smaller, more
  targeted change; the `forkpty()` rewrite remains on the table later if
  the `stdbuf`/`gstdbuf` dependency itself becomes a real pain point.
- New, beyond the four originally listed here: `gaster` was being built
  with `HAVE_LIBUSB` unconditionally regardless of target OS
  (`CMakeLists.txt`'s `add_executable(gaster ...)` block), which — per the
  next bullet's old wording — meant even a macOS build would've gone
  through libusb's Darwin backend rather than gaster's own real upstream
  IOKit implementation (gaster.c's `#else` branch, gated on `!HAVE_LIBUSB`:
  `<CommonCrypto/CommonCrypto.h>` + `<IOKit/usb/IOUSBLib.h>`, no vendored
  deps needed at all — both are always-present system frameworks). Now
  `if(APPLE) ... else() ... endif()`-gated: Apple builds skip `HAVE_LIBUSB`/
  `deps::usb`/`deps::wolfssl` entirely and link `-framework CoreFoundation
  -framework IOKit` instead; Linux keeps the exact libusb+wolfSSL build it
  already had. This directly answers the next bullet's old open question
  for gaster's own exploit step (no longer inheriting libusb's Darwin
  backend at all) — it doesn't touch `libimobiledevice`/`libirecovery`'s own
  USB transport, which normal-mode/DFU-mode device communication elsewhere
  in `blackb0x` still goes through; whether an IOKit-native path is
  available/needed there too is unexplored.

Needs real macOS hardware to verify, not just code review — nothing below
has changed, still all open:

- Whether libusb's Darwin backend can claim a checkm8/DFU-mode Apple TV
  without the system's own `usbmuxd`/`MobileDevice` stack interfering, for
  the libirecovery-mediated parts of the chain (`DeviceManager.cpp`'s
  `sendiBSS`/`sendiBEC`/etc., `irecovery` itself) that still go through
  libusb on every platform, gaster's own IOKit switch above notwithstanding.
- Whether the `--no-preflight` usbmuxd workaround this repo needs on Linux
  is even relevant against macOS's built-in `usbmuxd` (probably not, since
  it's Apple's own reference daemon) — a docs question, not code.
- End-to-end: whether building the CLI chain against gaster's native IOKit
  path on real macOS hardware actually sidesteps the Linux-side DFU-state
  races this reopening was prompted by, or whether that turns out to be a
  property of the exploited device's own state machine rather than the
  host's USB stack. Only a real Mac + real AppleTV3,2 run can answer this.

Low-risk, expected to already work: `libusb_ext`'s `--disable-udev` is a
no-op on Darwin (that configure branch is Linux-only, backend is
autodetected); no other GNU-ld-only flags exist in the vendored
`ExternalProject_Add` blocks; the autotools-based vendored deps
(`libimobiledevice`, `libirecovery`, `wolfssl`, `curl`, `libzip`,
`libpng`, `bzip2`, `zlib`) all build on Darwin routinely elsewhere, given
Xcode CLT + Homebrew's autoconf/automake/libtool/pkg-config in place of
the Linux build-deps list. `Cli.cpp`'s udev/sudo help text and
`IPSWDownloader.hpp`'s `ipswDataRoot()` XDG-only fallback are both
cosmetically Linux-flavored but not blockers.

### 4a. macOS support for the ramdisk baker (`bake-all-ramdisks`/`BakeRamdisk.cpp`)

Not started. Unlike the `blackb0x` CLI path above, this one plausibly gets
*simpler* on macOS rather than harder, since HFS+ and DMG tooling are
native there instead of a bolted-on Linux kernel driver:

- `BakeRamdisk.cpp`'s whole reason for existing as a privileged,
  `CAP_SYS_ADMIN`/`CAP_CHOWN`-requiring step is that Linux has no
  in-process way to grow/write a real HFS+ volume — it has to loop-mount
  one via the kernel's `hfsplus` driver and shell out to
  `mkfs.hfsplus`/`fsck.hfsplus` (see `Patcher.hpp`'s and
  `BakeRamdisk.cpp`'s own long comments on this). macOS has first-party
  `hdiutil`/`diskutil` for creating, resizing, and attaching HFS+ `.dmg`
  images, and attaching a user-owned image doesn't require root at all —
  the whole loop-mount/`CAP_SYS_ADMIN` mechanism this file exists to work
  around may not be needed on macOS in the first place. Worth designing
  fresh against `hdiutil attach`/`hdiutil create` rather than porting the
  Linux mount-based flow as-is.
- `mount`/`umount`/`blkid` subprocess calls in `BakeRamdisk.cpp` would need
  a `hdiutil attach -mountpoint ...`/`hdiutil detach` equivalent path.
- `scripts/build_deb_cache.py`'s `podman` dependency (`BakeRamdisk.cpp`'s
  `buildPicklist()`) — Podman does run on macOS, but always through a
  Linux VM (`podman machine`), not natively; needs verifying the picklist
  resolution flow still works through that indirection, or an alternative.
- The `$SUDO_USER`/`runuser` re-invocation dance (for podman's rootless
  storage) is Linux/systemd-flavored and likely doesn't apply to macOS at
  all if the `hdiutil`-based rewrite above removes the root requirement
  entirely.
- Same `xxd`/autotools/toolchain build-time deps as the `blackb0x` side.

No code started here — this is scoping notes only, to pick up whenever
this is prioritized.

### 4b. `blackb0x-pwn`: Blackb0x's own original checkm8/SHAtter, standalone and IOKit-only

Added: a new `if(APPLE)`-gated executable target (`CMakeLists.txt`,
`Blackb0x/Source/Pwn/`), independent of both the main `blackb0x` binary's
gaster-based `checkm8Attempt()` and of gaster itself. Ports
`DeviceManager.m`'s original `+SHAtter:`/`+checkm8:` (deleted from the
tree in `907b64b`, recovered from git history — see `Checkm8Pwn.c`'s own
header comment for the exact `git show` invocation) byte-for-byte to C,
same "translate, don't improve" rule the main binary's own `SHAtter()`
port already follows.

"IOKit-only" is a real, verified build-time guarantee, not just "happens
to run on macOS": `third_party/libirecovery` has its own genuine upstream
`--with-iokit` native Darwin backend (confirmed by reading
`configure.ac`/`libirecovery.c` directly, not assumed) as an alternative
to libusb. `blackb0x-pwn` gets its own separate libirecovery build
(`libirecovery_iokit_ext`, own install prefix, out-of-source so it can't
collide with the shared in-source `libirecovery_ext`) with `--with-iokit`
passed explicitly, so its configure step fails loudly if IOKit isn't
available rather than silently falling back to libusb. Confirmed: no
`deps::usb` anywhere in this target's link line at all.

Verified so far: the exploit-body C file (`Checkm8Pwn.c`) is genuinely
backend-agnostic (only calls through libirecovery's public `irecv_*` API,
never touches IOKit/CoreFoundation directly) — confirmed directly by
compiling and linking it *on Linux* against the existing libusb-backed
libirecovery headers/library with `-Wall -Wextra` (zero warnings) and
inspecting its undefined symbols (only `irecv_*` + libc, nothing
IOKit-shaped). That's real signal the C is correct, but it is **not** a
substitute for actually building/running on macOS, which hasn't happened
yet — the CMake plumbing (the new out-of-source `libirecovery_iokit_ext`
ExternalProject in particular) is entirely unverified on a real host.
