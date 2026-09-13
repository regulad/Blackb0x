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
