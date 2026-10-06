#include "RNSkPipelineStorage.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <system_error>
#include <thread>
#include <utility>

#include "utils/RNSkLog.h"

namespace RNSkia {

namespace fs = std::filesystem;

namespace {

constexpr std::array<uint8_t, 4> kBlobMagic = {'R', 'N', 'S', 'B'};
constexpr uint32_t kBlobFormatVersion = 1;
constexpr size_t kBlobHeaderSize =
    kBlobMagic.size() + sizeof(uint32_t) + sizeof(uint64_t);

constexpr const char *kStorageDirectoryName = "react-native-skia";
constexpr const char *kPipelineCacheDirectoryName = "pipeline-cache";
constexpr const char *kBlobDirectoryName = "dawn";
constexpr const char *kPipelineDirectoryName = "graphite";
constexpr const char *kPipelineKeyExtension = ".key";
constexpr const char *kRejectedPipelineKeyExtension = ".rejected";
constexpr const char *kBlobExtension = ".blob";

uint64_t fnv1a(const uint8_t *bytes, size_t size, uint64_t basis) {
  uint64_t hash = basis;
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= 0x100000001b3ULL;
  }
  return hash;
}

void appendLittleEndian(std::vector<uint8_t> &out, uint64_t value,
                        size_t byteCount) {
  for (size_t i = 0; i < byteCount; ++i) {
    out.push_back(static_cast<uint8_t>(value >> (8 * i)));
  }
}

uint64_t readLittleEndian(const uint8_t *bytes, size_t byteCount) {
  uint64_t value = 0;
  for (size_t i = 0; i < byteCount; ++i) {
    value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
  }
  return value;
}

bool readFile(const fs::path &path, std::vector<uint8_t> *contents) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  contents->assign(std::istreambuf_iterator<char>(file),
                   std::istreambuf_iterator<char>());
  return !file.bad();
}

} // namespace

namespace RNSkPipelineStorageFormat {

std::string HashName(const uint8_t *bytes, size_t size) {
  static constexpr char kHex[] = "0123456789abcdef";
  const std::array<uint64_t, 2> halves = {
      fnv1a(bytes, size, 0xcbf29ce484222325ULL),
      fnv1a(bytes, size, 0x84222325cbf29ce4ULL)};
  std::string name;
  name.reserve(32);
  for (uint64_t half : halves) {
    for (int shift = 60; shift >= 0; shift -= 4) {
      name.push_back(kHex[(half >> shift) & 0xf]);
    }
  }
  return name;
}

std::vector<uint8_t> EncodeBlob(const uint8_t *key, size_t keySize,
                                const uint8_t *value, size_t valueSize) {
  std::vector<uint8_t> contents;
  contents.reserve(kBlobHeaderSize + keySize + valueSize);
  contents.insert(contents.end(), kBlobMagic.begin(), kBlobMagic.end());
  appendLittleEndian(contents, kBlobFormatVersion, sizeof(uint32_t));
  appendLittleEndian(contents, keySize, sizeof(uint64_t));
  contents.insert(contents.end(), key, key + keySize);
  contents.insert(contents.end(), value, value + valueSize);
  return contents;
}

bool DecodeBlob(const std::vector<uint8_t> &contents, const uint8_t *key,
                size_t keySize, size_t *valueOffset, size_t *valueSize) {
  if (contents.size() < kBlobHeaderSize) {
    return false;
  }
  if (!std::equal(kBlobMagic.begin(), kBlobMagic.end(), contents.begin())) {
    return false;
  }
  const uint8_t *cursor = contents.data() + kBlobMagic.size();
  if (readLittleEndian(cursor, sizeof(uint32_t)) != kBlobFormatVersion) {
    return false;
  }
  cursor += sizeof(uint32_t);
  const uint64_t storedKeySize = readLittleEndian(cursor, sizeof(uint64_t));
  if (storedKeySize != keySize ||
      contents.size() - kBlobHeaderSize < storedKeySize) {
    return false;
  }
  const uint8_t *storedKey = contents.data() + kBlobHeaderSize;
  if (keySize > 0 && std::memcmp(storedKey, key, keySize) != 0) {
    return false;
  }
  *valueOffset = kBlobHeaderSize + keySize;
  *valueSize = contents.size() - *valueOffset;
  return *valueSize > 0;
}

} // namespace RNSkPipelineStorageFormat

std::unique_ptr<RNSkPipelineStorage>
RNSkPipelineStorage::Open(const std::string &cacheDirectory,
                          const std::string &version) {
  if (cacheDirectory.empty() || version.empty()) {
    return nullptr;
  }
  const fs::path parent = fs::path(cacheDirectory) / kStorageDirectoryName /
                          kPipelineCacheDirectoryName;
  const fs::path root = parent / version;
  const fs::path blobDirectory = root / kBlobDirectoryName;
  const fs::path pipelineDirectory = root / kPipelineDirectoryName;

  std::error_code error;
  fs::create_directories(blobDirectory, error);
  if (!error) {
    fs::create_directories(pipelineDirectory, error);
  }
  if (error) {
    RNSkLogger::logToConsole(
        "The pipeline cache could not create %s: %s. Pipelines will not be "
        "cached on disk.",
        root.c_str(), error.message().c_str());
    return nullptr;
  }

  for (fs::directory_iterator it(parent, error), end; !error && it != end;
       it.increment(error)) {
    if (it->path().filename() == version) {
      continue;
    }
    std::error_code removeError;
    fs::remove_all(it->path(), removeError);
    if (removeError) {
      RNSkLogger::logToConsole(
          "The pipeline cache could not remove the stale directory %s: %s.",
          it->path().c_str(), removeError.message().c_str());
    }
  }

  return std::unique_ptr<RNSkPipelineStorage>(
      new RNSkPipelineStorage(root, blobDirectory, pipelineDirectory));
}

