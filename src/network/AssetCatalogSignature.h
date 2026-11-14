#pragma once

#include <cstddef>
#include <cstdint>

#include "AssetCatalog.h"

// This fork signs every catalog it publishes (scripts/export_pages_language_assets.py) in a
// short text envelope served beside it as "<catalog>.json.sig":
//
//   CrossInk signed asset catalog 1
//   kind=translation
//   catalog=<SHA-256 of the catalog's bytes>
//   notices=<SHA-256 of its snapshot's notices.json>
//   sources=<SHA-256 of its snapshot's sources.json>
//   signature=<hex DER ECDSA P-256 over the SHA-256 of every line above>
//
// The catalog already pins each pack's SHA-256 and sources.json each companion's, so the
// whole download is authenticated whatever the transport. On the C3 it arrives over wolfSSL,
// which does not check the server's certificate (AssetDownloadService::transport()).
namespace AssetCatalogSignature {
constexpr size_t MAX_ENVELOPE_BYTES = 512;
constexpr char SUFFIX[] = ".sig";
constexpr size_t PUBLIC_KEY_BYTES = 65;  // uncompressed P-256 point

struct Envelope {
  char catalog[65] = {};
  char notices[65] = {};
  char sources[65] = {};
};

// Checks the envelope against this build's catalog key, and in the simulator also against
// CROSSINK_SIM_ASSET_CATALOG_KEY (the smoke test signs its fixtures with a throwaway key).
// Returns nullptr when it holds, else the reason, for the log.
const char* verify(const char* text, size_t length, AssetCatalog::Kind kind, Envelope& out);
const char* verifyWithKey(const char* text, size_t length, AssetCatalog::Kind kind,
                          const uint8_t (&publicKey)[PUBLIC_KEY_BYTES], Envelope& out);
}  // namespace AssetCatalogSignature
