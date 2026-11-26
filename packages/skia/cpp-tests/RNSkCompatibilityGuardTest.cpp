#include "RNDawnFeatureLevel.h"
#include "RNSkCompatibilityGuard.h"

#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
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

  /** The names of the files in the directory. */
  std::vector<std::string> files() const {
    std::vector<std::string> names;
    if (DIR *entries = ::opendir(path.c_str())) {
      while (const dirent *entry = ::readdir(entries)) {
        const std::string name = entry->d_name;
        if (name != "." && name != "..") {
          names.push_back(name);
        }
      }
      ::closedir(entries);
    }
    return names;
  }

  ~Directory() {
    ::chmod(path.c_str(), S_IRWXU);
    for (const auto &name : files()) {
      ::chmod((path + "/" + name).c_str(), S_IRUSR | S_IWUSR);
      ::unlink((path + "/" + name).c_str());
    }
    ::rmdir(path.c_str());
  }
};

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
  auto parsed = parseCompatibilityAttempts("2\n9.9.9 (99)");
  expect(parsed && parsed->appVersion == "9.9.9 (99)" &&
             parsed->unfinished == 2,
         "a record reads back as written");
  expect(parseCompatibilityAttempts("0\nv") &&
             parseCompatibilityAttempts("0\nv")->unfinished == 0,
         "a count of 0 reads back");
  expect(parseCompatibilityAttempts("1000\nv") &&
             parseCompatibilityAttempts("1000\nv")->unfinished == 1000,
         "a count of 1000 reads back");
  expect(!parseCompatibilityAttempts("").has_value(),
         "an empty file is no record");
  expect(!parseCompatibilityAttempts("2").has_value(),
         "a count without a line break is no record");
  expect(!parseCompatibilityAttempts("002\nv").has_value(),
         "a count with a leading zero is no record");
  expect(!parseCompatibilityAttempts("-1\nv").has_value(),
         "a negative count is no record");
  expect(!parseCompatibilityAttempts("2\r\nv").has_value(),
         "a count followed by a carriage return is no record");
  expect(!parseCompatibilityAttempts("two\nv").has_value(),
         "a count that is not a number is no record");
  expect(!parseCompatibilityAttempts("1234567890\nv").has_value(),
         "a count of more than nine digits is no record");
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
  expect(directory.files() ==
             std::vector<std::string>{"rnskia-compatibility-attempts"},
         "the temporary file is renamed away");
  file.remove();
  expect(!file.read().has_value(), "a removed record is gone");
  file.remove();
}

