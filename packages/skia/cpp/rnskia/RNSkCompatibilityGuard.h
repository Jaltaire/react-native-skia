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

#include "RNSkCompatibilityConfig.h"

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
 The process's attempt at the compatibility level, counted only while the
 driver is exercised for the first time, in two windows:
 - creation: from just before the device is requested until its Graphite
   context exists;
 - first work: from just before the first work is submitted to the device
   (a window frame, an offscreen surface, or a readback) until that work is
   known finished (a window frame's present, and the GPU's report that the
   work is done).
 Each window records one more unfinished attempt than the process found, and
 puts the record back when it closes; a finished first work removes it. A
 process that never draws neither counts nor resets. A process that never
 begins (every device with a core adapter) only reads one atomic value per
 submission.
 */
class RNSkCompatibilityAttempt {
public:
  enum class Phase {
    Unused,
    Creating,
    AwaitingFirstWork,
    FirstWork,
    Finished,
  };

  static RNSkCompatibilityAttempt &process() {
    static auto *attempt = new RNSkCompatibilityAttempt();
    return *attempt;
  }

  Phase phase() const { return _phase.load(std::memory_order_acquire); }

  /**
   Opens the creation window: records `counted` (one more unfinished attempt
   than the process found). False when it cannot be recorded.
   */
  bool beginCreation(const RNSkCompatibilityGuardFile &file,
                     const RNSkCompatibilityAttempts &counted) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (phase() != Phase::Unused) {
      return false;
    }
    _file = file;
    _counted = counted;
    if (!_file->write(_counted)) {
      _phase.store(Phase::Finished, std::memory_order_release);
      return false;
    }
    _phase.store(Phase::Creating, std::memory_order_release);
    return true;
  }

  /** Closes the creation window: the device and its context exist, or not. */
  void endCreation(bool created) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (phase() != Phase::Creating) {
      return;
    }
    restoreLocked();
    _phase.store(created ? Phase::AwaitingFirstWork : Phase::Finished,
                 std::memory_order_release);
  }

  /**
   Opens the first-work window before the process's first submission to the
   device. `presents` says whether that work is a window frame, which also
   has to be presented. True when this submission is the one watched.
   */
  bool beginFirstWork(bool presents) {
    if (phase() != Phase::AwaitingFirstWork) {
      return false;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    if (phase() != Phase::AwaitingFirstWork) {
      return false;
    }
    if (!_file->write(_counted)) {
      _phase.store(Phase::Finished, std::memory_order_release);
      return false;
    }
    _presents = presents;
    _workDone = false;
    _presented = false;
    _phase.store(Phase::FirstWork, std::memory_order_release);
    return true;
  }

  /**
   The watched submission carried no work after all (every recording was
   rejected): the record is put back, and the next submission is watched.
   */
  void cancelFirstWork() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (phase() != Phase::FirstWork) {
      return;
    }
    restoreLocked();
    _phase.store(Phase::AwaitingFirstWork, std::memory_order_release);
  }

  /** The GPU finished the watched work. True when that settles the attempt. */
  bool noteWorkDone() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (phase() != Phase::FirstWork) {
      return false;
    }
    _workDone = true;
    return settleLocked();
  }

  /** The watched window frame was presented. True when that settles it. */
  bool notePresented() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (phase() != Phase::FirstWork) {
      return false;
    }
    _presented = true;
    return settleLocked();
  }

private:
  void restoreLocked() {
    const int before = _counted.unfinished - 1;
    if (before <= 0) {
      _file->remove();
    } else if (!_file->write({_counted.appVersion, before})) {
      _file->remove();
    }
  }

  bool settleLocked() {
    if (!_workDone || (_presents && !_presented)) {
      return false;
    }
    _file->remove();
    _phase.store(Phase::Finished, std::memory_order_release);
    return true;
  }

  std::atomic<Phase> _phase{Phase::Unused};
  std::mutex _mutex;
  std::optional<RNSkCompatibilityGuardFile> _file;
  RNSkCompatibilityAttempts _counted;
  bool _presents = false;
  bool _workDone = false;
  bool _presented = false;
};

/**
 What the guard decided for a device about to be requested (see
 consultCompatibilityGuard): the record's file and the attempt to count when
 the guard is consulted, nothing when the process's attempt is already under
 way or done (a device that replaces a lost one).
 */
struct RNSkCompatibilityGuardState {
  std::optional<RNSkCompatibilityGuardFile> file;
  std::optional<RNSkCompatibilityDecision> decision;
};

/**
 The gate on the compatibility level, asked only on a device without a core
 adapter (see chooseAdapter): why the level is not tried, or nullopt when it
 may be. Only the process's first device consults the guard; a later one (a
 device that replaces a lost one) is allowed the level and touches no
 record, so that one session cannot use up the attempts the guard allows.
 */
inline std::optional<std::string>
consultCompatibilityGuard(const RNSkCompatibilityAttempt &attempt,
                          const RNSkCompatibilityConfig &config,
                          RNSkCompatibilityGuardState &state) {
  if (auto refusal = compatibilityRefusal(config)) {
    return refusal;
  }
  if (attempt.phase() != RNSkCompatibilityAttempt::Phase::Unused) {
    return std::nullopt;
  }
  state.file.emplace(config.guardDirectory);
  state.decision = decideCompatibilityAttempt(
      state.file->read(), config.appVersion, state.file->usable());
  if (state.decision->plan != RNSkCompatibilityPlan::Attempt) {
    return compatibilityStopReason(state.decision->plan);
  }
  return std::nullopt;
}

} // namespace RNSkia
