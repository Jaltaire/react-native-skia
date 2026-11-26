#include "RNSkCompatibilityGuard.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

using RNSkia::compatibilityStopReason;
using RNSkia::decideCompatibilityAttempt;
using RNSkia::kRNSkCompatibilityUnfinishedLaunchesToStop;
using RNSkia::parseCompatibilityAttempts;
using RNSkia::RNSkCompatibilityAttempt;
using RNSkia::RNSkCompatibilityAttempts;
using RNSkia::RNSkCompatibilityGuardFile;
using RNSkia::RNSkCompatibilityPlan;

namespace {

int failures = 0;

void expect(bool condition, const char *description) {
  if (!condition) {
    std::fprintf(stderr, "FAILED: %s\n", description);
    failures++;
  }
}

/** A fresh directory of the test's own, removed by its destructor. */
struct Directory {
  std::string path;

  Directory() {
    char name[] = "/tmp/rnskia-guard-test.XXXXXX";
    path = ::mkdtemp(name);
  }

  ~Directory() {
    ::chmod(path.c_str(), S_IRWXU);
    RNSkCompatibilityGuardFile(path).remove();
    ::unlink((RNSkCompatibilityGuardFile(path).path() + ".tmp").c_str());
    ::rmdir(path.c_str());
  }
};

/** Where a simulated launch on a device without a core adapter ends. */
enum class Ending {
  // The process ends inside the device's creation (a driver crash or hang).
  InCreation,
  // The device and its context are created, and nothing is ever submitted.
  NeverDraws,
  // The process ends inside its first work on the device.
  InFirstWork,
  // The first work is a window frame, finished and presented.
  FinishesWindowFrame,
  // The first work is offscreen, finished.
  FinishesOffscreen,
};

/**
 One launch in a process of its own, through the attempt's shipped sequence:
 the guard decides, the creation window opens and closes, and the first-work
 window opens and closes, unless the launch ends inside one. Returns the plan.
 */
RNSkCompatibilityPlan launch(const RNSkCompatibilityGuardFile &file,
                             const std::string &appVersion, Ending ending) {
  auto decision =
      decideCompatibilityAttempt(file.read(), appVersion, file.usable());
  if (decision.plan != RNSkCompatibilityPlan::Attempt) {
    return decision.plan;
  }
  RNSkCompatibilityAttempt attempt;
  if (!attempt.beginCreation(file, decision.record)) {
    return RNSkCompatibilityPlan::StopUnrecorded;
  }
  if (ending == Ending::InCreation) {
    return decision.plan;
  }
  attempt.endCreation(true);
  if (ending == Ending::NeverDraws) {
    return decision.plan;
  }
  const bool window = ending != Ending::FinishesOffscreen;
  if (!attempt.beginFirstWork(window)) {
    return decision.plan;
  }
  if (ending == Ending::InFirstWork) {
    return decision.plan;
  }
  attempt.noteWorkDone();
  if (window) {
    attempt.notePresented();
  }
  return decision.plan;
}

int recorded(const RNSkCompatibilityGuardFile &file) {
  auto record = file.read();
  return record ? record->unfinished : 0;
}

void ruleAttemptsWithoutARecord() {
  auto decision = decideCompatibilityAttempt(std::nullopt, "1.0 (1)", true);
  expect(decision.plan == RNSkCompatibilityPlan::Attempt,
         "the level is tried when nothing is recorded");
  expect(decision.record.appVersion == "1.0 (1)" &&
             decision.record.unfinished == 1,
         "the attempt is recorded as the first unfinished one on this "
         "version");
}

void ruleAttemptsAfterOneUnfinishedLaunch() {
  auto decision = decideCompatibilityAttempt(
      RNSkCompatibilityAttempts{"1.0 (1)", 1}, "1.0 (1)", true);
  expect(decision.plan == RNSkCompatibilityPlan::Attempt,
         "one unfinished launch does not stop the level");
  expect(decision.record.unfinished == 2,
         "the next attempt is recorded as the second in a row");
}

void ruleStopsAfterTwoUnfinishedLaunches() {
  expect(kRNSkCompatibilityUnfinishedLaunchesToStop == 2,
         "two unfinished launches in a row stop the level");
  auto decision = decideCompatibilityAttempt(
      RNSkCompatibilityAttempts{"1.0 (1)", 2}, "1.0 (1)", true);
  expect(decision.plan == RNSkCompatibilityPlan::StopUnfinished,
         "two unfinished launches in a row on this version stop the level");
}

void ruleRetriesOnAnotherAppVersion() {
  auto decision = decideCompatibilityAttempt(
      RNSkCompatibilityAttempts{"1.0 (1)", 2}, "1.1 (2)", true);
  expect(decision.plan == RNSkCompatibilityPlan::Attempt,
         "an update tries the level again, and so does a record restored "
         "from another version");
  expect(decision.record.appVersion == "1.1 (2)" &&
             decision.record.unfinished == 1,
         "the count starts over on the new version");
}

void ruleStopsWhenNothingCanBeRecorded() {
  auto decision = decideCompatibilityAttempt(std::nullopt, "1.0 (1)", false);
  expect(decision.plan == RNSkCompatibilityPlan::StopUnrecorded,
         "without a place to record the attempt the level is not tried");
  expect(compatibilityStopReason(decision.plan).find("could not record") !=
             std::string::npos,
         "and Skia.gpu.error says why");
}

void recordRoundTripsAndRejectsOtherText() {
  auto parsed = parseCompatibilityAttempts("2.4.1 (263300659)\n2\n");
  expect(parsed && parsed->appVersion == "2.4.1 (263300659)" &&
             parsed->unfinished == 2,
         "a record reads back as written");
  expect(!parseCompatibilityAttempts("").has_value(),
         "an empty file is no record");
  expect(!parseCompatibilityAttempts("1.0\n").has_value(),
         "a record without a count is no record");
  expect(!parseCompatibilityAttempts("1.0\ntwo\n").has_value(),
         "a record with a count that is not a number is no record");
  expect(!parseCompatibilityAttempts("1.0\n2\nmore\n").has_value(),
         "a record with more lines is no record");
}

void fileWritesReadsAndRemoves() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  expect(file.usable(), "a directory makes the record usable");
  expect(!file.read().has_value(), "a fresh directory has no record");
  expect(file.write({"1.0 (1)", 1}), "a record is written");
  auto read = file.read();
  expect(read && read->appVersion == "1.0 (1)" && read->unfinished == 1,
         "the written record reads back");
  expect(::access((file.path() + ".tmp").c_str(), F_OK) != 0,
         "the temporary file is renamed away");
  file.remove();
  expect(!file.read().has_value(), "a removed record is gone");
  file.remove();
}