RNSkCompatibilityConfig configFor(const Directory &directory) {
  RNSkCompatibilityConfig config;
  config.guardDirectory = directory.path;
  config.appVersion = "1.0 (1)";
  return config;
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


using RNSkia::RNSkCompatibilityWork;

int recorded(const RNSkCompatibilityGuardFile &file) {
  auto record = file.read();
  return record ? record->unfinished : 0;
}

/**
 A launch in a process of its own on a device without a core adapter, driven
 through the shipped gate and the attempt's methods in the order the context
 calls them. The process dies where the test stops driving it (when the
 launch goes out of scope).
 */
struct Launch {
  RNSkCompatibilityAttempt attempt;
  RNSkCompatibilityPlan plan = RNSkCompatibilityPlan::StopUnrecorded;

  explicit Launch(const RNSkCompatibilityGuardFile &file,
                  const std::string &appVersion = "1.0 (1)") {
    RNSkCompatibilityConfig config;
    config.guardDirectory = file.directory();
    config.appVersion = appVersion;
    RNSkCompatibilityGuardState state;
    consultCompatibilityGuard(
        attempt, [&config]() { return config; }, state);
    if (!state.decision) {
      return;
    }
    plan = state.decision->plan;
    if (plan == RNSkCompatibilityPlan::Attempt &&
        !attempt.beginCreation(*state.file, state.decision->record)) {
      plan = RNSkCompatibilityPlan::StopUnrecorded;
    }
  }

  bool tried() const { return plan == RNSkCompatibilityPlan::Attempt; }

  // The device and its context exist (the constructor: the precompile, when
  // pipelines are stored, starts before the creation window closes).
  void create(bool precompiles = false) {
    if (precompiles) {
      attempt.beginPrecompile();
    }
    attempt.endCreation(true);
  }

  // A window frame from start to end: the window surface, its frame snapped,
  // submitted, presented, and reported done.
  void drawWindowFrame() {
    attempt.openFirstWork();
    attempt.openFirstWork();
    if (attempt.watch(RNSkCompatibilityWork::WindowFrame)) {
      attempt.notePresented(true);
      attempt.noteWorkDone(true);
    }
  }

  // An offscreen surface's draws from start to end.
  void drawOffscreen() {
    attempt.openFirstWork();
    if (attempt.watch(RNSkCompatibilityWork::OffscreenDraw)) {
      attempt.noteWorkDone(true);
    }
  }
};

/** Where a simulated launch ends (see the state table in the guard). */
enum class Ending {
  // Inside the device's creation (a driver crash or hang).
  InCreation,
  // The device or its context cannot be created, and Dawn says why.
  FailsCleanly,
  // Created, and nothing is ever drawn.
  NeverDraws,
  // Inside the precompile of stored pipelines.
  InPrecompile,
  // Inside Dawn's Configure of the first window surface (the swapchain).
  InConfigure,
  // At the first image the first window frame acquires.
  AtFirstImage,
  // Inside the pipeline compile the first snap starts on Dawn's worker.
  InCompileAfterSnap,
  // Inside the first window frame's submitted work.
  InFirstWork,
  // The first window frame finished and presented.
  FinishesWindowFrame,
  // The first offscreen draw finished.
  FinishesOffscreen,
};

RNSkCompatibilityPlan launch(const RNSkCompatibilityGuardFile &file,
                             const std::string &appVersion, Ending ending,
                             bool precompiles = false) {
  Launch launch(file, appVersion);
  if (!launch.tried() || ending == Ending::InCreation) {
    return launch.plan;
  }
  if (ending == Ending::FailsCleanly) {
    launch.attempt.endCreation(false);
    return launch.plan;
  }
  if (ending == Ending::InPrecompile) {
    launch.create(/* precompiles= */ true);
    return launch.plan;
  }
  launch.create(precompiles);
  if (precompiles) {
    launch.attempt.endPrecompile();
  }
  switch (ending) {
  case Ending::NeverDraws:
    break;
  case Ending::InConfigure:
  case Ending::AtFirstImage:
    // The window surface is made, then configured, then its first image
    // acquired: the window opened before the first of these.
    launch.attempt.openFirstWork();
    break;
  case Ending::InCompileAfterSnap:
    launch.attempt.openFirstWork();
    break;
  case Ending::InFirstWork:
    launch.attempt.openFirstWork();
    launch.attempt.watch(RNSkCompatibilityWork::WindowFrame);
    break;
  case Ending::FinishesWindowFrame:
    launch.drawWindowFrame();
    break;
  case Ending::FinishesOffscreen:
    launch.drawOffscreen();
    break;
  default:
    break;
  }
  return launch.plan;
}

// Row Unused: a core device never begins, and nothing is touched.
void rowUnusedWatchesNothing() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  file.write({"1.0 (1)", 1});
  RNSkCompatibilityAttempt attempt;
  const auto operations = RNSkCompatibilityGuardFile::durableOperations();
  expect(attempt.phase() == RNSkCompatibilityAttempt::Phase::Unused,
         "Unused: a process on a device with a core adapter never begins");
  expect(!attempt.openFirstWork() &&
             !attempt.watch(RNSkCompatibilityWork::WindowFrame) &&
             !attempt.watch(RNSkCompatibilityWork::OffscreenDraw) &&
             !attempt.noteWorkDone(true) && !attempt.notePresented(true) &&
             !attempt.beginPrecompile() && !attempt.endPrecompile(),
         "Unused: no window surface, snap, submission, or precompile begins "
         "anything");
  attempt.unwatch();
  attempt.endCreation(true);
  expect(RNSkCompatibilityGuardFile::durableOperations() == operations &&
             recorded(file) == 1,
         "Unused: and nothing is written or removed");
}

// Row Creating: a death counts.
void rowCreatingDeathCounts() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  expect(launch(file, "1.0 (1)", Ending::InCreation) ==
             RNSkCompatibilityPlan::Attempt,
         "Creating: launch 1 tries the level and dies in the creation");
  expect(recorded(file) == 1, "Creating: the record holds F + 1");
  launch(file, "1.0 (1)", Ending::InCreation);
  expect(launch(file, "1.0 (1)", Ending::FinishesWindowFrame) ==
             RNSkCompatibilityPlan::StopUnfinished,
         "Creating: two deaths in a row stop the level on that version");
  expect(launch(file, "1.1 (2)", Ending::FinishesWindowFrame) ==
             RNSkCompatibilityPlan::Attempt,
         "Creating: the next version tries it again");
  expect(!file.read().has_value(), "and its finished first frame settles");
}

