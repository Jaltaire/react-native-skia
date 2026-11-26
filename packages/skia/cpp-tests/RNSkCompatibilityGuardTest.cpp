#include "RNDawnFeatureLevel.h"
#include "RNSkCompatibilityGuard.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

using RNSkia::chooseAdapter;
using RNSkia::compatibilityStopReason;
using RNSkia::consultCompatibilityGuard;
using RNSkia::decideCompatibilityAttempt;
using RNSkia::kRNSkCompatibilityUnfinishedLaunchesToStop;
using RNSkia::parseCompatibilityAttempts;
using RNSkia::RNSkCompatibilityAttempt;
using RNSkia::RNSkCompatibilityAttempts;
using RNSkia::RNSkCompatibilityConfig;
using RNSkia::RNSkCompatibilityGuardFile;
using RNSkia::RNSkCompatibilityGuardState;
using RNSkia::RNSkCompatibilityPlan;
using RNSkia::RNSkFeatureLevel;

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
  attempt.noteWorkDone(true);
  if (window) {
    attempt.notePresented(true);
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
  expect(compatibilityStopReason(decision.plan, "1.0 (1)")
                 .find("could not record") != std::string::npos,
         "and Skia.gpu.error says why");
}

void theRefusalSaysWhatWasFoundWhyAndWhen() {
  const auto message = RNSkia::compatibilityRefusedMessage(
      "Vulkan", "PowerVR Rogue GE8320", "driver 1.13",
      compatibilityStopReason(RNSkCompatibilityPlan::StopUnfinished,
                              "9.9.9 (99)"));
  expect(message ==
             "React Native Skia found the Vulkan adapter PowerVR Rogue "
             "GE8320 (driver 1.13) only at the WebGPU compatibility feature "
             "level, and did not use it: the last 2 launches of this app "
             "version (9.9.9 (99)) that tried it ended before their first "
             "work on it finished. It is tried again when the app's version "
             "changes.",
         "the guard's refusal names the adapter found, why it was not used, "
         "and when it is tried again");
  expect(message.find("v2") == std::string::npos &&
             message.find("core or compatibility") == std::string::npos,
         "and neither denies the adapter nor points to another major "
         "version");
  RNSkCompatibilityConfig off;
  off.allowed = false;
  const auto turnedOff = RNSkia::compatibilityRefusedMessage(
      "Vulkan", "PowerVR Rogue GE8320", "",
      *RNSkia::compatibilityRefusal(off));
  expect(turnedOff.find("found the Vulkan adapter PowerVR Rogue GE8320 only "
                        "at the WebGPU compatibility feature level") !=
                 std::string::npos &&
             turnedOff.find("turned off in this app's build settings") !=
                 std::string::npos &&
             turnedOff.find("tried again once the setting is removed") !=
                 std::string::npos,
         "the build setting's refusal says the same three things");
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
  expect(!attempt.noteWorkDone(true),
         "a window frame whose work finished is not settled before its "
         "present");
  expect(recorded(file) == 1, "and stays counted until then");
  expect(attempt.notePresented(true), "its present settles it");
  expect(!file.read().has_value(), "and removes the record");
  expect(!attempt.beginFirstWork(false), "later work is not watched");
}

void aFailedFirstWorkStaysCounted() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  for (int launchNumber = 0; launchNumber < 2; launchNumber++) {
    auto decision = decideCompatibilityAttempt(file.read(), "1.0 (1)", true);
    RNSkCompatibilityAttempt attempt;
    attempt.beginCreation(file, decision.record);
    attempt.endCreation(true);
    attempt.beginFirstWork(true);
    if (launchNumber == 0) {
      attempt.noteWorkDone(true);
      expect(!attempt.notePresented(false),
             "a present that failed, or a device lost during the first "
             "frame, does not settle the attempt");
    } else {
      expect(!attempt.noteWorkDone(false),
             "nor does work the GPU did not finish");
    }
    expect(!attempt.notePresented(true) && !attempt.noteWorkDone(true),
           "and later reports change nothing");
  }
  expect(decideCompatibilityAttempt(file.read(), "1.0 (1)", true).plan ==
             RNSkCompatibilityPlan::StopUnfinished,
         "a driver that loses the device on its first frame at every launch "
         "is stopped");
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

RNSkCompatibilityConfig configFor(const Directory &directory) {
  RNSkCompatibilityConfig config;
  config.guardDirectory = directory.path;
  config.appVersion = "1.0 (1)";
  return config;
}

