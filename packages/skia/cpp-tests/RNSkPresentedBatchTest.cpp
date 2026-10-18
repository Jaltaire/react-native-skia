#include "RNSkPresentedBatch.h"
#include "RNSkFrameSchedule.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

using RNSkia::RNSkFrameOutcome;
using RNSkia::RNSkFrameSchedule;
using RNSkia::settlePresentedBatch;

namespace {

int failures = 0;

void expect(bool condition, const char *description) {
  if (!condition) {
    std::fprintf(stderr, "FAILED: %s\n", description);
    failures++;
  }
}

constexpr size_t kMaxQueued = RNSkFrameSchedule::kMaxQueued;

bool withSurface() { return true; }

/**
 A producer and the view presenting its frames, with the window replaced by
 how many recordings of a batch it takes before it declines one. The
 producer's bookkeeping is RNSkFrameSchedule, and the view settles each
 batch with settlePresentedBatch, as RNSkGraphiteProducer and RNSkView do;
 the target's queue is a deque of recording numbers.
 */
struct Bench {
  RNSkFrameSchedule schedule;
  std::deque<int> queue;
  int nextRecording = 1;

  Bench() { schedule.contentReplaced(true); }

  /** New values arrived; the producer records a frame if its gate allows. */
  bool animate() {
    schedule.redrawRequested();
    if (!schedule.canStart(true, true, withSurface)) {
      return false;
    }
    schedule.frameScheduled();
    const auto start = schedule.frameBegan();
    queue.push_back(nextRecording++);
    schedule.frameFinished(RNSkFrameOutcome::kQueued, start);
    return true;
  }

  /**
   A Reanimated step on the UI thread records a frame of its own, which the
   view presents right after it, with the queue, in one batch.
   */
  bool animateInStep() {
    schedule.valuesUpdated();
    if (!schedule.canStartInStep(true, true, /* presentsNow= */ true,
                                 withSurface)) {
      return false;
    }
    schedule.frameScheduled();
    const auto start = schedule.frameBegan();
    queue.push_back(nextRecording++);
    schedule.frameFinished(RNSkFrameOutcome::kQueued, start);
    return true;
  }

  void fill() {
    while (schedule.queued() < kMaxQueued) {
      animate();
    }
  }

  /**
   The view takes the queue and the window takes the first `accepted`
   recordings of it.
   */
  bool present(size_t accepted) { return presentWhileSubmitting(accepted, {}); }

  /**
   Same as present(), while the producer submits recordings to the target:
   they land behind the ones put back.
   */
  bool presentWhileSubmitting(size_t accepted,
                              const std::vector<int> &submitted) {
    std::vector<int> batch(queue.begin(), queue.end());
    queue.assign(submitted.begin(), submitted.end());
    return settlePresentedBatch(
        batch, std::min(accepted, batch.size()),
        [this](const std::vector<int> &kept) {
          queue.insert(queue.begin(), kept.begin(), kept.end());
        },
        [this](size_t done) { schedule.framesPresented(done); });
  }

  bool slotsMatchTheQueue() const { return schedule.queued() == queue.size(); }
};

void testAWholeBatchFreesEverySlot() {
  Bench bench;
  bench.fill();
  expect(bench.present(kMaxQueued), "A batch the window takes whole went "
                                    "through.");
  expect(bench.queue.empty(), "Nothing is queued again.");
  expect(bench.schedule.queued() == 0, "Every slot is free.");
}

void testAPartialPresentFreesTheSlotsOfThePresentedRecordings() {
  Bench bench;
  bench.fill();
  expect(!bench.present(1),
         "A batch whose second recording the window declined did not go "
         "through.");
  expect(bench.queue == std::deque<int>{2},
         "The declined recording is queued again.");
  expect(bench.slotsMatchTheQueue(),
         "The recording presented ahead of the declined one frees its "
         "slot.");
  expect(bench.animate(), "The producer records into the freed slot.");
}

void testARetryAfterAPartialPresentLeavesNoSlotTaken() {
  Bench bench;
  bench.fill();
  bench.present(1);
  expect(bench.present(kMaxQueued), "The retry presents the rest.");
  expect(bench.queue.empty() && bench.schedule.queued() == 0,
         "Once every recording is presented, no slot stays taken.");
}

void testADeclinedFirstRecordingFreesNothing() {
  Bench bench;
  bench.fill();
  expect(!bench.present(0), "Nothing went through.");
  expect(bench.queue == std::deque<int>{1, 2},
         "The whole batch is queued again, in order.");
  expect(bench.schedule.queued() == kMaxQueued,
         "Recordings still queued keep their slots.");
}

void testKeptRecordingsGoAheadOfOnesSubmittedMeanwhile() {
  Bench bench;
  bench.fill();
  bench.presentWhileSubmitting(1, {3});
  expect(bench.queue == std::deque<int>{2, 3},
         "The declined recording goes back ahead of a newer one.");
}

void testPartialPresentsNeverStopTheProducer() {
  Bench bench;
  bool matched = true;
  bool recordsWhenEmpty = true;
  for (int step = 0; step < 50; step++) {
    // The producer records ahead of the display, as far as its gate allows.
    const bool empty = bench.queue.empty();
    if (!bench.animate() && empty) {
      recordsWhenEmpty = false;
    }
    bench.animate();
    matched = matched && bench.slotsMatchTheQueue();
    // Every other present, the window declines all but the first recording.
    bench.present(step % 2 == 0 ? 1 : kMaxQueued);
    matched = matched && bench.slotsMatchTheQueue();
  }
  expect(matched, "The slots the producer counts are the recordings queued "
                  "on the target, after every partial present and retry.");
  expect(recordsWhenEmpty,
         "With nothing queued to present, the producer always records.");
}

void testPartialPresentsOfInStepBatchesNeverStopTheProducer() {
  Bench bench;
  bool recordsWhenEmpty = true;
  for (int step = 0; step < 10; step++) {
    if (!bench.animate()) {
      recordsWhenEmpty = false;
    }
    bench.animate();
    bench.animateInStep();
    // The window takes the first recording of the step's batch only, and
    // the rest on the next frame.
    bench.present(1);
    bench.present(kMaxQueued + 1);
    expect(bench.queue.empty(), "Every recording is presented by the retry.");
  }
  expect(recordsWhenEmpty,
         "Batches the window took in two goes leave the producer recording.");
  expect(bench.slotsMatchTheQueue(), "No slot is left taken.");
}

} // namespace

int main() {
  testAWholeBatchFreesEverySlot();
  testAPartialPresentFreesTheSlotsOfThePresentedRecordings();
  testARetryAfterAPartialPresentLeavesNoSlotTaken();
  testADeclinedFirstRecordingFreesNothing();
  testKeptRecordingsGoAheadOfOnesSubmittedMeanwhile();
  testPartialPresentsNeverStopTheProducer();
  testPartialPresentsOfInStepBatchesNeverStopTheProducer();
  if (failures > 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("RNSkPresentedBatchTest passed\n");
  return EXIT_SUCCESS;
}
