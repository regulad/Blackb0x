//
//  ResourcePath.hpp
//  Blackb0x
//
//  Resolves paths under Blackb0x/Files/ (Cydia tarball, per-firmware .keys,
//  setup.sh, etc.) — replaces every [[NSBundle mainBundle]
//  pathForResource:...] call site in the original. Proper install-prefix
//  resolution (CMAKE_INSTALL_DATADIR, /usr/share/blackb0x) is Phase 7 work;
//  for now this checks an override env var, then falls back to a path
//  relative to the current working directory, which is sufficient for
//  running the CLI from a build/dev tree.
//

#pragma once

#include <string>

// Resolves `relativePath` (e.g. "Keys/AppleTV2,1/AppleTV2,1_10A406e.keys")
// against the resource root: $BLACKB0X_RESOURCE_DIR if set, otherwise
// "Blackb0x/Files" relative to the current working directory.
std::string resolveResourcePath(const std::string& relativePath);

// Resolves the path to the vendored `gaster` binary (see CMakeLists.txt —
// built as its own executable, landing in the same output directory as
// blackb0x itself): $BLACKB0X_GASTER if set, otherwise the "gaster"
// alongside blackb0x's own executable (via /proc/self/exe), which is
// reliable regardless of the current working directory the CLI happens to
// be invoked from — unlike resolveResourcePath() above, a CWD-relative
// fallback would break as soon as someone runs it from outside the build
// tree.
std::string resolveGasterPath();
