# Blackb0x

Untethered jailbreak tool for the 2nd/3rd-gen Apple TV, via the checkm8/SHAtter DFU-mode
boot exploit — side-loads Cydia + Kodi. This is a Linux CLI port of the original macOS
app; it runs entirely from the command line, no GUI.

Devices supported:
- Apple TV 3,2 (A1469) (tvOS 8.4.x untethered, tvOS 7.x tethered)
- Apple TV 3,1 (A1427) (tvOS 8.4.x untethered, tvOS 7.x tethered) — needs external
  hardware (an Arduino running [synackuk's fork of checkm8-A5](https://github.com/synackuk/checkm8-a5))
  to pwn DFU mode first; `blackb0x` picks up from there
- Apple TV 2,1 (A1378) (tvOS 7.1.2 tethered, tvOS 6.1.4 untethered)

IMPORTANT: make sure your device is connected to the internet for the first boot. Do
not turn it off during first boot until Kodi appears.

## Build

```sh
git clone --recurse-submodules https://github.com/NSSpiral/Blackb0x.git
cd Blackb0x
cmake -S . -B build
cmake --build build -j$(nproc)
```

**Clone recursively** (`--recurse-submodules`) — every third-party dependency is
vendored as a git submodule and built from source; without it, the build will fail
with missing headers. Already cloned without it? `git submodule update --init --recursive`.

### Build-time system dependencies

Everything else — wolfSSL, curl, libusb, libimobiledevice, zlib, etc. — is vendored
and built from source as part of the build above; nothing else needs installing
system-wide. What *does* need to already be on the system (verified against a real
fresh clone + build, not just assumed):

- A C/C++ toolchain (gcc or clang) and **GNU make** — most of the dependency tree is
  autotools-based and shells out to `make` directly regardless of which CMake
  generator you use for the top-level build.
- **CMake ≥3.16.**
- **autoconf, automake, libtool, pkg-config** — for the autotools-based dependencies'
  own `./configure`/`autoreconf` steps.
- **`xxd`** (usually in a `vim-common`/`xxd`/`vim` package) — used to embed `gaster`'s
  exploit payload binaries as C arrays at build time.

### Runtime system dependencies

- **`hfsprogs`** (`mkfs.hfsplus`/`fsck.hfsplus`) and a kernel built with
  `CONFIG_HFSPLUS_FS` (built-in or loadable module) — `patchRamdisk()` builds and
  loop-mounts a real HFS+ volume.
- **`mount`/`umount`/`blkid`/`cp`/`tar`** — used directly (as subprocesses, no shell)
  by `patchRamdisk()`. Present on any mainstream Linux distro as a matter of course.
- The system **`usbmuxd`**, running with `--no-preflight` — see below.
- **`ssh-keygen`** — you need a real SSH keypair of your own (see "Steps to
  jailbreak" below); this tool doesn't generate one for you.
- **`stdbuf`** (GNU coreutils) — optional but recommended: without it, `gaster`'s
  exploit-progress output won't stream live while it runs (a fallback still works,
  just silently, until it finishes or times out).

### One-time system setup: `usbmuxd --no-preflight`

The system `usbmuxd` daemon needs to run with `--no-preflight`, or this hardware's
Normal-mode discovery will silently never work. Add a systemd drop-in:

```sh
sudo mkdir -p /etc/systemd/system/usbmuxd.service.d
sudo tee /etc/systemd/system/usbmuxd.service.d/override.conf <<'EOF'
[Service]
ExecStart=
ExecStart=/usr/bin/usbmuxd --user usbmuxd --systemd --no-preflight
EOF
sudo systemctl daemon-reload
sudo systemctl restart usbmuxd
```

(Adjust the `ExecStart=` path/args to match your distro's existing unit —
`systemctl cat usbmuxd` shows the original.)

## Steps to jailbreak

0. (3,1 only) PWN with Arduino + [synackuk's fork of checkm8-A5](https://github.com/synackuk/checkm8-a5) first.
1. Plug in your Apple TV via micro-USB **and** plug in the power cable.
2. Run `sudo ./build/blackb0x` (root is required — raw USB access and the ramdisk
   patching step both need it). Add `--dry-run` to preview the exploit/firmware steps
   without actually running the exploit or uploading anything to the device.
3. Follow the on-screen instructions to enter DFU mode.
4. Once the jailbreak finishes installing, connect to your TV and wait 5–10 minutes
   until Kodi appears (be patient, go have a coffee).

SSH access on the jailbroken device uses your own `~/.ssh/authorized_keys`, not a
shared default — make sure you have a real SSH keypair (`ssh-keygen`) before running.

## Development

See [`AGENTS.md`](AGENTS.md) for repo conventions, and
[`docs/HISTORY.md`](docs/HISTORY.md) for the full port/debugging history.

## Credits
**nyan_satan**
* libbootkit (iBSS loader for AppleTV3,2)

**dora2ios**
* iBSS loader for AppleTV3,1

**tihmstar**
* etasonATV jsc untether
* answering questions about patches

**zzanehip**
* updated CBPatcher (Created by Jonathan Seals)
* updated iBoot32Patcher (Created by iH8sn0w)
* updated xpwntool (Created by planetbeing)

**a1exdandy, synackuk, nyan_satan**
* checkm8-A5

**axi0mx**
* checkm8

**p0sixninja**
* SHAtter

**[verygenericname](https://github.com/verygenericname/gaster)**
* gaster (the checkm8 implementation this port shells out to)
