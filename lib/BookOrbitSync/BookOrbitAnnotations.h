#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

/**
 * Wire types for BookOrbit's two-way annotation exchange.
 *
 * The server keys every annotation by md5(datetime | pos0), so both fields have to stay
 * byte-identical between syncs or the same highlight comes back as a new one. Positions are
 * therefore minted once, when a highlight is created, and never recomputed (see
 * BookOrbitAnnotationStore); the datetime is validated server-side against
 * ^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d$ and the whole request is rejected otherwise.
 */

// A highlight on its way to the server.
struct BookOrbitAnnotation {
  char datetime[20] = {};  // "YYYY-MM-DD HH:MM:SS"
  std::string chapter;
  std::string pos0;  // xpointer at the highlight's first codepoint
  std::string pos1;  // exclusive end, so the server can resolve the range and read it back
  std::string text;
  uint16_t pageno = 0;
};

/**
 * A change the server wants applied on the device: a highlight created on the web, or a
 * deletion. (The exchange can also carry note/style edits; the device draws one kind of
 * highlight and does not consume them.)
 *
 * `serverId` is what the acknowledgment refers to, and the server keeps an entry pending until
 * it is acknowledged -- so an entry that cannot be applied is simply left unacknowledged rather
 * than reported as done.
 */
struct BookOrbitIncomingAnnotation {
  uint32_t serverId = 0;  // integer on the wire, and what the acknowledgment names
  // Adds carry the version the acknowledgment must echo; the deletion ack schema has no
  // version field and rejects a payload that includes one.
  uint32_t version = 0;
  char datetime[20] = {};  // "YYYY-MM-DD HH:MM:SS"
  // Each kind identifies its target its own way: a deletion by `key`, the md5 this device
  // itself reported, with no position or text at all; an add by `pos0` plus its text.
  std::string key;
  std::string pos0;  // xpointer; carries the chapter and paragraph the text lives in
  std::string text;
  std::string chapter;
  bool deleted = false;  // true for entries the server wants removed locally
};

/**
 * One acknowledgment line. An applied add must carry the version the server handed out, which
 * is how it detects an ack racing a newer edit. A deletion must not: its schema has no version
 * field, so zero here means "omit it from the payload".
 */
struct BookOrbitAckEntry {
  uint32_t serverId = 0;
  uint32_t version = 0;
};

// One row of the device's key set: the identity hash plus the datetime it was built from,
// which the server validates as a pair.
struct BookOrbitAnnotationKey {
  static constexpr size_t DIGEST_SIZE = 33;  // 32 hex characters plus a terminator
  char k[DIGEST_SIZE] = {};                  // md5(datetime | pos0)
  char dt[20] = {};                          // "YYYY-MM-DD HH:MM:SS"
};

/**
 * The device's key set, which the server diffs against its own per-device state to find what
 * was deleted here. Fixed-width rows rather than a container of strings, so the whole set is
 * one allocation -- it goes out in the same request as an annotation batch.
 *
 * When the set could not be built in full, send it empty with complete=false: the server then
 * skips deletion processing entirely rather than reading the gap as deletions. An empty set
 * sent WITH complete=true is meaningful too -- it deletes everything this device held.
 */
struct BookOrbitAnnotationKeys {
  const BookOrbitAnnotationKey* rows = nullptr;
  size_t count = 0;
  bool complete = true;
};

/**
 * Upload batch bounds. The reference plugin sends 50 entries, but it runs on hardware with
 * room to spare: here the client refuses to open a TLS session below 55 KB free and WiFi
 * leaves roughly 65 KB, so the serialized body has about ten to work with. Bounded both by
 * count and by total text bytes, since a single entry may carry up to 2 KB of text.
 */
inline constexpr size_t BOOKORBIT_ANNOTATION_BATCH = 8;
inline constexpr size_t BOOKORBIT_ANNOTATION_BATCH_TEXT_BYTES = 5 * 1024;

// Wire-side bound on one annotation's text. Keep equal to CLIPPING_TEXT_MAX: anything longer
// would be dropped by the clipping store anyway, so trimming at parse time just bounds the
// RAM the incoming batch can hold.
inline constexpr size_t BOOKORBIT_ANNOTATION_TEXT_MAX = 2048;

/**
 * Convert between the server's datetime strings and UTC epoch seconds.
 *
 * Deliberately no timezone interpretation: the server's datetimes are "device datetimes" whose
 * zone it never states, and both directions use the same UTC reading, so a value round-trips to
 * exactly the string it came from. That is what the identity hash needs -- meaning is irrelevant,
 * byte-equality is not.
 *
 * @return false when the string is not `YYYY-MM-DD HH:MM:SS` or holds an implausible date
 */
bool bookOrbitParseDatetime(const char* datetime, uint32_t& outEpochSeconds);
bool bookOrbitFormatDatetime(uint32_t epochSeconds, char (&outDatetime)[20]);

/**
 * Compute the server's identity key for an annotation.
 * @param datetime The annotation's datetime, exactly as it will be sent
 * @param pos0 The annotation's xpointer, exactly as it will be sent
 * @param outKey Receives 32 lowercase hex characters plus a terminator
 * @return false when either input is empty, leaving outKey untouched
 */
bool bookOrbitAnnotationKey(const char* datetime, const char* pos0,
                            char (&outKey)[BookOrbitAnnotationKey::DIGEST_SIZE]);
