#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "include/core/SkData.h"
#include "include/core/SkRefCnt.h"

namespace RNSkia {

class RNSkPipelineStorage {
public:
  static std::unique_ptr<RNSkPipelineStorage>
  Open(const std::string &cacheDirectory, const std::string &version);

  size_t loadBlob(const uint8_t *key, size_t keySize, uint8_t *value,
                  size_t valueSize) const;
  void storeBlob(const uint8_t *key, size_t keySize, const uint8_t *value,
                 size_t valueSize) const;

  void storePipelineKey(const SkData &pipelineKey) const;
  std::vector<sk_sp<SkData>> loadPipelineKeys() const;
  void rejectPipelineKey(const SkData &pipelineKey) const;

  const std::filesystem::path &root() const { return _root; }

private:
  RNSkPipelineStorage(std::filesystem::path root,
                      std::filesystem::path blobDirectory,
                      std::filesystem::path pipelineDirectory);

  std::filesystem::path blobPath(const uint8_t *key, size_t keySize) const;
  std::filesystem::path pipelinePath(const SkData &pipelineKey,
                                     const char *extension) const;
  bool writeAtomically(const std::filesystem::path &path,
                       const std::vector<uint8_t> &contents) const;

  std::filesystem::path _root;
  std::filesystem::path _blobDirectory;
  std::filesystem::path _pipelineDirectory;
  mutable std::atomic<uint64_t> _nextTemporaryId{0};
};

namespace RNSkPipelineStorageFormat {

std::string HashName(const uint8_t *bytes, size_t size);

std::vector<uint8_t> EncodeBlob(const uint8_t *key, size_t keySize,
                                const uint8_t *value, size_t valueSize);

bool DecodeBlob(const std::vector<uint8_t> &contents, const uint8_t *key,
                size_t keySize, size_t *valueOffset, size_t *valueSize);

} // namespace RNSkPipelineStorageFormat

} // namespace RNSkia