// Row Created: a launch that never draws neither counts nor resets.
void rowCreatedNeverDrawsNeitherCountsNorResets() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  for (int i = 0; i < 3; i++) {
    launch(file, "1.0 (1)", Ending::NeverDraws);
  }
  expect(!file.read().has_value(),
         "Created: three launches that make no window surface and snap "
         "nothing leave no record");
  launch(file, "1.0 (1)", Ending::InFirstWork);
  launch(file, "1.0 (1)", Ending::NeverDraws);
  expect(recorded(file) == 1, "Created: one keeps the count it found");
  Launch uploads(file);
  uploads.create();
  expect(!uploads.attempt.watch(RNSkCompatibilityWork::Other),
         "Created: an upload or a readback opens no window");
  expect(recorded(file) == 1 &&
             uploads.attempt.phase() ==
                 RNSkCompatibilityAttempt::Phase::AwaitingFirstWork,
         "Created: and leaves the count found");
}

// Row Created + P: a death in the precompile counts; its end puts it back.
void rowCreatedWithPrecompile() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  launch(file, "1.0 (1)", Ending::InPrecompile);
  expect(recorded(file) == 1,
         "Created + P: a death in the precompile of stored pipelines counts");
  launch(file, "1.0 (1)", Ending::InPrecompile);
  expect(launch(file, "1.0 (1)", Ending::FinishesWindowFrame) ==
             RNSkCompatibilityPlan::StopUnfinished,
         "Created + P: two in a row stop the level");
  file.remove();
  for (int i = 0; i < 3; i++) {
    launch(file, "1.0 (1)", Ending::NeverDraws, /* precompiles= */ true);
  }
  expect(!file.read().has_value(),
         "Created + P: a precompile that ends before any draw puts the count "
         "back");
  Launch early(file);
  early.attempt.beginPrecompile();
  early.attempt.endPrecompile();
  expect(recorded(file) == 1,
         "Created + P: a precompile that ends inside the creation window "
         "leaves the creation's count");
  early.attempt.endCreation(true);
  expect(!file.read().has_value(), "and the creation's end puts it back");
}

// Row FirstWork: a death after the first window surface or the first snap
// counts, with no precompile (a fresh install has nothing stored).
void rowFirstWorkDeathCounts(Ending ending, const char *where) {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  launch(file, "1.0 (1)", ending);
  if (recorded(file) != 1) {
    expect(false, where);
    return;
  }
  launch(file, "1.0 (1)", ending);
  if (launch(file, "1.0 (1)", Ending::FinishesWindowFrame) !=
      RNSkCompatibilityPlan::StopUnfinished) {
    expect(false, where);
  }
}

void rowFirstWorkDeathsCount() {
  rowFirstWorkDeathCounts(Ending::InConfigure,
                          "FirstWork: two deaths in Configure, on a fresh "
                          "install, are counted and stop the level");
  rowFirstWorkDeathCounts(Ending::AtFirstImage,
                          "FirstWork: two deaths at the first image "
                          "acquired are counted and stop the level");
  rowFirstWorkDeathCounts(Ending::InCompileAfterSnap,
                          "FirstWork: two deaths in the compile the first "
                          "snap starts are counted and stop the level");
  rowFirstWorkDeathCounts(Ending::InFirstWork,
                          "FirstWork: two deaths in the first frame's "
                          "submitted work are counted and stop the level");
}

// Row FirstWork: what opens it, and how often it writes.
void rowFirstWorkOpensOnceAndWritesOnce() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  Launch launch(file);
  launch.create();
  expect(launch.attempt.openFirstWork(),
         "FirstWork: the first window surface or snap opens it");
  const auto operations = RNSkCompatibilityGuardFile::durableOperations();
  expect(!launch.attempt.openFirstWork() && !launch.attempt.openFirstWork(),
         "FirstWork: later surfaces and snaps do not reopen it");
  expect(RNSkCompatibilityGuardFile::durableOperations() == operations,
         "FirstWork: nor write again");
  Directory other;
  RNSkCompatibilityGuardFile otherFile(other.path);
  Launch direct(otherFile);
  direct.create();
  expect(direct.attempt.watch(RNSkCompatibilityWork::OffscreenDraw) &&
             recorded(otherFile) == 1,
         "FirstWork: a draw submitted with nothing opened before it opens "
         "the window itself");
}