void fileFailsCleanlyWhereItCannotWrite() {
  RNSkCompatibilityGuardFile none("");
  expect(!none.usable(), "no directory makes the record unusable");
  expect(!none.write({"1.0 (1)", 1}), "nothing is written without one");
  RNSkCompatibilityGuardFile missing("/tmp/rnskia-guard-test-missing/a/b");
  expect(!missing.write({"1.0 (1)", 1}),
         "a write into a missing directory fails");
  Directory directory;
  ::chmod(directory.path.c_str(), S_IRUSR | S_IXUSR);
  RNSkCompatibilityGuardFile readOnly(directory.path);
  expect(!readOnly.write({"1.0 (1)", 1}),
         "a write into an unwritable directory fails");
  expect(launch(readOnly, "1.0 (1)", Ending::FinishesWindowFrame) ==
             RNSkCompatibilityPlan::StopUnrecorded,
         "a launch that cannot record its attempt does not try the level");
}

void twoCrashesInCreationStopTheLevel() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  expect(launch(file, "1.0 (1)", Ending::InCreation) ==
             RNSkCompatibilityPlan::Attempt,
         "launch 1 tries the level and ends inside the device's creation");
  expect(recorded(file) == 1, "and counts once");
  expect(launch(file, "1.0 (1)", Ending::InCreation) ==
             RNSkCompatibilityPlan::Attempt,
         "launch 2 tries it again and ends there too");
  expect(launch(file, "1.0 (1)", Ending::FinishesWindowFrame) ==
             RNSkCompatibilityPlan::StopUnfinished,
         "launch 3 does not try the level");
  expect(launch(file, "1.1 (2)", Ending::FinishesWindowFrame) ==
             RNSkCompatibilityPlan::Attempt,
         "the next version tries it again");
  expect(!file.read().has_value(),
         "and its finished first work settles the attempt");
}

void twoCrashesInFirstWorkStopTheLevel() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  launch(file, "1.0 (1)", Ending::InFirstWork);
  expect(recorded(file) == 1,
         "a launch that ends inside its first work counts once");
  launch(file, "1.0 (1)", Ending::InFirstWork);
  expect(launch(file, "1.0 (1)", Ending::FinishesWindowFrame) ==
             RNSkCompatibilityPlan::StopUnfinished,
         "two in a row stop the level");
}

void aCrashInEachWindowStopsTheLevel() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  launch(file, "1.0 (1)", Ending::InCreation);
  launch(file, "1.0 (1)", Ending::InFirstWork);
  expect(launch(file, "1.0 (1)", Ending::FinishesWindowFrame) ==
             RNSkCompatibilityPlan::StopUnfinished,
         "a crash in creation and one in the first work, in a row, stop it");
}

