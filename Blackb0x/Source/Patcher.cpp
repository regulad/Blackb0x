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
#include <xpwn/libxpwn.h>
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

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
    deviceModel_ = deviceID;
    buildID_ = buildID;
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
    int patchResult = iBootPatcher(const_cast<char*>(decPath.c_str()), const_cast<char*>(patchedPath.c_str()),
                                    nullptr, (char*)"TRUE", (char*)"FALSE", (char*)"FALSE", (char*)"FALSE");
    if (patchResult != 0) {
        // iBootPatcher() never writes patchedPath on failure (e.g.
        // patch_rsa_check() couldn't find its target instruction pattern
        // in this specific iBSS build) -- same "decrypt() a nonexistent
        // file" heap corruption patchKernel() was already fixed for (see
        // its own comment), just never fixed here. Stop here instead of
        // silently shipping garbage/stale output that would still fail
        // signature verification once actually sent.
        fprintf(stderr, "patchiBSS: iBootPatcher() failed for %s\n", path.c_str());
        std::error_code ec;
        fs::remove(decPath, ec);
        return false;
    }
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

// See Patcher.hpp's own comment. Deliberately minimal compared to
// patchiBSS() above: decrypt()s exactly the same way, but skips the
// iBootPatcher() call (and the AppleTV2-vs-AppleTV3 patchedPath/outPath
// filename-heuristic branch that only matters for picking which of
// patchiBSS()'s two *patched* outputs to keep) entirely -- there's only
// ever one output here, the plain decrypted file, since nothing about it
// differs by device model when unpatched.
bool Patcher::useStockIBSS(const std::string& path, bool stockSecurom) {
    if (stockSecurom) {
        // See this method's own comment in Patcher.hpp -- a genuinely
        // un-pwned device's SecureROM verifies the img3 signature over the
        // original encrypted bytes; decrypt()ing first (even without any
        // patch applied) would invalidate that signature before it's ever
        // checked.
        fprintf(stderr,
                "--stock-securom: sending the original downloaded iBSS untouched (still encrypted, still "
                "img3-wrapped) -- decrypting it first would invalidate Apple's own signature before a real "
                "SecureROM ever gets to check it.\n");
        outputs_.iBSS = path;
        checkPatching();
        return true;
    }

    const FirmwareKeyPair* k = keyFor("iBSS");
    if (!k) {
        fprintf(stderr, "useStockIBSS: no iBSS keys loaded\n");
        return false;
    }

    std::string outPath = outputPathFor(path);

    fprintf(stderr,
            "--stock-recovery: decrypting the stock iBSS exactly as downloaded from Apple -- no boot-args/"
            "KASLR/ticket-check patches applied.\n");

    decrypt(const_cast<char*>(path.c_str()), const_cast<char*>(outPath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE", nullptr);

    outputs_.iBSS = outPath;
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

    int patchedResult = iBootPatcher(const_cast<char*>(decPath.c_str()), const_cast<char*>(patchedPath.c_str()),
                                      args1, (char*)"TRUE", (char*)"FALSE", t, (char*)"TRUE");
    int prebootResult = iBootPatcher(const_cast<char*>(decPath.c_str()), const_cast<char*>(prebootPath.c_str()),
                                      args2, (char*)"TRUE", (char*)"FALSE", t, (char*)"TRUE");
    if (patchedResult != 0 || prebootResult != 0) {
        // Same reasoning as patchiBSS()'s own comment -- iBootPatcher()
        // never writes its output file on failure (e.g. patch_ticket_check()/
        // patch_rsa_check() couldn't find their target instruction pattern
        // in this specific iBEC build), and decrypt()ing a missing/stale
        // file next would silently ship garbage that still fails
        // verification once sent, rather than failing cleanly here.
        fprintf(stderr, "patchiBEC: iBootPatcher() failed for %s (patchedPath=%d, prebootPath=%d)\n", path.c_str(),
                patchedResult, prebootResult);
        std::error_code ec;
        fs::remove(decPath, ec);
        return false;
    }

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

// See Patcher.hpp's own comment. Unlike patchiBEC(), there's no
// downgrade-vs-boot distinction to make here at all -- both boot-args
// variants above exist purely to steer iBootPatcher()'s own patch
// selection for two different call sites (tethered downgrade vs full
// jailbreak boot); with no patching happening, both outputs are the exact
// same plain decrypted file, so both PatchedComponents fields just point
// at it.
bool Patcher::useStockIBEC(const std::string& path) {
    // See this method's own comment in Patcher.hpp -- always sent
    // untouched, unconditionally: whichever iBSS is now running
    // (useStockIBSS()'s own unpatched output) still has its RSA check
    // intact and verifies iBEC's img3 signature over the original
    // encrypted bytes, same as a real SecureROM does for iBSS.
    // Decrypting first invalidates that signature before it's ever
    // checked, regardless of --stock-securom.
    fprintf(stderr,
            "--stock-recovery: sending the original downloaded iBEC untouched (still encrypted, still "
            "img3-wrapped) -- the stock iBSS that's now running still verifies its signature.\n");
    outputs_.iBECDowngrade = path;
    outputs_.iBECBoot = path;
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

// See Patcher.hpp's own comment. Same decrypt()-only pattern as
// useStockIBSS()/useStockIBEC()/useStockRamdisk() -- no patch_kernel()
// call at all.
bool Patcher::useStockKernel(const std::string& path, bool stockRecovery) {
    if (stockRecovery) {
        // See this method's own comment in Patcher.hpp -- whichever iBEC
        // is running (useStockIBEC()'s own unpatched output, since that's
        // the only iBEC stockRecovery ever produces) still verifies the
        // kernelcache's img3 signature over the original encrypted bytes.
        fprintf(stderr,
                "--stock-firmware --stock-recovery: sending the original downloaded kernelcache untouched "
                "(still encrypted, still img3-wrapped) -- the stock iBEC that's now running still verifies "
                "its signature.\n");
        outputs_.kernel = path;
        checkPatching();
        return true;
    }

    const FirmwareKeyPair* k = keyFor("Kernelcache");
    if (!k) {
        fprintf(stderr, "useStockKernel: no Kernelcache keys loaded\n");
        return false;
    }

    std::string outPath = outputPathFor(path);

    fprintf(stderr,
            "--stock-firmware: decrypting the stock kernelcache exactly as downloaded from Apple -- "
            "no tfp0/AMFI/sandbox patches applied.\n");

    decrypt(const_cast<char*>(path.c_str()), const_cast<char*>(outPath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE", nullptr);

    outputs_.kernel = outPath;
    checkPatching();
    return true;
}

// The actual mount/merge/decrypt logic (and the design-history comment
// explaining why a real kernel loop-mount is unavoidable) lives in
// BakeRamdisk.cpp now — see bakeRamdisk()'s header comment there for the
// full investigation. blackb0x never invokes it itself: baking is a
// separate, manually-run, one-time-per-firmware step done in bulk by
// bake-all-ramdisks (BakeAllRamdisks.cpp), not something this tool does on
// every run. That's possible because the ramdisk/ overlay content is fully
// static (no per-device secrets get baked in — see BakeRamdisk.cpp), so the
// same patched output is valid for every device on a given firmware build.
// This just checks whether bake-all-ramdisks has already produced the
// dist/ entry this firmware needs, and tells the user to run it if not.
bool Patcher::patchRamdisk(const std::string& path) {
    const FirmwareKeyPair* k = keyFor("RestoreRamdisk");
    if (!k) {
        fprintf(stderr, "patchRamdisk: no RestoreRamdisk keys loaded\n");
        return false;
    }

    // Matches bake-all-ramdisks' own naming convention exactly (see
    // BakeAllRamdisks.cpp) — both sides need to agree on this without a
    // round trip, hence the plain, duplicated (not shared-header) format
    // string on each side.
    const std::string patchedDMG = "dist/" + deviceModel_ + "_" + buildID_ + "-Ramdisk.dmg";
    if (!fs::exists(patchedDMG)) {
        // dist/ has *something* in it (runCli() already checked that up
        // front) — just not this specific device+firmware. That's not a
        // recoverable "try the next component" failure the way a flaky
        // download is: there is no ramdisk to send this device no matter
        // what else this run does, so stop hard here instead of limping on
        // to "Not all required components patched successfully", which
        // would leave the real cause one level removed from what's
        // actually printed.
        fprintf(stderr,
                "blackb0x: PANIC: no baked ramdisk for %s %s (%s doesn't exist).\n"
                "Either update the device to the latest firmware Apple currently signs (the\n"
                "one bake-all-ramdisks --signed-only would have picked up), or bake every known\n"
                "combination instead, including older/unsigned ones, by re-running:\n"
                "  sudo ./bake-all-ramdisks\n"
                "(without --signed-only)\n",
                deviceModel_.c_str(), buildID_.c_str(), patchedDMG.c_str());
        std::exit(1);
    }

    // Refuse a stale artifact rather than silently uploading old content:
    // if ramdisk/ has been edited since this was baked, its .sum sidecar
    // (written by bake-all-ramdisks) won't match the overlay's current
    // hash. --dont-check-firmware-sums bypasses this entirely (see
    // dontCheckFirmwareSums's own comment in Patcher.hpp).
    if (dontCheckFirmwareSums) {
        fprintf(stderr,
                "patchRamdisk: --dont-check-firmware-sums set — using %s as-is without checking "
                "whether it still matches the current ramdisk/ overlay.\n",
                patchedDMG.c_str());
    } else {
        std::string storedHash;
        {
            std::ifstream sumIn(sumFileFor(patchedDMG));
            if (sumIn) std::getline(sumIn, storedHash);
        }
        if (storedHash != ramdiskOverlayContentHash()) {
            fprintf(stderr,
                    "patchRamdisk: %s is stale — the ramdisk/ overlay has changed since this was baked.\n"
                    "Re-run this (as root) before using blackb0x against this firmware:\n"
                    "  sudo ./bake-all-ramdisks\n"
                    "Or pass --dont-check-firmware-sums to use it anyway.\n",
                    patchedDMG.c_str());
            return false;
        }
    }

    outputs_.ramdisk = patchedDMG;
    checkPatching();
    return true;
}

// See Patcher.hpp's own comment on why this exists. Deliberately minimal
// compared to patchRamdisk()/bake-all-ramdisks' own bakeRamdisk(): no
// dist/ lookup, no .sum check, no HFS+/loop-mount work at all -- just
// decrypt()'s the freshly-downloaded RestoreRamdisk exactly the way
// patchiBSS()/patchiBEC() decrypt their own components, and sends that
// straight through. This is byte-for-byte what a real, unmodified Apple
// restore would send the device.
bool Patcher::useStockRamdisk(const std::string& path, bool stockRecovery) {
    if (stockRecovery) {
        // See this method's own comment in Patcher.hpp -- whichever iBEC
        // is running (useStockIBEC()'s own unpatched output, since that's
        // the only iBEC stockRecovery ever produces) still verifies the
        // ramdisk's img3 signature over the original encrypted bytes.
        fprintf(stderr,
                "--stock-ramdisk --stock-recovery: sending the original downloaded RestoreRamdisk untouched "
                "(still encrypted, still img3-wrapped) -- the stock iBEC that's now running still verifies "
                "its signature.\n");
        outputs_.ramdisk = path;
        checkPatching();
        return true;
    }

    const FirmwareKeyPair* k = keyFor("RestoreRamdisk");
    if (!k) {
        fprintf(stderr, "useStockRamdisk: no RestoreRamdisk keys loaded\n");
        return false;
    }

    std::string outPath = outputPathFor(path);

    fprintf(stderr,
            "--stock-ramdisk: decrypting the stock RestoreRamdisk exactly as downloaded from Apple -- "
            "skipping the blackb0x-patched dist/ ramdisk and entrypoint.c entirely, for isolating "
            "whether a boot failure is in blackb0x's own ramdisk patching or earlier in the chain.\n");

    decrypt(const_cast<char*>(path.c_str()), const_cast<char*>(outPath.c_str()),
            const_cast<char*>(k->key.c_str()), const_cast<char*>(k->iv.c_str()), (char*)"FALSE", nullptr);

    outputs_.ramdisk = outPath;
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
