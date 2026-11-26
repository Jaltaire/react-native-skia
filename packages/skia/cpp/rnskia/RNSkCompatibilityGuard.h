#pragma once

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <mutex>
#include <optional>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace RNSkia {

/**
 How many launches in a row on one app version may end before the first frame
 presented on a compatibility device before Skia stops trying that level on
 that version. Two rather than one: a driver fault ends every attempt the
 same way, while a single launch can end early for reasons of its own (the
 user leaves, or the system stops the process).
 */
inline constexpr int kRNSkCompatibilityUnfinishedLaunchesToStop = 2;

/**
 The guard's record: the app version the attempts were made on, and how many
 in a row ended before their first frame.
 */
struct RNSkCompatibilityAttempts {
  std::string appVersion;
  int unfinished = 0;
};

/** What Skia does about the compatibility level on this launch. */
enum class RNSkCompatibilityPlan {
  // Try it, after recording the attempt.
  Attempt,
  // Not on this app version: its last attempts ended before their first
  // frame.
  StopUnfinished,
  // Not without a record: a crash on that level could not be caught on the
  // next launch.
  StopUnrecorded,
};

struct RNSkCompatibilityDecision {
  RNSkCompatibilityPlan plan;
  // What to record before the attempt (Attempt only).
  RNSkCompatibilityAttempts record;
};

/**
 The guard's rule. A record of another app version counts for nothing, so an
 update tries again, and so does a record restored from elsewhere.
 */
inline RNSkCompatibilityDecision
decideCompatibilityAttempt(const std::optional<RNSkCompatibilityAttempts> &recorded,
                           const std::string &appVersion, bool canRecord) {
  if (!canRecord) {
    return {RNSkCompatibilityPlan::StopUnrecorded, {}};
  }
  const int before = recorded && recorded->appVersion == appVersion
                         ? recorded->unfinished
                         : 0;
  if (before >= kRNSkCompatibilityUnfinishedLaunchesToStop) {
    return {RNSkCompatibilityPlan::StopUnfinished, *recorded};
  }
  return {RNSkCompatibilityPlan::Attempt, {appVersion, before + 1}};
}

/** Why the compatibility level is not tried, for Skia.gpu.error. */
inline std::string compatibilityStopReason(RNSkCompatibilityPlan plan) {
  switch (plan) {
  case RNSkCompatibilityPlan::StopUnfinished:
    return "React Native Skia did not use the WebGPU compatibility level: "
           "its last two attempts on this app version ended before their "
           "first frame.";
  case RNSkCompatibilityPlan::StopUnrecorded:
    return "React Native Skia did not use the WebGPU compatibility level: it "
           "could not record the attempt, so a crash on that level could not "
           "be caught on the next launch.";
  case RNSkCompatibilityPlan::Attempt:
    return "";
  }
  return "";
}

inline std::string
formatCompatibilityAttempts(const RNSkCompatibilityAttempts &attempts) {
  return attempts.appVersion + "\n" + std::to_string(attempts.unfinished) +
         "\n";
}

/** The record from its file's text; nullopt when the text is not one. */
inline std::optional<RNSkCompatibilityAttempts>
parseCompatibilityAttempts(const std::string &text) {
  const auto versionEnd = text.find('\n');
  if (versionEnd == std::string::npos) {
    return std::nullopt;
  }
  const auto countEnd = text.find('\n', versionEnd + 1);
  if (countEnd == std::string::npos || countEnd + 1 != text.size()) {
    return std::nullopt;
  }
  const auto count = text.substr(versionEnd + 1, countEnd - versionEnd - 1);
  if (count.empty() || count.size() > 3 ||
      count.find_first_not_of("0123456789") != std::string::npos) {
    return std::nullopt;
  }
  return RNSkCompatibilityAttempts{text.substr(0, versionEnd),
                                   std::stoi(count)};
}

/**
 The record's file, in a directory the platform keeps out of backups (see
 RNSkPlatformContext::getCompatibilityConfig). It is written to a temporary
 file, flushed, and renamed over the record, so a crash right after the
 write leaves either the old record or the new one, never a partial one.
 */
class RNSkCompatibilityGuardFile {
public:
  explicit RNSkCompatibilityGuardFile(std::string directory)
      : _directory(std::move(directory)) {}

  bool usable() const { return !_directory.empty(); }

  std::string path() const {
    return _directory + "/rnskia-compatibility-attempts";
  }

  /** The record, or nullopt when there is none or it is not one. */
  std::optional<RNSkCompatibilityAttempts> read() const {
    if (!usable()) {
      return std::nullopt;
    }
    FILE *file = std::fopen(path().c_str(), "rb");
    if (file == nullptr) {
      return std::nullopt;
    }
    std::string text;
    char buffer[256];
    size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0 &&
           text.size() < 1024) {
      text.append(buffer, count);
    }
    std::fclose(file);
    return parseCompatibilityAttempts(text);
  }

  /** Records `attempts` durably; false when any step fails. */
  bool write(const RNSkCompatibilityAttempts &attempts) const {
    if (!usable()) {
      return false;
    }
    const auto temporary = path() + ".tmp";
    const auto text = formatCompatibilityAttempts(attempts);
    const int fd =
        ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
               S_IRUSR | S_IWUSR);
    if (fd < 0) {
      return false;
    }
    bool written =
        ::write(fd, text.data(), text.size()) ==
            static_cast<ssize_t>(text.size()) &&
        ::fsync(fd) == 0;
    written = ::close(fd) == 0 && written;
    if (!written || ::rename(temporary.c_str(), path().c_str()) != 0) {
      ::unlink(temporary.c_str());
      return false;
    }
    const int directory = ::open(_directory.c_str(), O_RDONLY | O_CLOEXEC);
    if (directory < 0) {
      return false;
    }
    const bool synced = ::fsync(directory) == 0;
    ::close(directory);
    return synced;
  }

  /** Removes the record; nothing when there is none. */
  void remove() const {
    if (usable()) {
      ::unlink(path().c_str());
    }
  }

private:
  std::string _directory;
};

/**
 An attempt in flight: armed once its record is on disk, right before the
 compatibility device is created, and settled by the first frame presented
 on that device, which removes the record. The process has one (process());
 a process that never arms it (every device with a core adapter) only ever
 reads one atomic flag per presented frame.
 */
class RNSkCompatibilityAttempt {
public:
  static RNSkCompatibilityAttempt &process() {
    static auto *attempt = new RNSkCompatibilityAttempt();
    return *attempt;
  }

  void arm(const RNSkCompatibilityGuardFile &file) {
    {
      std::lock_guard<std::mutex> lock(_mutex);
      _path = file.path();
    }
    _armed.store(true, std::memory_order_release);
  }

  /** A frame was presented: the first one settles an armed attempt. */
  bool notePresented() {
    if (!_armed.load(std::memory_order_acquire) ||
        !_armed.exchange(false, std::memory_order_acq_rel)) {
      return false;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    ::unlink(_path.c_str());
    return true;
  }

  bool armed() const { return _armed.load(std::memory_order_acquire); }

private:
  std::atomic<bool> _armed{false};
  std::mutex _mutex;
  std::string _path;
};

} // namespace RNSkia
