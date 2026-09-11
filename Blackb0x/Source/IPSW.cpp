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

    std::string path = resolveResourcePath("Keys/" + device + "/" + device + "_" + buildID + ".keys");
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
