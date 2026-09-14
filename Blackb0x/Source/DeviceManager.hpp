//
//  DeviceManager.hpp
//  Blackb0x
//
//  C++/Linux port of DeviceManager.h. The checkm8/SHAtter exploit logic and
//  the DFU/iRecovery upload helpers are ported verbatim from the original
//  Objective-C (see DeviceManager.cpp) — this header only replaces the
//  Cocoa/AppKit-facing surface (AppleTVIcon, NSDictionary plist handling,
//  the implicit MainView/Blackb0x singleton reach-back) with plain
//  structs/callbacks a CLI front end can consume.
//

#pragma once

#include <cstdint>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <libirecovery.h>
#include <libimobiledevice/libimobiledevice.h>
}

// Declared here (with C linkage, matching their definitions in
// DeviceManager.cpp) so the friend declarations below grant access to these
// exact functions rather than implicitly declaring new C++-linkage ones.
extern "C" {
void blackb0x_irecv_device_event_cb(const irecv_device_event_t* event, void* user_data);
void blackb0x_idevice_event_cb(const idevice_event_t* event, void* user_data);
int blackb0x_irecv_progress_cb(irecv_client_t client, const irecv_event_t* event);
}

// Plain data replacement for the original AppleTVIcon (an NSImageView
// subclass). All rendering-only members (ATVImage, deviceField,
// versionField, checkingIndicator) are dropped — Cli.cpp owns all
// device-status text now, formatted directly from these fields, so there's
// no printDeviceInfo()/humanDeviceName() here to keep in sync with it.
struct AppleTVDevice {
    std::string deviceModel;
    std::string mode;
    std::string version;
    std::string buildID;
    std::string udid;
    uint64_t ecid = 0;

    int connected = 0;
    int pwnedDFU = -1;
    int jailbroken = 0;
    int jailbreakRunning = -1;
    int didTetheredBoot = 0;
    int needsPostInstall = 1;
    int waitForRecovery = 0;
};

// Replaces every dispatch_async(dispatch_get_main_queue(), ^{ view.xxx = ... })
// call site in the original DeviceManager.m. The CLI front end (a later
// phase) wires these up; DeviceManager itself no longer knows about any UI.
struct DeviceEventSink {
    std::function<void(const AppleTVDevice&)> onDeviceAdded;
    std::function<void(uint64_t ecid, const std::string& udid)> onDeviceRemoved;
    std::function<void(const std::string&)> onStatus;
    std::function<void(double)> onProgress;
    // Called when a device believed jailbroken finishes being checked for a
    // running jailbreak (AFC2 reachable) or when jailbreak status changes.
    std::function<void(const AppleTVDevice&)> onDeviceUpdated;
};

// Only the fields Blackb0x's plist handling actually ever consumed (see the
// original dictionaryFromPlist:/addDeviceWithInfo:), extracted directly via
// libplist's C API instead of building a generic NSDictionary equivalent.
struct NormalModeInfo {
    bool valid = false;
    std::string productType;
    std::string productVersion;
    std::string buildVersion;
    std::string uniqueDeviceID;
    uint64_t uniqueChipID = 0;
};

NormalModeInfo plistInfoForDeviceUUID(const std::string& udid);
int isJailbroken(const std::string& udid);
int isJailbreakRunning(const std::string& udid);

class DeviceManager {
public:
    DeviceManager();

    void setEventSink(DeviceEventSink sink) { sink_ = std::move(sink); }
    DeviceEventSink& sink() { return sink_; }

    // --- Exploits (ported verbatim from the original; see DeviceManager.cpp) ---
    int SHAtter(uint64_t ecid);
    // pwnTool: "gaster" (default off Apple platforms) or "blackb0x-pwn"
    // (Apple-only, default there — see Cli.hpp's CliOptions::pwnTool and
    // docs/HISTORY.md for why gaster specifically doesn't work on macOS).
    int checkm8(uint64_t ecid, const std::string& pwnTool = "gaster");

    irecv_client_t get_tv(uint64_t ecid);

