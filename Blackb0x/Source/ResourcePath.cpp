//
//  ResourcePath.cpp
//  Blackb0x
//

#include "ResourcePath.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <vector>

#include <climits>
#include <cstdlib>

#include <unistd.h>

namespace fs = std::filesystem;

std::string resolveRamdiskPath() {
    if (const char* override_ = getenv("BLACKB0X_RAMDISK_DIR")) {
        return std::string(override_);
    }
    return "Blackb0x/ramdisk";
}

std::string resolveImageKeyPath(const std::string& relativePath) {
    if (const char* override_ = getenv("BLACKB0X_IMAGEKEYS_DIR")) {
        return std::string(override_) + "/" + relativePath;
    }
    return "Blackb0x/ImageKeys/" + relativePath;
}

std::string resolveGasterPath() {
    if (const char* override_ = getenv("BLACKB0X_GASTER")) {
        return std::string(override_);
    }
    char exePath[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len > 0) {
        exePath[len] = '\0';
        std::string dir(exePath);
        size_t slash = dir.find_last_of('/');
        if (slash != std::string::npos) {
            return dir.substr(0, slash) + "/gaster";
        }
    }
    return "gaster";
}

std::string resolveDebsPath() {
    if (const char* override_ = getenv("BLACKB0X_DEBS_DIR")) {
        return std::string(override_);
    }
    return "Blackb0x/Debs";
}

std::string resolveMiscPath(const std::string& relativePath) {
    if (const char* override_ = getenv("BLACKB0X_MISC_DIR")) {
        return std::string(override_) + "/" + relativePath;
    }
    return "Blackb0x/Misc/" + relativePath;
}

std::string resolveEntrypointPath() {
    if (const char* override_ = getenv("BLACKB0X_ENTRYPOINT_DIR")) {
        return std::string(override_);
    }
    return "entrypoint";
}

std::string decryptedDMGFor(const std::string& path) {
    return fs::path(path).replace_extension("").string() + "-decrypted.dmg";
}

// FNV-1a 64-bit — see ramdiskOverlayContentHash()'s doc comment for why a
// non-cryptographic hash is the right choice here.
static void fnv1aUpdate(uint64_t& h, const void* data, size_t len) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
}

static void fnv1aUpdateStr(uint64_t& h, const std::string& s) {
    fnv1aUpdate(h, s.data(), s.size());
    fnv1aUpdate(h, "\0", 1);
}

static void hashDirectoryTreeInto(uint64_t& h, const fs::path& root) {
    std::error_code existsEc;
    if (!fs::exists(root, existsEc)) return;

    std::vector<fs::path> entries;
    std::error_code walkEc;
    for (const auto& e :
         fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, walkEc)) {
        entries.push_back(e.path());
    }
    std::sort(entries.begin(), entries.end());

    for (const auto& p : entries) {
        fnv1aUpdateStr(h, fs::relative(p, root).string());

        std::error_code typeEc;
        if (fs::is_symlink(p, typeEc)) {
            fnv1aUpdateStr(h, "SYMLINK");
            std::error_code linkEc;
            fnv1aUpdateStr(h, fs::read_symlink(p, linkEc).string());
        } else if (fs::is_regular_file(p, typeEc) && !typeEc) {
            fnv1aUpdateStr(h, "FILE");
            std::ifstream f(p, std::ios::binary);
            char buf[65536];
            while (f) {
                f.read(buf, sizeof(buf));
                std::streamsize n = f.gcount();
                if (n > 0) fnv1aUpdate(h, buf, (size_t)n);
            }
        } else {
            fnv1aUpdateStr(h, "DIR");
        }
    }
}

std::string ramdiskOverlayContentHash() {
    uint64_t h = 0xcbf29ce484222325ULL;
    hashDirectoryTreeInto(h, resolveRamdiskPath());
    hashDirectoryTreeInto(h, resolveDebsPath());
    // Misc/ (rc.boot content spliced into /etc/rc.boot) and entrypoint/'s
    // own source (rebuilt fresh via podman into /sbin/launchd on every
    // bake — see buildEntrypointBinary() in BakeRamdisk.cpp) both now
    // affect the baked output just as much as ramdisk/ and Debs/ do.
    // Hashing entrypoint/'s whole directory would also pick up its own
    // obj/ build output and churn the hash on every bake for no reason;
    // hash only the inputs that actually change what gets compiled.
    hashDirectoryTreeInto(h, resolveMiscPath(""));
    fnv1aUpdateStr(h, "FILE");
    {
        std::ifstream f(resolveEntrypointPath() + "/entrypoint.c", std::ios::binary);
        char buf[65536];
        while (f) {
            f.read(buf, sizeof(buf));
            std::streamsize n = f.gcount();
            if (n > 0) fnv1aUpdate(h, buf, (size_t)n);
        }
    }
    fnv1aUpdateStr(h, "FILE");
    {
        std::ifstream f(resolveEntrypointPath() + "/Makefile", std::ios::binary);
        char buf[65536];
        while (f) {
            f.read(buf, sizeof(buf));
            std::streamsize n = f.gcount();
            if (n > 0) fnv1aUpdate(h, buf, (size_t)n);
        }
    }

    char buf[17];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)h);
    return std::string(buf);
}

std::string sumFileFor(const std::string& outputPath) { return outputPath + ".sum"; }
