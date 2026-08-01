#pragma once
#include <string>

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
   * Get human-readable error message for the last error.
   */
  static std::string errorString(Error error);

  /** HTTP status code from the last request (for diagnostics). */
  static int lastHttpCode;

  /** Transport-layer error from the last request (for diagnostics). */
  static int lastTransportError;
};