// Row FirstWork: what is watched, and what settles.
void rowFirstWorkSettles() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  file.write({"1.0 (1)", 1});
  {
    Launch launch(file);
    launch.create();
    launch.attempt.openFirstWork();
    expect(launch.attempt.watch(RNSkCompatibilityWork::WindowFrame),
           "FirstWork: the first window frame submitted is watched");
    expect(!launch.attempt.watch(RNSkCompatibilityWork::WindowFrame),
           "FirstWork: no second one while it is");
    expect(!launch.attempt.noteWorkDone(true),
           "FirstWork: a window frame done on the GPU but not presented "
           "does not settle");
    expect(recorded(file) == 2, "FirstWork: the record holds F + 1");
    expect(launch.attempt.notePresented(true),
           "Settled: its present on a device that was not lost settles it");
    expect(!file.read().has_value(), "Settled: and removes the record");
  }
  {
    Launch launch(file);
    launch.create();
    launch.attempt.openFirstWork();
    launch.attempt.watch(RNSkCompatibilityWork::WindowFrame);
    expect(!launch.attempt.notePresented(true),
           "FirstWork: a present before the GPU's report does not settle");
    expect(launch.attempt.noteWorkDone(true), "Settled: the report then does");
  }
  {
    Launch launch(file);
    launch.create();
    launch.attempt.openFirstWork();
    launch.attempt.watch(RNSkCompatibilityWork::OffscreenDraw);
    expect(!launch.attempt.notePresented(true),
           "FirstWork: an offscreen draw has no present to report");
    expect(launch.attempt.noteWorkDone(true),
           "Settled: an offscreen draw reported done settles");
  }
}

// Row FirstWork: an upload, a readback, or a surface's first clear never
// settles.
void rowOtherWorkNeverSettles() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  file.write({"1.0 (1)", 1});
  Launch launch(file);
  launch.create();
  launch.attempt.openFirstWork();
  expect(!launch.attempt.watch(RNSkCompatibilityWork::Other),
         "FirstWork: a clear, an upload, or a readback is never watched");
  expect(!launch.attempt.noteWorkDone(true) && recorded(file) == 2,
         "FirstWork: so a report of it settles nothing, and the count found "
         "is not erased");
  expect(launch.attempt.watch(RNSkCompatibilityWork::OffscreenDraw) &&
             launch.attempt.noteWorkDone(true) && !file.read().has_value(),
         "Settled: the surface's drawing behind its clear settles it");
}

// Row FirstWork: rejected batches neither settle nor write.
void rowRejectedBatchesWriteNothing() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  file.write({"1.0 (1)", 1});
  Launch launch(file);
  launch.create();
  launch.attempt.openFirstWork();
  const auto operations = RNSkCompatibilityGuardFile::durableOperations();
  for (int frame = 0; frame < 1000; frame++) {
    if (!launch.attempt.watch(RNSkCompatibilityWork::WindowFrame)) {
      expect(false, "FirstWork: every rejected batch's frame is watched");
      return;
    }
    launch.attempt.unwatch();
  }
  expect(RNSkCompatibilityGuardFile::durableOperations() == operations,
         "FirstWork: 1,000 frames whose batches are all rejected make no "
         "write: the only one was the window's opening");
  expect(recorded(file) == 2 &&
             launch.attempt.phase() ==
                 RNSkCompatibilityAttempt::Phase::FirstWork,
         "FirstWork: and leave the window open at F + 1, settling nothing");
  launch.drawWindowFrame();
  expect(!file.read().has_value(), "Settled: a frame that draws then settles");
}

// Row Failed: a watched draw that failed stays counted.
void rowFailedStaysCounted() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  for (int launchNumber = 0; launchNumber < 2; launchNumber++) {
    Launch launch(file);
    launch.create();
    launch.attempt.openFirstWork();
    launch.attempt.watch(RNSkCompatibilityWork::WindowFrame);
    if (launchNumber == 0) {
      launch.attempt.noteWorkDone(true);
      expect(!launch.attempt.notePresented(false),
             "Failed: a present on a lost device does not settle");
    } else {
      expect(!launch.attempt.noteWorkDone(false),
             "Failed: nor work the GPU did not finish");
    }
    expect(!launch.attempt.notePresented(true) &&
               !launch.attempt.noteWorkDone(true) &&
               !launch.attempt.watch(RNSkCompatibilityWork::WindowFrame),
           "Failed: and nothing later changes it");
  }
  expect(decideCompatibilityAttempt(file.read(), "1.0 (1)", true).plan ==
             RNSkCompatibilityPlan::StopUnfinished,
         "Failed: two in a row stop the level");
}

