#include "RNSkFrameSchedule.h"

#include <cstdio>
#include <cstdlib>

using RNSkia::RNSkFrameSchedule;

namespace {

int failures = 0;

void expect(bool condition, const char *description) {
  if (!condition) {
    std::fprintf(stderr, "FAILED: %s\n", description);
    failures++;
  }
}

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

uint64_t runFrame(RNSkFrameSchedule &schedule, bool submitted) {
  schedule.frameScheduled();
  const uint64_t generation = schedule.frameBegan();
  schedule.frameFinished(submitted, generation);
  return generation;
}

RNSkFrameSchedule withContent() {
  RNSkFrameSchedule schedule;
  schedule.contentReplaced(true);
  return schedule;
}

void testStartsWhenNothingWaits() {
  auto schedule = withContent();
  surfaceChecks = 0;
  expect(startsWithSurface(schedule),
         "A frame starts when nothing is in flight or waiting.");
  expect(surfaceChecks == 0,
         "The surface is only consulted while a present is pending.");
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

void testWaitsForThePresentWithASurface() {
  auto schedule = withContent();
  runFrame(schedule, true);
  schedule.contentReplaced(true);
  expect(!startsWithSurface(schedule),
         "New content waits for the pending present when the view can "
         "present it.");
}

void testNewContentPassesWithoutASurface() {
  auto schedule = withContent();
  runFrame(schedule, true);
  schedule.contentReplaced(true);
  expect(startsWithoutSurface(schedule),
         "New content does not wait for a present that a view without a "
         "surface cannot make.");
}

void testRedrawsWaitWithoutASurface() {
  auto schedule = withContent();
  runFrame(schedule, true);
  schedule.redrawRequested();
  surfaceChecks = 0;
  expect(!startsWithoutSurface(schedule),
         "A redraw of content already submitted waits for the present, so "
         "that animations do not pile frames up without a surface.");
  expect(surfaceChecks == 0,
         "The surface is not consulted for content already submitted.");
}

void testContentDuringASubmittedFrameStartsTheNextOne() {
  auto schedule = withContent();
  schedule.frameScheduled();
  const uint64_t first = schedule.frameBegan();
  schedule.contentReplaced(true);
  expect(!startsWithoutSurface(schedule),
         "Content replaced mid-frame is refused while the frame is in "
         "flight.");
  const bool requested = schedule.frameFinished(true, first);
  expect(requested,
         "A submitted frame reports the content that arrived while it was "
         "recorded.");
  expect(startsWithoutSurface(schedule),
         "Without a surface, the content that arrived mid-frame starts the "
         "next frame instead of waiting for a present that never comes.");
  schedule.frameScheduled();
  const uint64_t second = schedule.frameBegan();
  expect(second == first + 1, "The next frame records the newer content.");
  expect(second == schedule.awaitedGeneration(),
         "The next frame records the content a waiter waits for.");
}

void testContentDuringASubmittedFrameWaitsForThePresentWithASurface() {
  auto schedule = withContent();
  schedule.frameScheduled();
  const uint64_t generation = schedule.frameBegan();
  schedule.contentReplaced(true);
  expect(schedule.frameFinished(true, generation),
         "The submitted frame reports the content that arrived mid-frame.");
  expect(!startsWithSurface(schedule),
         "With a surface, that content waits for the present.");
  expect(schedule.framePresented(),
         "The present starts the frame for that content.");
  expect(startsWithSurface(schedule),
         "Once presented, the frame for that content may start.");
}

void testASubmittedFrameWithNothingNewRequestsNothing() {
  auto schedule = withContent();
  schedule.frameScheduled();
  const uint64_t generation = schedule.frameBegan();
  expect(!schedule.frameFinished(true, generation),
         "A submitted frame that nothing changed during requests nothing.");
  expect(!schedule.framePresented(),
         "Its present starts nothing either.");
}

void testARedrawDuringASubmittedFrameIsKept() {
  auto schedule = withContent();
  schedule.frameScheduled();
  const uint64_t generation = schedule.frameBegan();
  schedule.redrawRequested();
  expect(schedule.frameFinished(true, generation),
         "A redraw asked for mid-frame is reported when the frame finishes.");
  expect(!startsWithoutSurface(schedule),
         "Without a surface, that redraw still waits for the present.");
  expect(schedule.framePresented(),
         "The present starts that redraw.");
}

void testAFrameThatRecordedNothingStaysDirty() {
  auto schedule = withContent();
  schedule.frameScheduled();
  const uint64_t generation = schedule.frameBegan();
  expect(!schedule.frameFinished(false, generation),
         "A frame that recorded nothing, with no request during it, starts "
         "nothing.");
  expect(!schedule.isPresentPending(),
         "A frame that recorded nothing leaves no present pending.");
  expect(startsWithSurface(schedule),
         "The next request may start a frame right away.");
  schedule.frameScheduled();
  const uint64_t retried = schedule.frameBegan();
  expect(retried == generation,
         "The retried frame records the same content.");
}

void testAFrameThatRecordedNothingReportsARequestDuringIt() {
  auto schedule = withContent();
  schedule.frameScheduled();
  const uint64_t generation = schedule.frameBegan();
  schedule.redrawRequested();
  expect(schedule.frameFinished(false, generation),
         "A request during a frame that recorded nothing starts the next "
         "frame.");
}

void testAPresentClearsThePendingFrame() {
  auto schedule = withContent();
  runFrame(schedule, true);
  expect(schedule.isPresentPending(), "A submitted frame waits for a present.");
  schedule.framePresented();
  expect(!schedule.isPresentPending(), "The present clears the wait.");
}

void testClearedContentIsNotDirty() {
  auto schedule = withContent();
  runFrame(schedule, true);
  schedule.contentReplaced(false);
  expect(!schedule.framePresented(),
         "Cleared content has nothing to record when the present lands.");
}

void testSubmittedGenerationNeverGoesBack() {
  auto schedule = withContent();
  schedule.contentReplaced(true);
  schedule.frameScheduled();
  schedule.frameBegan();
  schedule.frameFinished(true, 2);
  schedule.framePresented();
  schedule.frameScheduled();
  schedule.frameBegan();
  schedule.frameFinished(true, 1);
  expect(!startsWithoutSurface(schedule),
         "An older frame finishing late does not make the newer content "
         "look unsubmitted.");
}

void testAwaitedGeneration() {
  RNSkFrameSchedule schedule;
  expect(schedule.awaitedGeneration() == 1,
         "A view that never had content waits for its first.");
  schedule.contentReplaced(true);
  schedule.contentReplaced(true);
  expect(schedule.awaitedGeneration() == 2,
         "A waiter waits for the newest content.");
}

} // namespace

int main() {
  testStartsWhenNothingWaits();
  testNeverStartsWhileInFlight();
  testNeedsATarget();
  testNeedsContent();
  testWaitsForThePresentWithASurface();
  testNewContentPassesWithoutASurface();
  testRedrawsWaitWithoutASurface();
  testContentDuringASubmittedFrameStartsTheNextOne();
  testContentDuringASubmittedFrameWaitsForThePresentWithASurface();
  testASubmittedFrameWithNothingNewRequestsNothing();
  testARedrawDuringASubmittedFrameIsKept();
  testAFrameThatRecordedNothingStaysDirty();
  testAFrameThatRecordedNothingReportsARequestDuringIt();
  testAPresentClearsThePendingFrame();
  testClearedContentIsNotDirty();
  testSubmittedGenerationNeverGoesBack();
  testAwaitedGeneration();
  if (failures > 0) {
    std::fprintf(stderr, "%d failure(s)\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("RNSkFrameScheduleTest passed\n");
  return EXIT_SUCCESS;
}
