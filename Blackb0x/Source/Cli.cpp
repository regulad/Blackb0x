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

extern "C" {
#include <plist/plist.h>
}

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <set>
#include <thread>
#include <vector>

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
    printf("  --no-pwn                  Never attempt to run a pwntool (gaster/\n");
    printf("                            blackb0x-pwn) at all — if the connected device\n");
    printf("                            isn't already reporting a pwned DFU serial\n");
    printf("                            string, fail instead of attempting the exploit.\n");
    printf("                            For iterating on the post-exploit send flow\n");
    printf("                            against an already-pwned device without\n");
    printf("                            spawning a pwntool again.\n");
#if defined(__APPLE__)
    printf("  --pwntool <gaster|blackb0x-pwn>\n");
    printf("                            Which tool runs the checkm8 exploit\n");
    printf("                            (default: blackb0x-pwn — gaster does not\n");
    printf("                            work on macOS no matter what has been\n");
    printf("                            tried; see README.md)\n");
#endif
    printf("  --dont-check-firmware-sums\n");
    printf("                            Skip patchRamdisk()'s check that the baked\n");
    printf("                            dist/ ramdisk still matches ramdisk/'s current\n");
    printf("                            content (its .sum sidecar) — uses it as-is\n");
    printf("                            even if stale. For iterating without\n");
    printf("                            re-running bake-all-ramdisks every time; NOT\n");
    printf("                            the default, since it can silently ship a\n");
    printf("                            stale ramdisk.\n");
    printf("  --stock-ramdisk           DIAGNOSTIC: send the stock RestoreRamdisk exactly\n");
    printf("                            as downloaded from Apple, instead of the\n");
    printf("                            blackb0x-patched one -- to check whether a boot\n");
    printf("                            failure is in blackb0x's own ramdisk\n");
    printf("                            patching/entrypoint.c or earlier in the chain.\n");
    printf("                            The device will NOT be jailbroken by a run\n");
    printf("                            using this flag.\n");
    printf("  --stock-recovery          DIAGNOSTIC: send the stock iBSS/iBEC exactly\n");
    printf("                            as downloaded from Apple (still runs checkm8\n");
    printf("                            first -- SecureROM's own signature check still\n");
    printf("                            needs bypassing to accept any file at all -- but\n");
    printf("                            no boot-args/KASLR/ticket-check patches applied\n");
    printf("                            to the bootloader itself) -- to check whether a\n");
    printf("                            boot failure is in blackb0x's own iBSS/iBEC\n");
    printf("                            patches or elsewhere in the chain. REQUIRES\n");
    printf("                            --stock-firmware (refuses to start otherwise): the\n");
    printf("                            resulting stock iBEC still enforces real APTicket\n");
    printf("                            verification on whatever it loads next, and a real\n");
    printf("                            ticket can never authorize blackb0x's own patched\n");
    printf("                            kernel/ramdisk. The device will NOT be jailbroken\n");
    printf("                            by a run using this flag.\n");
    printf("  --stock-firmware          DIAGNOSTIC: send a stock kernelcache (no\n");
    printf("                            tfp0/AMFI/sandbox patches) and stock ramdisk\n");
    printf("                            (same as --stock-ramdisk) -- meaningful alone\n");
    printf("                            (keeps blackb0x's own patched iBSS/iBEC) to check\n");
    printf("                            whether blackb0x's own patched bootloader can\n");
    printf("                            still boot an otherwise-unmodified OS: if this\n");
    printf("                            boots fine, the iBSS/iBEC patches are confirmed OK\n");
    printf("                            and the failure is in blackb0x's own kernel/\n");
    printf("                            ramdisk patches specifically; if it fails the same\n");
    printf("                            way, the iBSS/iBEC patches themselves are\n");
    printf("                            implicated. Also required alongside\n");
    printf("                            --stock-recovery for a fully-stock suite end to\n");
    printf("                            end (see that flag's own entry for why). The\n");
    printf("                            device will NOT be jailbroken by a run using this\n");
    printf("                            flag.\n");
    printf("  --stock-securom           DIAGNOSTIC: never attempt to run a pwntool, for a\n");
    printf("                            genuinely un-exploited device still running real,\n");
    printf("                            un-bypassed SecureROM signature enforcement --\n");
    printf("                            ERRORS if the device already reports PWND: in its\n");
    printf("                            serial string (contradicts what this flag is for),\n");
    printf("                            instead of skipping a pwntool. iBSS gets personalized\n");
    printf("                            with a real, ECID-bound SHSH ticket fetched from\n");
    printf("                            Apple's TSS server before being sent, and a combined\n");
    printf("                            APTicket covering everything after it (see\n");
    printf("                            Personalize.hpp) -- REQUIRES both --stock-recovery\n");
    printf("                            and --stock-firmware (refuses to start otherwise):\n");
    printf("                            those tickets are only ever valid for the exact,\n");
    printf("                            unmodified stock components, so anything blackb0x\n");
    printf("                            has patched can never pass.\n");
    printf("  --help                    Show this message\n");
    printf("\n");
    printf("blackb0x needs root by default: talking to a DFU/Recovery-mode device needs\n");
    printf("raw USB access, which the kernel restricts to root unless a udev rule grants\n");
    printf("it to your own user (see the README's own setup section). Run it via\n");
    printf("`sudo blackb0x ...` if you haven't set that up.\n");
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
        } else if (arg == "--no-pwn") {
            options.noPwn = true;
        } else if (arg == "--dont-check-firmware-sums") {
            options.dontCheckFirmwareSums = true;
        } else if (arg == "--stock-ramdisk") {
            options.stockRamdisk = true;
        } else if (arg == "--stock-recovery") {
            options.stockRecovery = true;
        } else if (arg == "--stock-firmware") {
            options.stockFirmware = true;
        } else if (arg == "--stock-securom") {
            options.stockSecurom = true;
        } else if (arg == "--pwntool") {
            std::string value = nextArg("--pwntool");
#if defined(__APPLE__)
            if (value != "gaster" && value != "blackb0x-pwn") {
                fprintf(stderr, "--pwntool must be 'gaster' or 'blackb0x-pwn' (got '%s')\n", value.c_str());
                exit(2);
            }
            options.pwnTool = value;
#else
            fprintf(stderr,
                    "--pwntool is only meaningful on macOS (blackb0x-pwn isn't built on this "
                    "platform, gaster is the only option) -- ignoring.\n");
#endif
        } else if (arg == "--help" || arg == "-h") {
            options.help = true;
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            printCliUsage(argv[0]);
            exit(2);
        }
    }
    // --stock-firmware already implies stock ramdisk (see
    // downloadAndPatchComponents()'s own `stockRamdisk || stockFirmware`
    // check) -- not an error, just redundant, so warn rather than reject.
    // --stock-recovery is NOT redundant with --stock-firmware: --stock-
    // firmware deliberately keeps blackb0x's own patched iBSS/iBEC and
    // only stocks the kernel/ramdisk (isolating whether the *bootloader*
    // patches themselves are the problem); combining both flags is how
    // you get a fully-stock suite end to end, a real, distinct diagnostic
    // of its own, not a redundant restatement. --no-pwn is unrelated to
    // either -- it controls whether a pwntool runs at all.
    if (options.stockFirmware && options.stockRamdisk) {
        fprintf(stderr, "--stock-ramdisk is redundant with --stock-firmware\n");
    }
    // Opposite PWND-state requirements (noPwn hard-fails if NOT already
    // pwned; stockSecurom hard-fails if it IS) -- not useful together,
    // and checkExploit() checks stockSecurom first, then noPwn, so
    // combining them just means noPwn's hard-fail wins once stockSecurom's
    // own check passes.
    if (options.noPwn && options.stockSecurom) {
        fprintf(stderr,
                "--no-pwn and --stock-securom require opposite device states (already pwned vs. not) -- "
                "combining them is not useful.\n");
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
// printing what would have run instead. `noPwn`/`stockSecurom` only gate
// the AppleTV3,2/checkm8 branch below (the one that actually spawns a
// pwntool) — SHAtter (AppleTV2,1) is a separate, hand-rolled exploit that
// never touches a pwntool at all, so there's nothing for these flags to
// refuse there. (`noPwn` was named noCheckm8/--no-checkm8; renamed once
// "pwntool" became the general term for gaster/blackb0x-pwn both, keeping
// its original hard-fail-if-not-already-pwned behavior.)
//
// stockSecurom is checked before the early pwnedDFU return below, not
// after: its whole point is a genuinely un-exploited device (real,
// Apple-signed SecureROM DFU, not checkm8'd) -- if the device is already
// pwned, that contradicts the test setup this flag exists for, so it
// needs to error out even though checkExploit() would otherwise treat an
// already-pwned device as trivially "done" and return success.
bool checkExploit(DeviceManager& deviceManager, const AppleTVDevice& device, bool dryRun, bool noPwn,
                   bool stockSecurom, const std::string& pwnTool) {
    if (stockSecurom && device.pwnedDFU) {
        fprintf(stderr,
                "--stock-securom: device is already in pwned DFU (PWND: in its serial string) -- this "
                "flag requires a genuinely un-exploited device (real SecureROM signature enforcement "
                "still intact) to be a meaningful test. Re-enter DFU mode on a device that hasn't been "
                "pwned, or drop --stock-securom.\n");
        return false;
    }

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
        if (noPwn) {
            fprintf(stderr,
                    "--no-pwn: device is not already in pwned DFU (no PWND: in its serial string) — "
                    "refusing to run checkm8/%s. Pwn it separately first (e.g. `%s pwn`%s), or drop "
                    "--no-pwn to let blackb0x do it.\n",
                    pwnTool.c_str(), pwnTool.c_str(),
                    pwnTool == "blackb0x-pwn" ? " — note blackb0x-pwn's own verb is `checkm8`, not `pwn`" : "");
            return false;
        }
        if (stockSecurom) {
            // Already confirmed not pwned (the check at the top of this
            // function would have errored out otherwise) -- skip the
            // pwntool entirely and proceed straight into the rest of the
            // boot chain, relying on the device's own real, un-bypassed
            // SecureROM signature verification (backed by a real,
            // TSS-issued personalization ticket -- see
            // Personalize.hpp/sendiBSS()'s own comments) the whole way.
            // runCli() already hard-refuses --stock-securom without
            // --stock-recovery before this ever runs, so there's nothing
            // left to caveat here.
            fprintf(stderr,
                    "--stock-securom: device confirmed not already pwned -- skipping %s entirely and "
                    "proceeding into the rest of the boot chain, relying on the device's own real "
                    "SecureROM signature verification.\n",
                    pwnTool.c_str());
            return true;
        }
        if (dryRun) {
            printf("(dry run) Would try checkm8\n");
            return true;
        }
        printf("Trying checkm8 (%s)...\n", pwnTool.c_str());
        if (deviceManager.checkm8(device.ecid, pwnTool) == 0) {
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
                                                              bool onlyBootComponents,
                                                              bool dontCheckFirmwareSums, bool stockRamdisk,
                                                              bool stockRecovery, bool stockFirmware,
                                                              bool stockSecurom) {
    patcher.onlyBootComponents = onlyBootComponents;
    patcher.dontCheckFirmwareSums = dontCheckFirmwareSums;

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
    patcher.setBuildIdentity(manifest->buildIdentity);
    patcher.setBuildID(manifest->realBuildID);

    std::optional<PatchedComponents> result;
    patcher.onComponentsReady = [&](const PatchedComponents& c) { result = c; };

    // required=false for the genuinely-optional components (RestoreLogo,
    // loaded-by-iBoot entries -- some builds' manifests just don't have
    // them, see their own call sites below): silently skipping those is
    // correct, expected behavior, not a bug. For every other (required)
    // component, an empty remotePath means BuildManifest.plist itself
    // has no Path for it at all -- a real, surprising problem
    // (Patcher::missingRequiredComponents() would otherwise report this
    // component as "missing" with no explanation anywhere in the output
    // at all, since neither the download-failure nor the patch-failure
    // branches below ever ran).
    auto downloadAndPatch = [&](const char* label, const std::string& remotePath, auto&& patchFn,
                                 bool required = true) {
        if (remotePath.empty()) {
            if (required) {
                fprintf(stderr,
                        "%s: BuildManifest.plist has no Path for this component -- can't download it at all.\n",
                        label);
            }
            return;
        }
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

    // stockRecovery, not stockFirmware, gates iBSS/iBEC: --stock-firmware
    // alone deliberately keeps blackb0x's own patched bootloader and only
    // stocks the kernel/ramdisk it hands off to (see that flag's own
    // comment in Cli.hpp). --stock-recovery is the complementary test
    // (stock bootloader too) -- runCli() requires --stock-firmware
    // alongside it (a stock bootloader's own real APTicket verification
    // can never authorize blackb0x's own patched kernel/ramdisk), so in
    // practice --stock-recovery never appears here without
    // --stock-firmware also stocking the rest of the suite.
    downloadAndPatch("iBSS", manifest->iBSSPath, [&](const std::string& path) {
        if (stockRecovery) {
            patcher.useStockIBSS(path, stockSecurom);
        } else {
            patcher.patchiBSS(path);
        }
    });

    downloadAndPatch("iBEC", manifest->iBECPath, [&](const std::string& path) {
        if (stockRecovery) {
            patcher.useStockIBEC(path);
            return;
        }
        // Verbatim version heuristic from the original setIBECPath: — iBEC
        // files for 4.x-era firmware need empty flags and no ticket.
        if (path.find("4.") != std::string::npos) {
            patcher.patchiBEC(path, "", false);
        } else {
            patcher.patchiBEC(path);
        }
    });

    downloadAndPatch("KernelCache", manifest->kernelCachePath, [&](const std::string& path) {
        if (stockFirmware) {
            patcher.useStockKernel(path, stockRecovery);
        } else {
            patcher.patchKernel(path, manifest->productVersion);
        }
    });

    downloadAndPatch("DeviceTree", manifest->deviceTreePath,
                      [&](const std::string& path) { patcher.setDeviceTreePath(path); });

    // required=false: confirmed genuinely optional, not just "usually
    // present" -- idevicerestore's own recovery_send_applelogo() checks
    // build_identity_has_component() first and returns success outright
    // if the manifest doesn't have one at all (see
    // ManifestInfo::restoreLogoPath's own comment). downloadAndPatch()
    // skips the callback entirely when the path is empty either way, so
    // this is a silent no-op wherever it's absent -- correctly so here.
    downloadAndPatch(
        "RestoreLogo", manifest->restoreLogoPath, [&](const std::string& path) { patcher.setRestoreLogoPath(path); },
        /*required=*/false);

    // Almost always empty (see ManifestInfo::loadedByIBootComponents' own
    // comment) -- one downloadAndPatch() call per manifest entry flagged
    // Info.IsLoadedByiBoot, matching idevicerestore's own generic
    // iteration instead of a fixed component list.
    for (const auto& [name, remotePath] : manifest->loadedByIBootComponents) {
        downloadAndPatch(name.c_str(), remotePath,
                          [&](const std::string& path) { patcher.addLoadedByIBootComponent(name, path); });
    }

    if (!onlyBootComponents) {
        downloadAndPatch("RestoreRamdisk", manifest->restoreRamdiskPath, [&](const std::string& path) {
            if (stockRamdisk || stockFirmware) {
                patcher.useStockRamdisk(path, stockRecovery);
            } else {
                patcher.patchRamdisk(path);
            }
        });
    }

    if (!result) {
        std::vector<std::string> missing = patcher.missingRequiredComponents();
        std::string joined;
        for (size_t i = 0; i < missing.size(); i++) joined += (i ? ", " : "") + missing[i];
        // Not "...patched successfully" -- on any --stock-* route
        // nothing here actually patches anything (useStockIBSS()/
        // useStockIBEC()/etc. just decrypt or pass the original file
        // through untouched), so that wording pointed at the wrong half
        // of the problem when the real cause was e.g. a missing
        // decryption key or a download failure instead.
        fprintf(stderr,
                "Not all required components were prepared successfully -- still missing: %s. Look further up "
                "for the actual reason that component's own step failed (a download failure prints \"Failed "
                "to download <name>\"; a missing decryption key prints \"no ... keys loaded\"; an actual patch "
                "failure -- only possible on a non-stock route -- prints its own iBootPatcher()/patch_kernel() "
                "error).\n",
                joined.empty() ? "(nothing? this shouldn't happen)" : joined.c_str());
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
                             bool tetherBoot, bool dryRun, bool stockRecovery, bool stockSecurom) {
    if (dryRun) {
        printf("(dry run) Would send:\n");
        printf("  iBSS%s\n", components.iBSS ? "" : " (missing, would fail here)");
        if (tetherBoot) {
            printf("  iBEC (downgrade)%s\n", components.iBECDowngrade ? "" : " (missing)");
        } else {
            printf("  iBEC (boot)%s\n", components.iBECBoot ? "" : " (missing)");
            if (components.restoreLogo) printf("  RestoreLogo\n");
            printf("  Ramdisk%s\n", components.ramdisk ? "" : " (missing)");
            printf("  DeviceTree%s\n", components.deviceTree ? "" : " (missing)");
        }
        printf("  KernelCache%s\n", components.kernel ? "" : " (missing, would fail here)");
        printf("(dry run) Would then wait for the Apple TV to %s\n", tetherBoot ? "boot" : "reboot");
        return true;
    }

    printf("Sending iBSS -> ");
    fflush(stdout);
    if (deviceManager.sendiBSS(*components.iBSS, device.ecid, stockRecovery, stockSecurom,
                                components.buildIdentity, device.deviceModel, components.buildID) != 0) {
        printf("Error\n");
        fprintf(stderr, "Failed to send iBSS. Please re-enter DFU mode and try again.%s\n",
                stockSecurom ? " (--stock-securom personalizes the image with a real TSS-issued SHSH ticket "
                               "before sending it via the standard DFU route -- if this still fails, it's a "
                               "real signal about the image/device/firmware match itself, not an artifact of "
                               "this tool's own delivery mechanism)"
                             : "");
        return false;
    }
    printf("Sent\n");
    // iBSS running successfully means the device is about to reboot and
    // re-enumerate in Recovery mode -- sendiBEC() below (get_tv_patient())
    // blocks retrying for up to ~30s waiting for exactly that, with no
    // status of its own in between. Without this, that whole window reads
    // as silently stuck rather than an expected, normal wait.
    printf("Waiting for device to come back up in Recovery mode...\n");
    fflush(stdout);

    // Gated on stockRecovery, NOT stockSecurom: this is about whether
    // useStockIBEC()'s genuinely-unpatched iBEC is what's running, not
    // whether checkm8 ran to get there. patch_ticket_check() (patchiBEC(),
    // applied by default) is what makes a ticket unnecessary, and it only
    // ever runs against blackb0x's own patched iBEC -- stockRecovery's
    // stock iBEC has no such patch applied regardless of stockSecurom, so
    // it enforces real ticket verification on whatever it loads next
    // (DeviceTree/Ramdisk/KernelCache) either way.
    //
    // Everything from the ticket through KernelCache runs as ONE call
    // (DeviceManager::sendStockRestoreTail()) on a single persistent
    // connection when stockRecovery is set, rather than this function's
    // usual reconnect-per-step calls -- confirmed directly on real
    // hardware that reconnecting right after the ticket specifically
    // (not after any of the OTHER resets in this chain) drops the device
    // all the way back to DFU mode, matching real idevicerestore's own
    // recovery_enter_restore(), which never closes its connection across
    // this same span either. See sendStockRestoreTail()'s own comment.
    auto sendStockTail = [&](bool onlyBootComponents) -> bool {
        printf("Sending APTicket%s -> ", onlyBootComponents ? " + KernelCache"
                                                              : " + RestoreLogo + Ramdisk + DeviceTree + "
                                                                "KernelCache");
        fflush(stdout);
        int i = deviceManager.sendStockRestoreTail(device.ecid, components, device.deviceModel, onlyBootComponents);
        printf("%s\n", (i == 0) ? "Sent" : "Error");
        if (i != 0) {
            fprintf(stderr, "Failed to send the post-iBEC stock restore sequence. Re-enter DFU mode and try "
                             "again.\n");
            return false;
        }
        return true;
    };

    if (tetherBoot) {
        printf("Sending iBEC (downgrade) -> ");
        fflush(stdout);
        int i = components.iBECDowngrade ? deviceManager.sendiBEC(*components.iBECDowngrade, device.ecid) : -1;
        printf("%s\n", (i == 0) ? "Sent" : "Error");
        if (i != 0) {
            fprintf(stderr,
                    "Failed to send iBEC (downgrade) -- the device may have rebooted out of the exploited\n"
                    "state instead of staying put. Re-enter DFU mode and try again.\n");
            return false;
        }
        device.didTetheredBoot = 1;

        if (stockRecovery) {
            if (!sendStockTail(/*onlyBootComponents=*/true)) return false;
            device.waitForRecovery = 1;
            printf("Waiting for Apple TV to boot\n");
            return true;
        }
    } else {
        printf("Sending iBEC (boot) -> ");
        fflush(stdout);
        int i = components.iBECBoot ? deviceManager.sendiBEC(*components.iBECBoot, device.ecid) : -1;
        printf("%s\n", (i == 0) ? "Sent" : "Error");
        if (i != 0) {
            fprintf(stderr,
                    "Failed to send iBEC (boot) -- the device may have rebooted out of the exploited state\n"
                    "instead of staying put (see any reconnect-attempt lines above for detail). Re-enter DFU\n"
                    "mode and try again.\n");
            return false;
        }

        if (stockRecovery) {
            if (!sendStockTail(/*onlyBootComponents=*/false)) return false;
            device.needsPostInstall = 1;
            device.waitForRecovery = 1;
            printf("Waiting for Apple TV to reboot\n");
            return true;
        }

        printf("Sending Ramdisk -> ");
        fflush(stdout);
        i = components.ramdisk ? deviceManager.sendRamdisk(*components.ramdisk, device.ecid) : -1;
        printf("%s\n", (i == 0) ? "Sent" : "Error");
        if (i != 0) {
            fprintf(stderr, "Failed to send Ramdisk. Re-enter DFU mode and try again.\n");
            return false;
        }

        printf("Sending DeviceTree -> ");
        fflush(stdout);
        i = components.deviceTree ? deviceManager.sendDeviceTree(*components.deviceTree, device.ecid) : -1;
        printf("%s\n", (i == 0) ? "Sent" : "Error");
        if (i != 0) {
            fprintf(stderr, "Failed to send DeviceTree. Re-enter DFU mode and try again.\n");
            return false;
        }

        device.needsPostInstall = 1;
    }

    // Only reached when stockRecovery is unset -- sendStockTail() above
    // already includes KernelCache and returns directly otherwise.
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

    // --stock-securom without --stock-recovery would send blackb0x's own
    // PATCHED iBSS through personalizeIMG3Component() (Personalize.cpp) --
    // the TSS ticket that fetches is only ever valid for the exact,
    // unmodified component digest BuildManifest.plist lists, so stitching
    // it into anything blackb0x has patched can never pass a real
    // SecureROM's verification, regardless of how correctly everything
    // else here behaves. Same reasoning transitively requires
    // --stock-firmware too (see that check just below) -- checked
    // directly here as well, rather than only relying on that second
    // check to catch it, so this specific combination gets a message
    // that actually names --stock-securom as the reason. Refuse outright
    // rather than attempting (and failing) a combination that can never
    // do anything else.
    if (options.stockSecurom && !(options.stockRecovery && options.stockFirmware)) {
        fprintf(stderr,
                "--stock-securom requires both --stock-recovery and --stock-firmware: personalizing "
                "anything other than the unmodified, stock iBSS/iBEC/kernel/ramdisk against a real TSS "
                "ticket can never pass a genuine SecureROM's signature check. Pass --stock-recovery "
                "--stock-firmware --stock-securom together.\n");
        return 1;
    }

    // --stock-recovery's own stock iBEC (useStockIBEC(), never patched --
    // patch_ticket_check() only ever runs against blackb0x's own patched
    // iBEC) enforces real APTicket verification on whatever it loads
    // next, checkm8 or not (see DeviceManager::sendStockRestoreTail()'s
    // own comment). A real APTicket only ever authorizes the exact,
    // unmodified component digests BuildManifest.plist lists -- blackb0x's
    // own patched kernel/ramdisk (patchKernel()/patchRamdisk(), what
    // --stock-recovery without --stock-firmware would still send) have
    // different digests by definition, so a stock iBEC can never accept
    // them regardless of which build gets requested. Refuse outright
    // rather than attempting (and failing) a combination that can never
    // do anything else -- this also sidesteps a real, previously-silent
    // failure mode: --stock-recovery alone still points buildToRequest at
    // "latest" (needed for the ticket itself), but patchKernel()/
    // patchRamdisk() have zero support for whatever build that resolves
    // to (no Blackb0x/ImageKeys/ entry, no baked dist/ ramdisk), so they
    // fail with no output component set at all, surfacing several layers
    // away as a generic "Not all required components patched
    // successfully".
    if (options.stockRecovery && !options.stockFirmware) {
        fprintf(stderr,
                "--stock-recovery requires --stock-firmware: a stock iBEC verifies a real APTicket against "
                "the exact, unmodified component digests BuildManifest.plist lists, and blackb0x's own "
                "patched kernel/ramdisk can never match those regardless of which build gets requested. Pass "
                "--stock-recovery --stock-firmware together.\n");
        return 1;
    }

    printf("blackb0x (regulad's portable port) — Apple TV 2/3 jailbreak tool\n");

    if (options.dryRun) {
        printf("(dry run) Discovery, DFU wait, download, and patch all happen for real.\n");
        printf("(dry run) Only the exploit and the USB upload are skipped.\n\n");
    }
    fflush(stdout);

    // No hard root requirement: raw DFU/Recovery-mode USB access is a kernel
    // device-node permission, not something this process can determine in
    // advance for every possible udev/group setup. Running as root always
    // works; running as a normal user works too, given the right udev rule
    // (see README's own setup section) — either way, gaster/libirecovery's
    // own device-open calls are what actually surface a real permission
    // error, with a real errno behind it, if access genuinely isn't there.

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
    std::set<std::string> jailbreakRunningAnnouncedFor;
    sink.onDeviceUpdated = [&jailbreakRunningAnnouncedFor](const AppleTVDevice& d) {
        if (d.jailbroken) {
            printf("%s is jailbroken%s\n", d.deviceModel.c_str(), d.jailbreakRunning == 1 ? " and running" : "");
        }
        if (d.jailbreakRunning == 1 && jailbreakRunningAnnouncedFor.insert(d.udid).second) {
            printf("Run scripts/push_authorized_keys.sh if you want SSH access to %s.\n", d.deviceModel.c_str());
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

    if (!checkExploit(deviceManager, device, options.dryRun, options.noPwn, options.stockSecurom,
                       options.pwnTool)) {
        return 1;
    }

    // Which flags in this run guarantee the device stays non-jailbroken
    // (any use of unpatched/stock content) -- --no-pwn is deliberately NOT
    // one of these: it only skips re-running the exploit, which is
    // perfectly compatible with a real, successful jailbreak if the
    // device was already pwned by a separate run.
    std::vector<std::string> stockFlags;
    if (options.stockFirmware) stockFlags.push_back("--stock-firmware (patched bootloader, stock kernel/ramdisk)");
    if (options.stockRecovery) stockFlags.push_back("--stock-recovery (stock iBSS/iBEC)");
    if (options.stockRamdisk && !options.stockFirmware) stockFlags.push_back("--stock-ramdisk (stock RestoreRamdisk)");
    if (!stockFlags.empty()) {
        std::string joined;
        for (size_t i = 0; i < stockFlags.size(); i++) {
            joined += (i ? ", " : " ") + stockFlags[i];
        }
        fprintf(stderr, "DIAGNOSTIC run:%s -- the device will NOT be jailbroken even if everything "
                        "below succeeds.\n",
                joined.c_str());
    }

    std::string buildToRequest = tetherBoot ? device.buildID : kJailbreakTargetBuild;
    if (device.jailbroken) buildToRequest = device.buildID;
    // Both --stock-securom (real SecureROM, no checkm8) AND --stock-recovery
    // (real, unpatched iBEC via useStockIBEC() -- patch_ticket_check()
    // never runs against it, checkm8 or not) need a build the device's
    // real signature/ticket verification will actually accept -- either
    // one alone means SOMETHING in this boot chain is enforcing real
    // Apple signing, not just SecureROM specifically. kJailbreakTargetBuild
    // is fixed to the specific old build this project's own jailbreak
    // patches/ImageKeys/baked ramdisks are tuned for, almost never Apple's
    // current signing window. Apple always has at least one currently-
    // signed build for any still-supported device (this is what makes
    // ipsw.me's "latest" endpoint meaningful at all) --
    // IpswFetch::firmwareURLForDevice() already treats the literal string
    // "latest" as a request for exactly that (see its own implementation),
    // so just ask for it directly instead of guessing/enumerating
    // signedBuildsForDevice()'s own (unordered, not "give me the newest")
    // result set.
    if (options.stockSecurom || options.stockRecovery) buildToRequest = "latest";

    auto components = downloadAndPatchComponents(patcher, device, buildToRequest, tetherBoot,
                                                   options.dontCheckFirmwareSums, options.stockRamdisk,
                                                   options.stockRecovery, options.stockFirmware,
                                                   options.stockSecurom);
    if (!components) {
        // Not "...to patch..." -- on any --stock-* route nothing here
        // actually patches anything (useStockIBSS()/useStockIBEC()/etc.
        // just decrypt or pass the original file through untouched), so
        // that wording pointed at the wrong half of the problem. The
        // real reason is already printed above (see
        // downloadAndPatchComponents()'s own message).
        fprintf(stderr, "Failed to download or prepare firmware components.\n");
        return 1;
    }

    if (!sendComponentsToDevice(deviceManager, device, *components, tetherBoot, options.dryRun,
                                 options.stockRecovery, options.stockSecurom)) {
        return 1;
    }

    if (options.dryRun) {
        printf("\n(dry run) Done — nothing was written to the device.\n");
        return 0;
    }

    if (!stockFlags.empty()) {
        std::string joined;
        for (size_t i = 0; i < stockFlags.size(); i++) {
            joined += (i ? ", " : "") + stockFlags[i];
        }
        printf("\nDone. DIAGNOSTIC run (%s) -- the Apple TV should reboot into a stock, "
               "non-jailbroken state if this run succeeded.\n",
               joined.c_str());
    } else {
        printf("\nDone. %s\n", tetherBoot ? "The Apple TV should now boot the tethered jailbreak."
                                           : "The Apple TV should now reboot into the jailbroken system.");
    }
    return 0;
}
