#pragma once
#include <cstdint>
#include <string>

/**
 * Per-book sidecar linking a local EPUB to its BookFusion book ID.
 *
 * Not a PersistableStore singleton -- one JSON file per book, living inside
 * that book's existing cache directory (Epub::cachePathForFilePath(), i.e.
 * /.crosspoint/epub_<fnvHash64(path)>/bookfusion.json), so it shares the same
 * path-hash identity the EPUB cache already uses. See BookCacheUtils.cpp,
 * where "bookfusion.json" is added to the cache-preservation lists so this
 * sidecar survives cache clears/rebuilds like progress.bin does.
 *
 * Returns 0 from loadBookId() when no sidecar exists -- 0 is never a valid
 * BookFusion book ID.
 */
class BookFusionBookIdStore {
 public:
  // Reads the sidecar for epubPath, or returns 0 if none exists.
  static uint32_t loadBookId(const std::string& epubPath);

  // Writes (or overwrites) the sidecar for epubPath. Returns false on failure.
  static bool saveBookId(const std::string& epubPath, uint32_t bookId);

  // Removes the sidecar for epubPath, if any.
  static void clearBookId(const std::string& epubPath);

 private:
  static std::string sidecarPath(const std::string& epubPath);
};
