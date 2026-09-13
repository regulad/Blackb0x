//
//  Cli.cpp
//  Blackb0x
//
//  See Cli.hpp for what this replaces. The overall flow (select a device,
//  make sure it's in DFU mode, run the model-appropriate exploit, download
//  firmware components for the right build, patch them, send them to the
//  device) is a direct, linear port of MainView.m's
//  jailbreakClick/tetherbootClick/checkExploit/downloadComponentsForBuildID/
//  componentsReady chain — the original's dispatch_async-driven UI updates
//  become plain sequential C++ (there's no GUI event loop to marshal onto),
//  and MainView's `spawnDFUHelper` popup (which just displayed instructions
//  and waited for the user to notice and re-click Jailbreak/Boot) becomes an
//  actual blocking poll loop, since a CLI has no button to re-click.
//

#include "Cli.hpp"

#include "DeviceManager.hpp"
#include "IPSW.hpp"
#include "IPSWDownloader.hpp"
#include "Patcher.hpp"
#include "ResourcePath.hpp"

extern "C" {
#include <plist/plist.h>
}

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Option parsing
// ---------------------------------------------------------------------------

void printCliUsage(const char* argv0) {
    printf("Usage: %s [options]\n", argv0);
    printf("  --ecid <hex-or-decimal>   Pre-select a device by ECID (skips the menu)\n");
    printf("  --udid <udid>             Pre-select a device by UDID (Normal mode only)\n");
    printf("  --tether-boot             Tether-boot an already-jailbroken device\n");
    printf("                            (default: install the jailbreak fresh)\n");
    printf("  --dry-run                 Do everything up to but not including the\n");
    printf("                            exploit and the USB upload to the device —\n");
    printf("                            prints what would run/be sent instead\n");
    printf("  --help                    Show this message\n");
    printf("\n");
    printf("blackb0x must be run as root: patching the restore ramdisk loop-mounts a\n");
    printf("real HFS+ image (needs CAP_SYS_ADMIN) and preserves file ownership via\n");
    printf("`cp -a` (needs CAP_CHOWN), and talking to a DFU-mode device needs raw USB\n");
    printf("access. Run it via `sudo blackb0x ...`.\n");
}

CliOptions parseCliOptions(int argc, char** argv) {
    CliOptions options;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        auto nextArg = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires a value\n", flag);
                exit(2);
            }
            return argv[++i];
        };

        if (arg == "--ecid") {
            options.ecid = strtoull(nextArg("--ecid").c_str(), nullptr, 0);
        } else if (arg == "--udid") {
            options.udid = nextArg("--udid");
        } else if (arg == "--tether-boot") {
            options.tetherBoot = true;
        } else if (arg == "--dry-run") {
            options.dryRun = true;
        } else if (arg == "--help" || arg == "-h") {
            options.help = true;
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            printCliUsage(argv[0]);
            exit(2);
        }
    }
    return options;
}

// ---------------------------------------------------------------------------
// Device selection / DFU-mode wait
// ---------------------------------------------------------------------------

