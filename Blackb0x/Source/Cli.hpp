//
//  Cli.hpp
//  Blackb0x
//
//  Replaces AppDelegate.h/.m + MainView.h/.m + Blackb0x.h/.m + main.m — the
//  Cocoa app shell, the jailbreak/tether-boot button handlers, and the
//  Objective-C `Blackb0x` singleton (which just held one DeviceManager) all
//  collapse into a single linear CLI session. TaskManager.h/.m is dropped
//  entirely — it only ever drove NSProgressIndicator widgets, no logic of
//  its own to port.
//

#pragma once

#include <cstdint>
#include <string>

// Parsed from argv. `--ecid`/`--udid` pre-select a device (skipping the
// interactive numbered menu) for scripting/automation; if neither is given
// and more than one device is connected, runCli() prompts interactively.
struct CliOptions {
    uint64_t ecid = 0;    // 0 = not specified
    std::string udid;     // empty = not specified
    bool tetherBoot = false;
    bool dryRun = false;
    bool noCheckm8 = false;
    bool dontCheckFirmwareSums = false;
    bool help = false;
    // Which tool actually runs the checkm8 exploit -- "gaster" or
    // "blackb0x-pwn". Only ever meaningfully choosable on Apple platforms
    // (--pwntool, see printCliUsage()/parseCliOptions()): gaster does not
    // work on macOS no matter what has been tried, blackb0x-pwn does (see
    // README.md/docs/HISTORY.md), so blackb0x-pwn is the Apple default;
    // blackb0x-pwn itself is never built at all on Linux, so gaster is the
    // only option there, unconditionally.
#if defined(__APPLE__)
    std::string pwnTool = "blackb0x-pwn";
#else
    std::string pwnTool = "gaster";
#endif
};

CliOptions parseCliOptions(int argc, char** argv);
void printCliUsage(const char* argv0);

// Runs the full interactive session (device wait/select, DFU-mode wait,
// exploit, firmware download+patch, component upload) to completion.
// Returns a process exit code.
int runCli(const CliOptions& options);
