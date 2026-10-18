#include "RNSkFrameSchedule.h"

#include <cstdio>
#include <cstdlib>

using RNSkia::RNSkFrameOutcome;
using RNSkia::RNSkFrameSchedule;
using RNSkia::RNSkFrameStart;

namespace {

int failures = 0;

void expect(bool condition, const char *description) {
  if (!condition) {
    std::fprintf(stderr, "FAILED: %s\n", description);
    failures++;
  }
}

constexpr size_t kMaxQueued = RNSkFrameSchedule::kMaxQueued;

int surfaceChecks = 0;

bool withSurface() {
  surfaceChecks++;
  return true;
}

bool withoutSurface() {
  surfaceChecks++;
  return false;
}

bool startsWithSurface(const RNSkFrameSchedule &schedule) {
  return schedule.canStart(true, true, withSurface);
}

bool startsWithoutSurface(const RNSkFrameSchedule &schedule) {
  return schedule.canStart(true, true, withoutSurface);
}

RNSkFrameStart at(uint64_t generation) { return {generation, generation}; }

RNSkFrameStart runFrame(RNSkFrameSchedule &schedule, RNSkFrameOutcome outcome) {
  schedule.frameScheduled();
  const auto start = schedule.frameBegan();
  schedule.frameFinished(outcome, start);
  return start;
}

RNSkFrameStart queueFrame(RNSkFrameSchedule &schedule) {
  return runFrame(schedule, RNSkFrameOutcome::kQueued);
}

void fillQueue(RNSkFrameSchedule &schedule) {
  while (schedule.queued() < kMaxQueued) {
    queueFrame(schedule);
  }
}

RNSkFrameSchedule withContent() {
  RNSkFrameSchedule schedule;
  schedule.contentReplaced(true);
  return schedule;
}

void testAFrameThatReachedNoTargetKeepsTheContentDirty() {
  for (auto outcome : {RNSkFrameOutcome::kNotRecorded,
                       RNSkFrameOutcome::kQueuedOnRetiredTarget}) {
    auto schedule = withContent();
    runFrame(schedule, outcome);
    expect(schedule.isDirty(),
           "A frame that did not reach the current target leaves the content "
           "to be recorded by the next request.");
  }
  auto schedule = withContent();
  queueFrame(schedule);
  expect(!schedule.isDirty(),
         "A frame queued on the current target leaves nothing to record.");
}

bool startsInStep(const RNSkFrameSchedule &schedule, bool presentsNow) {
  return schedule.canStartInStep(true, true, presentsNow, withSurface);
}

void testAStepThatPresentsRecordsBehindAFullQueue() {
  auto schedule = withContent();
  fillQueue(schedule);
  schedule.redrawRequested();
  expect(!startsWithSurface(schedule),
         "The pool waits for a present behind a full queue.");
  expect(startsInStep(schedule, true),
         "A step that presents the queue right after it records may record "
         "behind the full queue.");
  queueFrame(schedule);
  expect(schedule.queued() == kMaxQueued + 1,
         "The step's frame is queued behind the pool's.");
  expect(!startsInStep(schedule, true),
         "A step whose batch the window could not take records no further "
         "frame: the queue holds at most one frame more than the pool's.");
  schedule.framesPresented(kMaxQueued + 1);
  expect(startsInStep(schedule, true),
         "Once the batch is presented, the next step records again.");
}

void testAStepThatCannotPresentFollowsThePoolGate() {
  auto schedule = withContent();
  fillQueue(schedule);
  schedule.redrawRequested();
  expect(!schedule.canStartInStep(true, true, false, withSurface),
         "A step whose window cannot present now waits behind a full queue, "
         "like the pool.");
  expect(!schedule.canStartInStep(true, true, false, withoutSurface),
         "Without a surface, a redraw in step waits behind a full queue too.");
  schedule.contentReplaced(true);
  expect(schedule.canStartInStep(true, true, false, withoutSurface),
         "Without a surface, new content in step queues behind it, like the "
         "pool.");
}

void testAStepNeedsATargetContentAndNoJobInFlight() {
  auto schedule = withContent();
  expect(!schedule.canStartInStep(false, true, true, withSurface),
         "A step needs a target to record into.");
  expect(!schedule.canStartInStep(true, false, true, withSurface),
         "A step needs content to record.");
  schedule.frameScheduled();
  expect(!startsInStep(schedule, true),
         "A step does not record while a pool job is in flight.");
}

void testTheQueueKeepsOneFrameAheadOfTheDisplay() {
  expect(kMaxQueued == 2,
         "The pool keeps one frame ahead of the one waiting for its vsync.");
}

void testStartsWhenNothingWaits() {
  auto schedule = withContent();
  surfaceChecks = 0;
  expect(startsWithSurface(schedule),
         "A frame starts when nothing is in flight or waiting.");
  expect(surfaceChecks == 0,
         "The surface is only consulted once the queue is full.");
}

void testStartsWhileTheQueueHasRoom() {
  auto schedule = withContent();
  queueFrame(schedule);
  schedule.redrawRequested();
  surfaceChecks = 0;
  expect(schedule.queued() == 1, "A submitted frame waits for a present.");
  expect(startsWithSurface(schedule),
         "A frame starts while the one before it waits for its vsync.");
  expect(surfaceChecks == 0,
         "The surface is not consulted while the queue has room.");
}

void testNeverStartsWhileInFlight() {
  auto schedule = withContent();
  schedule.frameScheduled();
  expect(!startsWithoutSurface(schedule),
         "A frame never starts while another one is scheduled.");
  schedule.frameBegan();
  schedule.contentReplaced(true);
  expect(!startsWithoutSurface(schedule),
         "A frame never starts while another one is being recorded, even "
         "for newer content.");
}

void testNeedsATarget() {
  auto schedule = withContent();
  expect(!schedule.canStart(false, true, withoutSurface),
         "A frame needs a target to record into.");
}

void testNeedsContent() {
  auto schedule = withContent();
  expect(!schedule.canStart(true, false, withoutSurface),
         "A frame needs content to record.");
}

void testWaitsForThePresentWhenTheQueueIsFull() {
  auto schedule = withContent();
  fillQueue(schedule);
  schedule.contentReplaced(true);
  expect(!startsWithSurface(schedule),
         "New content waits for a present when the queue is full and the "
         "view can present.");
}

void testNewContentPassesAFullQueueWithoutASurface() {
  auto schedule = withContent();
  fillQueue(schedule);
  schedule.contentReplaced(true);
  expect(startsWithoutSurface(schedule),
         "New content does not wait for a present that a view without a "
         "surface cannot make.");
}

void testRedrawsWaitOnAFullQueueWithoutASurface() {
  auto schedule = withContent();
  fillQueue(schedule);
  schedule.redrawRequested();
  surfaceChecks = 0;
  expect(!startsWithoutSurface(schedule),
         "A redraw of content already submitted waits for a present, so that "
         "animations do not pile frames up without a surface.");
  expect(surfaceChecks == 0,
         "The surface is not consulted for content already submitted.");
}

void testNewContentQueuesOneFrameWithoutASurface() {
  auto schedule = withContent();
  fillQueue(schedule);
  schedule.contentReplaced(true);
  queueFrame(schedule);
  expect(schedule.queued() == kMaxQueued + 1,
         "Without a surface, new content queues behind the full queue.");
  schedule.redrawRequested();
  expect(!startsWithoutSurface(schedule),
         "Once that content is queued, its redraws wait for a present again.");
  schedule.contentReplaced(true);
  expect(startsWithoutSurface(schedule),
         "The next React commit queues one more frame.");
}

void testContentDuringASubmittedFrameStartsTheNextOne() {
  auto schedule = withContent();
  fillQueue(schedule);
  schedule.contentReplaced(true);
  schedule.frameScheduled();
  const auto first = schedule.frameBegan();
  schedule.contentReplaced(true);
  expect(!startsWithoutSurface(schedule),
         "Content replaced mid-frame is refused while the frame is in "
         "flight.");
  const bool requested =
      schedule.frameFinished(RNSkFrameOutcome::kQueued, first);
  expect(requested,
         "A submitted frame reports the content that arrived while it was "
         "recorded.");
  expect(startsWithoutSurface(schedule),
         "Without a surface, the content that arrived mid-frame starts the "
         "next frame instead of waiting for a present that never comes.");
  schedule.frameScheduled();
  const auto second = schedule.frameBegan();
  expect(second.generation == first.generation + 1,
         "The next frame records the newer content.");
  expect(second.revision == schedule.awaitedRevision(),
         "The next frame records the content a waiter waits for.");
}

void testContentDuringASubmittedFrameStartsTheNextOneWhileThereIsRoom() {
  auto schedule = withContent();
  schedule.frameScheduled();
  const auto start = schedule.frameBegan();
  schedule.contentReplaced(true);
  expect(schedule.frameFinished(RNSkFrameOutcome::kQueued, start),
         "The submitted frame reports the content that arrived mid-frame.");
  expect(startsWithSurface(schedule),
         "With a surface and room in the queue, that content starts the next "
         "frame while the submitted one waits for its vsync.");
}

void testContentDuringAFrameThatFillsTheQueueWaitsForThePresent() {
  auto schedule = withContent();
  queueFrame(schedule);
  schedule.frameScheduled();
  const auto start = schedule.frameBegan();
  schedule.contentReplaced(true);
  expect(schedule.frameFinished(RNSkFrameOutcome::kQueued, start),
         "The submitted frame reports the content that arrived mid-frame.");
  expect(!startsWithSurface(schedule),
         "With a surface and a full queue, that content waits for a present.");
  expect(schedule.framesPresented(1),
         "The present starts the frame for that content.");
  expect(startsWithSurface(schedule),
         "Once a frame is presented, the frame for that content may start.");
}

void testASubmittedFrameWithNothingNewRequestsNothing() {
  auto schedule = withContent();
  schedule.frameScheduled();
  const auto start = schedule.frameBegan();
  expect(!schedule.frameFinished(RNSkFrameOutcome::kQueued, start),
         "A submitted frame that nothing changed during requests nothing.");
  expect(!schedule.framesPresented(1), "Its present starts nothing either.");
}

void testARedrawDuringAFrameThatFillsTheQueueIsKept() {
  auto schedule = withContent();
  queueFrame(schedule);
  schedule.frameScheduled();
  const auto start = schedule.frameBegan();
  schedule.redrawRequested();
  expect(schedule.frameFinished(RNSkFrameOutcome::kQueued, start),
         "A redraw asked for mid-frame is reported when the frame finishes.");
  expect(!startsWithoutSurface(schedule),
         "Without a surface, that redraw still waits for a present.");
  expect(schedule.framesPresented(1), "The present starts that redraw.");
}

void testAFrameThatRecordedNothingStaysDirty() {
  auto schedule = withContent();
  schedule.frameScheduled();
  const auto start = schedule.frameBegan();
  expect(!schedule.frameFinished(RNSkFrameOutcome::kNotRecorded, start),
         "A frame that recorded nothing, with no request during it, starts "
         "nothing.");
  expect(schedule.queued() == 0,
         "A frame that recorded nothing leaves nothing to present.");
  expect(startsWithSurface(schedule),
         "The next request may start a frame right away.");
  schedule.frameScheduled();
  const auto retried = schedule.frameBegan();
  expect(retried.generation == start.generation,
         "The retried frame records the same content.");
}

void testAFrameThatRecordedNothingReportsARequestDuringIt() {
  auto schedule = withContent();
  schedule.frameScheduled();
  const auto start = schedule.frameBegan();
  schedule.redrawRequested();
  expect(schedule.frameFinished(RNSkFrameOutcome::kNotRecorded, start),
         "A request during a frame that recorded nothing starts the next "
         "frame.");
}

void testAFrameOnARetiredTargetIsNotQueued() {
  auto schedule = withContent();
  queueFrame(schedule);
  schedule.frameScheduled();
  const auto start = schedule.frameBegan();
  schedule.targetReplaced();
  expect(
      schedule.frameFinished(RNSkFrameOutcome::kQueuedOnRetiredTarget, start),
      "The new target asked for a frame while the old one was recorded.");
  expect(schedule.queued() == 0,
         "A frame queued on a retired target is not waited for.");
  expect(schedule.framesPresented(0),
         "The content stays dirty, to be recorded for the new target.");
}

void testAFrameOnARetiredTargetDoesNotCountAsSubmitted() {
  auto schedule = withContent();
  fillQueue(schedule);
  schedule.contentReplaced(true);
  runFrame(schedule, RNSkFrameOutcome::kQueuedOnRetiredTarget);
  expect(schedule.queued() == kMaxQueued,
         "A frame on a retired target takes no slot of the current one.");
  expect(startsWithoutSurface(schedule),
         "Content only counts as submitted once it reaches the current "
         "target: without a surface, it still queues a frame there.");
}

void testTargetReplacedForgetsTheQueue() {
  auto schedule = withContent();
  fillQueue(schedule);
  schedule.targetReplaced();
  expect(schedule.queued() == 0,
         "Frames queued on the previous target are not waited for.");
  expect(startsWithSurface(schedule),
         "The new target gets a frame right away.");
  expect(schedule.framesPresented(0),
         "The content is recorded again for the new target.");
}

void testPresentsFreeQueueSlots() {
  auto schedule = withContent();
  fillQueue(schedule);
  schedule.framesPresented(1);
  expect(schedule.queued() == kMaxQueued - 1,
         "Each presented recording frees a slot.");
  schedule.framesPresented(kMaxQueued + 3);
  expect(schedule.queued() == 0,
         "Recordings that are not the producer's (a client recording, the "
         "last frame shown again) do not make the count go negative.");
}

void testTheQueueStaysBoundedWithASurface() {
  auto schedule = withContent();
  bool bounded = true;
  for (int step = 0; step < 50; step++) {
    if (step % 3 == 0) {
      schedule.contentReplaced(true);
    } else {
      schedule.redrawRequested();
    }
    while (startsWithSurface(schedule)) {
      queueFrame(schedule);
      bounded = bounded && schedule.queued() <= kMaxQueued;
    }
    if (step % 2 == 0) {
      schedule.framesPresented(1);
    }
  }
  expect(bounded,
         "With a surface, the queue never holds more than kMaxQueued frames.");
}

void testClearedContentIsNotDirty() {
  auto schedule = withContent();
  queueFrame(schedule);
  schedule.contentReplaced(false);
  expect(!schedule.framesPresented(1),
         "Cleared content has nothing to record when the present lands.");
}

void testSubmittedGenerationNeverGoesBack() {
  auto schedule = withContent();
  schedule.contentReplaced(true);
  queueFrame(schedule);
  schedule.frameScheduled();
  schedule.frameBegan();
  schedule.frameFinished(RNSkFrameOutcome::kQueued, at(2));
  schedule.framesPresented(1);
  schedule.frameScheduled();
  schedule.frameBegan();
  schedule.frameFinished(RNSkFrameOutcome::kQueued, at(1));
  expect(schedule.queued() == kMaxQueued, "The queue is full.");
  expect(!startsWithoutSurface(schedule),
         "An older frame finishing late does not make the newer content "
         "look unsubmitted.");
}

void testAwaitedRevision() {
  RNSkFrameSchedule schedule;
  expect(schedule.awaitedRevision() == 1,
         "A view that never had content waits for its first.");
  schedule.contentReplaced(true);
  schedule.contentReplaced(true);
  expect(schedule.awaitedRevision() == 2,
         "A waiter waits for the newest content.");
}

void testValuesUpdatedAdvanceTheAwaitedRevision() {
  auto schedule = withContent();
  schedule.valuesUpdated();
  expect(schedule.awaitedRevision() == 2,
         "A waiter after an animated value update waits for a frame that "
         "draws the new values.");
  schedule.frameScheduled();
  const auto start = schedule.frameBegan();
  expect(start.revision == 2,
         "A frame records the revision of the values it replays.");
  expect(start.generation == 1,
         "An animated value update keeps the content's generation.");
}

void testContentReplacedAdvancesTheRevision() {
  auto schedule = withContent();
  schedule.valuesUpdated();
  schedule.contentReplaced(true);
  schedule.frameScheduled();
  const auto start = schedule.frameBegan();
  expect(start.generation == 2 && start.revision == 3,
         "New content advances both the generation and the revision.");
}

void testValuesUpdatedDuringAFrameAreNotCoveredByIt() {
  auto schedule = withContent();
  schedule.frameScheduled();
  const auto start = schedule.frameBegan();
  schedule.valuesUpdated();
  schedule.redrawRequested();
  expect(start.revision < schedule.awaitedRevision(),
         "A frame that began before a value update does not satisfy a waiter "
         "for it.");
  expect(schedule.frameFinished(RNSkFrameOutcome::kQueued, start),
         "The update made during the frame asks for the next one.");
}

void testValuesUpdatedDoNotQueueFramesBehindAFullQueueWithoutASurface() {
  auto schedule = withContent();
  fillQueue(schedule);
  schedule.valuesUpdated();
  schedule.redrawRequested();
  expect(!startsWithoutSurface(schedule),
         "Without a surface, an animated value update waits for a present "
         "instead of queueing behind the full queue.");
}

void testAWaitForNewerValuesQueuesAFrameWithoutASurface() {
  auto schedule = withContent();
  fillQueue(schedule);
  schedule.valuesUpdated();
  schedule.redrawRequested();
  expect(schedule.waitRequested(schedule.awaitedRevision()),
         "A wait for values no submitted frame has drawn asks for a frame.");
  expect(startsWithoutSurface(schedule),
         "Without a surface, a frame a caller waits for queues behind the "
         "full queue rather than waiting for a present that never comes.");
}

void testAWaitForNewerValuesStillWaitsForThePresentWithASurface() {
  auto schedule = withContent();
  fillQueue(schedule);
  schedule.valuesUpdated();
  schedule.waitRequested(schedule.awaitedRevision());
  expect(!startsWithSurface(schedule),
         "With a surface, the waited-for frame follows a present as usual.");
}

void testAWaitForNewerValuesStartsAFrameWhileTheQueueHasRoom() {
  auto schedule = withContent();
  queueFrame(schedule);
  schedule.valuesUpdated();
  expect(schedule.waitRequested(schedule.awaitedRevision()),
         "A wait for values no submitted frame has drawn asks for a frame.");
  expect(startsWithSurface(schedule),
         "With room in the queue, that frame starts at once.");
}

void testAWaitForDrawnValuesAsksForNothing() {
  auto schedule = withContent();
  schedule.valuesUpdated();
  fillQueue(schedule);
  expect(!schedule.waitRequested(schedule.awaitedRevision()),
         "A wait for values a submitted frame already drew needs no frame.");
  expect(!startsWithoutSurface(schedule),
         "Without a surface, a covered wait does not queue another frame.");
}

void testOnlyTheWaitedFrameQueuesWithoutASurface() {
  auto schedule = withContent();
  fillQueue(schedule);
  schedule.valuesUpdated();
  schedule.waitRequested(schedule.awaitedRevision());
  queueFrame(schedule);
  expect(schedule.queued() == kMaxQueued + 1,
         "The waited-for frame queues behind the full queue.");
  schedule.valuesUpdated();
  schedule.redrawRequested();
  expect(!startsWithoutSurface(schedule),
         "Once the waited-for frame is queued, later animation steps wait "
         "for a present again.");
}

void testAWaitDoesNotCountAFrameOnARetiredTarget() {
  auto schedule = withContent();
  fillQueue(schedule);
  schedule.valuesUpdated();
  const uint64_t revision = schedule.awaitedRevision();
  schedule.waitRequested(revision);
  runFrame(schedule, RNSkFrameOutcome::kQueuedOnRetiredTarget);
  expect(schedule.waitRequested(revision),
         "A frame that went to a retired target does not cover the wait.");
  expect(startsWithoutSurface(schedule),
         "Without a surface, the waited-for frame still queues on the "
         "current target.");
}

void testSubmittedRevisionNeverGoesBack() {
  auto schedule = withContent();
  schedule.valuesUpdated();
  schedule.valuesUpdated();
  queueFrame(schedule);
  schedule.frameScheduled();
  schedule.frameBegan();
  schedule.frameFinished(RNSkFrameOutcome::kQueued, {1, 3});
  schedule.framesPresented(1);
  schedule.frameScheduled();
  schedule.frameBegan();
  schedule.frameFinished(RNSkFrameOutcome::kQueued, {1, 2});
  expect(!schedule.waitRequested(3),
         "An older frame finishing late does not make newer values look "
         "undrawn.");
}

} // namespace

