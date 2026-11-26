#include <gtest/gtest.h>
#include <wolfssl/wolfcrypt/ecc.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssl/wolfcrypt/random.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "AssetCatalogSignature.h"

using AssetCatalog::Kind;
using AssetCatalogSignature::Envelope;

namespace {
// Signed by scripts/export_pages_language_assets.py (OpenSSL) with the release key, whose
// public half is the firmware's built-in ASSET_CATALOG_PUBLIC_KEY.
const std::string RELEASE_ENVELOPE =
    "CrossInk signed asset catalog 1\n"
    "kind=hyphenation\n"
    "catalog=b028b1de2d88f6a1af0a32f69e1a5506227c3d6454cf36b53f33416970fcbeed\n"
    "notices=31605e4cf4944705e228beffae25b01f0dd49d3e25138ccf5c945ab67af95354\n"
    "sources=d7e8ae0ac7ddd7dc9b17ed8acac6d5c8e6e1c7e802c3520edc28cff590eacd7d\n"
    "signature=30440220288a72a8bbf1a6f9f6c569680bc218af6ccc9dc8fedad6fda57ce8359c01443202202adfaaa678ce7dac995671"
    "f4ec956aa701886b41ffd4ad308143a100f7a9dae2\n";

std::string hex(const uint8_t* bytes, size_t length) {
  std::string out;
  char pair[3];
  for (size_t i = 0; i < length; ++i) {
    std::snprintf(pair, sizeof(pair), "%02x", bytes[i]);
    out += pair;
  }
  return out;
}

std::string signedLines(const char* kind = "translation") {
  return std::string("CrossInk signed asset catalog 1\nkind=") + kind + "\ncatalog=" + std::string(64, 'a') +
         "\nnotices=" + std::string(64, 'b') + "\nsources=" + std::string(64, 'c') + "\n";
}

struct TestKey {
  WC_RNG rng{};
  ecc_key key{};
  uint8_t publicKey[AssetCatalogSignature::PUBLIC_KEY_BYTES] = {};

  TestKey() {
    EXPECT_EQ(wc_InitRng(&rng), 0);
    EXPECT_EQ(wc_ecc_init(&key), 0);
    EXPECT_EQ(wc_ecc_make_key_ex(&rng, 32, &key, ECC_SECP256R1), 0);
    word32 length = sizeof(publicKey);
    EXPECT_EQ(wc_ecc_export_x963(&key, publicKey, &length), 0);
    EXPECT_EQ(length, sizeof(publicKey));
  }
  ~TestKey() {
    wc_ecc_free(&key);
    wc_FreeRng(&rng);
  }
  std::string sign(const std::string& lines) {
    uint8_t digest[32];
    EXPECT_EQ(wc_Sha256Hash(reinterpret_cast<const byte*>(lines.data()), lines.size(), digest), 0);
    uint8_t der[80];
    word32 length = sizeof(der);
    EXPECT_EQ(wc_ecc_sign_hash(digest, sizeof(digest), der, &length, &rng, &key), 0);
    return lines + "signature=" + hex(der, length) + "\n";
  }
  const char* verify(const std::string& text, Kind kind, Envelope& out) const {
    return AssetCatalogSignature::verifyWithKey(text.data(), text.size(), kind, publicKey, out);
  }
};

const char* verifyBuiltIn(const std::string& text, Kind kind, Envelope& out) {
  return AssetCatalogSignature::verify(text.data(), text.size(), kind, out);
}
}  // namespace

TEST(AssetCatalogSignature, AcceptsTheReleaseKeysOpenSslSignature) {
  Envelope out;
  EXPECT_EQ(verifyBuiltIn(RELEASE_ENVELOPE, Kind::Hyphenation, out), nullptr);
  EXPECT_STREQ(out.catalog, "b028b1de2d88f6a1af0a32f69e1a5506227c3d6454cf36b53f33416970fcbeed");
  EXPECT_STREQ(out.notices, "31605e4cf4944705e228beffae25b01f0dd49d3e25138ccf5c945ab67af95354");
  EXPECT_STREQ(out.sources, "d7e8ae0ac7ddd7dc9b17ed8acac6d5c8e6e1c7e802c3520edc28cff590eacd7d");
}

TEST(AssetCatalogSignature, RefusesTheReleaseEnvelopeForTheOtherKind) {
  Envelope out;
  EXPECT_STREQ(verifyBuiltIn(RELEASE_ENVELOPE, Kind::Translation, out), "envelope kind");
}

TEST(AssetCatalogSignature, RefusesAnyChangeToTheSignedLines) {
  for (const char* field : {"catalog=", "notices=", "sources="}) {
    std::string text = RELEASE_ENVELOPE;
    const size_t at = text.find(field) + std::strlen(field);
    text[at] = text[at] == '0' ? '1' : '0';
    Envelope out;
    EXPECT_STREQ(verifyBuiltIn(text, Kind::Hyphenation, out), "signature does not match the catalog key") << field;
    EXPECT_STREQ(out.catalog, "") << "a refused envelope vouches for nothing";
  }
}

TEST(AssetCatalogSignature, AcceptsItsOwnKeyOnly) {
  TestKey signer;
  TestKey other;
  const std::string text = signer.sign(signedLines());
  Envelope out;
  EXPECT_EQ(signer.verify(text, Kind::Translation, out), nullptr);
  EXPECT_STREQ(out.sources, std::string(64, 'c').c_str());
  EXPECT_STREQ(other.verify(text, Kind::Translation, out), "signature does not match the catalog key");
  EXPECT_NE(verifyBuiltIn(text, Kind::Translation, out), nullptr);
}

TEST(AssetCatalogSignature, RefusesMalformedEnvelopes) {
  TestKey signer;
  const std::string good = signer.sign(signedLines());
  std::string upper = signedLines();
  upper[upper.find("catalog=") + 8] = 'A';
  std::string crlf = signedLines();
  crlf.insert(crlf.find("\nkind="), "\r");
  const std::string cases[] = {
      "",
      good.substr(0, good.size() - 1),                                       // no final newline
      good + "x",                                                            // trailing data
      signer.sign(upper),                                                    // digests are lowercase hex
      signer.sign(crlf),                                                     // header line must be exact
      signer.sign(signedLines().substr(0, signedLines().find("notices="))),  // missing fields
      good.substr(0, good.find("signature=")) + "signature=30\n",            // too short to be DER
      good + std::string(AssetCatalogSignature::MAX_ENVELOPE_BYTES, '\n'),   // oversize
  };
  for (const auto& text : cases) {
    Envelope out;
    EXPECT_NE(signer.verify(text, Kind::Translation, out), nullptr) << text;
  }
}

TEST(AssetCatalogSignature, SimulatorAlsoTrustsTheSmokeTestKey) {
  TestKey smoke;
  const std::string text = smoke.sign(signedLines("hyphenation"));
  Envelope out;
  unsetenv("CROSSINK_SIM_ASSET_CATALOG_KEY");
  EXPECT_NE(verifyBuiltIn(text, Kind::Hyphenation, out), nullptr);
  setenv("CROSSINK_SIM_ASSET_CATALOG_KEY", hex(smoke.publicKey, sizeof(smoke.publicKey)).c_str(), 1);
  EXPECT_EQ(verifyBuiltIn(text, Kind::Hyphenation, out), nullptr);
  EXPECT_EQ(verifyBuiltIn(RELEASE_ENVELOPE, Kind::Hyphenation, out), nullptr) << "the release key still holds";
  unsetenv("CROSSINK_SIM_ASSET_CATALOG_KEY");
}
