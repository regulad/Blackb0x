//
//  Personalize.hpp
//  Blackb0x
//
//  --stock-securom (Cli.hpp's CliOptions): a genuinely un-pwned device's
//  real SecureROM/iBoot don't just check "is this Apple's signature" --
//  they require a live, ECID-personalized SHSH ticket, the same one a
//  real (non-exploited) idevicerestore DFU restore fetches from Apple's
//  TSS server before sending iBSS. checkm8/boot_client() never needed
//  this because the exploit patches SecureROM's own signature check out
//  of memory; --stock-securom deliberately skips the exploit, so this
//  requirement is back in full force. Implemented via libtatsu (the same
//  TSS client library idevicerestore itself now uses, vendored as
//  third_party/libtatsu) for the request/response, plus
//  Blackb0x/Libraries/idevicerestore_img3.c (also lifted directly from
//  idevicerestore) for stitching the returned ticket into the
//  already-downloaded, still-encrypted img3 file -- per AGENTS.md's own
//  "prefer adapting an existing, battle-tested library over hand-rolling
//  the equivalent" rule, neither the TSS protocol nor the img3 tag
//  surgery is reimplemented here.
//

#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// componentName: "iBSS" or "iBEC" -- must match BuildManifest.plist's own
// Manifest.<name> key exactly (also what the TSS response keys its
// per-component blob by).
// rawImg3Path: the original, untouched, still-encrypted file exactly as
// downloaded from Apple (useStockIBSS()/useStockIBEC()'s own output when
// stockSecurom is set -- see Patcher.cpp) -- NOT a decrypted one; the
// signature this stitches in is computed over these exact bytes.
// buildIdentity: IPSW.hpp's ManifestInfo::buildIdentity (the matching
// BuildIdentity plist_t, type-erased the same way).
// ecid/apNonce/apNonceSize: read from the live DFU-mode device
// (irecv_get_device_info()) -- this device's personalization is only
// valid for the ECID and nonce it was actually requested against.
//
// Returns the personalized (SHSH-stitched) img3 bytes on success. On
// failure -- including Apple's TSS server explicitly refusing (the
// expected outcome once a build has left Apple's current signing window)
// -- returns std::nullopt after printing the real reason to stderr, so
// that answer is never confused with a silent/ambiguous DFU failure.
std::optional<std::vector<uint8_t>> personalizeIMG3Component(const std::string& componentName,
                                                               const std::string& rawImg3Path,
                                                               std::shared_ptr<void> buildIdentity, uint64_t ecid,
                                                               const unsigned char* apNonce,
                                                               unsigned int apNonceSize);