int main() {
  testAStepThatPresentsRecordsBehindAFullQueue();
  testAStepThatCannotPresentFollowsThePoolGate();
  testAStepNeedsATargetContentAndNoJobInFlight();
  testAFrameThatReachedNoTargetKeepsTheContentDirty();
  testTheQueueKeepsOneFrameAheadOfTheDisplay();
  testStartsWhenNothingWaits();
  testStartsWhileTheQueueHasRoom();
  testNeverStartsWhileInFlight();
  testNeedsATarget();
  testNeedsContent();
  testWaitsForThePresentWhenTheQueueIsFull();
  testNewContentPassesAFullQueueWithoutASurface();
  testRedrawsWaitOnAFullQueueWithoutASurface();
  testNewContentQueuesOneFrameWithoutASurface();
  testContentDuringASubmittedFrameStartsTheNextOne();
  testContentDuringASubmittedFrameStartsTheNextOneWhileThereIsRoom();
  testContentDuringAFrameThatFillsTheQueueWaitsForThePresent();
  testASubmittedFrameWithNothingNewRequestsNothing();
  testARedrawDuringAFrameThatFillsTheQueueIsKept();
  testAFrameThatRecordedNothingStaysDirty();
  testAFrameThatRecordedNothingReportsARequestDuringIt();
  testAFrameOnARetiredTargetIsNotQueued();
  testAFrameOnARetiredTargetDoesNotCountAsSubmitted();
  testTargetReplacedForgetsTheQueue();
  testPresentsFreeQueueSlots();
  testTheQueueStaysBoundedWithASurface();
  testClearedContentIsNotDirty();
  testSubmittedGenerationNeverGoesBack();
  testAwaitedRevision();
  testValuesUpdatedAdvanceTheAwaitedRevision();
  testContentReplacedAdvancesTheRevision();
  testValuesUpdatedDuringAFrameAreNotCoveredByIt();
  testValuesUpdatedDoNotQueueFramesBehindAFullQueueWithoutASurface();
  testAWaitForNewerValuesQueuesAFrameWithoutASurface();
  testAWaitForNewerValuesStillWaitsForThePresentWithASurface();
  testAWaitForNewerValuesStartsAFrameWhileTheQueueHasRoom();
  testAWaitForDrawnValuesAsksForNothing();
  testOnlyTheWaitedFrameQueuesWithoutASurface();
  testAWaitDoesNotCountAFrameOnARetiredTarget();
  testSubmittedRevisionNeverGoesBack();
  if (failures > 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("RNSkFrameScheduleTest passed\n");
  return EXIT_SUCCESS;
}
