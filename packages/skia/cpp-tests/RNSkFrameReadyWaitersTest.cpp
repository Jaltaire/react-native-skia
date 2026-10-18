#include "RNSkFrameReadyWaiters.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using RNSkia::RNSkFrameReadyWaiters;

namespace {

int failures = 0;

void expect(bool condition, const char *description) {
  if (!condition) {
    std::fprintf(stderr, "FAILED: %s\n", description);
    failures++;
  }
}

void testRunsAtOnceWhenAlreadyReady() {
  RNSkFrameReadyWaiters waiters;
  waiters.ready(3, waiters.epoch());
  int calls = 0;
  waiters.wait(3, [&]() { calls++; });
  waiters.wait(1, [&]() { calls++; });
  expect(calls == 2, "A generation already ready runs its callback at once.");
  expect(waiters.pendingCount() == 0, "Nothing is left waiting.");
}

void testNothingReadyYetAndNoContent() {
  RNSkFrameReadyWaiters waiters;
  int calls = 0;
  waiters.wait(0, [&]() { calls++; });
  expect(calls == 1, "Generation zero counts as ready before any frame.");
}

void testWaitsForItsGeneration() {
  RNSkFrameReadyWaiters waiters;
  int calls = 0;
  waiters.wait(2, [&]() { calls++; });
  expect(calls == 0, "The callback waits before any frame is ready.");
  waiters.ready(1, waiters.epoch());
  expect(calls == 0, "An older generation does not run the callback.");
  expect(waiters.pendingCount() == 1, "The callback is still waiting.");
  waiters.ready(2, waiters.epoch());
  expect(calls == 1, "Its own generation runs the callback.");
  waiters.ready(2, waiters.epoch());
  expect(calls == 1, "A callback runs only once.");
}

void testLaterGenerationReleasesEarlierWaits() {
  RNSkFrameReadyWaiters waiters;
  std::vector<int> order;
  waiters.wait(1, [&]() { order.push_back(1); });
  waiters.wait(3, [&]() { order.push_back(3); });
  waiters.wait(2, [&]() { order.push_back(2); });
  waiters.ready(2, waiters.epoch());
  expect(order.size() == 2, "Generations up to the ready one are released.");
  expect(order.size() == 2 && order[0] == 1 && order[1] == 2,
         "Released callbacks run in the order they waited.");
  expect(waiters.pendingCount() == 1, "The later generation keeps waiting.");
  waiters.ready(5, waiters.epoch());
  expect(order.size() == 3 && order[2] == 3,
         "A later generation releases the rest.");
}

void testReadyGenerationNeverGoesBack() {
  RNSkFrameReadyWaiters waiters;
  waiters.ready(4, waiters.epoch());
  waiters.ready(2, waiters.epoch());
  expect(waiters.readyGeneration() == 4,
         "An older frame does not lower the ready generation.");
  int calls = 0;
  waiters.wait(4, [&]() { calls++; });
  expect(calls == 1, "The newer generation still counts as ready.");
}

void testCallbackMayWaitAgain() {
  RNSkFrameReadyWaiters waiters;
  int calls = 0;
  waiters.wait(1, [&]() {
    calls++;
    waiters.wait(2, [&]() { calls++; });
  });
  waiters.ready(1, waiters.epoch());
  expect(calls == 1, "A callback can register another wait without deadlock.");
  expect(waiters.pendingCount() == 1, "The nested wait is pending.");
  waiters.ready(2, waiters.epoch());
  expect(calls == 2, "The nested wait runs on its own generation.");
}

void testConcurrentWaitsAndReadyFrames() {
  RNSkFrameReadyWaiters waiters;
  std::atomic<int> calls{0};
  constexpr int kWaiters = 2000;
  std::thread waiting([&]() {
    for (int i = 1; i <= kWaiters; i++) {
      waiters.wait(static_cast<uint64_t>(i), [&]() { calls++; });
    }
  });
  std::thread producing([&]() {
    for (int i = 1; i <= kWaiters; i++) {
      waiters.ready(static_cast<uint64_t>(i), waiters.epoch());
    }
  });
  waiting.join();
  producing.join();
  waiters.ready(kWaiters, waiters.epoch());
  expect(calls == kWaiters, "Every wait runs once across threads.");
  expect(waiters.pendingCount() == 0, "No wait is lost across threads.");
}

void testEachTargetReplacementStartsAnEpoch() {
  RNSkFrameReadyWaiters waiters;
  const uint64_t first = waiters.epoch();
  waiters.targetReplaced();
  expect(waiters.epoch() == first + 1, "A replacement starts a new epoch.");
}

void testAFrameOfARetiredTargetResolvesNothing() {
  RNSkFrameReadyWaiters waiters;
  int calls = 0;
  waiters.wait(2, [&]() { calls++; });
  const uint64_t epoch = waiters.epoch();
  waiters.targetReplaced();
  waiters.ready(2, epoch);
  expect(calls == 0, "A frame queued before the target was replaced, whose "
                     "readiness lands after it, resolves no wait.");
  expect(waiters.readyGeneration() == 0,
         "It does not count as ready for later waits either.");
  expect(waiters.pendingCount() == 1, "The wait is still pending.");
  waiters.ready(3, waiters.epoch());
  expect(calls == 1, "A frame on the new target resolves the wait.");
}

} // namespace

int main() {
  testRunsAtOnceWhenAlreadyReady();
  testNothingReadyYetAndNoContent();
  testWaitsForItsGeneration();
  testLaterGenerationReleasesEarlierWaits();
  testReadyGenerationNeverGoesBack();
  testCallbackMayWaitAgain();
  testConcurrentWaitsAndReadyFrames();
  testEachTargetReplacementStartsAnEpoch();
  testAFrameOfARetiredTargetResolvesNothing();
  if (failures > 0) {
    std::fprintf(stderr, "%d expectation(s) failed.\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("All frame ready waiter tests passed.\n");
  return EXIT_SUCCESS;
}
