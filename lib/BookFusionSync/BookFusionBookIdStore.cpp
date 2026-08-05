#include "BookFusionBookIdStore.h"

#include <ArduinoJson.h>
#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>

namespace {
constexpr char SIDECAR_FILENAME[] = "bookfusion.json";
}  // namespace

std::string BookFusionBookIdStore::sidecarPath(const std::string& epubPath) {
  return Epub::cachePathForFilePath(epubPath, "/.crosspoint") + "/" + SIDECAR_FILENAME;
}

uint32_t BookFusionBookIdStore::loadBookId(const std::string& epubPath) {
  const std::string path = sidecarPath(epubPath);
  if (!Storage.exists(path.c_str())) {
    return 0;
  }

  JsonDocument doc;
  if (!PersistableStoreBase::readDocFromFile(path.c_str(), doc)) {
    return 0;
  }

  return doc["bookId"] | (uint32_t)0;
}

bool BookFusionBookIdStore::saveBookId(const std::string& epubPath, uint32_t bookId) {
  if (bookId == 0) {
    LOG_ERR("BFS", "Refusing to save sidecar with bookId=0 (reserved for 'no id')");
    return false;
  }

  const std::string cacheDir = Epub::cachePathForFilePath(epubPath, "/.crosspoint");
  Storage.mkdir(cacheDir.c_str());

  JsonDocument doc;
  doc["bookId"] = bookId;
  return PersistableStoreBase::writeDocToFile(sidecarPath(epubPath).c_str(), doc);
}

void BookFusionBookIdStore::clearBookId(const std::string& epubPath) {
  const std::string path = sidecarPath(epubPath);
  if (Storage.exists(path.c_str())) {
    Storage.remove(path.c_str());
  }
}