// Rows FirstWorkFinished and Failed, with the precompile.
void rowFirstWorkFinishedWhilePrecompiling() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  {
    Launch launch(file);
    launch.create(/* precompiles= */ true);
    launch.drawWindowFrame();
    expect(launch.attempt.phase() ==
               RNSkCompatibilityAttempt::Phase::FirstWorkFinished,
           "FirstWorkFinished: a first frame that finished while the "
           "precompile runs waits for it");
    expect(recorded(file) == 1, "FirstWorkFinished: the record holds F + 1");
    expect(launch.attempt.endPrecompile(),
           "Settled: the precompile's end settles it");
    expect(!file.read().has_value(), "Settled: and removes the record");
  }
  for (int launchNumber = 0; launchNumber < 2; launchNumber++) {
    Launch launch(file);
    launch.create(/* precompiles= */ true);
    launch.drawWindowFrame();
  }
  expect(decideCompatibilityAttempt(file.read(), "1.0 (1)", true).plan ==
             RNSkCompatibilityPlan::StopUnfinished,
         "FirstWorkFinished: two deaths in what is left of the precompile "
         "after the first frame stop the level");
  file.remove();
  {
    Launch launch(file);
    launch.create(/* precompiles= */ true);
    expect(!launch.attempt.endPrecompile(/* deviceLost= */ true),
           "Failed: a precompile that ends on a lost device does not settle");
    expect(launch.attempt.phase() == RNSkCompatibilityAttempt::Phase::Finished &&
               recorded(file) == 1,
           "Failed: it stays counted, as a first work that lost its device");
  }
}

// Row NotTried: a clean failure, or a creation that cannot be recorded.
void rowNotTried() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  for (int i = 0; i < 3; i++) {
    expect(launch(file, "1.0 (1)", Ending::FailsCleanly) ==
               RNSkCompatibilityPlan::Attempt,
           "NotTried: a device that cannot be created is tried at every "
           "launch, so Skia.gpu.error keeps Dawn's reason");
  }
  expect(!file.read().has_value(), "NotTried: and leaves no record");
  launch(file, "1.0 (1)", Ending::InCreation);
  launch(file, "1.0 (1)", Ending::FailsCleanly);
  expect(recorded(file) == 1, "NotTried: a clean failure keeps F");
  RNSkCompatibilityGuardFile none("");
  expect(launch(none, "1.0 (1)", Ending::FinishesWindowFrame) ==
             RNSkCompatibilityPlan::StopUnrecorded,
         "NotTried: with no directory the level is not tried");
  if (::geteuid() == 0) {
    std::printf("skipped: creations that cannot record (root ignores "
                "directory permissions)\n");
    return;
  }
  ::chmod(directory.path.c_str(), S_IRUSR | S_IXUSR);
  expect(launch(file, "1.0 (1)", Ending::FinishesWindowFrame) ==
             RNSkCompatibilityPlan::StopUnrecorded,
         "NotTried: a launch that cannot record its attempt does not try the "
         "level");
  ::chmod(directory.path.c_str(), S_IRWXU);
  expect(recorded(file) == 1, "NotTried: and leaves F as it found it");
  ::chmod(directory.path.c_str(), S_IWUSR | S_IXUSR);
  file.write({"1.0 (1)", 1});
  Launch unflushable(file);
  ::chmod(directory.path.c_str(), S_IRWXU);
  expect(!unflushable.tried(),
         "NotTried: a record renamed into place whose directory cannot be "
         "flushed is not trusted: the level is not tried");
  expect(recorded(file) >= 1,
         "NotTried: and the record keeps at least the count found");
}

