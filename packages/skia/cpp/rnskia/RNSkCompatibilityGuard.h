#pragma once

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "RNSkCompatibilityConfig.h"

namespace RNSkia {

/**
 How many launches in a row on one app version may end inside a counted window
 on a compatibility device (see RNSkCompatibilityAttempt) before Skia stops
 trying that level on that version. Two rather than one: a driver fault ends every attempt the
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
  // Not on this launch: a record exists but the file system could not read
  // it, and counting from nothing could lose a count that stops the level.
  StopUnreadable,
};

struct RNSkCompatibilityDecision {
  RNSkCompatibilityPlan plan;
  // What to record before the attempt (Attempt only).
  RNSkCompatibilityAttempts record;
};

/**
 What reading the record found: the record, or nothing (there is none, or its
 content is not a record), and whether the file system failed to read a
 record that exists.
 */
struct RNSkCompatibilityRecordRead {
  std::optional<RNSkCompatibilityAttempts> record;
  bool failed = false;
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
 The same from what reading the record found: a record the file system could
 not read stops the level for this launch and is left as it is, so that a
 passing failure cannot lose a count, a count of two included.
 */
inline RNSkCompatibilityDecision
decideCompatibilityAttempt(const RNSkCompatibilityRecordRead &read,
                           const std::string &appVersion, bool canRecord) {
  if (canRecord && read.failed) {
    return {RNSkCompatibilityPlan::StopUnreadable, {}};
  }
  return decideCompatibilityAttempt(read.record, appVersion, canRecord);
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
  case RNSkCompatibilityPlan::StopUnreadable:
    return "Skia could not read its record of earlier attempts, so it could "
           "not tell whether they stopped the level. It is tried again on a "
           "launch that can read it.";
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
 renamed over the record, and the directory is flushed after a rename or an
 unlink, so a crash or a power loss leaves either the old record or the new
 one, never a partial one.

 Two processes of one app that load this library each decide from what they
 read, so the second can read the first's raised count as its own found
 count: two processes ended inside their windows then record two for one
 launch, and the level stops after one such launch rather than two. That is
 the conservative direction (the level stops earlier, never later). One
 process's consult can also remove the temporary file of a write the other
 is making (see removeStaleTemporaryFiles): that write fails, so that
 process does not try the level on that launch, or stops counting and leaves
 the record as it was. An app whose library loads in one process only, as
 Android starts a process per process name and React Native runs in the
 app's main one, never meets either.
 */
class RNSkCompatibilityGuardFile {
public:
  /** The largest record read: a longer file is not a record. */
  static constexpr size_t kMaxRecordBytes = 1 << 20;

  /**
   The record in `directory`. `beforeWrite`, when given, runs at the start of
   every write, on the writing thread: the host test uses it to see the
   attempt's state while one of its writes is under way.
   */
  explicit RNSkCompatibilityGuardFile(std::string directory,
                                      std::function<void()> beforeWrite = {})
      : _directory(std::move(directory)),
        _beforeWrite(std::move(beforeWrite)) {}

  bool usable() const { return !_directory.empty(); }

  const std::string &directory() const { return _directory; }

  /** The record's file name in its directory. */
  static constexpr const char *kRecordName = "rnskia-compatibility-attempts";

  std::string path() const { return _directory + "/" + kRecordName; }

  /**
   The record, read whole, or nullopt when there is none, it is not one, it
   is longer than kMaxRecordBytes, or it cannot be read.
   */
  std::optional<RNSkCompatibilityAttempts> read() const {
    return readRecord().record;
  }

  /**
   What reading the record finds. Content that is not a record, or longer
   than kMaxRecordBytes, reads as no record: its app version cannot be known,
   so it cannot stop the level, and the guard replaces it with its own, so a
   damaged record stops nothing for good. A record the file system fails to
   open (other than its absence) or to read is reported as failed, and the
   guard leaves it and does not try the level on that launch (see
   decideCompatibilityAttempt).
   */
  RNSkCompatibilityRecordRead readRecord() const {
    if (!usable()) {
      return {};
    }
    FILE *file = std::fopen(path().c_str(), "rb");
    if (file == nullptr) {
      return {std::nullopt, errno != ENOENT};
    }
    std::string text;
    char buffer[4096];
    size_t count = 0;
    bool tooLong = false;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
      text.append(buffer, count);
      if (text.size() > kMaxRecordBytes) {
        tooLong = true;
        break;
      }
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed) {
      return {std::nullopt, true};
    }
    if (tooLong) {
      return {};
    }
    return {parseCompatibilityAttempts(text), false};
  }

  /**
   Removes the temporary files that other processes' interrupted writes left
   beside the record (see write): a process that dies inside a write leaves
   its own for good. The guard calls it when it is consulted, which only a
   device without a core adapter does.
   */
  void removeStaleTemporaryFiles() const {
    if (!usable()) {
      return;
    }
    DIR *directory = ::opendir(_directory.c_str());
    if (directory == nullptr) {
      return;
    }
    const std::string prefix = std::string(kRecordName) + ".tmp.";
    const std::string own = prefix + std::to_string(::getpid()) + ".";
    std::vector<std::string> stale;
    while (const auto *entry = ::readdir(directory)) {
      const std::string name = entry->d_name;
      if (name.rfind(prefix, 0) == 0 && name.rfind(own, 0) != 0) {
        stale.push_back(name);
      }
    }
    ::closedir(directory);
    for (const auto &name : stale) {
      ::unlink((_directory + "/" + name).c_str());
    }
  }

  /** Records `attempts` durably; false when any step fails. */
  bool write(const RNSkCompatibilityAttempts &attempts) const {
    if (!usable()) {
      return false;
    }
    if (_beforeWrite) {
      _beforeWrite();
    }
    operations().fetch_add(1, std::memory_order_relaxed);
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
    return syncDirectory();
  }

  /**
   Removes the record durably; true when there is none afterwards (also when
   there was none).
   */
  bool remove() const {
    if (!usable()) {
      return false;
    }
    operations().fetch_add(1, std::memory_order_relaxed);
    if (::unlink(path().c_str()) != 0) {
      return errno == ENOENT;
    }
    return syncDirectory();
  }

  /**
   How many durable writes and removals every record file of the process
   has made so far, for the tests that bound them.
   */
  static uint64_t durableOperations() {
    return operations().load(std::memory_order_relaxed);
  }

private:
  bool syncDirectory() const {
    const int directory = ::open(_directory.c_str(), O_RDONLY | O_CLOEXEC);
    if (directory < 0) {
      return false;
    }
    const bool synced = ::fsync(directory) == 0;
    ::close(directory);
    return synced;
  }

  static std::atomic<uint64_t> &operations() {
    static std::atomic<uint64_t> count{0};
    return count;
  }

  std::string _directory;
  std::function<void()> _beforeWrite;
};

/**
 What a submission to the device carries, as far as the attempt's first work
 is concerned (see RNSkCompatibilityAttempt::watch).
 */
enum class RNSkCompatibilityWork {
  // A window frame's draws, presented after the submission.
  WindowFrame,
  // An offscreen surface's draws.
  OffscreenDraw,
  // Anything else: a texture upload, a readback, a surface's first clear.
  Other,
};

/**
 The process's attempt at the compatibility level. It is counted while the
 driver is exercised for the first time, in three windows that may overlap:
 - creation: from just before the device is requested until its Graphite
   context exists (or its creation failed cleanly);
 - precompile: from just before the process's first device starts compiling
   stored pipelines on a thread of its own until that thread is done;
 - first work: from the first of the first window surface made (before Dawn
   configures it, which creates the swapchain) and the first draw recording
   snapped (where Graphite starts compiling its pipelines), until a watched
   window frame has finished: submitted, presented on a device that was not
   lost, and reported done by the GPU. An offscreen surface's draws reported
   done by the GPU close the window only while no window surface has been
   made and no window frame snapped, and never end the attempt: the first
   window surface made or window frame snapped after them opens the window
   again, so that the first window's swapchain, first image, and first frame
   are counted whatever offscreen work finished before them. An upload, a
   readback, or a surface's first clear is never watched.

 The record holds the count this launch found (F) while no window is open,
 F + 1 while any is, and nothing once the attempt has settled, which takes a
 finished window frame and every counted window having closed: a launch
 that dies in what is left of the precompile after its first frame counts,
 so a driver that faults there at every launch is stopped too. A process
 that only ever draws offscreen never settles: its record keeps the count
 the launch found, nothing on a launch that found none, until the app's
 version changes. The file is written only when the value it should hold
 changes.

 | State             | Entered by                                   | Record  | A death here: the next launch finds                    |
 |-------------------|----------------------------------------------|---------|--------------------------------------------------------|
 | Unused            | (start; a core device never leaves it)       | as read | what this one found; nothing was touched               |
 | Creating          | beginCreation                                | F + 1   | F + 1: counted                                         |
 | Created           | endCreation(true), no precompile running     | F       | F: not counted (a launch that never draws)             |
 | Created + P       | beginPrecompile, or endCreation(true) with P | F + 1   | F + 1: counted (a fault while compiling stored ones)   |
 | FirstWork         | openFirstWork, or watch of a draw            | F + 1   | F + 1: counted (swapchain, first image, first compile) |
 | OffscreenFinished | the watched offscreen draw finished, with no | F       | F: not counted (no window open)                        |
 |                   | window surface made nor window frame snapped |         |                                                        |
 | OffscreenFinished | the same while P runs                        | F + 1   | F + 1: counted (a fault in what is left of P)          |
 |   + P             |                                              |         |                                                        |
 | FirstWork again   | a window surface made, or a window frame     | F + 1   | F + 1: counted (swapchain, first image, first frame)   |
 |                   | snapped or submitted, in OffscreenFinished   |         |                                                        |
 | FirstWorkFinished | the watched window frame finished while P    | F + 1   | F + 1: counted (a fault in what is left of P)          |
 |                   | runs                                         |         |                                                        |
 | Settled           | the watched window frame finished, or P      | none    | nothing: the attempt succeeded                         |
 |                   | ended after it                               |         |                                                        |
 | Failed            | a watched draw failed, or P ended on a lost  | F + 1   | F + 1: counted                                         |
 |                   | device                                       |         |                                                        |
 | NotTried          | endCreation(false), or beginCreation failed  | F, or   | F: the device was never used (F + 1 when the count     |
 |                   |                                              | F + 1   | could not be put back, the conservative side)          |
 | Stopped           | a write failed after creation                | as left | F or F + 1, never less than F                          |

 Settled, Failed, NotTried, and Stopped are the phase Finished: the attempt
 watches nothing more and touches the record no more: when one is reached
 while P runs, P's end writes nothing. Within FirstWork, a submission of a
 window frame or an offscreen draw becomes the watched one when none is
 (watch); a watched submission that carried no work (every recording
 rejected) stops being watched (unwatch) and the next one is watched, with
 no write: a canvas whose batches are all rejected keeps the window open,
 raises nothing more, and settles nothing. An offscreen draw that finishes
 after a window surface was made or a window frame snapped leaves the window
 open, and from then on only a window frame is watched; in OffscreenFinished
 an offscreen draw opens nothing and is not watched. A window frame
 submitted while an offscreen draw is the watched work is not watched
 itself; the next window frame is. So a canvas that draws exactly one frame
 in that gap leaves a healthy launch counted until a later frame settles it:
 the over-counting side. A device lost in FirstWork with nothing watched
 leaves the window open, so the launch counts when it ends, as in Failed.

 The first-work window is published (the phase becomes FirstWork) only once
 F + 1 is on disk. The check that lets a thread go on without the mutex sees
 AwaitingFirstWork until then, so a thread that finds the window being opened
 waits for the opening write instead of drawing ahead of its count.

 Not counted, by design: the buffers and textures Graphite creates while a
 drawing is recorded, before its snap opens the window; an upload or a
 readback before the first drawing; and a second device made alongside the
 first.

 The settle's removal runs on the thread that delivers the last of the
 window frame's present and the GPU's report: the main thread when the
 report came first, once per process. It stays there: by then no thread that
 writes the record is still running, and its order is safe on any thread,
 as the attempt is Finished before the removal and nothing writes after it.

 Every transition is taken under the attempt's mutex. A process that never
 begins (every device with a core adapter) reads one atomic value per
 submission, per draw snapped, per window surface made, and per precompile;
 one whose attempt has settled or stopped reads the same.
 */
class RNSkCompatibilityAttempt {
public:
  enum class Phase {
    Unused,
    Creating,
    // Created, with no draw yet: the record holds F, or F + 1 while a
    // precompile runs.
    AwaitingFirstWork,
    FirstWork,
    // The watched offscreen draw finished before any window surface was
    // made or window frame snapped: the record holds F, or F + 1 while a
    // precompile runs, and only a window surface or a window frame opens
    // the first-work window again.
    OffscreenFinished,
    // The watched window frame finished while the precompile still runs.
    FirstWorkFinished,
    Finished,
  };