RNSkPipelineStorage::RNSkPipelineStorage(fs::path root, fs::path blobDirectory,
                                         fs::path pipelineDirectory)
    : _root(std::move(root)), _blobDirectory(std::move(blobDirectory)),
      _pipelineDirectory(std::move(pipelineDirectory)) {}

fs::path RNSkPipelineStorage::blobPath(const uint8_t *key,
                                       size_t keySize) const {
  return _blobDirectory /
         (RNSkPipelineStorageFormat::HashName(key, keySize) + kBlobExtension);
}

fs::path RNSkPipelineStorage::pipelinePath(const SkData &pipelineKey,
                                           const char *extension) const {
  return _pipelineDirectory / (RNSkPipelineStorageFormat::HashName(
                                   pipelineKey.bytes(), pipelineKey.size()) +
                               extension);
}

bool RNSkPipelineStorage::writeAtomically(
    const fs::path &path, const std::vector<uint8_t> &contents) const {
  const fs::path temporary =
      path.string() + ".tmp-" +
      std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id())) +
      "-" + std::to_string(_nextTemporaryId.fetch_add(1));
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
      RNSkLogger::logToConsole("The pipeline cache could not open %s.",
                               temporary.c_str());
      return false;
    }
    file.write(reinterpret_cast<const char *>(contents.data()),
               static_cast<std::streamsize>(contents.size()));
    if (!file) {
      RNSkLogger::logToConsole("The pipeline cache could not write %s.",
                               temporary.c_str());
      file.close();
      std::error_code ignored;
      fs::remove(temporary, ignored);
      return false;
    }
  }
  std::error_code error;
  fs::rename(temporary, path, error);
  if (error) {
    RNSkLogger::logToConsole("The pipeline cache could not move %s into "
                             "place: %s.",
                             path.c_str(), error.message().c_str());
    std::error_code ignored;
    fs::remove(temporary, ignored);
    return false;
  }
  return true;
}

size_t RNSkPipelineStorage::loadBlob(const uint8_t *key, size_t keySize,
                                     uint8_t *value, size_t valueSize) const {
  std::vector<uint8_t> contents;
  if (!readFile(blobPath(key, keySize), &contents)) {
    return 0;
  }
  size_t storedOffset = 0;
  size_t storedSize = 0;
  if (!RNSkPipelineStorageFormat::DecodeBlob(contents, key, keySize,
                                             &storedOffset, &storedSize)) {
    return 0;
  }
  if (value == nullptr || valueSize == 0) {
    return storedSize;
  }
  if (valueSize != storedSize) {
    return 0;
  }
  std::memcpy(value, contents.data() + storedOffset, storedSize);
  return storedSize;
}

void RNSkPipelineStorage::storeBlob(const uint8_t *key, size_t keySize,
                                    const uint8_t *value,
                                    size_t valueSize) const {
  if (key == nullptr || keySize == 0 || value == nullptr || valueSize == 0) {
    return;
  }
  writeAtomically(blobPath(key, keySize), RNSkPipelineStorageFormat::EncodeBlob(
                                              key, keySize, value, valueSize));
}

void RNSkPipelineStorage::storePipelineKey(const SkData &pipelineKey) const {
  if (pipelineKey.empty()) {
    return;
  }
  const fs::path path = pipelinePath(pipelineKey, kPipelineKeyExtension);
  std::error_code error;
  if (fs::exists(path, error) ||
      fs::exists(pipelinePath(pipelineKey, kRejectedPipelineKeyExtension),
                 error)) {
    return;
  }
  writeAtomically(
      path, std::vector<uint8_t>(pipelineKey.bytes(),
                                 pipelineKey.bytes() + pipelineKey.size()));
}

std::vector<sk_sp<SkData>> RNSkPipelineStorage::loadPipelineKeys() const {
  std::vector<sk_sp<SkData>> keys;
  std::error_code error;
  for (fs::directory_iterator it(_pipelineDirectory, error), end;
       !error && it != end; it.increment(error)) {
    if (it->path().extension() != kPipelineKeyExtension) {
      continue;
    }
    std::vector<uint8_t> contents;
    if (!readFile(it->path(), &contents) || contents.empty()) {
      continue;
    }
    keys.push_back(SkData::MakeWithCopy(contents.data(), contents.size()));
  }
  if (error) {
    RNSkLogger::logToConsole("The pipeline cache could not list %s: %s.",
                             _pipelineDirectory.c_str(),
                             error.message().c_str());
  }
  return keys;
}

void RNSkPipelineStorage::rejectPipelineKey(const SkData &pipelineKey) const {
  std::error_code error;
  fs::rename(pipelinePath(pipelineKey, kPipelineKeyExtension),
             pipelinePath(pipelineKey, kRejectedPipelineKeyExtension), error);
  if (error) {
    RNSkLogger::logToConsole("The pipeline cache could not set aside a "
                             "pipeline key that cannot be precompiled: %s.",
                             error.message().c_str());
  }
}

} // namespace RNSkia
