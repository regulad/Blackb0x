//
//  Patcher.cpp
//  Blackb0x
//

#include "Patcher.hpp"
#include "ResourcePath.hpp"

extern "C" {
#include <xpwntool.h>
#include <libiboot32patcher.h>
#include <CBPatcher.h>
}

extern "C" {
#include <abstractfile.h>
#include <dmg/dmglib.h>
#include <hfs/hfslib.h>
#include <hfs/hfsplus.h>
#include <xpwn/libxpwn.h>
}

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <vector>

#include <fcntl.h>
#include <pwd.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Small path-string helpers (replace Patcher.mm's input:/decrypted:/patched:/
// output:/patchedDMG:/decryptedDMG:/downgrade:/preboot:/replaceExtension:with:)
// ---------------------------------------------------------------------------

static std::string replaceExtension(const std::string& path, const std::string& ext) {
    fs::path p(path);
    p.replace_extension(ext);
    return p.string();
}

static std::string withoutExtension(const std::string& path) {
    fs::path p(path);
    return p.replace_extension("").string();
}

static std::string decryptedPathFor(const std::string& path) { return replaceExtension(path, "dec"); }
static std::string patchedPathFor(const std::string& path) { return replaceExtension(path, "patched"); }
static std::string prebootPathFor(const std::string& path) { return replaceExtension(path, "preboot"); }
static std::string downgradePathFor(const std::string& path) { return replaceExtension(path, "downgrade"); }
static std::string outputPathFor(const std::string& path) { return withoutExtension(path); }
static std::string decryptedDMGFor(const std::string& path) { return withoutExtension(path) + "-decrypted.dmg"; }
static std::string patchedDMGFor(const std::string& path) { return withoutExtension(path) + "-patched.dmg"; }

// ---------------------------------------------------------------------------
// HFS+ volume helpers (replace hdiutil attach/detach/resize and tar -C) —
// see the long comment on patchRamdisk() below for why this went through
// two other designs (an in-memory xpwn Volume, then libhfsp) before landing
// on "loop-mount via the real Linux kernel driver, then use ordinary POSIX
// tools" as the one that actually survives real firmware data.
// ---------------------------------------------------------------------------

// Runs a command to completion and returns whether it exited 0. Uses
// fork()/execvp() (argv array, no shell) rather than system()/popen() so
// paths never pass through shell interpretation.
static bool runCommand(const std::vector<std::string>& argv) {
    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

// Same as runCommand(), but captures stdout (used only for
// `blkid -o value -s LABEL`). Stderr is discarded rather than inherited, so
// a missing/unreadable label doesn't spam the console — readVolumeLabel()
// below already treats an empty result as "no label found" and falls back.
static std::string runCommandCapture(const std::vector<std::string>& argv) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return "";

    std::vector<char*> cargv;
    cargv.reserve(argv.size() + 1);
    for (const auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return "";
    }
    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    close(pipefd[1]);

    std::string output;
    char buf[256];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) output.append(buf, (size_t)n);
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    return output;
}

static std::string makeTempDir(const std::string& prefix) {
    std::string tmpl = (fs::temp_directory_path() / (prefix + "XXXXXX")).string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    if (!mkdtemp(buf.data())) return "";
    return std::string(buf.data());
}

// RAII guard so a failed patchRamdisk() run never leaks a stale mount or
// temp mountpoint directory — `mounted` is only set true once the mount
// actually succeeds, and unmounting/removal here is best-effort (there's
// nothing more useful to do with a failure during cleanup after an error).
struct MountGuard {
    std::string mountpoint;
    bool mounted = false;
    ~MountGuard() {
        if (mounted) runCommand({"umount", mountpoint});
        if (!mountpoint.empty()) {
            std::error_code ec;
            fs::remove(mountpoint, ec);
        }
    }
};