  static RNSkCompatibilityAttempt &process() {
    static auto *attempt = new RNSkCompatibilityAttempt();
    return *attempt;
  }

  Phase phase() const { return _phase.load(std::memory_order_acquire); }

  /**
   Opens the creation window: records one more unfinished attempt than the
   process found (`counted`, see decideCompatibilityAttempt). False when it
   cannot be recorded; the level is then not tried, and the record is put
   back to what the launch found when that can be written.
   */
  bool beginCreation(const RNSkCompatibilityGuardFile &file,
                     const RNSkCompatibilityAttempts &counted) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (phase() != Phase::Unused) {
      return false;
    }
    _file = file;
    _appVersion = counted.appVersion;
    _found = counted.unfinished - 1;
    _phase.store(Phase::Creating, std::memory_order_release);
    if (!storeLocked()) {
      _phase.store(Phase::Finished, std::memory_order_release);
      storeValueLocked(_found);
      return false;
    }
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
      _phase.store(Phase::Finished, std::memory_order_release);
      storeValueLocked(_found);
      return;
    }
    _phase.store(Phase::AwaitingFirstWork, std::memory_order_release);
    storeOrStopLocked();
  }

  /**
   Opens the precompile window before the process's first device starts
   compiling stored pipelines on a thread of its own, from the creation
   window on. True when this precompile is the one counted, and then
   endPrecompile() must follow once its thread is done, or if it cannot be
   started; any other (a second one, or one on a device with a core adapter)
   is not.
   */
  bool beginPrecompile() {
    if (!precompileMayBegin(phase())) {
      return false;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    if (_precompileBegun || !precompileMayBegin(phase())) {
      return false;
    }
    _precompileBegun = true;
    _precompiling = true;
    return storeOrStopLocked();
  }

  /**
   Closes the precompile window: its thread is done, whether it compiled
   every pipeline or stopped early, or it could not be started.
   `deviceLost` says the device was lost meanwhile, which fails the attempt
   as a lost device fails a first work. True when that settles the attempt
   (its first work had already finished).
   */
  bool endPrecompile(bool deviceLost = false) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_precompiling) {
      return false;
    }
    _precompiling = false;
    if (phase() == Phase::Finished) {
      return false;
    }
    if (deviceLost) {
      _phase.store(Phase::Finished, std::memory_order_release);
      return false;
    }
    if (phase() == Phase::FirstWorkFinished) {
      _phase.store(Phase::Finished, std::memory_order_release);
      return storeValueLocked(0);
    }
    storeOrStopLocked();
    return false;
  }

  /**
   Opens the first-work window before a window surface is made (`work`
   WindowFrame) or a draw recording is snapped (`work` the kind of the draw):
   the first of them opens it, and once an offscreen draw has finished only a
   window surface or a window frame opens it again. Other work opens
   nothing. True when this call opened it.
   */
  bool openFirstWork(RNSkCompatibilityWork work) {
    if (!mayOpen(phase(), work)) {
      return false;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    return openFirstWorkLocked(work);
  }

  /**
   A submission to the device, before it is made. A window frame or an
   offscreen draw becomes the watched work when none is, opening the window
   first if nothing has; other work is never watched. True when this
   submission is the watched one: its caller then reports its outcome
   (noteWorkDone, and notePresented for a window frame), or unwatch() when
   it carried no work.
   */
  bool watch(RNSkCompatibilityWork work) {
    if (work == RNSkCompatibilityWork::Other) {
      return false;
    }
    const auto current = phase();
    if (current != Phase::FirstWork && !mayOpen(current, work)) {
      return false;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    openFirstWorkLocked(work);
    if (phase() != Phase::FirstWork || _watching ||
        (work == RNSkCompatibilityWork::OffscreenDraw && _offscreenFinished)) {
      return false;
    }
    _watching = true;
    _watchedWork = work;
    _workDone = false;
    _presented = false;
    return true;
  }

  /**
   The watched submission carried no work after all (every recording was
   rejected), or its window frame was dropped before its present: the next
   draw is watched instead. The window stays open and nothing is written.
   */
  void unwatch() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (phase() == Phase::FirstWork) {
      _watching = false;
    }
  }

  /**
   The GPU's report on the watched work: `succeeded` only when it finished
   and the device was not lost. True when that settles the attempt; a
   failure leaves it counted.
   */
  bool noteWorkDone(bool succeeded) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (phase() != Phase::FirstWork || !_watching) {
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
   The watched window frame's present: `succeeded` only when it was issued
   on a device that was not lost (Present() reports an error only for a
   surface that is not configured; a failed swapchain present loses the
   device or is reported by the GPU's report on the frame's work). True when
   that settles the attempt; a failure leaves it counted.
   */
  bool notePresented(bool succeeded) {
    std::lock_guard<std::mutex> lock(_mutex);
    if (phase() != Phase::FirstWork || !_watching ||
        _watchedWork != RNSkCompatibilityWork::WindowFrame) {
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
  static bool precompileMayBegin(Phase phase) {
    return phase == Phase::Creating || phase == Phase::AwaitingFirstWork ||
           phase == Phase::FirstWork || phase == Phase::OffscreenFinished;
  }

  // Whether `work` may open the first-work window, or mark the first window
  // seen, in `phase`: read without the mutex, so that a process past its
  // first work (and every device with a core adapter) only loads the phase.
  bool mayOpen(Phase phase, RNSkCompatibilityWork work) const {
    if (work == RNSkCompatibilityWork::Other) {
      return false;
    }
    if (phase == Phase::AwaitingFirstWork) {
      return true;
    }
    return work == RNSkCompatibilityWork::WindowFrame &&
           (phase == Phase::OffscreenFinished ||
            (phase == Phase::FirstWork &&
             !_windowSeen.load(std::memory_order_acquire)));
  }

  // Opens the first-work window for `work` from AwaitingFirstWork, or for a
  // window from OffscreenFinished; marks a window seen in FirstWork. The
  // window is published only once F + 1 is on disk.
  bool openFirstWorkLocked(RNSkCompatibilityWork work) {
    const bool window = work == RNSkCompatibilityWork::WindowFrame;
    if (window && phase() == Phase::FirstWork) {
      _windowSeen.store(true, std::memory_order_release);
      return false;
    }
    if (!mayOpen(phase(), work)) {
      return false;
    }
    _watching = false;
    if (!storeValueLocked(_found + 1)) {
      _phase.store(Phase::Finished, std::memory_order_release);
      return false;
    }
    if (window) {
      _windowSeen.store(true, std::memory_order_release);
    }
    _phase.store(Phase::FirstWork, std::memory_order_release);
    return true;
  }

  bool settleLocked() {
    if (!_workDone || (_watchedWork == RNSkCompatibilityWork::WindowFrame &&
                       !_presented)) {
      return false;
    }
    if (_watchedWork == RNSkCompatibilityWork::OffscreenDraw) {
      // Not the end of the attempt: a window surface may still come, and its
      // swapchain, first image, and first frame are counted when it does.
      _watching = false;
      _offscreenFinished = true;
      if (!_windowSeen.load(std::memory_order_acquire)) {
        _phase.store(Phase::OffscreenFinished, std::memory_order_release);
        storeOrStopLocked();
      }
      return false;
    }
    if (_precompiling) {
      _phase.store(Phase::FirstWorkFinished, std::memory_order_release);
      storeOrStopLocked();
      return false;
    }
    _phase.store(Phase::Finished, std::memory_order_release);
    return storeValueLocked(0);
  }

  // What the record should hold now, by the table above.
  int targetLocked() const {
    const bool windowOpen = phase() == Phase::Creating ||
                            phase() == Phase::FirstWork || _precompiling;
    return windowOpen ? _found + 1 : _found;
  }

  // Brings the record to the value the table gives; false when that fails.
  bool storeLocked() { return storeValueLocked(targetLocked()); }

  // Same, after creation: a failure stops the attempt, leaving the record as
  // it is (F or F + 1, never less than F).
  bool storeOrStopLocked() {
    if (storeLocked()) {
      return true;
    }
    _phase.store(Phase::Finished, std::memory_order_release);
    return false;
  }

  bool storeValueLocked(int value) {
    if (value == _stored) {
      return true;
    }
    const bool stored = value == 0
                            ? _file->remove()
                            : _file->write({_appVersion, value});
    if (stored) {
      _stored = value;
    }
    return stored;
  }

  std::atomic<Phase> _phase{Phase::Unused};
  std::mutex _mutex;
  std::optional<RNSkCompatibilityGuardFile> _file;
  std::string _appVersion;
  int _found = 0;
  // What the record holds as far as this attempt knows: -1 before its first
  // write (the file may hold another version's record), 0 for none.
  int _stored = -1;
  bool _precompileBegun = false;
  bool _precompiling = false;
  bool _watching = false;
  // A window surface was made or a window frame snapped or submitted: an
  // offscreen draw no longer closes the window. Read without the mutex by
  // mayOpen, written under it.
  std::atomic<bool> _windowSeen{false};
  // The watched offscreen draw finished: only a window frame is watched now.
  bool _offscreenFinished = false;
  RNSkCompatibilityWork _watchedWork = RNSkCompatibilityWork::Other;
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
  state.file->removeStaleTemporaryFiles();
  state.decision = decideCompatibilityAttempt(
      state.file->readRecord(), config.appVersion, state.file->usable());
  if (state.decision->plan != RNSkCompatibilityPlan::Attempt) {
    return compatibilityStopReason(state.decision->plan, config.appVersion);
  }
  return std::nullopt;
}

} // namespace RNSkia
