//
//  IPSW.cpp
//  Blackb0x
//

#include "IPSW.hpp"
#include "ResourcePath.hpp"

#include <curl/curl.h>
#include <plist/plist.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <sstream>

static const char* kBaseUrl = "https://api.ipsw.me/v2.1/";

static size_t curlWriteCallback(char* ptr, size_t size, size_t nmemb, void* userdata) {
    std::string* out = static_cast<std::string*>(userdata);
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

std::string httpGet(const std::string& url) {
    CURL* curl = curl_easy_init();
    if (!curl) return "";

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "blackb0x");

    CURLcode res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
        fprintf(stderr, "httpGet(%s) failed: %s\n", url.c_str(), curl_easy_strerror(res));
        response.clear();
    }

    curl_easy_cleanup(curl);
    return response;
}

std::string IpswFetch::firmwareURLForDevice(const std::string& deviceModel, const std::string& buildID) {
    std::string url = kBaseUrl + deviceModel + "/" + (buildID.empty() ? "latest" : buildID) + "/url";
    return httpGet(url);
}

std::map<std::string, FirmwareKeyPair> IpswFetch::keysForDevice(const std::string& device,
                                                                  const std::string& buildID) {
    std::map<std::string, FirmwareKeyPair> result;

    std::string path = resolveImageKeyPath(device + "/" + device + "_" + buildID + ".keys");
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        fprintf(stderr, "keysForDevice: cannot open %s\n", path.c_str());
        return result;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    std::string contents = ss.str();

    plist_t root = nullptr;
    plist_from_xml(contents.c_str(), (uint32_t)contents.size(), &root);
    if (!root) {
        fprintf(stderr, "keysForDevice: failed to parse %s\n", path.c_str());
        return result;
    }

    plist_dict_iter it = nullptr;
    plist_dict_new_iter(root, &it);

    char* key = nullptr;
    plist_t subnode = nullptr;
    plist_dict_next_item(root, it, &key, &subnode);

    while (subnode) {
        if (key && plist_get_node_type(subnode) == PLIST_ARRAY && plist_array_get_size(subnode) >= 2) {
            plist_t ivNode = plist_array_get_item(subnode, 0);
            plist_t keyNode = plist_array_get_item(subnode, 1);

            char* ivStr = nullptr;
            char* keyStr = nullptr;
            plist_get_string_val(ivNode, &ivStr);
            plist_get_string_val(keyNode, &keyStr);

            FirmwareKeyPair pair;
            pair.iv = ivStr ? ivStr : "";
            pair.key = keyStr ? keyStr : "";
            result[key] = pair;

            free(ivStr);
            free(keyStr);
        }

        free(key);
        key = nullptr;
        plist_dict_next_item(root, it, &key, &subnode);
    }

    free(it);
    plist_free(root);

    return result;
}

static std::string plistDictString(plist_t dict, const char* key) {
    plist_t node = plist_dict_get_item(dict, key);
    if (!node) return "";
    char* val = nullptr;
    plist_get_string_val(node, &val);
    std::string result = val ? val : "";
    free(val);
    return result;
}

std::optional<ManifestInfo> parseManifest(const std::string& manifestPath, bool onlyBootComponents) {
    std::ifstream f(manifestPath, std::ios::binary);
    if (!f) return std::nullopt;
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string contents = ss.str();

    plist_t root = nullptr;
    if (contents.size() >= 6 && contents.compare(0, 6, "bplist") == 0) {
        plist_from_bin(contents.data(), (uint32_t)contents.size(), &root);
    } else {
        plist_from_xml(contents.data(), (uint32_t)contents.size(), &root);
    }
    if (!root) return std::nullopt;

    ManifestInfo info;
    info.realBuildID = plistDictString(root, "ProductBuildVersion");
    info.productVersion = plistDictString(root, "ProductVersion");

    plist_t identities = plist_dict_get_item(root, "BuildIdentities");
    uint32_t count = identities ? plist_array_get_size(identities) : 0;
    if (count == 0) {
        plist_free(root);
        return std::nullopt;
    }
    plist_t identity = plist_array_get_item(identities, count - 1);
    plist_t manifest = plist_dict_get_item(identity, "Manifest");

    auto componentPath = [&](const char* component) -> std::string {
        plist_t comp = plist_dict_get_item(manifest, component);
        plist_t info_ = comp ? plist_dict_get_item(comp, "Info") : nullptr;
        return info_ ? plistDictString(info_, "Path") : "";
    };

    info.iBSSPath = componentPath("iBSS");
    info.iBECPath = componentPath("iBEC");
    info.kernelCachePath = componentPath("KernelCache");
    info.deviceTreePath = componentPath("DeviceTree");
    if (!onlyBootComponents) info.restoreRamdiskPath = componentPath("RestoreRamDisk");

    plist_free(root);
    return info;
}

std::set<std::string> signedBuildsForDevice(const std::string& deviceModel) {
    std::set<std::string> result;
    std::string json = httpGet("https://api.ipsw.me/v4/device/" + deviceModel + "?type=ipsw");
    if (json.empty()) return result;

    // Each firmware entry is a flat object (no nested {}), e.g.:
    //   {"identifier":"AppleTV2,1", ..., "buildid":"11D258", ..., "signed":true}
    // so matching non-nested {...} spans is a safe, simple way to isolate
    // one entry at a time without a real JSON parser (see IPSW.hpp).
    std::regex entryRe(R"RE(\{[^{}]*\})RE");
    std::regex buildidRe(R"RE("buildid"\s*:\s*"([^"]*)")RE");
    std::regex signedRe(R"RE("signed"\s*:\s*true)RE");

    for (auto it = std::sregex_iterator(json.begin(), json.end(), entryRe); it != std::sregex_iterator(); ++it) {
        std::string entry = it->str();
        std::smatch buildidMatch;
        if (!std::regex_search(entry, buildidMatch, buildidRe)) continue;
        if (std::regex_search(entry, signedRe)) {
            result.insert(buildidMatch[1].str());
        }
    }
    return result;
}
