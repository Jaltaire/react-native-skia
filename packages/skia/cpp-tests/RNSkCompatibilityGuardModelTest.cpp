// Host-side exhaustive check of RNSkia::RNSkCompatibilityAttempt (see
// run.sh): every order of the attempt's calls (window and offscreen opens,
// accepted and rejected submissions of each kind, GPU reports, presents, and
// the precompile's end, which can find the device lost) is driven through
// the shipped header with a real record file, on a fresh install and on a
// found count of one, with and without a precompile. After each call the
// record is checked against the guard's invariant: never counted twice,
// never lowered or cleared without a finished window frame, never raised
// with every window closed (outside a failure), and raised for as long as a
// window was made or a frame snapped and none finished. The attempt's
// private state is read only to tell states apart, so that each is explored
// once.

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#define private public
#include "RNSkCompatibilityGuard.h"
#undef private

using namespace RNSkia;
using Work = RNSkCompatibilityWork;
using Phase = RNSkCompatibilityAttempt::Phase;

enum Event {
  OpenWin,
  OpenOff,
  SubmitWin,
  SubmitWinRejected,
  SubmitOff,
  SubmitOffRejected,
  ReportOk,
  ReportFail,
  PresentOk,
  PresentFail,
  EndPOk,
  EndPLost,
  EventCount
};

const char *names[] = {"OpenWin",   "OpenOff",     "SubmitWin", "SubmitWinRejected",
                       "SubmitOff", "SubmitOffRejected", "ReportOk",  "ReportFail",
                       "PresentOk", "PresentFail", "EndPOk",    "EndPLost"};

struct Harness {
  int pendingReport = 0; // 0 none, 1 offscreen, 2 window
  bool pendingPresent = false;
  bool windowEver = false;
  bool winReported = false;
  bool winPresented = false;
  bool windowFrameFinished = false;
  bool failed = false;
  bool precompileRunning = false;
  bool unwatchedWindowFramePresented = false;
};

using Key = std::tuple<int, bool, bool, bool, bool, bool, bool, bool, int, int,
                       int, bool, bool, bool, bool, bool, bool, bool, bool>;

struct Run {
  std::string directory;
  RNSkCompatibilityGuardFile file;
  RNSkCompatibilityAttempt attempt;
  Harness harness;
  int found;

  Run(const std::string &dir, int foundCount, bool precompiles)
      : directory(dir), file(dir), found(foundCount) {
    file.remove();
    if (found > 0) {
      file.write({"v", found});
    }
    const auto decision =
        decideCompatibilityAttempt(file.readRecord(), "v", true);
    attempt.beginCreation(file, decision.record);
    if (precompiles) {
      harness.precompileRunning = attempt.beginPrecompile();
    }
    attempt.endCreation(true);
  }

  // False when the event does not apply in this state.
  bool apply(Event event) {
    auto &h = harness;
    switch (event) {
    case OpenWin:
      attempt.openFirstWork(Work::WindowFrame);
      h.windowEver = true;
      return true;
    case OpenOff:
      attempt.openFirstWork(Work::OffscreenDraw);
      return true;
    case SubmitWin: {
      const bool finished = attempt.phase() == Phase::Finished ||
                            attempt.phase() == Phase::FirstWorkFinished;
      const bool watched = attempt.watch(Work::WindowFrame);
      h.windowEver = true;
      if (watched) {
        h.pendingReport = 2;
        h.pendingPresent = true;
        h.winReported = false;
        h.winPresented = false;
      } else if (!finished) {
        h.unwatchedWindowFramePresented = true;
      }
      return true;
    }
    case SubmitWinRejected:
      if (attempt.watch(Work::WindowFrame)) {
        attempt.unwatch();
      }
      h.windowEver = true;
      return true;
    case SubmitOff:
      if (attempt.watch(Work::OffscreenDraw)) {
        h.pendingReport = 1;
      }
      return true;
    case SubmitOffRejected:
      if (attempt.watch(Work::OffscreenDraw)) {
        attempt.unwatch();
      }
      return true;
    case ReportOk:
    case ReportFail: {
      if (h.pendingReport == 0) {
        return false;
      }
      const bool ok = event == ReportOk;
      const bool live = attempt.phase() == Phase::FirstWork;
      attempt.noteWorkDone(ok);
      if (live && !ok) {
        h.failed = true;
      }
      if (live && ok && h.pendingReport == 2) {
        h.winReported = true;
      }
      h.pendingReport = 0;
      h.windowFrameFinished = h.windowFrameFinished ||
                              (h.winReported && h.winPresented);
      return true;
    }
    case PresentOk:
    case PresentFail: {
      if (!h.pendingPresent) {
        return false;
      }
      const bool ok = event == PresentOk;
      const bool live = attempt.phase() == Phase::FirstWork;
      attempt.notePresented(ok);
      if (live && !ok) {
        h.failed = true;
      }
      if (live && ok) {
        h.winPresented = true;
      }
      h.pendingPresent = false;
      h.windowFrameFinished = h.windowFrameFinished ||
                              (h.winReported && h.winPresented);
      return true;
    }
    case EndPOk:
    case EndPLost: {
      if (!h.precompileRunning) {
        return false;
      }
      const bool lost = event == EndPLost;
      if (lost && attempt.phase() != Phase::Finished) {
        h.failed = true;
      }
      attempt.endPrecompile(lost);
      h.precompileRunning = false;
      return true;
    }
    default:
      return false;
    }
  }