// Row Stopped: a write that fails after creation stops the attempt and
// never lowers the count below F.
void rowStoppedNeverLowersTheCount() {
  if (::geteuid() == 0) {
    std::printf("skipped: writes that fail after creation (root ignores "
                "directory permissions)\n");
    return;
  }
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  file.write({"1.0 (1)", 1});
  {
    Launch launch(file);
    launch.create();
    ::chmod(directory.path.c_str(), S_IRUSR | S_IXUSR);
    expect(!launch.attempt.openFirstWork(),
           "Stopped: a first-work window whose count cannot be written does "
           "not open");
    expect(launch.attempt.phase() ==
               RNSkCompatibilityAttempt::Phase::Finished &&
               !launch.attempt.watch(RNSkCompatibilityWork::WindowFrame),
           "Stopped: the attempt watches nothing more");
    ::chmod(directory.path.c_str(), S_IRWXU);
    expect(recorded(file) == 1, "Stopped: the record keeps F");
  }
  {
    Launch launch(file);
    ::chmod(directory.path.c_str(), S_IRUSR | S_IXUSR);
    launch.attempt.endCreation(true);
    ::chmod(directory.path.c_str(), S_IRWXU);
    expect(launch.attempt.phase() == RNSkCompatibilityAttempt::Phase::Finished,
           "Stopped: a creation whose count cannot be put back stops");
    expect(recorded(file) == 2,
           "Stopped: and leaves F + 1, never less than F");
  }
  file.write({"1.0 (1)", 1});
  {
    Launch launch(file);
    launch.create();
    launch.attempt.openFirstWork();
    launch.attempt.watch(RNSkCompatibilityWork::OffscreenDraw);
    ::chmod(directory.path.c_str(), S_IRUSR | S_IXUSR);
    launch.attempt.noteWorkDone(true);
    ::chmod(directory.path.c_str(), S_IRWXU);
    expect(recorded(file) == 2,
           "Stopped: a settle whose record cannot be removed leaves F + 1: "
           "the conservative side");
  }
}

// The record: written only when its value changes.
void theRecordIsWrittenOnlyWhenItsValueChanges() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  const auto before = RNSkCompatibilityGuardFile::durableOperations();
  launch(file, "1.0 (1)", Ending::FinishesWindowFrame);
  expect(RNSkCompatibilityGuardFile::durableOperations() - before == 4,
         "a fresh install's launch makes four durable operations: the "
         "creation's write, its removal, the first work's write, and the "
         "settle's removal");
  file.write({"1.0 (1)", 1});
  const auto found = RNSkCompatibilityGuardFile::durableOperations();
  {
    Launch spanning(file);
    spanning.create(/* precompiles= */ true);
    spanning.drawWindowFrame();
    spanning.attempt.endPrecompile();
  }
  expect(RNSkCompatibilityGuardFile::durableOperations() - found == 2,
         "a launch whose precompile spans its creation and its first frame "
         "writes once and removes once");
}

// The record: an unreadable or foreign record.
void anUnreadableRecordIsReplacedByThisLaunch() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  {
    FILE *large = std::fopen(file.path().c_str(), "wb");
    const std::string chunk(4096, 'x');
    for (size_t written = 0;
         written <= RNSkCompatibilityGuardFile::kMaxRecordBytes;
         written += chunk.size()) {
      std::fwrite(chunk.data(), 1, chunk.size(), large);
    }
    std::fclose(large);
  }
  expect(!file.read().has_value(),
         "a file longer than kMaxRecordBytes is not a record");
  if (::geteuid() == 0) {
    std::printf("skipped: an unreadable record (root reads it)\n");
    return;
  }
  file.write({"1.0 (1)", 1});
  ::chmod(file.path().c_str(), 0);
  {
    Launch launch(file);
    expect(launch.tried(),
           "an unreadable record's version cannot be known, so it stops "
           "nothing: the level is tried");
    ::chmod(file.path().c_str(), S_IRUSR | S_IWUSR);
    expect(recorded(file) == 1,
           "and the record is replaced by this launch's own, counting it");
  }
}

void exactlyOneThreadSettlesTheFirstWork() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  for (int round = 0; round < 200; round++) {
    Launch launch(file);
    launch.create();
    launch.attempt.openFirstWork();
    launch.attempt.watch(RNSkCompatibilityWork::OffscreenDraw);
    std::atomic<int> settled{0};
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 8; thread++) {
      threads.emplace_back([&launch, &settled]() {
        if (launch.attempt.noteWorkDone(true)) {
          settled.fetch_add(1);
        }
      });
    }
    for (auto &thread : threads) {
      thread.join();
    }
    if (settled.load() != 1) {
      expect(false, "exactly one of eight threads settles the first work");
      return;
    }
  }
  expect(!file.read().has_value(), "and the record is removed");
}

void exactlyOneOfThePrecompileAndTheWorkSettles() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  for (int round = 0; round < 200; round++) {
    Launch launch(file);
    launch.create(/* precompiles= */ true);
    launch.attempt.openFirstWork();
    launch.attempt.watch(RNSkCompatibilityWork::OffscreenDraw);
    std::atomic<int> settled{0};
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 8; thread++) {
      threads.emplace_back([&launch, &settled, thread]() {
        const bool settles = thread == 0 ? launch.attempt.endPrecompile()
                                         : launch.attempt.noteWorkDone(true);
        if (settles) {
          settled.fetch_add(1);
        }
      });
    }
    for (auto &thread : threads) {
      thread.join();
    }
    if (settled.load() != 1) {
      expect(false, "exactly one of the precompile's end and seven reports "
                    "of the first work settles the attempt");
      return;
    }
  }
  expect(!file.read().has_value(), "and the record is removed");
}