void aProcessThatNeverDrawsNeitherCountsNorResets() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  for (int i = 0; i < 3; i++) {
    launch(file, "1.0 (1)", Ending::NeverDraws);
  }
  expect(!file.read().has_value(),
         "three launches that never draw leave no record: a background or "
         "headless start, a restore without a canvas, a canvas-free screen");
  launch(file, "1.0 (1)", Ending::InFirstWork);
  launch(file, "1.0 (1)", Ending::NeverDraws);
  expect(recorded(file) == 1,
         "one that never draws keeps the count it found");
  launch(file, "1.0 (1)", Ending::NeverDraws);
  launch(file, "1.0 (1)", Ending::InFirstWork);
  expect(launch(file, "1.0 (1)", Ending::FinishesWindowFrame) ==
             RNSkCompatibilityPlan::StopUnfinished,
         "and does not rescue a crashing driver either");
}

void offscreenFirstWorkSettlesWithoutAPresent() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  launch(file, "1.0 (1)", Ending::InFirstWork);
  launch(file, "1.0 (1)", Ending::FinishesOffscreen);
  expect(!file.read().has_value(),
         "an app that only renders offscreen settles when the GPU finishes "
         "its first work");
}

void aWindowFrameNeedsItsPresentToo() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  auto decision = decideCompatibilityAttempt(file.read(), "1.0 (1)", true);
  RNSkCompatibilityAttempt attempt;
  attempt.beginCreation(file, decision.record);
  attempt.endCreation(true);
  expect(attempt.beginFirstWork(true), "the first window frame is watched");
  expect(!attempt.noteWorkDone(),
         "a window frame whose work finished is not settled before its "
         "present");
  expect(recorded(file) == 1, "and stays counted until then");
  expect(attempt.notePresented(), "its present settles it");
  expect(!file.read().has_value(), "and removes the record");
  expect(!attempt.beginFirstWork(false), "later work is not watched");
}

void aBatchWithNoWorkIsNotTheFirstWork() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  file.write({"1.0 (1)", 1});
  auto decision = decideCompatibilityAttempt(file.read(), "1.0 (1)", true);
  RNSkCompatibilityAttempt attempt;
  attempt.beginCreation(file, decision.record);
  attempt.endCreation(true);
  expect(recorded(file) == 1, "a created device puts the count back");
  expect(attempt.beginFirstWork(true), "a submission opens the window");
  expect(recorded(file) == 2, "and counts");
  attempt.cancelFirstWork();
  expect(recorded(file) == 1,
         "a submission whose every recording was rejected puts it back");
  expect(attempt.beginFirstWork(false), "and the next one is watched");
}

void aSuccessResetsTheCount() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  launch(file, "1.0 (1)", Ending::InFirstWork);
  launch(file, "1.0 (1)", Ending::FinishesWindowFrame);
  expect(!file.read().has_value(),
         "a launch whose first work finishes leaves no record");
  expect(launch(file, "1.0 (1)", Ending::InFirstWork) ==
             RNSkCompatibilityPlan::Attempt,
         "an unfinished launch after a good one tries the level");
  expect(launch(file, "1.0 (1)", Ending::FinishesWindowFrame) ==
             RNSkCompatibilityPlan::Attempt,
         "and so does the next: one unfinished launch between good ones "
         "never stops it");
}

void anUnusedAttemptWatchesNothing() {
  RNSkCompatibilityAttempt attempt;
  expect(attempt.phase() == RNSkCompatibilityAttempt::Phase::Unused,
         "a process on a device with a core adapter never begins an attempt");
  expect(!attempt.beginFirstWork(true) && !attempt.noteWorkDone() &&
             !attempt.notePresented(),
         "and watches no submission");
}

} // namespace

int main() {
  ruleAttemptsWithoutARecord();
  ruleAttemptsAfterOneUnfinishedLaunch();
  ruleStopsAfterTwoUnfinishedLaunches();
  ruleRetriesOnAnotherAppVersion();
  ruleStopsWhenNothingCanBeRecorded();
  recordRoundTripsAndRejectsOtherText();
  fileWritesReadsAndRemoves();
  fileFailsCleanlyWhereItCannotWrite();
  twoCrashesInCreationStopTheLevel();
  twoCrashesInFirstWorkStopTheLevel();
  aCrashInEachWindowStopsTheLevel();
  aProcessThatNeverDrawsNeitherCountsNorResets();
  offscreenFirstWorkSettlesWithoutAPresent();
  aWindowFrameNeedsItsPresentToo();
  aBatchWithNoWorkIsNotTheFirstWork();
  aSuccessResetsTheCount();
  anUnusedAttemptWatchesNothing();
  if (failures > 0) {
    std::fprintf(stderr, "%d expectation(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("RNSkCompatibilityGuardTest: all expectations met\n");
  return EXIT_SUCCESS;
}
