//
//  Patcher.hpp
//  Blackb0x
//
//  C++/Linux port of Patcher.h/.mm. patchiBSS/patchiBEC/patchKernel are
//  ported near-verbatim (thin wrappers around the already-portable
//  decrypt()/iBootPatcher()/patch_kernel()). patchRamdisk is the one
//  substantially rewritten method: the original shelled out to macOS's
//  hdiutil (attach/detach/resize/create) and tar to build the ramdisk's
//  filesystem contents. This port instead:
//    1. decrypt()s the ramdisk exactly as before (Apple's own AES-CBC
//       encryption, via the already-portable xpwntool.c).
//    2. Detects whether the decrypted image is UDIF-wrapped ("koly" trailer)
//       or already a raw HFS+ partition — confirmed empirically that older
//       (A4-era Apple TV 2/3) restore ramdisks decrypt straight to raw HFS+
//       with no UDIF wrapper at all, unlike the UDIF-wrapped root-filesystem
//       images third_party/xpwn's own ipsw-patch/main.c reference code
//       assumes. Only genuinely UDIF-wrapped images go through xpwn's
//       extractDmg() to unwrap to a raw HFS+ partition image first.
//    3. Builds a brand-new HFS+ image at the final target size via the
//       system `mkfs.hfsplus`, copies the ORIGINAL volume's entire tree and
//       header identity fields (volume label, finderInfo, createDate,
//       lastMountedVersion) into it, then loop-mounts it via the real Linux
//       kernel `hfsplus` driver and injects every payload with ordinary
//       `tar`/`cp`. This is the third design tried for this step — an
//       in-memory xpwn Volume (add_hfs()/grow_hfs()) and a from-scratch
//       userspace HFS+ writer (libhfsp) were both tried first and both hit
//       real, reproducible data-corrupting bugs under real firmware
//       payloads; growing the ORIGINAL volume in place (rather than
//       building a right-sized replacement) was also tried and is
//       genuinely unsupported on Linux — see the long comment on
//       patchRamdisk() in Patcher.cpp for the full investigation, evidence,
//       and why this is the design that actually survives real data.
//    4. Writes the modified image back out, overwriting the decrypted file
//       in place (replaces `hdiutil detach`) — rewrapped via buildDmg() only
//       if step 2 found it UDIF-wrapped to begin with; otherwise written as
//       raw bytes, matching what decrypt() actually produced.
//    5. decrypt()s (re-encrypts) exactly as before.
//  Because step 3 loop-mounts a device node and preserves arbitrary file
//  ownership (setuid binaries, etc.) via `cp -a`, this function needs
//  CAP_SYS_ADMIN and CAP_CHOWN — the calling process must run as root.
//

#pragma once

#include <functional>
#include <map>
#include <optional>
#include <string>

#include "IPSW.hpp"

struct PatchedComponents {
    std::optional<std::string> iBSS;
    std::optional<std::string> iBECDowngrade;
    std::optional<std::string> iBECBoot;
    std::optional<std::string> kernel;
    std::optional<std::string> ramdisk;
    std::optional<std::string> deviceTree;
};

class Patcher {
public:
    Patcher();

    // Called once firmware keys are needed for a given device/build; loads
    // them and (re-)runs any patch step whose input path was already set.
    void loadKeysForDevice(const std::string& deviceID, const std::string& buildID);

    bool patchiBSS(const std::string& path);
    bool patchiBEC(const std::string& path, const std::string& flags = "", bool ticket = true);
    bool patchKernel(const std::string& path, const std::string& productVersion);
    bool patchRamdisk(const std::string& path);

    void setDeviceTreePath(const std::string& path);

    bool onlyBootComponents = false;

    // Called once every component checkPatching() requires is available
    // (replaces MainView's componentsReady:). Fired synchronously from
    // whichever patch*() call completes the last required component.
    std::function<void(const PatchedComponents&)> onComponentsReady;

private:
    IpswFetch fetcher_;
    std::map<std::string, FirmwareKeyPair> keys_;
    PatchedComponents outputs_;
    // Set by loadKeysForDevice() — patchRamdisk() needs these to compute
    // which dist/<device>_<buildID>-Ramdisk.dmg bake-all-ramdisks should
    // already have produced.
    std::string deviceModel_;
    std::string buildID_;

    std::string getRealVersion(const std::string& version) const;
    void checkPatching();
    void clearComponents();

    const FirmwareKeyPair* keyFor(const std::string& imageName) const;
};