    // --- iRecovery upload helpers ---
    // stockRecovery/stockSecurom: mirror Cli.hpp's CliOptions of the same
    // name.
    //   - stockRecovery alone: device was genuinely pwned by this run's own
    //     checkm8 call, just skip the hard "PWND:[" serial-string check
    //     that boot_client() (DeviceManager.cpp) would otherwise still
    //     enforce -- has no practical effect here since a real checkm8 run
    //     already leaves that string in place, kept for symmetry/defense in
    //     depth.
    //   - stockSecurom: the device was never pwned at all, and for
    //     AppleTV3,1/3,2 the usual boot_client() soft-DFU path is skipped
    //     entirely in favor of the standard irecv_send_file() DFU-class
    //     protocol -- boot_client()'s raw control-transfer sequence is
    //     shaped around checkm8's own post-exploit memory-corruption state,
    //     not the real DFU protocol, so it has no reason to work against a
    //     device whose SecureROM was never exploited (see boot_client()'s
    //     own comment). Before sending, the file also gets personalized
    //     with a real, ECID/nonce-bound SHSH ticket fetched from Apple's
    //     TSS server (see Personalize.hpp's personalizeIMG3Component()) --
    //     a genuinely un-pwned SecureROM's signature check requires this
    //     regardless of how correct the delivery protocol is. buildIdentity
    //     is the matching BuildManifest.plist identity (IPSW.hpp's
    //     ManifestInfo::buildIdentity) that personalization needs;
    //     deviceModel/buildID (IPSW.hpp's ManifestInfo::realBuildID) let
    //     it check signedBuildsForDevice() before ever sending a real TSS
    //     request. All four ignored unless stockSecurom is set.
    int sendiBSS(const std::string& path, uint64_t ecid, bool stockRecovery = false, bool stockSecurom = false,
                 std::shared_ptr<void> buildIdentity = nullptr, const std::string& deviceModel = "",
                 const std::string& buildID = "");
    int sendiBEC(const std::string& path, uint64_t ecid);
    // Gated on stockRecovery (Cli.hpp's CliOptions), not stockSecurom --
    // this is about whether useStockIBEC()'s genuinely-unpatched iBEC is
    // what's running, not whether checkm8 ran to get there (see
    // Cli.cpp's sendComponentsToDevice() own comment for the full
    // reasoning). Fetches and sends the combined APTicket that
    // authorizes every component after iBSS together -- see
    // Personalize.hpp's own fetchAPTicket() comment for why this is a
    // separate step from personalizing iBSS itself. Must be called once,
    // after sendiBEC() succeeds and before
    // sendDeviceTree()/sendRamdisk()/sendKernelCache() -- a stock iBEC
    // won't accept any of those without this on file first, matching
    // real idevicerestore's own ordering (recovery.c's
    // recovery_send_ticket(), called immediately on entering Recovery
    // mode, before anything else).
    int sendAPTicket(uint64_t ecid, std::shared_ptr<void> buildIdentity, const std::string& deviceModel,
                      const std::string& buildID);
    int sendRamdisk(const std::string& path, uint64_t ecid);
    int sendKernelCache(const std::string& path, uint64_t ecid);
    int sendDeviceTree(const std::string& path, uint64_t ecid);

    // --- Jailbreak status polling (was checkJailbreak/checkJailbreakRunning) ---
    void checkJailbreak(const std::string& udid);

    // --- Device bookkeeping (replaces MainView.AppleTVs) ---
    std::vector<AppleTVDevice> devicesSnapshot() const;
    AppleTVDevice* deviceWithUDID(const std::string& udid, uint64_t ecid = 0);

    static DeviceManager* instance() { return instance_; }

private:
    static DeviceManager* instance_;

    DeviceEventSink sink_;
    mutable std::mutex devicesMutex_;
    // deque, not vector: push_back must not invalidate AppleTVDevice*
    // pointers a background thread (e.g. checkJailbreak's poll loop) may
    // still be holding onto while a second device connects concurrently.
    std::deque<AppleTVDevice> devices_;

    void newDevice(const std::string& productType, const std::string& modeStr,
                   const std::string& version, const std::string& buildID,
                   uint64_t ecid, const std::string& udid, int pwnedDFU);
    void disconnectDevice(uint64_t ecid, const std::string& udid);
    void checkJailbreakRunning(const std::string& udid);

    // One full pass through the checkm8 exploit sequence — shells out to
    // either the vendored `gaster` binary or `blackb0x-pwn` (pwnTool;
    // rather than driving the low-level USB request sequence itself — see
    // docs/HISTORY.md for why); checkm8() (public) retries this a bounded
    // number of times on failure — see its own comment for why.
    bool checkm8Attempt(uint64_t ecid, const std::string& pwnTool);

    friend void ::blackb0x_irecv_device_event_cb(const irecv_device_event_t* event, void* user_data);
    friend void ::blackb0x_idevice_event_cb(const idevice_event_t* event, void* user_data);
    friend int ::blackb0x_irecv_progress_cb(irecv_client_t client, const irecv_event_t* event);
};
