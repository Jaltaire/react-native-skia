#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
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

/**
 Why the guard does not let the level be tried on version `appVersion`: the
 end of the sentence compatibilityRefusedMessage begins.
 */
inline std::string compatibilityStopReason(RNSkCompatibilityPlan plan,
                                           const std::string &appVersion) {
  switch (plan) {
  case RNSkCompatibilityPlan::StopUnfinished:
    return "the last " +
           std::to_string(kRNSkCompatibilityUnfinishedLaunchesToStop) +
           " launches of this app version (" + appVersion +
           ") that tried it ended before their first work on it finished. "
           "It is tried again when the app's version changes.";
  case RNSkCompatibilityPlan::StopUnrecorded:
    return "Skia could not record the attempt, so a crash on that level "
           "could not be caught on the next launch. It is tried again on a "
           "launch that can record it.";
  case RNSkCompatibilityPlan::Attempt:
    return "";
  }
  return "";
}

/**
 The record's text: the count on the first line, then the app version as
 the rest of the file, byte for byte, so that any version string (with line
 breaks, empty, or of any length) reads back whole.
 */
inline std::string
formatCompatibilityAttempts(const RNSkCompatibilityAttempts &attempts) {
  return std::to_string(attempts.unfinished) + "\n" + attempts.appVersion;
}

/**
 The record from its file's text; nullopt when the text is not one. The count
 is a plain decimal of at most nine digits, without a sign or a leading zero.
 */
inline std::optional<RNSkCompatibilityAttempts>
parseCompatibilityAttempts(const std::string &text) {
  const auto countEnd = text.find('\n');
  if (countEnd == std::string::npos || countEnd == 0 || countEnd > 9) {
    return std::nullopt;
  }
  const auto count = text.substr(0, countEnd);
  if (count.find_first_not_of("0123456789") != std::string::npos ||
      (count.size() > 1 && count[0] == '0')) {
    return std::nullopt;
  }
  return RNSkCompatibilityAttempts{text.substr(countEnd + 1),
                                   std::stoi(count)};
}

/**
 The record's file, in a directory the platform keeps out of backups (see
 RNSkPlatformContext::getCompatibilityConfig). It is written to a temporary
 file of its own (named after the process and the write), flushed, and
 renamed over the record, so a crash right after the write leaves either the
 old record or the new one, never a partial one. Two processes of one app
 that write at once each rename a whole record of their own: the record is
 then the last one renamed.
 */
class RNSkCompatibilityGuardFile {
public:
  explicit RNSkCompatibilityGuardFile(std::string directory)
      : _directory(std::move(directory)) {}

  bool usable() const { return !_directory.empty(); }

  const std::string &directory() const { return _directory; }

  std::string path() const {
    return _directory + "/rnskia-compatibility-attempts";
  }