namespace {

// The original hardcoded this exact firmware build for a fresh jailbreak
// install (MainView.m's downloadInstall calling
// downloadComponentsForBuildID:@"10B329a") — the jailbreak patches are
// built/tested against this specific build, not a UI default to change
// lightly.
const std::string kJailbreakTargetBuild = "10B329a";

void printDeviceLine(const AppleTVDevice& d, int index) {
    printf("  [%d] %s (%llu) in %s", index, d.deviceModel.c_str(), (unsigned long long)d.ecid, d.mode.c_str());
    if (!d.version.empty()) printf(", %s", d.version.c_str());
    if (d.jailbroken) printf(", jailbroken");
    printf("\n");
}

// Blocks until at least one AppleTV is connected, then either auto-selects
// the one matching --ecid/--udid, the sole connected device, or prompts an
// interactive numbered menu — replacing MainView's icon click-to-select.
// Prints a one-time reminder if nothing shows up within 5 seconds: a device
// left mid-exploit by a failed checkm8/SHAtter attempt (some of its early
// failure paths return without ever calling irecv_reset()/irecv_close(),
// see docs/HISTORY.md) can end up wedged at the USB level and stop enumerating
// entirely until it's fully power-cycled, not just re-DFU'd.
std::optional<uint64_t> selectDevice(DeviceManager& deviceManager, const CliOptions& options) {
    printf("Waiting for an Apple TV 2 or 3 (any mode)...\n");
    auto waitStart = std::chrono::steady_clock::now();
    bool remindedToPowerCycle = false;
    for (;;) {
        auto devices = deviceManager.devicesSnapshot();

        if (!remindedToPowerCycle && std::chrono::steady_clock::now() - waitStart >= std::chrono::seconds(5)) {
            printf("Still searching... Please power-cycle your Apple TV after a failed exploit attempt.\n");
            remindedToPowerCycle = true;
        }

        if (options.ecid != 0) {
            for (auto& d : devices) {
                if (d.ecid == options.ecid) return d.ecid;
            }
        } else if (!options.udid.empty()) {
            for (auto& d : devices) {
                if (d.udid == options.udid) return d.ecid;
            }
        } else if (devices.size() == 1) {
            return devices[0].ecid;
        } else if (devices.size() > 1) {
            printf("\nMultiple devices connected:\n");
            for (size_t i = 0; i < devices.size(); i++) printDeviceLine(devices[i], (int)i);
            printf("Select a device number: ");
            fflush(stdout);
            int choice = -1;
            if (scanf("%d", &choice) == 1 && choice >= 0 && (size_t)choice < devices.size()) {
                return devices[(size_t)choice].ecid;
            }
            printf("Invalid selection, still waiting.\n");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// Replaces MainView's spawnDFUHelper — prints the same instructions, but
// actually polls for the transition rather than requiring the user to
// notice a popup and re-click a button, since a CLI has no button.
bool waitForDFUMode(DeviceManager& deviceManager, uint64_t ecid, AppleTVDevice& outDevice) {
    for (;;) {
        AppleTVDevice* d = deviceManager.deviceWithUDID("", ecid);
        if (d) {
            if (d->mode == "DFU") {
                outDevice = *d;
                return true;
            }
        } else {
            // Device with this ECID isn't currently connected at all (it may
            // be mid-reboot into DFU) — keep waiting rather than failing.
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
}

// Replaces MainView's checkExploit: — per-model exploit dispatch. Verbatim
// device-model branching from the original, not simplified. `dryRun` skips
// the actual SHAtter/checkm8 USB call (the point where this function stops
// being observation and starts writing exploit payloads into the device),
// printing what would have run instead.
bool checkExploit(DeviceManager& deviceManager, const AppleTVDevice& device, bool dryRun) {
    if (device.pwnedDFU) return true;

    if (device.deviceModel == "AppleTV2,1") {
        if (dryRun) {
            printf("(dry run) Would try SHAtter\n");
            return true;
        }
        printf("Trying SHAtter...\n");
        if (deviceManager.SHAtter(device.ecid) == 0) {
            fprintf(stderr, "Exploit failed.\n");
            return false;
        }
        return true;
    }

    if (device.deviceModel == "AppleTV3,1") {
        fprintf(stderr,
                "AppleTV3,1 needs external hardware for checkm8: plug in an Arduino and use\n"
                "synackuk's checkm8 tool to put the device into pwned DFU first, then re-run\n"
                "blackb0x with --ecid %llu.\n",
                (unsigned long long)device.ecid);
        return false;
    }

    if (device.deviceModel == "AppleTV3,2") {
        if (dryRun) {
            printf("(dry run) Would try checkm8\n");
            return true;
        }
        printf("Trying checkm8...\n");
        if (deviceManager.checkm8(device.ecid) == 0) {
            fprintf(stderr, "Exploit failed.\n");
            return false;
        }
        return true;
    }

    fprintf(stderr, "Unrecognized device model: %s\n", device.deviceModel.c_str());
    return false;
}

// ---------------------------------------------------------------------------
// Firmware download + patch
// ---------------------------------------------------------------------------

// ManifestInfo / parseManifest() now live in IPSW.hpp/.cpp — shared with
// bake-all-ramdisks (BakeAllRamdisks.cpp), which needs the exact same
// BuildManifest.plist parsing to locate RestoreRamDisk across every known
// firmware, not just the one connected device this CLI flow targets.

// Replaces MainView's downloadComponentsForBuildID: — downloads every
// component sequentially (see the note in Cli.hpp/this file's header
// comment on why this isn't parallelized like the original's dispatch_async
// fire-and-forget) into ipswDataRoot(), patching each immediately after it
// lands (matching the original's setXPath: custom setters, which triggered
// the same patch* calls as a side effect of assignment).
std::optional<PatchedComponents> downloadAndPatchComponents(Patcher& patcher, const AppleTVDevice& device,
                                                              const std::string& buildToRequest,
                                                              bool onlyBootComponents) {
    patcher.onlyBootComponents = onlyBootComponents;

    printf("Downloading firmware for %s %s...\n", device.deviceModel.c_str(), buildToRequest.c_str());
    IpswFetch fetcher;
    std::string firmwareURL = fetcher.firmwareURLForDevice(device.deviceModel, buildToRequest);
    if (firmwareURL.empty()) {
        fprintf(stderr, "Could not resolve a firmware URL for %s %s\n", device.deviceModel.c_str(),
                buildToRequest.c_str());
        return std::nullopt;
    }

    FragmentDownloader downloader(firmwareURL);
    if (!downloader.open()) {
        fprintf(stderr, "Failed to open remote IPSW\n");
        return std::nullopt;
    }

    std::string workDir = ipswDataRoot() + "/" + device.deviceModel + "/" + buildToRequest;
    fs::create_directories(workDir);

    std::string manifestPath = workDir + "/BuildManifest.plist";
    if (!downloader.downloadComponent("BuildManifest.plist", manifestPath, nullptr)) {
        fprintf(stderr, "Failed to download BuildManifest.plist\n");
        return std::nullopt;
    }

    auto manifest = parseManifest(manifestPath, onlyBootComponents);
    if (!manifest) {
        fprintf(stderr, "Failed to parse BuildManifest.plist\n");
        return std::nullopt;
    }

    patcher.loadKeysForDevice(device.deviceModel, manifest->realBuildID);

    std::optional<PatchedComponents> result;
    patcher.onComponentsReady = [&](const PatchedComponents& c) { result = c; };

    auto downloadAndPatch = [&](const char* label, const std::string& remotePath, auto&& patchFn) {
        if (remotePath.empty()) return;
        std::string localPath = workDir + "/" + fs::path(remotePath).filename().string();
        printf("Downloading %s...\n", label);
        // The underlying progress callback fires far more often than the
        // percentage actually changes (once per chunk received, not once
        // per percentage point) — without tracking the last value printed,
        // "pct % 25 == 0" reprints the same "0%"/"25%"/etc line every time
        // a chunk happens to land while still at that percentage, which is
        // most of them.
        unsigned int lastPrinted = 101;
        if (!downloader.downloadComponent(remotePath, localPath, [label, &lastPrinted](unsigned int pct) {
                if (pct % 25 == 0 && pct != lastPrinted) {
                    printf("  %s: %u%%\n", label, pct);
                    lastPrinted = pct;
                }
            })) {
            fprintf(stderr, "Failed to download %s\n", label);
            return;
        }
        patchFn(localPath);
    };

    downloadAndPatch("iBSS", manifest->iBSSPath, [&](const std::string& path) { patcher.patchiBSS(path); });

    downloadAndPatch("iBEC", manifest->iBECPath, [&](const std::string& path) {
        // Verbatim version heuristic from the original setIBECPath: — iBEC
        // files for 4.x-era firmware need empty flags and no ticket.
        if (path.find("4.") != std::string::npos) {
            patcher.patchiBEC(path, "", false);
        } else {
            patcher.patchiBEC(path);
        }
    });

    downloadAndPatch("KernelCache", manifest->kernelCachePath, [&](const std::string& path) {
        patcher.patchKernel(path, manifest->productVersion);
    });

    downloadAndPatch("DeviceTree", manifest->deviceTreePath,
                      [&](const std::string& path) { patcher.setDeviceTreePath(path); });

    if (!onlyBootComponents) {
        downloadAndPatch("RestoreRamdisk", manifest->restoreRamdiskPath,
                          [&](const std::string& path) { patcher.patchRamdisk(path); });
    }

    if (!result) {
        fprintf(stderr, "Not all required components patched successfully\n");
        return std::nullopt;
    }
    return result;
}

// Replaces MainView's componentsReady: — the final upload sequence. Sends
// iBSS first; aborts back to a fresh DFU wait if that fails (matching the
// original's "spawn the DFU helper again" recovery path). Which iBEC gets
// sent, and whether DeviceTree/Ramdisk get sent at all, depends on whether
// this is a tether-boot of an already-jailbroken device or a fresh install
// — verbatim from the original's `self.selected_device.jailbroken == 1`
// branch.
bool sendComponentsToDevice(DeviceManager& deviceManager, AppleTVDevice& device, const PatchedComponents& components,
                             bool tetherBoot, bool dryRun) {
    if (dryRun) {
        printf("(dry run) Would send:\n");
        printf("  iBSS%s\n", components.iBSS ? "" : " (missing, would fail here)");
        if (tetherBoot) {
            printf("  iBEC (downgrade)%s\n", components.iBECDowngrade ? "" : " (missing)");
        } else {
            printf("  iBEC (boot)%s\n", components.iBECBoot ? "" : " (missing)");
            printf("  DeviceTree%s\n", components.deviceTree ? "" : " (missing)");
            printf("  Ramdisk%s\n", components.ramdisk ? "" : " (missing)");
        }
        printf("  KernelCache%s\n", components.kernel ? "" : " (missing, would fail here)");
        printf("(dry run) Would then wait for the Apple TV to %s\n", tetherBoot ? "boot" : "reboot");
        return true;
    }

    printf("Sending iBSS -> ");
    fflush(stdout);
    if (deviceManager.sendiBSS(*components.iBSS, device.ecid) != 0) {
        printf("Error\n");
        fprintf(stderr, "Failed to send iBSS. Please re-enter DFU mode and try again.\n");
        return false;
    }
    printf("Sent\n");

    if (tetherBoot) {
        printf("Sending iBEC (downgrade) -> ");
        fflush(stdout);
        int i = components.iBECDowngrade ? deviceManager.sendiBEC(*components.iBECDowngrade, device.ecid) : -1;
        printf("%s\n", (i == 0) ? "Sent" : "Error");
        device.didTetheredBoot = 1;
    } else {
        printf("Sending iBEC (boot) -> ");
        fflush(stdout);
        int i = components.iBECBoot ? deviceManager.sendiBEC(*components.iBECBoot, device.ecid) : -1;
        printf("%s\n", (i == 0) ? "Sent" : "Error");

        printf("Sending DeviceTree -> ");
        fflush(stdout);
        i = components.deviceTree ? deviceManager.sendDeviceTree(*components.deviceTree, device.ecid) : -1;
        printf("%s\n", (i == 0) ? "Sent" : "Error");

        printf("Sending Ramdisk -> ");
        fflush(stdout);
        i = components.ramdisk ? deviceManager.sendRamdisk(*components.ramdisk, device.ecid) : -1;
        printf("%s\n", (i == 0) ? "Sent" : "Error");

        device.needsPostInstall = 1;
    }

    printf("Sending KernelCache -> ");
    fflush(stdout);
    int kernelResult = components.kernel ? deviceManager.sendKernelCache(*components.kernel, device.ecid) : -1;
    printf("%s\n", (kernelResult == 0) ? "Sent" : "Error");

    if (kernelResult != 0) return false;

    device.waitForRecovery = 1;
    printf("%s\n", tetherBoot ? "Waiting for Apple TV to boot" : "Waiting for Apple TV to reboot");
    return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// Top-level session
// ---------------------------------------------------------------------------

int runCli(const CliOptions& options) {
    if (options.help) {
        printCliUsage("blackb0x");
        return 0;
    }

    printf("blackb0x (regulad's linux port) — Apple TV 2/3 jailbreak tool\n");

    // Read once, up front — pushed over AFC2 once the jailbreak is confirmed
    // running (see the onDeviceUpdated sink below), not baked into the
    // ramdisk. Missing is a warning, not a hard failure: unlike the old
    // ramdisk-baked path, the jailbreak itself doesn't depend on this.
    std::string authorizedKeysContents;
    if (auto authorizedKeysPath = findUserAuthorizedKeysPath()) {
        std::ifstream f(*authorizedKeysPath, std::ios::binary);
        std::ostringstream ss;
        ss << f.rdbuf();
        authorizedKeysContents = ss.str();
        printf("SSH access will be granted from %s once the jailbreak finishes booting.\n\n",
               authorizedKeysPath->c_str());
    } else {
        printf(
            "No ~/.ssh/authorized_keys found — SSH access will not be granted automatically.\n"
            "Generate one first (e.g. `ssh-keygen`) and add your public key there if you want\n"
            "the jailbroken device reachable over SSH.\n\n");
    }

    if (options.dryRun) {
        printf("(dry run) Discovery, DFU wait, download, and patch all happen for real.\n");
        printf("(dry run) Only the exploit and the USB upload are skipped.\n\n");
    }
    fflush(stdout);

    if (geteuid() != 0) {
        fprintf(stderr, "blackb0x must run as root (raw USB access). Re-run with sudo.\n");
        return 1;
    }

    // Nothing this tool can ever do succeeds without at least one baked
    // ramdisk sitting in dist/ — patchRamdisk() (Patcher.cpp) checks for a
    // specific device+firmware's own entry once a device is actually
    // connected, but an entirely empty dist/ means bake-all-ramdisks was
    // simply never run at all, which is worth failing on immediately
    // rather than waiting for a device to show up first.
    {
        bool haveAnyRamdisk = false;
        std::error_code ec;
        if (fs::exists("dist", ec) && fs::is_directory("dist", ec)) {
            for (const auto& entry : fs::directory_iterator("dist", ec)) {
                if (ec) break;
                if (entry.path().extension() == ".dmg") {
                    haveAnyRamdisk = true;
                    break;
                }
            }
        }
        if (!haveAnyRamdisk) {
            fprintf(stderr,
                    "blackb0x: dist/ has no baked ramdisks at all. Run this once (as root) before using\n"
                    "blackb0x against any device:\n"
                    "  sudo ./bake-all-ramdisks\n");
            return 1;
        }
    }

    // Collapses the original Blackb0x.h/.m singleton (which just held one
    // DeviceManager) — this session IS that single instance, function-local
    // rather than a dispatch_once class method.
    DeviceManager deviceManager;
    Patcher patcher;

    // One line per real event, no restating what the previous line already
    // said — this is the only place device connection/exploit status gets
    // printed; DeviceManager itself stays quiet on success and only writes
    // to stderr for genuine, otherwise-unexplained failures.
    DeviceEventSink sink;
    sink.onDeviceAdded = [](const AppleTVDevice& d) {
        printf("Connected to %s (%llu) in %s\n", d.deviceModel.c_str(), (unsigned long long)d.ecid,
               d.mode.c_str());
    };
    sink.onDeviceRemoved = [](uint64_t ecid, const std::string& udid) {
        (void)udid;
        printf("Disconnected (%llu)\n", (unsigned long long)ecid);
    };
    sink.onStatus = [](const std::string& status) { printf("%s\n", status.c_str()); };
    std::set<std::string> authorizedKeysPushedFor;
    sink.onDeviceUpdated = [&authorizedKeysContents, &authorizedKeysPushedFor](const AppleTVDevice& d) {
        if (d.jailbroken) {
            printf("%s is jailbroken%s\n", d.deviceModel.c_str(), d.jailbreakRunning == 1 ? " and running" : "");
        }
        if (d.jailbreakRunning == 1 && !authorizedKeysContents.empty() &&
            authorizedKeysPushedFor.insert(d.udid).second) {
            if (pushAuthorizedKeys(d.udid, authorizedKeysContents)) {
                printf("SSH access granted on %s\n", d.deviceModel.c_str());
            } else {
                fprintf(stderr, "Failed to push authorized_keys to %s over AFC2\n", d.deviceModel.c_str());
            }
        }
    };
    deviceManager.setEventSink(sink);

    auto ecidOpt = selectDevice(deviceManager, options);
    if (!ecidOpt) {
        fprintf(stderr, "No device selected.\n");
        return 1;
    }
    uint64_t ecid = *ecidOpt;

    AppleTVDevice device;
    {
        AppleTVDevice* d = deviceManager.deviceWithUDID("", ecid);
        if (!d) {
            fprintf(stderr, "Selected device disconnected before it could be used.\n");
            return 1;
        }
        device = *d;
    }

    bool tetherBoot = options.tetherBoot;

    if (tetherBoot) {
        // Verbatim from MainView's tetherbootClick: an already-jailbroken
        // AppleTV2,1 with unknown version/build is assumed to be 7.1.2 —
        // a real, documented device-support assumption, not a placeholder.
        // Any other model must already have connected in Normal mode once
        // (so its real version/buildID are known) before a tether-boot can
        // be attempted at all.
        device.jailbroken = 1;
        if (device.version.empty() || device.buildID.empty()) {
            if (device.deviceModel == "AppleTV2,1") {
                device.version = "7.1.2";
                device.buildID = "11D258";
            } else {
                fprintf(stderr, "Please connect this device in Normal Mode first (need its version/build).\n");
                return 1;
            }
        }
        // Verbatim from refreshInterface: AppleTV2,1 on 6.1.4 doesn't
        // support the tether-boot path.
        if (device.deviceModel == "AppleTV2,1" && device.version == "6.1.4") {
            fprintf(stderr, "Tethered boot is not supported on AppleTV2,1 6.1.4.\n");
            return 1;
        }
    }

    if (device.mode != "DFU") {
        printf("\nTo enter DFU mode, on the Apple TV's remote:\n\n");
        printf("  1. Hold MENU + DOWN together until the LED starts flashing rapidly\n");
        printf("     (~6 seconds), then let go of BOTH buttons completely.\n");
        printf("     This alone only reaches Recovery Mode, which blinks the same\n");
        printf("     way DFU does -- it is not DFU mode yet.\n");
        printf("  2. Immediately hold MENU + PLAY together until the LED starts\n");
        printf("     flashing rapidly again (~6-7 seconds), then let go. This second\n");
        printf("     step is what actually puts it in DFU mode.\n\n");
        printf("If this repeatedly doesn't take: try a different micro-USB cable\n");
        printf("(a bad cable is a common silent failure) and make sure the remote\n");
        printf("has a clear line of sight to the Apple TV -- a missed button edge\n");
        printf("during the handoff between steps 1 and 2 just leaves it in Recovery\n");
        printf("Mode instead.\n\n");
        printf("Waiting for the device to enter DFU mode...\n");
        if (!waitForDFUMode(deviceManager, ecid, device)) {
            fprintf(stderr, "Device never entered DFU mode.\n");
            return 1;
        }
    }

    if (!checkExploit(deviceManager, device, options.dryRun)) {
        return 1;
    }

    std::string buildToRequest = tetherBoot ? device.buildID : kJailbreakTargetBuild;
    if (device.jailbroken) buildToRequest = device.buildID;

    auto components = downloadAndPatchComponents(patcher, device, buildToRequest, tetherBoot);
    if (!components) {
        fprintf(stderr, "Failed to download/patch firmware components.\n");
        return 1;
    }

    if (!sendComponentsToDevice(deviceManager, device, *components, tetherBoot, options.dryRun)) {
        return 1;
    }

    if (options.dryRun) {
        printf("\n(dry run) Done — nothing was written to the device.\n");
        return 0;
    }

    printf("\nDone. %s\n", tetherBoot ? "The Apple TV should now boot the tethered jailbreak."
                                       : "The Apple TV should now reboot into the jailbroken system.");
    return 0;
}
