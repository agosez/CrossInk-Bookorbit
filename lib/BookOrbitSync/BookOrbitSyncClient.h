#pragma once
#include <string>
#include <vector>

#include "BookOrbitAnnotations.h"
#include "BookOrbitStatsQueue.h"
#include "KOReaderSyncClient.h"

/**
 * HTTP client for BookOrbit's KOReader-compatible sync API.
 *
 * BookOrbit exposes the same kosync wire protocol as a plain koreader-sync
 * server (same request/response shapes, same x-auth-user/x-auth-key headers,
 * same userkey = MD5(password)), just mounted under an API prefix instead of
 * the root. We reuse KOReaderProgress since the wire format is identical.
 *
 * Base URL: BookOrbitCredentialStore::getBaseUrl(), e.g.
 *   https://books.example.com/api/v1/koreader
 *
 * API Endpoints:
 *   GET /users/auth - Authenticate (validate credentials)
 *   GET /syncs/progress/:document - Get progress for a document
 *   PUT /syncs/progress - Update progress for a document
 *
 * Authentication:
 *   x-auth-user: username
 *   x-auth-key: MD5 hash of password
 *
 * Document identity: BookOrbit only supports the binary partial-MD5 document
 * hash (see KOReaderDocumentId::calculate), unlike plain koreader-sync servers
 * which can also be configured to match by filename.
 */
class BookOrbitSyncClient {
 public:
  using Error = KOReaderSyncClient::Error;
  // KOReaderSyncClient::Error's enumerators live in KOReaderSyncClient's scope, not in
  // any scope reachable through this type alias. Re-expose them as members of this
  // class so callers can write BookOrbitSyncClient::OK (and this class's own .cpp can
  // use the bare names) instead of reaching back into KOReaderSyncClient everywhere.
  static constexpr Error OK = KOReaderSyncClient::OK;
  static constexpr Error NO_CREDENTIALS = KOReaderSyncClient::NO_CREDENTIALS;
  static constexpr Error NETWORK_ERROR = KOReaderSyncClient::NETWORK_ERROR;
  static constexpr Error AUTH_FAILED = KOReaderSyncClient::AUTH_FAILED;
  static constexpr Error SERVER_ERROR = KOReaderSyncClient::SERVER_ERROR;
  static constexpr Error JSON_ERROR = KOReaderSyncClient::JSON_ERROR;
  static constexpr Error NOT_FOUND = KOReaderSyncClient::NOT_FOUND;
  static constexpr Error INVALID_AUTH_RESPONSE = KOReaderSyncClient::INVALID_AUTH_RESPONSE;
  static constexpr Error LOW_MEMORY = KOReaderSyncClient::LOW_MEMORY;

  /**
   * Keeps one TLS connection open across the requests of a single sync.
   *
   * A sync makes several requests to the same host in a row, and a handshake costs a second
   * or two on this hardware. Hold a Session around the sequence and they share one:
   *
   *   BookOrbitSyncClient::Session session;
   *   getProgress(...);
   *   uploadPageStats(...);
   *
   * Requests made with no Session open behave exactly as before, one connection each. Do
   * not hold one across a user decision — it would keep a socket open while the screen waits.
   */
  class Session {
   public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
  };

  /**
   * Authenticate with the BookOrbit server (validate credentials).
   * @return OK on success, error code on failure
   */
  static Error authenticate();

  /**
   * Get reading progress for a document.
   * @param documentHash The binary partial-MD5 document hash
   * @param outProgress Output: the progress data
   * @return OK on success, NOT_FOUND if no progress exists, error code on failure
   */
  static Error getProgress(const std::string& documentHash, KOReaderProgress& outProgress);

  /**
   * Update reading progress for a document.
   * @param progress The progress data to upload
   * @return OK on success, error code on failure
   */
  static Error updateProgress(const KOReaderProgress& progress);

  /**
   * Upload queued reading-session events for one document to BookOrbit's
   * page-stats endpoint (POST /plugin/page-stats). Upload-only: the endpoint has no
   * download counterpart, so stats flow CrossInk -> BookOrbit.
   * @param documentHash The binary partial-MD5 document hash
   * @param deviceModel Human-readable device name reported alongside the stats
   * @param events Events to upload (callers batch; keep each call small)
   * @param count Number of events
   * @return OK on success, error code on failure
   */
  static Error uploadPageStats(const std::string& documentHash, const std::string& deviceModel,
                               const BookOrbitStatEvent* events, size_t count);

  /**
   * Send one batch of local highlight changes to BookOrbit's annotation exchange
   * (POST /plugin/annotations/exchange).
   *
   * Two-way: local changes go up, and whatever the server wants applied locally comes back in
   * outIncoming. Pass nullptr to ignore that half -- the server keeps every change it offered
   * pending until ackAnnotations names it, so nothing is lost by not reading it.
   *
   * Send `keys` on the first request of a sync only, and empty on the ones that follow --
   * the server reads it as the device's complete set and deletes what is missing from it.
   * Keep batches at BOOKORBIT_ANNOTATION_BATCH; the payload has to fit beside an open TLS
   * session.
   *
   * @param documentHash The binary partial-MD5 document hash
   * @param deviceModel Human-readable device name reported alongside the annotations
   * @param keys The device's full key set, or count=0/complete=false to suppress deletions
   * @param changes Annotations to send (may be empty when only `keys` needs to go out)
   * @param changeCount Number of annotations
   * @param outUnmatched Set when the server does not recognise this document at all
   * @param outIncoming Receives the server's own changes, capped at BOOKORBIT_ANNOTATION_BATCH;
   *                    entries already present (same serverId) are not appended twice, so the
   *                    same vector can accumulate across pull rounds
   * @param outMorePending Set when the server holds more changes than this response carried,
   *                       or converted positions this round that ship on the next request
   * @return OK on success, error code on failure
   */
  static Error exchangeAnnotations(const std::string& documentHash, const std::string& deviceModel,
                                   const BookOrbitAnnotationKeys& keys, const BookOrbitAnnotation* changes,
                                   size_t changeCount, bool& outUnmatched,
                                   std::vector<BookOrbitIncomingAnnotation>* outIncoming = nullptr,
                                   bool* outMorePending = nullptr);

  /**
   * Acknowledge server-side annotation changes that landed on the device
   * (POST /plugin/annotations/exchange-ack).
   *
   * The server keeps every change it offered pending until this call names it, which is what
   * stops a highlight created on the web from being lost when applying it fails halfway. Only
   * acknowledge what actually landed: an entry left out simply comes back next sync.
   *
   * @param applied Entries that landed locally, with the serverId and version the server sent
   * @return OK on success, error code on failure
   */
  static Error ackAnnotations(const std::string& documentHash, const std::string& deviceModel,
                              const std::vector<BookOrbitAckEntry>& applied,
                              const std::vector<BookOrbitAckEntry>& deleted);

  /**
   * Get human-readable error message for the last error.
   */
  static std::string errorString(Error error);

  /** HTTP status code from the last request (for diagnostics). */
  static int lastHttpCode;

  /** Transport-layer error from the last request (for diagnostics). */
  static int lastTransportError;
};