  /**
   The record, read whole, or nullopt when there is none or it is not one
   (see readable for a record that exists and cannot be read).
   */
  std::optional<RNSkCompatibilityAttempts> read() const {
    if (!usable()) {
      return std::nullopt;
    }
    FILE *file = std::fopen(path().c_str(), "rb");
    if (file == nullptr) {
      return std::nullopt;
    }
    std::string text;
    char buffer[4096];
    size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
      text.append(buffer, count);
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) {
      return std::nullopt;
    }
    return parseCompatibilityAttempts(text);
  }

  /**
   Whether the record can be read: there is none, or it opens. A record that
   exists and cannot be read could hide unfinished attempts, so the guard
   then does not let the level be tried.
   */
  bool readable() const {
    if (!usable()) {
      return false;
    }
    const int fd = ::open(path().c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      return errno == ENOENT;
    }
    ::close(fd);
    return true;
  }

  /** Records `attempts` durably; false when any step fails. */
  bool write(const RNSkCompatibilityAttempts &attempts) const {
    if (!usable()) {
      return false;
    }
    static std::atomic<uint64_t> writes{0};
    const auto temporary = path() + ".tmp." + std::to_string(::getpid()) +
                           "." +
                           std::to_string(writes.fetch_add(
                               1, std::memory_order_relaxed));
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
 driver is exercised for the first time, in three windows:
 - creation: from just before the device is requested until its Graphite
   context exists;
 - precompile: from just before the process's first device starts compiling
   pipelines on a thread of its own (a precompile, or a warmup) until that
   thread is done;
 - first work: from just before the first work is submitted to the device
   (a window frame, an offscreen surface, or a readback) until that work is
   known finished (a window frame's present, and the GPU's report that the
   work is done).
 The windows may overlap. While any is open the record holds one more
 unfinished attempt than the process found; once the last one closes the
 count is put back. A finished first work removes the record, once no
 precompile is running any more. A process that never draws neither counts
 nor resets. A process that never begins (every device with a core adapter)
 only reads one atomic value per submission and per precompile.
 */
class RNSkCompatibilityAttempt {
public:
  enum class Phase {
    Unused,
    Creating,
    AwaitingFirstWork,
    FirstWork,
    // The first work finished; the attempt settles once the precompile that
    // is still running ends.
    FirstWorkFinished,
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
      _file->remove();
      _phase.store(Phase::Finished, std::memory_order_release);
      return false;
    }
    _phase.store(Phase::Creating, std::memory_order_release);
    return true;
  }

  /**
   Closes the creation window: the device and its context exist, or their
   creation failed cleanly (an error, not a crash), which does not count.
   */
  void endCreation(bool created) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (phase() != Phase::Creating) {
      return;
    }
    if (!created) {
      restoreLocked();
      _phase.store(Phase::Finished, std::memory_order_release);
      return;
    }
    _phase.store(Phase::AwaitingFirstWork, std::memory_order_release);
    if (!_precompiling) {
      restoreLocked();
    }
  }

  /**
   Opens the precompile window before the process's first device starts
   compiling pipelines on a thread of its own, from the creation window on.
   True when this precompile is the one counted, and then endPrecompile()
   must follow once its thread is done; any other (a second one, one on a
   device that replaces a lost one, or one on a device with a core adapter)
   is not.
   */
  bool beginPrecompile() {
    const auto current = phase();
    if (current != Phase::Creating && current != Phase::AwaitingFirstWork &&
        current != Phase::FirstWork) {
      return false;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    if (_precompileBegun || (phase() != Phase::Creating &&
                             phase() != Phase::AwaitingFirstWork &&
                             phase() != Phase::FirstWork)) {
      return false;
    }
    if (phase() == Phase::AwaitingFirstWork && !_file->write(_counted)) {
      _file->remove();
      _phase.store(Phase::Finished, std::memory_order_release);
      return false;
    }
    _precompileBegun = true;
    _precompiling = true;
    return true;
  }

  /**
   Closes the precompile window: its thread is done, whether it compiled
   every pipeline or stopped early. True when that settles the attempt (its
   first work had already finished).
   */
  bool endPrecompile() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_precompiling) {
      return false;
    }
    _precompiling = false;
    if (phase() == Phase::AwaitingFirstWork) {
      restoreLocked();
      return false;
    }
    if (phase() == Phase::FirstWorkFinished) {
      _file->remove();
      _phase.store(Phase::Finished, std::memory_order_release);
      return true;
    }
    return false;
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
    if (!_precompiling && !_file->write(_counted)) {
      _file->remove();
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
   rejected): the count is put back unless a precompile is running, and the
   next submission is watched.
   */
  void cancelFirstWork() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (phase() != Phase::FirstWork) {
      return;
    }
    if (!_precompiling) {
      restoreLocked();
    }
    _phase.store(Phase::AwaitingFirstWork, std::memory_order_release);
  }

  /**
   The GPU's report on the watched work: `succeeded` only when it finished
   and the device was not lost. True when that settles the attempt; a
   failure leaves it counted.
   */
  bool noteWorkDone(bool succeeded) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (phase() != Phase::FirstWork) {
      return false;
    }
    if (!succeeded) {
      _phase.store(Phase::Finished, std::memory_order_release);
      return false;
    }
    _workDone = true;
    return settleLocked();
  }

  /**
   The watched window frame's present: `succeeded` only when it succeeded
   and the device was not lost. True when that settles the attempt; a
   failure leaves it counted.
   */
  bool notePresented(bool succeeded) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (phase() != Phase::FirstWork) {
      return false;
    }
    if (!succeeded) {
      _phase.store(Phase::Finished, std::memory_order_release);
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
    if (_precompiling) {
      _phase.store(Phase::FirstWorkFinished, std::memory_order_release);
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
  bool _precompileBegun = false;
  bool _precompiling = false;
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
 may be. `resolveConfig` gives the app's configuration and is called here
 only, so that a device with a core adapter never reads the app's settings,
 directories, or version. Only the process's first device consults the
 guard; a later one (a device that replaces a lost one) is allowed the level
 and touches no record, so that one session cannot use up the attempts the
 guard allows.
 */
template <typename ResolveConfig>
std::optional<std::string>
consultCompatibilityGuard(const RNSkCompatibilityAttempt &attempt,
                          ResolveConfig &&resolveConfig,
                          RNSkCompatibilityGuardState &state) {
  const RNSkCompatibilityConfig config = resolveConfig();
  if (auto refusal = compatibilityRefusal(config)) {
    return refusal;
  }
  if (attempt.phase() != RNSkCompatibilityAttempt::Phase::Unused) {
    return std::nullopt;
  }
  state.file.emplace(config.guardDirectory);
  state.decision =
      decideCompatibilityAttempt(state.file->read(), config.appVersion,
                                 state.file->readable());
  if (state.decision->plan != RNSkCompatibilityPlan::Attempt) {
    return compatibilityStopReason(state.decision->plan, config.appVersion);
  }
  return std::nullopt;
}

} // namespace RNSkia
