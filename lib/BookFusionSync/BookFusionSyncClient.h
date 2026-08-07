#pragma once
#include <cstdint>
#include <string>
#include <vector>

// How many books are requested/shown per browser page. Kept small so a page
// response stays well under the streaming JSON buffer.
constexpr int BOOKFUSION_BOOKS_PER_PAGE = 8;

/**
 * Response from starting the OAuth device-code flow.
 * Field names follow RFC 8628 (OAuth 2.0 Device Authorization Grant).
 */
struct BookFusionDeviceAuth {
  std::string deviceCode;               // Opaque; used internally to poll for a token, never shown to the user.
  std::string userCode;                 // Short code shown on-screen for the user to confirm/enter.
  std::string verificationUri;          // Page the user opens to enter userCode; displayed as text.
  std::string verificationUriComplete;  // Same page with userCode pre-filled, if the server sent one; QR payload.
  int interval = 5;                     // Minimum seconds between poll attempts.
  int expiresIn = 600;                  // Seconds until deviceCode expires.
};

/** One book entry in a library search/browse page. */
struct BookFusionBook {
  uint32_t bookId = 0;
  std::string title;
  std::string author;
};

/** One page of library search results. */
struct BookFusionSearchResult {
  std::vector<BookFusionBook> books;  // Capacity reserved to BOOKFUSION_BOOKS_PER_PAGE by the caller.
  int currentPage = 0;
  bool hasMore = false;
};

/** Reading position for one book, as stored by BookFusion. */
struct BookFusionProgress {
  uint32_t bookId = 0;
  float percentage = 0.0f;  // 0.0..1.0
  int64_t timestamp = 0;    // Unix timestamp reported by the server (never generated locally, see below).
};

/**
 * HTTP client for the BookFusion API.
 *
 * Base URL: https://www.bookfusion.com
 *
 * Auth: OAuth 2.0 Device Authorization Grant (RFC 8628). There is no local
 * expiry tracking -- this device has no guaranteed wall clock (RTC is
 * optional hardware), so every method here treats an AUTH_FAILED response
 * reactively and asks the caller to re-run the device-code flow, the same
 * pattern KOReaderSyncClient uses for its own credential failures.
 */
class BookFusionSyncClient {
 public:
  enum Error {
    OK = 0,
    NO_TOKEN,
    NETWORK_ERROR,
    AUTH_FAILED,
    AUTH_PENDING,  // User has not yet approved the device code.
    SLOW_DOWN,     // Server asked us to increase the poll interval.
    EXPIRED,       // Device code expired before the user approved it.
    SERVER_ERROR,
    JSON_ERROR,
    NOT_FOUND,
    LOW_MEMORY,
  };

  /** Start the device-code flow. Populates outAuth on success. */
  static Error startDeviceAuth(BookFusionDeviceAuth& outAuth);

  /**
   * Poll once for the token tied to a prior startDeviceAuth() call. Callers
   * should wait outAuth.interval seconds between calls (more, if a prior
   * call returned SLOW_DOWN) and stop after outAuth.expiresIn seconds.
   * On OK, the caller-visible token has already been saved to
   * BookFusionTokenStore.
   */
  static Error pollForToken(const std::string& deviceCode);

  /**
   * Fetch one page of the user's library.
   * @param page 0-based page index.
   * @param list Optional BookFusion list/category filter (nullptr for "all books").
   */
  static Error searchBooks(int page, const char* list, BookFusionSearchResult& out);

  /**
   * Keep one HTTPS connection alive across the subsequent searchBooks()/
   * getDownloadUrl() calls instead of paying for a fresh TLS handshake on
   * every page turn. Each wolfSSL handshake leaves a lasting scar on
   * ESP.getMaxAllocHeap() (transient handshake buffers fragment the heap);
   * a few handshakes in a row inside one browsing session can leave the
   * heap unable to grow a response buffer, aborting the firmware (no
   * exceptions). Call once when entering the browser. No-op in the
   * simulator build, which uses a different HTTP client with no comparable
   * heap constraint.
   */
  static void beginSession();

  /** Ends a session started with beginSession(). Call on browser exit, and
   * before handing off to HttpDownloader for an actual book download so the
   * idle session connection isn't held open during the transfer. */
  static void endSession();

  /** Get the stored reading position for a book. */
  static Error getProgress(uint32_t bookId, BookFusionProgress& outProgress);

  /** Upload a reading position for a book. */
  static Error updateProgress(const BookFusionProgress& progress);

  /**
   * Fetch a one-time download URL for a book (authenticated POST to
   * /api/user/books/{id}/download). The returned URL is expected to be
   * pre-signed by BookFusion, but callers should still pass
   * DownloadOptions.bearerToken = getBearerToken() to HttpDownloader when
   * fetching it: HttpDownloader only ever attaches that header when the
   * download URL's origin matches the request that obtained it, so this is
   * a safe defense-in-depth measure, not a requirement the pre-signed URL
   * depends on.
   */
  static Error getDownloadUrl(uint32_t bookId, std::string& outUrl);

  /** The current access token, for HttpDownloader::DownloadOptions.bearerToken. Empty if signed out. */
  static std::string getBearerToken();

  /** Human-readable error message, using lastHttpCode for SERVER_ERROR detail. */
  static std::string errorString(Error error);

  /** HTTP status code from the last request (for diagnostics). */
  static int lastHttpCode;

  /** Transport-layer error from the last request (for diagnostics). */
  static int lastTransportError;
};