void aPrecompileStartedFromTwoThreadsCountsOnce() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  for (int round = 0; round < 200; round++) {
    file.write({"1.0 (1)", 1});
    Launch launch(file);
    if (round % 2 == 0) {
      launch.attempt.endCreation(true);
    }
    std::atomic<int> counted{0};
    std::vector<std::thread> threads;
    for (int thread = 0; thread < 2; thread++) {
      threads.emplace_back([&launch, &counted]() {
        if (launch.attempt.beginPrecompile()) {
          counted.fetch_add(1);
        }
      });
    }
    for (auto &thread : threads) {
      thread.join();
    }
    if (counted.load() != 1 || recorded(file) != 2) {
      expect(false, "two threads starting the precompile at once count it "
                    "once: the record holds F + 1, not F + 2");
      return;
    }
    launch.attempt.endCreation(true);
    launch.attempt.endPrecompile();
    if (recorded(file) != 1) {
      expect(false, "and its end puts back the count the launch found");
      return;
    }
    file.remove();
  }
  expect(true, "two threads starting the precompile at once count it once");
}

void onlyTheFirstPrecompileIsCounted() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  Launch launch(file);
  expect(launch.attempt.beginPrecompile(), "the first precompile is counted");
  expect(!launch.attempt.beginPrecompile(), "a second one is not");
  launch.attempt.endCreation(true);
  launch.attempt.endPrecompile();
  expect(!launch.attempt.beginPrecompile(),
         "nor one on a device that replaces a lost one");
  expect(!launch.attempt.endPrecompile(),
         "and an end without a begin does nothing");
}

void aStaleTemporaryFileIsIgnored() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  FILE *stale = std::fopen((file.path() + ".tmp.1.0").c_str(), "wb");
  std::fputs("7\nleft by an interrupted write", stale);
  std::fclose(stale);
  expect(!file.read().has_value(),
         "a temporary file left by an interrupted write is not the record");
  expect(launch(file, "1.0 (1)", Ending::InCreation) ==
             RNSkCompatibilityPlan::Attempt,
         "and does not stop the level");
  expect(recorded(file) == 1, "a later write does not trip over it");
}

void aReplacementDeviceTouchesNoRecord() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  const auto config = configFor(directory);
  Launch launch(file);
  launch.create();
  launch.drawWindowFrame();
  for (int loss = 0; loss < 3; loss++) {
    RNSkCompatibilityGuardState replacement;
    expect(!consultCompatibilityGuard(
                launch.attempt, [&config]() { return config; }, replacement)
                    .has_value() &&
               !replacement.decision.has_value() &&
               !replacement.file.has_value(),
           "a device that replaces a lost one is allowed the level and "
           "consults no record");
  }
  expect(!file.read().has_value(),
         "the retry loop after a settled session leaves no record");
}

void aLossDuringTheFirstWorkCountsOnce() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  const auto config = configFor(directory);
  Launch launch(file);
  launch.create();
  launch.attempt.openFirstWork();
  launch.attempt.watch(RNSkCompatibilityWork::WindowFrame);
  for (int loss = 0; loss < 3; loss++) {
    RNSkCompatibilityGuardState replacement;
    consultCompatibilityGuard(launch.attempt, [&config]() { return config; },
                              replacement);
  }
  expect(recorded(file) == 1,
         "a session that loses its device during its first work, and keeps "
         "replacing it, counts once");
}

