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
#include <memory>
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

    // --stock-securom (Cli.hpp's CliOptions): the matching BuildManifest.plist
    // identity (IPSW.hpp's ManifestInfo::buildIdentity, passed through
    // unchanged) -- sendComponentsToDevice()/DeviceManager::sendiBSS() need
    // this to personalize iBSS with a real TSS-issued SHSH ticket. Unset
    // (nullptr) whenever stockSecurom isn't in play.
    std::shared_ptr<void> buildIdentity;
    // IPSW.hpp's ManifestInfo::realBuildID, passed through unchanged --
    // personalizeIMG3Component() (Personalize.cpp) needs this alongside
    // buildIdentity above, to check against signedBuildsForDevice()
    // before ever sending a real TSS request.
    std::string buildID;
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

    // --stock-ramdisk (Cli.hpp's CliOptions): decrypts and sends the
    // RestoreRamdisk exactly as downloaded from Apple -- no /blackb0x
    // merge, no entrypoint.c, none of bake-all-ramdisks' own work at all
    // -- instead of patchRamdisk()'s usual dist/<device>_<buildID>-
    // Ramdisk.dmg lookup. A diagnostic: if the device boots this one fine,
    // the failure is somewhere in blackb0x's own ramdisk patching or
    // entrypoint.c; if it fails the same way even with a stock ramdisk,
    // the failure is earlier in the chain (iBSS/iBEC/KASLR-boot-args
    // patches, kernelcache, devicetree, or the boot trigger itself).
    //
    // stockRecovery: whoever actually verifies this ramdisk's signature is
    // whichever iBEC is running, not this function's own caller -- with
    // blackb0x's own patched iBEC (stockRecovery false: --stock-ramdisk/
    // --stock-firmware without --stock-recovery), RSA/ticket checks are
    // already bypassed and a decrypted, unwrapped ramdisk is fine. With
    // useStockIBEC()'s stock iBEC (stockRecovery true), those checks are
    // intact, so decrypting first would invalidate the signature before
    // that iBEC ever gets to check it -- send the original file untouched
    // instead, same as useStockIBSS()/useStockIBEC() do for the same
    // reason.
    bool useStockRamdisk(const std::string& path, bool stockRecovery = false);

    // --stock-recovery (Cli.hpp's CliOptions): decrypts and sends iBSS/iBEC
    // exactly as downloaded from Apple -- no iBootPatcher() call at all,
    // so no boot-args injection, no KASLR patch, no ticket/RSA-check
    // bypass. checkm8/pwnTool still needs to run beforehand for the
    // device to accept ANY file at all (SecureROM's own signature
    // enforcement is what checkm8 bypasses, separate from iBoot's own
    // patches these skip) -- this only changes which iBSS/iBEC content
    // gets uploaded once pwned, not whether pwning happens. Same
    // diagnostic purpose as useStockRamdisk() above, orthogonal to it: if
    // the device boots a fully-stock bootloader fine, the failure is in
    // blackb0x's own iBSS/iBEC patches; if it fails the same way, the
    // failure is elsewhere (ramdisk, kernelcache, devicetree, or the boot
    // trigger itself).
    // stockSecurom (--stock-securom, Cli.hpp's CliOptions): decrypt()ing
    // still produces a bare, decrypted, unwrapped binary (the img3
    // container gets stripped) -- fine for the checkm8-shaped soft-DFU
    // upload path (boot_client() in DeviceManager.cpp), which sends raw
    // post-verification bytes since checkm8 skips SecureROM's check
    // entirely. But sendiBSS()'s --stock-securom route sends this to a
    // device whose SecureROM was never exploited via the *standard* DFU
    // protocol, which hands the received bytes to SecureROM's own image
    // loader -- that loader expects (and cryptographically verifies) a
    // real img3 container, computed over the ORIGINAL encrypted bytes as
    // signed by Apple. A decrypted/unwrapped binary fails that check
    // immediately regardless of its content. So when stockSecurom is set,
    // skip decrypt() entirely and point straight at the original
    // downloaded file, untouched -- still encrypted, still img3-wrapped,
    // exactly as Apple shipped and signed it.
    bool useStockIBSS(const std::string& path, bool stockSecurom = false);
    // Unlike useStockIBSS() above, this one's correctness doesn't depend
    // on stockSecurom at all: iBEC is only ever sent once some iBSS is
    // already running, and useStockIBEC() is only ever called when
    // useStockIBSS() was too (both gated on stockRecovery in Cli.cpp) --
    // meaning that running iBSS is always useStockIBSS()'s own unpatched
    // output, whether it got there via checkm8/boot_client() or real DFU.
    // Either way its own RSA check is intact (useStockIBSS() never calls
    // iBootPatcher()), so it will verify iBEC's img3 signature over the
    // original encrypted bytes exactly like a real SecureROM does for
    // iBSS -- decrypting iBEC first invalidates that signature
    // unconditionally, not just under --stock-securom. Always sends the
    // original downloaded file untouched.
    bool useStockIBEC(const std::string& path);

    // --stock-firmware (Cli.hpp's CliOptions): the kernelcache half of the
    // same idea -- decrypts and sends the kernelcache exactly as
    // downloaded from Apple, no patch_kernel()/CBPatcher call at all (no
    // tfp0, no AMFI/memcmp bypass, no sandbox patch). Combined with
    // useStockIBSS()/useStockIBEC()/useStockRamdisk() (what --stock-firmware
    // sets all of, together) this sends a completely unmodified firmware
    // suite end to end -- devicetree is already always sent unmodified
    // regardless (see setDeviceTreePath()'s own comment), so there's no
    // separate stock/patched distinction to make there. If a fully-stock
    // suite boots fine, checkm8 and the bootx trigger are both confirmed
    // working and the failure is specifically in one of blackb0x's own
    // patches; if it fails the same way even fully stock, the failure is
    // somewhere checkm8/the boot trigger doesn't control at all (or this
    // device/firmware genuinely can't complete this boot path regardless
    // of what's sent).
    //
    // stockRecovery: same reasoning as useStockRamdisk()'s own comment
    // above -- the kernelcache's signature is checked by whichever iBEC
    // is running, so this needs to stay encrypted/img3-wrapped whenever
    // that's useStockIBEC()'s stock iBEC (stockRecovery true), and can
    // stay decrypt()-only when it's blackb0x's own patched iBEC
    // (stockRecovery false).
    bool useStockKernel(const std::string& path, bool stockRecovery = false);

    void setDeviceTreePath(const std::string& path);

    // See PatchedComponents::buildIdentity's own comment.
    void setBuildIdentity(std::shared_ptr<void> identity) { outputs_.buildIdentity = std::move(identity); }
    // See PatchedComponents::buildID's own comment.
    void setBuildID(const std::string& buildID) { outputs_.buildID = buildID; }

    bool onlyBootComponents = false;

    // --dont-check-firmware-sums (Cli.hpp's CliOptions): skip
    // patchRamdisk()'s own .sum sidecar staleness check (see that
    // function's comment) and use dist/<device>_<buildID>-Ramdisk.dmg as-
    // is even if it doesn't match ramdisk/'s current content. For
    // iterating without re-running bake-all-ramdisks every time — not a
    // default, since it reintroduces exactly the "silently ship a stale
    // ramdisk" failure mode the check exists to catch.
    bool dontCheckFirmwareSums = false;

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
