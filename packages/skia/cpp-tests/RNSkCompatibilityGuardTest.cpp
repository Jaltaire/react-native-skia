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

/**
 One launch on a device without a core adapter, as the library runs it:
 the guard decides, records the attempt and arms it, and the launch then
 either presents its first frame or ends before it. Returns the plan.
 */
RNSkCompatibilityPlan launch(const RNSkCompatibilityGuardFile &file,
                             const std::string &appVersion,
                             bool presentsFirstFrame) {
  auto decision =
      decideCompatibilityAttempt(file.read(), appVersion, file.usable());
  if (decision.plan != RNSkCompatibilityPlan::Attempt) {
    return decision.plan;
  }
  if (!file.write(decision.record)) {
    return RNSkCompatibilityPlan::StopUnrecorded;
  }
  RNSkCompatibilityAttempt attempt;
  attempt.arm(file);
  if (presentsFirstFrame) {
    attempt.notePresented();
  }
  return RNSkCompatibilityPlan::Attempt;
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
  expect(launch(readOnly, "1.0 (1)", true) ==
             RNSkCompatibilityPlan::StopUnrecorded,
         "a launch that cannot record its attempt does not try the level");
}

void twoUnfinishedLaunchesInARowStopTheLevel() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  expect(launch(file, "1.0 (1)", false) == RNSkCompatibilityPlan::Attempt,
         "launch 1 tries the level and ends before its first frame");
  expect(launch(file, "1.0 (1)", false) == RNSkCompatibilityPlan::Attempt,
         "launch 2 tries it again and ends before its first frame");
  expect(launch(file, "1.0 (1)", true) ==
             RNSkCompatibilityPlan::StopUnfinished,
         "launch 3 does not try the level");
  expect(launch(file, "1.0 (1)", true) ==
             RNSkCompatibilityPlan::StopUnfinished,
         "nor does any later launch on that version");
  expect(launch(file, "1.1 (2)", true) == RNSkCompatibilityPlan::Attempt,
         "the next version tries it again");
  expect(!file.read().has_value(),
         "and its first presented frame settles the attempt");
}

void aSuccessResetsTheCount() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  launch(file, "1.0 (1)", false);
  launch(file, "1.0 (1)", true);
  expect(!file.read().has_value(),
         "a launch that presents its first frame leaves no record");
  expect(launch(file, "1.0 (1)", false) == RNSkCompatibilityPlan::Attempt,
         "an unfinished launch after a good one tries the level");
  expect(launch(file, "1.0 (1)", true) == RNSkCompatibilityPlan::Attempt,
         "and so does the next: one unfinished launch between good ones "
         "never stops it");
}

void onlyAnArmedAttemptIsSettled() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  file.write({"1.0 (1)", 1});
  RNSkCompatibilityAttempt attempt;
  expect(!attempt.armed() && !attempt.notePresented(),
         "a frame on a process that armed nothing settles nothing");
  expect(file.read().has_value(),
         "and leaves any record alone, as on a device with a core adapter");
  attempt.arm(file);
  expect(attempt.armed(), "an attempt is armed once its record is written");
  expect(attempt.notePresented(),
         "the first frame after arming settles the attempt");
  expect(!file.read().has_value(), "settling removes the record");
  file.write({"1.0 (1)", 1});
  expect(!attempt.notePresented(), "later frames settle nothing");
  expect(file.read().has_value(), "and touch no file");
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
  twoUnfinishedLaunchesInARowStopTheLevel();
  aSuccessResetsTheCount();
  onlyAnArmedAttemptIsSettled();
  if (failures > 0) {
    std::fprintf(stderr, "%d expectation(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("RNSkCompatibilityGuardTest: all expectations met\n");
  return EXIT_SUCCESS;
}