void anyVersionStringReadsBackWhole() {
  Directory directory;
  RNSkCompatibilityGuardFile file(directory.path);
  const std::string longVersion(10000, 'x');
  std::string otherLong = longVersion;
  otherLong.back() = 'y';
  for (const std::string &version :
       {std::string("9.9.9\n(99)"), std::string(""), longVersion,
        std::string("9.9.9 (99)\r\n")}) {
    expect(file.write({version, 1}), "a record with any version is written");
    auto read = file.read();
    expect(read && read->appVersion == version && read->unfinished == 1,
           "and reads back whole: a line break, an empty version, 10,000 "
           "characters, a trailing carriage return");
    launch(file, version, Ending::InCreation);
    expect(launch(file, version, Ending::FinishesWindowFrame) ==
               RNSkCompatibilityPlan::StopUnfinished,
           "so two unfinished launches stop the level with that version too");
    file.remove();
  }
  file.write({longVersion, 2});
  expect(decideCompatibilityAttempt(file.read(), otherLong, true).plan ==
             RNSkCompatibilityPlan::Attempt,
         "two 10,000-character versions that differ in their last character "
         "are two versions");
  file.remove();
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

void theFirstWorkIsPublishedOnlyOnceItsCountIsOnDisk() {
  Directory directory;
  RNSkCompatibilityGuardFile plain(directory.path);
  RNSkCompatibilityAttempt attempt;
  std::vector<RNSkCompatibilityAttempt::Phase> phasesWhileWriting;
  RNSkCompatibilityGuardFile file(directory.path, [&]() {
    phasesWhileWriting.push_back(attempt.phase());
  });
  plain.write({"1.0 (1)", 1});
  const auto decision =
      decideCompatibilityAttempt(file.read(), "1.0 (1)", true);
  attempt.beginCreation(file, decision.record);
  attempt.endCreation(true);
  phasesWhileWriting.clear();
  expect(attempt.openFirstWork(), "FirstWork: the window opens");
  expect(phasesWhileWriting.size() == 1 &&
             phasesWhileWriting[0] ==
                 RNSkCompatibilityAttempt::Phase::AwaitingFirstWork,
         "FirstWork: while the window's opening write runs, the attempt "
         "still reports AwaitingFirstWork, so another thread's "
         "openFirstWork() waits for the write instead of drawing ahead of "
         "the count");
  expect(attempt.phase() == RNSkCompatibilityAttempt::Phase::FirstWork &&
             recorded(plain) == 2,
         "FirstWork: once the window is reported open, F + 1 is on disk");
}

void aWindowWhoseOpeningWriteFailsIsNeverPublished() {
  Directory directory;
  RNSkCompatibilityGuardFile plain(directory.path);
  RNSkCompatibilityAttempt attempt;
  bool failWrites = false;
  RNSkCompatibilityGuardFile file(directory.path, [&]() {
    if (failWrites) {
      ::chmod(directory.path.c_str(), S_IRUSR | S_IXUSR);
    }
  });
  plain.write({"1.0 (1)", 1});
  const auto decision =
      decideCompatibilityAttempt(file.read(), "1.0 (1)", true);
  attempt.beginCreation(file, decision.record);
  attempt.endCreation(true);
  failWrites = true;
  const bool opened = attempt.openFirstWork();
  ::chmod(directory.path.c_str(), S_IRWXU);
  expect(!opened && attempt.phase() ==
                        RNSkCompatibilityAttempt::Phase::Finished,
         "Stopped: a window whose opening write fails is never reported "
         "open, and the attempt stops");
  expect(recorded(plain) == 1,
         "Stopped: the record keeps the count the launch found");
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
  anyVersionStringReadsBackWhole();
  fileWritesReadsAndRemoves();
  rowUnusedWatchesNothing();
  rowCreatingDeathCounts();
  rowCreatedNeverDrawsNeitherCountsNorResets();
  rowCreatedWithPrecompile();
  rowFirstWorkDeathsCount();
  rowFirstWorkOpensOnceAndWritesOnce();
  rowFirstWorkSettles();
  rowOtherWorkNeverSettles();
  rowRejectedBatchesWriteNothing();
  rowFailedStaysCounted();
  rowFirstWorkFinishedWhilePrecompiling();
  rowNotTried();
  rowStoppedNeverLowersTheCount();
  theRecordIsWrittenOnlyWhenItsValueChanges();
  anUnreadableRecordIsReplacedByThisLaunch();
  exactlyOneThreadSettlesTheFirstWork();
  exactlyOneOfThePrecompileAndTheWorkSettles();
  aPrecompileStartedFromTwoThreadsCountsOnce();
  onlyTheFirstPrecompileIsCounted();
  aStaleTemporaryFileIsIgnored();
  aReplacementDeviceTouchesNoRecord();
  aLossDuringTheFirstWorkCountsOnce();
  aCorePhoneNeverResolvesTheConfiguration();
  theFirstWorkIsPublishedOnlyOnceItsCountIsOnDisk();
  aWindowWhoseOpeningWriteFailsIsNeverPublished();
  if (failures > 0) {
    std::fprintf(stderr, "%d expectation(s) failed\n", failures);
    return 1;
  }
  std::printf("RNSkCompatibilityGuardTest: all expectations met\n");
  return 0;
}
