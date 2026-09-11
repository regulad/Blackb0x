//
//  IPSW.hpp
//  Blackb0x
//
//  C++/Linux port of IPSW.h/.mm. Replaces IPSW_Fetch's NSURLSession-based
//  networking with libcurl, and its NSDictionary-based .keys file parsing
//  with libplist's C API directly. The IPSW.me API call's response is a
//  plain-text URL string (confirmed by how the original consumed it — not
//  JSON), so this is a trivial GET-into-string, no JSON parser needed.
//

#pragma once

#include <map>
#include <string>

// A firmware component's decryption [iv, key] pair, as stored in
// Blackb0x/Files/Keys/<device>/<device>_<buildID>.keys.
struct FirmwareKeyPair {
    std::string iv;
    std::string key;
};

// Plain HTTP GET via libcurl; returns the response body, or "" on failure.
std::string httpGet(const std::string& url);

class IpswFetch {
public:
    // GET https://api.ipsw.me/v2.1/<deviceModel>/<buildID|"latest">/url —
    // returns the raw response body (a plain-text .ipsw URL), or "" on
    // failure. Passing an empty buildID fetches the latest firmware
    // (replaces firmwareForDevice:/latestFirmwareForDevice:).
    std::string firmwareURLForDevice(const std::string& deviceModel, const std::string& buildID);

    // Parses the local bundled .keys file (an XML plist: component name ->
    // [iv, key]) for the given device/build.
    std::map<std::string, FirmwareKeyPair> keysForDevice(const std::string& device, const std::string& buildID);
};