  int recordedValue() const {
    const auto record = file.read();
    return record ? record->unfinished : 0;
  }

  Key key() const {
    const auto &a = attempt;
    const auto &h = harness;
    return {static_cast<int>(a.phase()),
            a._precompileBegun,
            a._precompiling,
            a._watching,
            a._windowSeen.load(),
            a._offscreenFinished,
            a._workDone,
            a._presented,
            static_cast<int>(a._watchedWork),
            a._stored,
            h.pendingReport,
            h.pendingPresent,
            h.windowEver,
            h.winReported,
            h.winPresented,
            h.windowFrameFinished,
            h.failed,
            h.precompileRunning,
            h.unwatchedWindowFramePresented};
  }
};

std::string describe(const std::vector<Event> &path) {
  std::string text;
  for (const auto event : path) {
    text += std::string(text.empty() ? "" : " > ") + names[event];
  }
  return text;
}

void removeDirectory(const std::string &directory) {
  if (DIR *entries = ::opendir(directory.c_str())) {
    while (const dirent *entry = ::readdir(entries)) {
      const std::string name = entry->d_name;
      if (name != "." && name != "..") {
        ::unlink((directory + "/" + name).c_str());
      }
    }
    ::closedir(entries);
  }
  ::rmdir(directory.c_str());
}

int main() {
  char name[] = "/tmp/rnskia-guard-model.XXXXXX";
  if (::mkdtemp(name) == nullptr) {
    std::printf("FAILED: no scratch directory\n");
    return 1;
  }
  const std::string directory = name;
  int violations = 0;
  size_t explored = 0;
  for (const int found : {0, 1}) {
    for (const bool precompiles : {false, true}) {
      std::map<Key, std::vector<Event>> seen;
      std::deque<std::vector<Event>> queue;
      queue.push_back({});
      size_t stuck = 0;
      std::string stuckExample;
      {
        Run root(directory, found, precompiles);
        seen[root.key()] = {};
      }
      while (!queue.empty()) {
        const auto path = queue.front();
        queue.pop_front();
        for (int next = 0; next < EventCount; next++) {
          Run run(directory, found, precompiles);
          for (const auto event : path) {
            run.apply(event);
          }
          const int before = run.recordedValue();
          if (!run.apply(static_cast<Event>(next))) {
            continue;
          }
          auto extended = path;
          extended.push_back(static_cast<Event>(next));
          const int value = run.recordedValue();
          const auto &h = run.harness;
          const auto &a = run.attempt;
          const auto phase = a.phase();
          auto violate = [&](const char *what) {
            violations++;
            std::printf("VIOLATION (F=%d, P=%d): %s | file %d -> %d | %s\n",
                        found, precompiles, what, before, value,
                        describe(extended).c_str());
          };
          if (value != a._stored) {
            violate("the file differs from what the attempt believes");
          }
          if (value > found + 1) {
            violate("(d) counted twice");
          }
          if (found > 0 && value < found &&
              !(value == 0 && h.windowFrameFinished && !h.precompileRunning)) {
            violate("(a)/(c) lowered or cleared without a finished window frame");
          }
          if (found == 0 && value == 0 && h.windowEver && !h.windowFrameFinished) {
            violate("the count is not raised after a window on a fresh install");
          }
          const bool windowOpen = phase == Phase::FirstWork || a._precompiling;
          if (value == found + 1 && !windowOpen && !h.failed) {
            violate("(b) raised with every window closed");
          }
          if (h.windowEver && !h.windowFrameFinished && value != found + 1) {
            violate("a window was made or a frame snapped, none finished, and "
                    "the count is not raised");
          }
          if (h.windowFrameFinished && h.precompileRunning && !h.failed &&
              value != found + 1) {
            violate("the precompile still runs and the count is not raised");
          }
          if (h.windowFrameFinished && !h.precompileRunning && !h.failed &&
              value != 0) {
            violate("a finished window frame did not clear the record");
          }
          if (!h.windowEver && !h.failed && !h.precompileRunning &&
              phase != Phase::FirstWork && value != found) {
            violate("offscreen-only, quiet, and the record is not F");
          }
          const bool quiet = h.pendingReport == 0 && !h.pendingPresent &&
                             !h.precompileRunning;
          const auto key = run.key();
          if (seen.emplace(key, extended).second) {
            queue.push_back(extended);
            if (quiet && !h.failed && h.unwatchedWindowFramePresented &&
                !h.windowFrameFinished && value == found + 1) {
              stuck++;
              if (stuckExample.empty()) {
                stuckExample = describe(extended);
              }
            }
          }
        }
      }
      explored += seen.size();
      std::printf("  F=%d, precompile %s: %zu states; %zu quiet with a window "
                  "frame presented unwatched and the count raised (the "
                  "over-counting side the header describes)%s%s\n",
                  found, precompiles ? "yes" : "no", seen.size(), stuck,
                  stuckExample.empty() ? "" : ", first: ",
                  stuckExample.c_str());
    }
  }
  removeDirectory(directory);
  if (violations > 0) {
    std::printf("FAILED: %d violation(s) of the guard's invariant\n",
                violations);
    return 1;
  }
  std::printf("RNSkCompatibilityGuardModelTest: %zu states explored, no "
              "violation\n",
              explored);
  return 0;
}