static bool copyLocalFileIntoMount(const std::string& mountRoot, const std::string& localPath,
                                    const std::string& relHfsPath) {
    fs::path dest = fs::path(mountRoot) / fs::path(relHfsPath).relative_path();
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    fs::copy_file(localPath, dest, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        fprintf(stderr, "copyLocalFileIntoMount: failed to copy %s -> %s (%s)\n", localPath.c_str(),
                dest.c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

// Locates the invoking human's own SSH authorized_keys, so the jailbroken
// device ends up reachable with keys the user already controls. blackb0x
// runs as root (see runCli()'s root check), so plain $HOME resolves to
// root's home, not the actual person running the tool — resolve via
// $SUDO_USER first when present, matching how the tool is actually expected
// to be invoked (`sudo blackb0x ...`).
static std::optional<std::string> findUserAuthorizedKeysPath() {
    std::string homeDir;

    if (const char* sudoUser = getenv("SUDO_USER"); sudoUser && *sudoUser) {
        if (struct passwd* pw = getpwnam(sudoUser)) {
            if (pw->pw_dir) homeDir = pw->pw_dir;
        }
    }
    if (homeDir.empty()) {
        if (const char* home = getenv("HOME"); home && *home) {
            homeDir = home;
        } else if (struct passwd* pw = getpwuid(getuid())) {
            if (pw->pw_dir) homeDir = pw->pw_dir;
        }
    }
    if (homeDir.empty()) return std::nullopt;

    std::string path = homeDir + "/.ssh/authorized_keys";
    std::error_code ec;
    if (!fs::exists(path, ec) || ec) return std::nullopt;
    return path;
}

// Reads the original ramdisk's volume label, so the freshly-mkfs'd
// replacement can be given the same name. mkfs.hfsplus can only set this at
// creation time — the volume name lives in the catalog (the root folder's
// own record), not in the fixed-size volume header, so there's no
// header-byte-copy shortcut for it the way there is for the fields in
// copyVolumeHeaderMetadata() below.
static std::string readVolumeLabel(const std::string& imagePath) {
    std::string label = runCommandCapture({"blkid", "-o", "value", "-s", "LABEL", imagePath});
    return label.empty() ? "ramdisk" : label;
}

// Copies a deliberately narrow set of "identity" fields from the original
// volume's fixed-size HFS+ header into the freshly-mkfs'd replacement:
//   - finderInfo (32 bytes): encodes blessed-folder/boot-related info. This
//     is a bootable restore ramdisk, so this can plausibly affect boot
//     behavior — cheap to preserve, and risky to guess is safe to drop.
//   - createDate: the original firmware build's genuine volume-creation
//     timestamp — meaningful provenance, not something mkfs.hfsplus can
//     know to set correctly on its own.
//   - lastMountedVersion: a 4-byte tag identifying what tool last wrote the
//     volume — likewise provenance worth carrying over.
// Deliberately NOT copied:
//   - `attributes` (a state/journaling bitfield): copying it wholesale
//     risks importing a stale "unmounted"/journaled bit that doesn't match
//     the freshly-mkfs'd volume's actual, correct state.
//   - fileCount/folderCount/blockSize/totalBlocks/free space/clump sizes:
//     all content- or geometry-derived. mkfs.hfsplus already computed
//     these correctly for the new volume's real size; copying the old
//     volume's values would be actively wrong.
//
// This patches BOTH the primary header (fixed offset 1024, per the HFS+
// spec) and the backup/alternate header (the second-to-last 512-byte
// sector) so fsck.hfsplus doesn't flag a primary/alternate mismatch for the
// fields touched here. This is a plain, fixed-offset byte copy touching
// only the 512-byte header itself — no B-tree or catalog interaction at
// all, unlike the catalog-growth code that broke both xpwn and libhfsp (see
// patchRamdisk()'s comment below).
static void copyVolumeHeaderMetadata(const std::string& origPath, const std::string& newPath) {
    char origHeader[512] = {0};
    {
        std::ifstream in(origPath, std::ios::binary);
        if (!in) return;
        in.seekg(1024);
        in.read(origHeader, sizeof(origHeader));
        if (!in) return;
    }

    std::error_code ec;
    auto newSize = fs::file_size(newPath, ec);
    if (ec) return;

    struct FieldCopy {
        size_t offset;
        size_t size;
    };
    static const FieldCopy fields[] = {
        {offsetof(HFSPlusVolumeHeader, createDate), sizeof(uint32_t)},
        {offsetof(HFSPlusVolumeHeader, lastMountedVersion), sizeof(uint32_t)},
        {offsetof(HFSPlusVolumeHeader, finderInfo), sizeof(uint32_t) * 8},
    };

    std::fstream out(newPath, std::ios::binary | std::ios::in | std::ios::out);
    if (!out) return;
    for (const auto& f : fields) {
        out.seekp((std::streamoff)1024 + (std::streamoff)f.offset);
        out.write(origHeader + f.offset, (std::streamsize)f.size);
        out.seekp((std::streamoff)newSize - 1024 + (std::streamoff)f.offset);
        out.write(origHeader + f.offset, (std::streamsize)f.size);
    }
}

// ---------------------------------------------------------------------------
// Patcher
// ---------------------------------------------------------------------------

Patcher::Patcher() {
    clearComponents();
    TestByteOrder();
}

std::string Patcher::getRealVersion(const std::string& version) const {
    if (version.empty()) return "";

    std::istringstream ss(version);
    std::string part;
    std::vector<int> parts;
    while (std::getline(ss, part, '.')) {
        parts.push_back(atoi(part.c_str()));
    }
    int first = parts.size() > 0 ? parts[0] : 0;
    int second = parts.size() > 1 ? parts[1] : 0;
    int third = parts.size() > 2 ? parts[2] : 0;

    printf("Version: %d.%d.%d\n", first, second, third);
    switch (first) {
        case 4:
            return (second < 4) ? "4.0" : "5.0";
        case 5:
            return (second == 0) ? "5.0" : "6.0";
        case 6:
            return "7.0";
        case 7:
            return (second || third) ? "8.1" : "8.0";
        default:
            return "8.1";
    }
}

const FirmwareKeyPair* Patcher::keyFor(const std::string& imageName) const {
    auto it = keys_.find(imageName);
    return (it == keys_.end()) ? nullptr : &it->second;
}

void Patcher::loadKeysForDevice(const std::string& deviceID, const std::string& buildID) {
    keys_ = fetcher_.keysForDevice(deviceID, buildID);
}

bool Patcher::patchiBSS(const std::string& path) {
    const FirmwareKeyPair* k = keyFor("iBSS");
    if (!k) {
        fprintf(stderr, "patchiBSS: no iBSS keys loaded\n");
        return false;
    }

    std::string decPath = decryptedPathFor(path);
    std::string patchedPath = patchedPathFor(path);
    std::string outPath = outputPathFor(path);

    printf("Patching iBSS...\n");

    decrypt(const_cast<char*>(path.c_str()), const_cast<char*>(decPath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE", nullptr);
    iBootPatcher(const_cast<char*>(decPath.c_str()), const_cast<char*>(patchedPath.c_str()), nullptr,
                 (char*)"TRUE", (char*)"FALSE", (char*)"FALSE", (char*)"FALSE");
    decrypt(const_cast<char*>(patchedPath.c_str()), const_cast<char*>(outPath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE",
            const_cast<char*>(path.c_str()));

    std::error_code ec;
    fs::remove(decPath, ec);

    if (path.find("j33i") != std::string::npos || path.find("j33ap") != std::string::npos) {
        // Apple TV 3
        fs::remove(outPath, ec);
        outputs_.iBSS = patchedPath;
    } else {
        // Apple TV 2
        fs::remove(patchedPath, ec);
        outputs_.iBSS = outPath;
    }

    checkPatching();
    return true;
}

bool Patcher::patchiBEC(const std::string& path, const std::string& flags, bool ticket) {
    (void)flags;
    const FirmwareKeyPair* k = keyFor("iBEC");
    if (!k) {
        fprintf(stderr, "patchiBEC: no iBEC keys loaded\n");
        return false;
    }

    std::string decPath = decryptedPathFor(path);
    std::string patchedPath = patchedPathFor(path);
    std::string prebootPath = prebootPathFor(path);
    std::string downgradePath = downgradePathFor(path);
    std::string outPath = outputPathFor(path);

    printf("Patching iBEC...\n");

    decrypt(const_cast<char*>(path.c_str()), const_cast<char*>(decPath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE", nullptr);

    char* args1 = (char*)"rd=md0 amfi=0xff cs_enforcement_disable=1 pio-error=0";
    char* args2 = (char*)"amfi=0xff cs_enforcement_disable=1 pio-error=0 amfi_get_out_of_my_way=1 "
                         "cs_enforcement_disable=1";
    char* t = ticket ? (char*)"TRUE" : (char*)"FALSE";

    iBootPatcher(const_cast<char*>(decPath.c_str()), const_cast<char*>(patchedPath.c_str()), args1, (char*)"TRUE",
                 (char*)"FALSE", t, (char*)"TRUE");
    iBootPatcher(const_cast<char*>(decPath.c_str()), const_cast<char*>(prebootPath.c_str()), args2, (char*)"TRUE",
                 (char*)"FALSE", t, (char*)"TRUE");

    decrypt(const_cast<char*>(patchedPath.c_str()), const_cast<char*>(downgradePath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE",
            const_cast<char*>(path.c_str()));
    decrypt(const_cast<char*>(prebootPath.c_str()), const_cast<char*>(outPath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE",
            const_cast<char*>(path.c_str()));

    std::error_code ec;
    fs::remove(decPath, ec);
    fs::remove(patchedPath, ec);
    fs::remove(prebootPath, ec);

    outputs_.iBECDowngrade = downgradePath;
    outputs_.iBECBoot = outPath;

    checkPatching();
    return true;
}

bool Patcher::patchKernel(const std::string& path, const std::string& productVersion) {
    const FirmwareKeyPair* k = keyFor("Kernelcache");
    if (!k) {
        fprintf(stderr, "patchKernel: no Kernelcache keys loaded\n");
        return false;
    }

    std::string decPath = decryptedPathFor(path);
    std::string patchedPath = patchedPathFor(path);
    std::string outPath = outputPathFor(path);

    printf("Patching kernelcache...\n");

    // productVersion comes straight from BuildManifest.plist's own
    // ProductVersion (e.g. "6.1.3") — NOT derived from `path`. The original
    // versionString(path) helper (deleted) assumed a single flat
    // "<device>_<version>" directory name, matching neither this port's own
    // actual workDir layout (Cli.cpp's downloadAndPatchComponents():
    // ipswDataRoot()/deviceModel/buildID, two nested segments, no version
    // anywhere in the path at all) nor any real value in the manifest —
    // confirmed directly: it silently produced an empty string every time,
    // which patch_kernel() (see below) correctly rejected as an unsupported
    // "iOS 0" — but the caller never checked *that* either, so it fell
    // through to decrypt()'ing a kernelcache.patched file that patch_kernel()
    // never actually wrote, corrupting the heap in third_party/xpwn's own
    // decrypt()-with-template path instead of failing cleanly.
    //
    // Deliberately NOT run through getRealVersion() here, even though the
    // original Patcher.mm always did for every AppleTV path (verbatim same
    // switch/case, confirmed by reading Patcher.mm directly) — that
    // "real firmware version -> iOS-equivalent kernel-signature family"
    // bucketing maps our actual target (10B329a / "6.1.3") to "7.0", and
    // against a real AppleTV3,2 6.1.3 kernelcache, "7.0" finds ZERO
    // matching CBPatcher signatures ("[CBPatch] One or more patches not
    // found") while just passing productVersion straight through — which
    // patch_kernel()'s own kernPat()/kernPatOld() truncates to major
    // version 6 internally (versionInt < 8 -> versionFloat =
    // (float)versionInt, see CBPatcher.c) — finds and applies every
    // expected patch (tfp0, AMFI/memcmp bypass, sandbox policy, ...) and
    // reports success. Most likely explanation: this project's
    // libcbpatcher.a was lost and rebuilt from a third-party source fork
    // (see docs/HISTORY.md), whose "7.0"-family signatures don't exactly match
    // what shipped in the original 2020 binary getRealVersion() was tuned
    // against — not that the original bucketing logic was conceptually
    // wrong. getRealVersion() is kept (unused from here) as a reference in
    // case that signature drift ever gets fixed upstream; for the one real
    // firmware this tool has ever been tested against, the raw version
    // string is what's actually confirmed to work.
    std::string internalFirmware = productVersion;

    decrypt(const_cast<char*>(path.c_str()), const_cast<char*>(decPath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE", nullptr);
    int patchResult = patch_kernel(const_cast<char*>(decPath.c_str()), const_cast<char*>(patchedPath.c_str()),
                                    const_cast<char*>(internalFirmware.c_str()));
    if (patchResult != 0) {
        // patch_kernel() never writes patchedPath on failure (see
        // CBPatcher.c) — the previous version of this code called decrypt()
        // on it anyway, which corrupted the heap in xpwntool's own
        // decrypt()-with-template path when handed a nonexistent input file
        // rather than failing cleanly. Stop here instead.
        fprintf(stderr, "patchKernel: patch_kernel() failed for productVersion=\"%s\" (resolved to \"%s\")\n",
                productVersion.c_str(), internalFirmware.c_str());
        std::error_code ec;
        fs::remove(decPath, ec);
        return false;
    }
    decrypt(const_cast<char*>(patchedPath.c_str()), const_cast<char*>(outPath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE",
            const_cast<char*>(path.c_str()));

    std::error_code ec;
    fs::remove(decPath, ec);

    outputs_.kernel = outPath;
    checkPatching();
    return true;
}

// patchRamdisk() went through three designs before this one. Recording why,
// since the investigation was expensive and the wrong lesson ("just patch
// the bug") would be easy for a future reader to draw:
//
// 1. In-memory xpwn Volume (add_hfs()/grow_hfs(), no mount at all — this
//    matched the original port plan's intent exactly, avoiding any
//    OS-level mount/loopback device). Real-data testing against a
//    downloaded AppleTV2,1 11D258 RestoreRamdisk got most of the way
//    through — ssh.tar, RamdiskBins.tar, and the entire ATV-Cydia.tgz bin/
//    directory all injected correctly, after separately fixing a
//    UF_COMPRESSED-overwrite crash — before hitting
//    hfs_panic("BTree inconsistent!") partway through Debs.tar. Two gdb
//    backtraces confirmed this is a genuine, pre-existing bug in xpwn's own
//    catalog-B-tree *growth* code: grow_hfs() only resizes the overall
//    volume bitmap, not the catalog file's own B-tree extents, and the
//    organic-growth path that kicks in once the catalog needs to grow past
//    its initial capacity has never been exercised enough by xpwn's own,
//    much smaller, reference tools to catch this. Not a bug in the ported
//    code.
// 2. libhfsp (Debian's `hfsplus`/`libhfsp-dev` package — a from-scratch
//    userspace HFS+ reader/writer, again no mount). Ruled out even faster:
//    a single hpmkdir() call, no file copy at all, on a freshly-
//    mkfs.hfsplus'd volume already corrupted an extent entry into the
//    reserved alternate-volume-header block, confirmed via fsck.hfsplus and
//    root-caused in libhfsp's own source (libhfsp/src/volume.c) to a bounds
//    check against exactly that reserved region existing in the source only
//    as commented-out dead code, in all three block-allocation functions.
//    libhfsp's own ChangeLog calls its 1.0.1 release (2000) "a stable,
//    readonly implementation" and never claims the write path reached that
//    bar through its final 1.0.4 release (2002).
// 3. The Linux kernel's own, actively-maintained, in-tree `hfsplus` driver
//    (fs/hfsplus/) — this DOES require a real loop mount, which the
//    original port plan explicitly wanted to avoid, but real functional
//    testing (loop-mounting a blank mkfs.hfsplus volume and copying the
//    *entire* real payload set onto it, including Debs.tar) confirmed it
//    handles exactly the catalog growth that broke both designs above, with
//    a clean fsck.hfsplus verdict and a byte-for-byte content match against
//    the source tarballs. This is what's actually used below.
//
// That still leaves resizing: this function needs to GROW an existing,
// already-populated Apple-built HFS+ image, not just write into a volume
// already sized correctly by mkfs. Linux has no resize2fs-equivalent CLI
// tool for HFS+, but there IS a real library that implements HFS+ resize —
// libparted-fs-resize (what GParted's HFS+ support uses; split out of
// libparted core in parted-3.0 into this separate add-on library because
// HFS+/FAT resize had no free alternative at the time). A throwaway test
// program was built directly against it
// (ped_file_system_open/get_resize_constraint/resize on a real populated
// volume): it reported its own resize constraint capping max_size at the
// volume's CURRENT size, and calling ped_file_system_resize() to grow past
// that returned, verbatim, "No Implementation: Sorry, HFS+ cannot be
// resized that way yet." Growing (as opposed to shrinking) HFS+ is an
// explicit, acknowledged non-implementation in the one library that does
// implement HFS+ resize on Linux — not an untested edge case, and not
// something worth waiting on upstream for.
//
// So instead of resizing in place: mkfs.hfsplus a brand new volume at the
// final target size, copy the ENTIRE original volume's tree across via a
// real mount + `cp -a` (preserving permissions/ownership/setuid
// bits/symlinks), copy the original's volume-header identity fields across
// too (see copyVolumeHeaderMetadata() above), and only then inject the new
// payloads — into a volume that was never grown, just built right the
// first time. `cp -a` preserving arbitrary ownership (root-owned setuid
// binaries, etc.) means this function needs CAP_CHOWN, and mount()/umount()
// need CAP_SYS_ADMIN — patchRamdisk() must run as root (e.g. `sudo
// blackb0x ...`), the same as it already implicitly needed for any of the
// three designs above to touch a loopback-mounted device node.
bool Patcher::patchRamdisk(const std::string& path, bool ssh) {
    const FirmwareKeyPair* k = keyFor("RestoreRamdisk");
    if (!k) {
        fprintf(stderr, "patchRamdisk: no RestoreRamdisk keys loaded\n");
        return false;
    }

    std::string decDMG = decryptedDMGFor(path);
    std::string patchedDMG = patchedDMGFor(path);

    printf("Patching ramdisk...\n");

    decrypt(const_cast<char*>(path.c_str()), const_cast<char*>(decDMG.c_str()), const_cast<char*>(k->key.c_str()),
            const_cast<char*>(k->iv.c_str()), (char*)"FALSE", nullptr);

    // NOTE: the original's "AppleTV2,1_4." branch rebuilds the ramdisk from
    // scratch via `hdiutil create ... -format UDRW` for the oldest ATV2 4.x
    // firmware. That needs formatting a brand-new empty HFS+ volume with no
    // pre-existing content to base it on at all — a materially different
    // problem from growing/injecting into an existing one, and out of scope
    // for this pass. Flag and bail rather than silently produce a broken
    // ramdisk.
    if (path.find("AppleTV2,1_4.") != std::string::npos) {
        fprintf(stderr, "patchRamdisk: AppleTV2,1 4.x ramdisk recreation is not implemented in this port\n");
        return false;
    }

    // Not every decrypted restore component is UDIF-wrapped: on this old
    // (A4-era) Apple TV 2/3 hardware, decrypting the RestoreRamDisk yields a
    // RAW HFS+ image directly (confirmed empirically — "H+" signature right
    // at the standard offset 0x400, no "koly" UDIF trailer at all), unlike
    // the UDIF-wrapped root-filesystem images third_party/xpwn's own
    // ipsw-patch/main.c reference code assumes. Detect which one this is
    // rather than assuming, and only extractDmg()/buildDmg() when genuinely
    // needed — the Linux kernel's hfsplus driver only understands raw
    // partition images, not Apple's UDIF/DMG wrapper, so a genuinely
    // UDIF-wrapped image still needs unwrapping before it can be mounted.
    bool isUDIF = false;
    {
        std::ifstream probe(decDMG, std::ios::binary);
        if (probe) {
            probe.seekg(0, std::ios::end);
            std::streamoff size = probe.tellg();
            if (size >= 512) {
                probe.seekg(size - 512);
                char magic[4] = {0};
                probe.read(magic, 4);
                isUDIF = (memcmp(magic, "koly", 4) == 0);
            }
        }
    }

    std::string origImgPath = decDMG + ".orig-raw.hfs";
    if (isUDIF) {
        FILE* decFile = fopen(decDMG.c_str(), "rb");
        if (!decFile) {
            fprintf(stderr, "patchRamdisk: cannot open %s\n", decDMG.c_str());
            return false;
        }
        void* rawBuffer = nullptr;
        size_t rawSize = 0;
        AbstractFile* decryptedAbs = createAbstractFileFromFile(decFile);
        AbstractFile* rawOut = createAbstractFileFromMemoryFile(&rawBuffer, &rawSize);
        extractDmg(decryptedAbs, rawOut, -1);

        std::ofstream out(origImgPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            fprintf(stderr, "patchRamdisk: cannot write %s\n", origImgPath.c_str());
            free(rawBuffer);
            return false;
        }
        out.write((const char*)rawBuffer, (std::streamsize)rawSize);
        free(rawBuffer);
    } else {
        std::error_code ec;
        fs::copy_file(decDMG, origImgPath, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            fprintf(stderr, "patchRamdisk: cannot copy %s (%s)\n", decDMG.c_str(), ec.message().c_str());
            return false;
        }
    }

    std::string label = readVolumeLabel(origImgPath);

    // Final size up front (replaces `hdiutil resize`, but by building right
    // rather than growing — see the long comment above).
    uint64_t newSize = ssh ? (40ull * 1024 * 1024) : (60ull * 1024 * 1024);
    std::string newImgPath = decDMG + ".new-raw.hfs";
    {
        std::ofstream create(newImgPath, std::ios::binary | std::ios::trunc);
        if (!create) {
            fprintf(stderr, "patchRamdisk: cannot create %s\n", newImgPath.c_str());
            return false;
        }
    }
    std::error_code sizeEc;
    fs::resize_file(newImgPath, newSize, sizeEc);
    if (sizeEc) {
        fprintf(stderr, "patchRamdisk: cannot size new image (%s)\n", sizeEc.message().c_str());
        return false;
    }
    if (!runCommand({"mkfs.hfsplus", "-v", label, newImgPath})) {
        fprintf(stderr, "patchRamdisk: mkfs.hfsplus failed on %s\n", newImgPath.c_str());
        return false;
    }
    copyVolumeHeaderMetadata(origImgPath, newImgPath);

    MountGuard origMount, newMount;
    origMount.mountpoint = makeTempDir("blackb0x-origmnt-");
    newMount.mountpoint = makeTempDir("blackb0x-newmnt-");
    if (origMount.mountpoint.empty() || newMount.mountpoint.empty()) {
        fprintf(stderr, "patchRamdisk: cannot create temp mountpoints\n");
        return false;
    }
    if (!runCommand({"mount", "-t", "hfsplus", "-o", "loop,ro", origImgPath, origMount.mountpoint})) {
        fprintf(stderr, "patchRamdisk: failed to mount original ramdisk (are we running as root?)\n");
        return false;
    }
    origMount.mounted = true;
    if (!runCommand({"mount", "-t", "hfsplus", "-o", "loop", newImgPath, newMount.mountpoint})) {
        fprintf(stderr, "patchRamdisk: failed to mount new ramdisk\n");
        return false;
    }
    newMount.mounted = true;

    // Bring across everything the original ramdisk already had first —
    // `-a` preserves permissions, ownership (including setuid bits, which
    // matter here — this is a real Unix root filesystem, not just data
    // files), and symlinks-as-symlinks rather than following them.
    if (!runCommand({"cp", "-a", origMount.mountpoint + "/.", newMount.mountpoint + "/"})) {
        fprintf(stderr, "patchRamdisk: failed to copy original ramdisk contents\n");
        return false;
    }

    const std::string& mnt = newMount.mountpoint;
    std::error_code mkdirEc;

    // ssh.tar is both extracted at the ramdisk root AND copied in as a raw
    // file at /ssh.tar (matching the original — presumably re-extracted by
    // setup.sh at first boot for the SSH-only install path).
    std::string sshTarPath = resolveResourcePath("ssh.tar");
    runCommand({"tar", "-xf", sshTarPath, "-C", mnt});
    runCommand({"tar", "-xf", resolveResourcePath("RamdiskBins.tar"), "-C", mnt});

    // ssh.tar ships no SSH host key at all — generate one fresh with the
    // host's own `ssh-keygen`, unique to this one patching run. RSA only:
    // this old sshd predates ed25519 support entirely, and modern
    // `ssh-keygen` can no longer generate the legacy SSH-1/RSA1 or DSA
    // formats it would otherwise also use (confirmed directly against this
    // host's OpenSSH 10.2, which prints "unknown key type dsa" for the
    // latter) — RSA alone is enough for this sshd to start and for any
    // modern client to connect.
    {
        fs::path sshEtcDir = fs::path(mnt) / "private/etc/ssh";
        if (!runCommand(
                {"ssh-keygen", "-q", "-N", "", "-t", "rsa", "-f", (sshEtcDir / "ssh_host_rsa_key").string()})) {
            fprintf(stderr, "patchRamdisk: ssh-keygen failed to generate a host key\n");
            return false;
        }
    }

    // Grant SSH access keyed to whoever is actually running blackb0x —
    // sshd's own compiled-in defaults already enable pubkey auth (confirmed
    // against ssh.tar's sshd_config: PubkeyAuthentication/AuthorizedKeysFile
    // are present only as commented-out, i.e. default-value, lines), so
    // dropping a real authorized_keys in is enough; no sshd_config edit
    // needed. Required, not optional: this is the only personalized way
    // onto the device at all (PasswordAuthentication is still enabled in
    // sshd_config, but that's ssh.tar's own baked default, not something
    // this port controls).
    auto authorizedKeys = findUserAuthorizedKeysPath();
    if (!authorizedKeys) {
        fprintf(stderr,
                "patchRamdisk: no ~/.ssh/authorized_keys found for the invoking user.\n"
                "This tool won't generate an SSH keypair on your behalf — if you don't already\n"
                "have one, generate it yourself first (e.g. `ssh-keygen`), then add your public\n"
                "key to ~/.ssh/authorized_keys (e.g. `cat ~/.ssh/id_rsa.pub >> "
                "~/.ssh/authorized_keys`)\n"
                "before patching, so the jailbroken device is actually reachable.\n");
        return false;
    }
    printf("Granting SSH access from %s\n", authorizedKeys->c_str());
    {
        fs::path sshDir = fs::path(mnt) / "private/var/root/.ssh";
        std::error_code sshEc;
        fs::create_directories(sshDir, sshEc);
        fs::path authorizedKeysDest = sshDir / "authorized_keys";
        fs::copy_file(*authorizedKeys, authorizedKeysDest, fs::copy_options::overwrite_existing, sshEc);
        // sshd refuses to use authorized_keys at all if it or its parent
        // directory are group/world-writable.
        fs::permissions(sshDir, fs::perms::owner_all, sshEc);
        fs::permissions(authorizedKeysDest, fs::perms::owner_read | fs::perms::owner_write, sshEc);
    }

    if (!ssh) {
        fs::create_directories(mnt + "/files/cydia", mkdirEc);
        runCommand({"tar", "-xzf", resolveResourcePath("ATV-Cydia.tgz"), "-C", mnt + "/files/cydia"});

        fs::create_directories(mnt + "/files", mkdirEc);
        runCommand({"tar", "-xf", resolveResourcePath("Debs.tar"), "-C", mnt + "/files"});

        fs::create_directories(mnt + "/files/p0sixspwn", mkdirEc);
        runCommand({"tar", "-xzf", resolveResourcePath("p0sixspwn.tgz"), "-C", mnt + "/files/p0sixspwn"});

        // Anthrax tether.
        copyLocalFileIntoMount(mnt, resolveResourcePath("launchd"), "/sbin/launchd");
        fs::create_directories(mnt + "/mnt", mkdirEc);

        // LaunchDaemons and config staged for setup.sh to redistribute.
        copyLocalFileIntoMount(mnt, resolveResourcePath(".blackb0x"), "/files/.blackb0x");
        copyLocalFileIntoMount(mnt, resolveResourcePath("com.blackb0x.postinstall.plist"),
                                "/files/com.blackb0x.postinstall.plist");
        copyLocalFileIntoMount(mnt, resolveResourcePath("com.openssh.sshd.plist"), "/files/com.openssh.sshd.plist");
        copyLocalFileIntoMount(mnt, resolveResourcePath("setup.sh"), "/files/setup.sh");
        copyLocalFileIntoMount(mnt, resolveResourcePath("profile"), "/files/profile");
        copyLocalFileIntoMount(mnt, resolveResourcePath("fstab.atv"), "/files/fstab.atv");

        fs::create_directories(mnt + "/files/etasonATV", mkdirEc);
        runCommand({"tar", "-xf", resolveResourcePath("tihmstar-untether.tar"), "-C", mnt + "/files/etasonATV"});

        copyLocalFileIntoMount(mnt, resolveResourcePath("rtbuddyd.bin"), "/files/rtbuddyd.bin");
        copyLocalFileIntoMount(mnt, resolveResourcePath("kodi.png"), "/files/kodi.png");
        copyLocalFileIntoMount(mnt, resolveResourcePath("nito.png"), "/files/nito.png");
        copyLocalFileIntoMount(mnt, resolveResourcePath("joshtv.list"), "/files/joshtv.list");
        copyLocalFileIntoMount(mnt, resolveResourcePath("pubkey.key"), "/files/pubkey.key");
        copyLocalFileIntoMount(mnt, resolveResourcePath("xbmc.list"), "/files/xbmc.list");
    }

    copyLocalFileIntoMount(mnt, sshTarPath, "/ssh.tar");

    sync();
    if (!runCommand({"umount", newMount.mountpoint})) {
        fprintf(stderr, "patchRamdisk: failed to unmount new ramdisk\n");
        return false;
    }
    newMount.mounted = false;
    runCommand({"umount", origMount.mountpoint});
    origMount.mounted = false;

    std::error_code rmEc;
    fs::remove(origImgPath, rmEc);

    // Write the modified image back out, overwriting the decrypted file in
    // place (replaces `hdiutil detach`) — rewrapped into UDIF only if the
    // source was genuinely UDIF-wrapped to begin with; otherwise the raw
    // HFS+ bytes are used directly, matching what decrypt() actually handed
    // us.
    if (isUDIF) {
        std::ifstream newFile(newImgPath, std::ios::binary);
        if (!newFile) {
            fprintf(stderr, "patchRamdisk: cannot open %s\n", newImgPath.c_str());
            return false;
        }
        std::ostringstream ss;
        ss << newFile.rdbuf();
        std::string contents = ss.str();
        size_t rawSize = contents.size();
        void* rawBuffer = malloc(rawSize);
        memcpy(rawBuffer, contents.data(), rawSize);

        FILE* rebuiltFile = fopen(decDMG.c_str(), "wb");
        if (!rebuiltFile) {
            fprintf(stderr, "patchRamdisk: cannot write %s\n", decDMG.c_str());
            free(rawBuffer);
            return false;
        }
        AbstractFile* rebuiltOut = createAbstractFileFromFile(rebuiltFile);
        buildDmg(createAbstractFileFromMemoryFile(&rawBuffer, &rawSize), rebuiltOut, 2048);
        free(rawBuffer);
        fs::remove(newImgPath, rmEc);
    } else {
        fs::remove(decDMG, rmEc);
        fs::rename(newImgPath, decDMG, rmEc);
        if (rmEc) {
            fprintf(stderr, "patchRamdisk: failed to move new image into place (%s)\n", rmEc.message().c_str());
            return false;
        }
    }

    decrypt(const_cast<char*>(decDMG.c_str()), const_cast<char*>(patchedDMG.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE",
            const_cast<char*>(path.c_str()));

    outputs_.ramdisk = patchedDMG;
    checkPatching();
    return true;
}

void Patcher::setDeviceTreePath(const std::string& path) {
    outputs_.deviceTree = path;
    checkPatching();
}

void Patcher::checkPatching() {
    if (!outputs_.iBSS) return;

    if (!onlyBootComponents) {
        if (!outputs_.iBECDowngrade) return;
        if (!outputs_.ramdisk) return;
    } else {
        if (!outputs_.iBECBoot) return;
    }

    if (!outputs_.kernel) return;
    if (!outputs_.deviceTree) return;

    if (onComponentsReady) onComponentsReady(outputs_);
    clearComponents();
}

void Patcher::clearComponents() {
    outputs_ = PatchedComponents{};
}