void aReplacementDeviceTouchesNoRecord() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  const auto config = configFor(directory);
  RNSkCompatibilityAttempt attempt;
  RNSkCompatibilityGuardState first;
  expect(!consultCompatibilityGuard(attempt, [&config]() { return config; }, first).has_value() &&
             first.decision.has_value(),
         "the process's first device consults the guard");
  attempt.beginCreation(*first.file, first.decision->record);
  attempt.endCreation(true);
  attempt.beginFirstWork(true);
  attempt.noteWorkDone(true);
  attempt.notePresented(true);
  for (int loss = 0; loss < 3; loss++) {
    RNSkCompatibilityGuardState replacement;
    expect(!consultCompatibilityGuard(attempt, [&config]() { return config; }, replacement)
                .has_value() &&
               !replacement.decision.has_value() &&
               !replacement.file.has_value(),
           "a device that replaces a lost one is allowed the level and "
           "consults no record");
  }
  expect(!file.read().has_value(),
         "the background retry loop after a settled session leaves no "
         "record");
}

void aLossDuringTheFirstWorkCountsOnce() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  const auto config = configFor(directory);
  RNSkCompatibilityAttempt attempt;
  RNSkCompatibilityGuardState first;
  consultCompatibilityGuard(attempt, [&config]() { return config; }, first);
  attempt.beginCreation(*first.file, first.decision->record);
  attempt.endCreation(true);
  attempt.beginFirstWork(true);
  for (int loss = 0; loss < 3; loss++) {
    RNSkCompatibilityGuardState replacement;
    consultCompatibilityGuard(attempt, [&config]() { return config; }, replacement);
  }
  expect(recorded(file) == 1,
         "a session that loses its device during its first work, and keeps "
         "replacing it, counts once");
}

/**
 The probe's choice of adapter with the shipped gate, as DawnUtils makes it,
 counting how often the app's configuration is resolved.
 */
std::optional<std::pair<std::string, RNSkFeatureLevel>>
probe(bool hasCoreAdapter, const RNSkCompatibilityConfig &config,
      int &resolved) {
  RNSkCompatibilityAttempt attempt;
  RNSkCompatibilityGuardState state;
  return chooseAdapter<std::string>(
      [hasCoreAdapter](RNSkFeatureLevel level) -> std::optional<std::string> {
        if (level == RNSkFeatureLevel::Core && !hasCoreAdapter) {
          return std::nullopt;
        }
        return std::string("GPU");
      },
      [&]() {
        return !consultCompatibilityGuard(
                    attempt,
                    [&]() {
                      resolved++;
                      return config;
                    },
                    state)
                    .has_value();
      });
}

void aCorePhoneNeverResolvesTheConfiguration() {
  Directory directory;
  int resolved = 0;
  auto chosen = probe(true, configFor(directory), resolved);
  expect(chosen && chosen->second == RNSkFeatureLevel::Core,
         "a phone with a core adapter renders at the core level");
  expect(resolved == 0,
         "and never resolves the app's configuration: no settings, "
         "directory, or version is read");
  expect(!RNSkCompatibilityGuardFile(directory.path).read().has_value(),
         "nor any record touched");
  chosen = probe(false, configFor(directory), resolved);
  expect(resolved == 1 && chosen &&
             chosen->second == RNSkFeatureLevel::Compatibility,
         "a phone without one resolves it once, at the gate");
}

void anUnusedAttemptWatchesNothing() {
  RNSkCompatibilityAttempt attempt;
  expect(attempt.phase() == RNSkCompatibilityAttempt::Phase::Unused,
         "a process on a device with a core adapter never begins an attempt");
  expect(!attempt.beginFirstWork(true) && !attempt.noteWorkDone(true) &&
             !attempt.notePresented(true),
         "and watches no submission");
}

} // namespace

int main() {
  ruleAttemptsWithoutARecord();
  ruleAttemptsAfterOneUnfinishedLaunch();
  ruleStopsAfterTwoUnfinishedLaunches();
  ruleRetriesOnAnotherAppVersion();
  ruleStopsWhenNothingCanBeRecorded();
  theRefusalSaysWhatWasFoundWhyAndWhen();
  recordRoundTripsAndRejectsOtherText();
  fileWritesReadsAndRemoves();
  fileFailsCleanlyWhereItCannotWrite();
  twoCrashesInCreationStopTheLevel();
  twoCrashesInFirstWorkStopTheLevel();
  aCrashInEachWindowStopsTheLevel();
  aProcessThatNeverDrawsNeitherCountsNorResets();
  offscreenFirstWorkSettlesWithoutAPresent();
  aWindowFrameNeedsItsPresentToo();
  aFailedFirstWorkStaysCounted();
  aBatchWithNoWorkIsNotTheFirstWork();
  aSuccessResetsTheCount();
  aReplacementDeviceTouchesNoRecord();
  aLossDuringTheFirstWorkCountsOnce();
  aCorePhoneNeverResolvesTheConfiguration();
  anUnusedAttemptWatchesNothing();
  if (failures > 0) {
    std::fprintf(stderr, "%d expectation(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("RNSkCompatibilityGuardTest: all expectations met\n");
  return EXIT_SUCCESS;
}
