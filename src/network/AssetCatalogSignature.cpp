#include "AssetCatalogSignature.h"

#include <Memory.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/hash.h>

#include <cstdlib>
#include <cstring>

// The public half of the key the release workflows sign with (secret
// ASSET_CATALOG_SIGNING_KEY). A build for another asset host passes its own through
// CROSSINK_ASSET_CATALOG_KEY (scripts/git_branch.py).
#ifndef ASSET_CATALOG_PUBLIC_KEY
#define ASSET_CATALOG_PUBLIC_KEY                                                                      \
  "047b2c9c2d31ae126f6232266f2f0886b46475932a33bc87034124d676214f2cc4277c83c7a07a2a1ff8780b261c630c5" \
  "5a6ba31a96a43149d81d144d18c49daa1"
#endif

namespace AssetCatalogSignature {
namespace {
constexpr char HEADER[] = "CrossInk signed asset catalog 1\n";
constexpr size_t MAX_DER_BYTES = 72;  // ECDSA P-256: SEQUENCE of two INTEGERs of up to 33 bytes

int hexValue(const char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;  // lowercase only, as the producer writes it
}

bool decodeHex(const char* hex, const size_t hexLength, uint8_t* out, const size_t capacity) {
  if (hexLength % 2 || hexLength / 2 > capacity) return false;
  for (size_t i = 0; i < hexLength; i += 2) {
    const int high = hexValue(hex[i]);
    const int low = hexValue(hex[i + 1]);
    if (high < 0 || low < 0) return false;
    out[i / 2] = static_cast<uint8_t>(high << 4 | low);
  }
  return true;
}

// Reads one "name=value\n" line at cursor and advances past it.
bool readField(const char*& cursor, const char* end, const char* name, const char*& value, size_t& valueLength) {
  const size_t nameLength = std::strlen(name);
  if (static_cast<size_t>(end - cursor) <= nameLength || std::memcmp(cursor, name, nameLength) != 0 ||
      cursor[nameLength] != '=')
    return false;
  value = cursor + nameLength + 1;
  const auto* newline = static_cast<const char*>(std::memchr(value, '\n', static_cast<size_t>(end - value)));
  if (!newline) return false;
  valueLength = static_cast<size_t>(newline - value);
  cursor = newline + 1;
  return true;
}

bool readDigest(const char*& cursor, const char* end, const char* name, char (&out)[65]) {
  const char* value = nullptr;
  size_t length = 0;
  uint8_t ignored[32];
  if (!readField(cursor, end, name, value, length) || length != 64 || !decodeHex(value, length, ignored, 32))
    return false;
  std::memcpy(out, value, 64);
  out[64] = '\0';
  return true;
}

bool signatureHolds(const uint8_t* der, const size_t derLength, const uint8_t (&digest)[32],
                    const uint8_t (&publicKey)[PUBLIC_KEY_BYTES]) {
  // ecc_key holds the curve point and its scratch; too large for the C3 loop stack.
  auto key = makeUniqueNoThrow<ecc_key>();
  if (!key || wc_ecc_init(key.get()) != 0) return false;
  int verified = 0;
  const bool ok =
      wc_ecc_import_x963_ex(publicKey, PUBLIC_KEY_BYTES, key.get(), ECC_SECP256R1) == 0 &&
      wc_ecc_verify_hash(der, static_cast<word32>(derLength), digest, sizeof(digest), &verified, key.get()) == 0 &&
      verified == 1;
  wc_ecc_free(key.get());
  return ok;
}

bool parseKey(const char* hex, uint8_t (&out)[PUBLIC_KEY_BYTES]) {
  return hex && std::strlen(hex) == 2 * PUBLIC_KEY_BYTES &&
         decodeHex(hex, 2 * PUBLIC_KEY_BYTES, out, PUBLIC_KEY_BYTES) && out[0] == 0x04;
}
}  // namespace

const char* verifyWithKey(const char* text, const size_t length, const AssetCatalog::Kind kind,
                          const uint8_t (&publicKey)[PUBLIC_KEY_BYTES], Envelope& out) {
  out = {};
  if (!text || length > MAX_ENVELOPE_BYTES) return "envelope size";
  const char* cursor = text;
  const char* end = text + length;
  constexpr size_t headerLength = sizeof(HEADER) - 1;
  if (length < headerLength || std::memcmp(cursor, HEADER, headerLength) != 0) return "envelope header";
  cursor += headerLength;

  const char* value = nullptr;
  size_t valueLength = 0;
  const char* kindName = kind == AssetCatalog::Kind::Translation ? "translation" : "hyphenation";
  if (!readField(cursor, end, "kind", value, valueLength) || valueLength != std::strlen(kindName) ||
      std::memcmp(value, kindName, valueLength) != 0)
    return "envelope kind";
  if (!readDigest(cursor, end, "catalog", out.catalog) || !readDigest(cursor, end, "notices", out.notices) ||
      !readDigest(cursor, end, "sources", out.sources))
    return "envelope digest";

  const size_t signedLength = static_cast<size_t>(cursor - text);
  uint8_t der[MAX_DER_BYTES];
  if (!readField(cursor, end, "signature", value, valueLength) || cursor != end || valueLength < 16 ||
      !decodeHex(value, valueLength, der, sizeof(der)))
    return "envelope signature";
  uint8_t digest[32];
  if (wc_Sha256Hash(reinterpret_cast<const byte*>(text), static_cast<word32>(signedLength), digest) != 0)
    return "envelope hash";
  if (!signatureHolds(der, valueLength / 2, digest, publicKey)) {
    out = {};
    return "signature does not match the catalog key";
  }
  return nullptr;
}

const char* verify(const char* text, const size_t length, const AssetCatalog::Kind kind, Envelope& out) {
  uint8_t key[PUBLIC_KEY_BYTES];
  if (!parseKey(ASSET_CATALOG_PUBLIC_KEY, key)) return "invalid built-in catalog key";
  const char* error = verifyWithKey(text, length, kind, key, out);
#ifdef SIMULATOR
  if (error && parseKey(std::getenv("CROSSINK_SIM_ASSET_CATALOG_KEY"), key))
    error = verifyWithKey(text, length, kind, key, out);
#endif
  return error;
}
}  // namespace AssetCatalogSignature
