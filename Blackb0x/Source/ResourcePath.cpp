//
//  ResourcePath.cpp
//  Blackb0x
//

#include "ResourcePath.hpp"

#include <climits>
#include <cstdlib>

#include <unistd.h>

std::string resolveResourcePath(const std::string& relativePath) {
    if (const char* override_ = getenv("BLACKB0X_RESOURCE_DIR")) {
        return std::string(override_) + "/" + relativePath;
    }
    return "Blackb0x/Files/" + relativePath;
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
