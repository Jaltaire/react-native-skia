// Everything that needs a complete Recorder lives here rather than in
// RNSkGraphiteProducer.h (which the view headers include on their own).
#include "RNSkGraphiteProducer.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

#include "RNSkGraphiteTarget.h"
#include "RNSkThreadPool.h"
#include "api/recorder/DrawingCtx.h"
#include "api/recorder/RNRecorder.h"
#include "utils/RNSkLog.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPicture.h"

namespace RNSkia {

RNSkGraphiteProducer::~RNSkGraphiteProducer() = default;

void RNSkGraphiteProducer::drawContent(SkCanvas *canvas, Recorder *recorder,
                                       const sk_sp<SkPicture> &picture,
                                       float pixelDensity) {
  canvas->clear(SK_ColorTRANSPARENT);
  canvas->save();
  canvas->scale(pixelDensity, pixelDensity);
  if (recorder != nullptr) {
    DrawingCtx ctx(canvas);
    recorder->play(&ctx);
  } else if (picture != nullptr) {
    canvas->drawPicture(picture);
  }
  canvas->restore();
}

void RNSkGraphiteProducer::setTarget(
    std::shared_ptr<RNSkGraphiteTarget> target) {
  std::shared_ptr<RNSkGraphiteTarget> retiredTarget;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    retiredTarget = std::exchange(_target, std::move(target));
    // A frame still queued on the previous target is not coming through this
    // view anymore (the view unbound itself from it): do not wait for it.
    _schedule.targetReplaced();
    _readyWaiters.targetReplaced();
    kickLocked();
  }
}

void RNSkGraphiteProducer::setRecorder(std::shared_ptr<Recorder> recorder) {
  sk_sp<SkPicture> picture;
  if (recorder != nullptr && recorder->variables.empty()) {
    picture = recorder->makePicture();
    recorder = nullptr;
  }
  replaceContent(std::move(recorder), std::move(picture), /* dirty= */ true);
}

void RNSkGraphiteProducer::setPicture(sk_sp<SkPicture> picture) {
  replaceContent(nullptr, std::move(picture), /* dirty= */ true);
}

void RNSkGraphiteProducer::replaceContent(std::shared_ptr<Recorder> recorder,
                                          sk_sp<SkPicture> picture,
                                          bool dirty) {
  std::shared_ptr<Recorder> retiredRecorder;
  sk_sp<SkPicture> retiredPicture;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    retiredRecorder = std::exchange(_recorder, std::move(recorder));
    retiredPicture = std::exchange(_picture, std::move(picture));
    _schedule.contentReplaced(dirty);
    if (dirty) {
      kickLocked();
    }
  }
  // Both are released here, outside the lock.
}

bool RNSkGraphiteProducer::hasContent() {
  std::lock_guard<std::mutex> lock(_mutex);
  return _recorder != nullptr || _picture != nullptr;
}

bool RNSkGraphiteProducer::applyUpdatesTo(
    const std::shared_ptr<Recorder> &recorder, jsi::Runtime &runtime,
    double recorderId, const jsi::Array &values) {
  if (recorder == nullptr || recorder->id != recorderId) {
    return false;
  }
  // The values are written into the commands by the next replay, which the
  // caller schedules: the mapper never waits for a draw.
  recorder->readUpdates(runtime, values);
  return true;
}

bool RNSkGraphiteProducer::applyUpdates(jsi::Runtime &runtime,
                                        double recorderId,
                                        const jsi::Array &values) {
  std::shared_ptr<Recorder> recorder;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    recorder = _recorder;
  }
  // Outside the lock: a commit replacing the recorder must not wait for the
  // read. Should it land while this runs, the values go into the retired
  // recorder and the commit's own frame draws the new one.
  if (!applyUpdatesTo(recorder, runtime, recorderId, values)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(_mutex);
  _schedule.valuesUpdated();
  _schedule.redrawRequested();
  kickLocked();
  return true;
}

bool RNSkGraphiteProducer::readUpdates(jsi::Runtime &runtime, double recorderId,
                                       const jsi::Array &values) {
  std::shared_ptr<Recorder> recorder;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    recorder = _recorder;
  }
  if (!applyUpdatesTo(recorder, runtime, recorderId, values)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(_mutex);
  _schedule.valuesUpdated();
  return true;
}

bool RNSkGraphiteProducer::produceInStep(bool presentsNow) {
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!canStartInStepLocked(presentsNow)) {
      return false;
    }
    _schedule.frameScheduled();
  }
  return produce();
}

void RNSkGraphiteProducer::whenFrameReady(std::function<void()> callback) {
  uint64_t revision;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    revision = _schedule.awaitedRevision();
  }
  _readyWaiters.wait(revision, std::move(callback));
  std::lock_guard<std::mutex> lock(_mutex);
  if (_schedule.waitRequested(revision)) {
    kickLocked();
  }
}

bool RNSkGraphiteProducer::requestFrame() {
  std::lock_guard<std::mutex> lock(_mutex);
  _schedule.redrawRequested();
  kickLocked();
  return _target != nullptr && (_recorder != nullptr || _picture != nullptr);
}

void RNSkGraphiteProducer::onFramePresented(size_t presented) {
  std::lock_guard<std::mutex> lock(_mutex);
  if (_schedule.framesPresented(presented)) {
    kickLocked();
  }
}

bool RNSkGraphiteProducer::canStartFrameLocked() {
  const auto target = _target;
  return _schedule.canStart(target != nullptr,
                            _recorder != nullptr || _picture != nullptr,
                            [&target]() { return target->canPresent(); });
}

bool RNSkGraphiteProducer::canStartInStepLocked(bool presentsNow) {
  const auto target = _target;
  return _schedule.canStartInStep(
      target != nullptr, _recorder != nullptr || _picture != nullptr,
      presentsNow, [&target]() { return target->canPresent(); });
}

void RNSkGraphiteProducer::kickLocked() {
  if (!canStartFrameLocked()) {
    return;
  }
  _schedule.frameScheduled();
  std::weak_ptr<RNSkGraphiteProducer> weakThis = weak_from_this();
  RNSkThreadPool::getInstance().post([weakThis]() {
    if (auto self = weakThis.lock()) {
      self->produce();
    }
  });
}

bool RNSkGraphiteProducer::produce() {
  std::shared_ptr<RNSkGraphiteTarget> target;
  std::shared_ptr<Recorder> recorder;
  sk_sp<SkPicture> picture;
  RNSkFrameStart start{};
  {
    std::lock_guard<std::mutex> lock(_mutex);
    target = _target;
    recorder = _recorder;
    picture = _picture;
    start = _schedule.frameBegan();
  }
  std::shared_ptr<RNSkGraphiteRecording> recording;
  if (target && (recorder || picture)) {
    SkCanvas *canvas = nullptr;
    try {
      canvas = target->beginRecording();
    } catch (const std::exception &) {
      // No surface and no layout yet: the view asks for a frame once it
      // has a size.
    }
    if (canvas != nullptr) {
      try {
        // The deferred canvas already draws in points.
        drawContent(canvas, recorder.get(), picture, /* pixelDensity= */ 1.0f);
      } catch (const std::exception &e) {
        RNSkLogger::logToConsole("Canvas: replaying the scene failed: %s",
                                 e.what());
      }
      try {
        recording = target->finishRecording();
      } catch (const std::exception &e) {
        RNSkLogger::logToConsole("Canvas: recording the frame failed: %s",
                                 e.what());
      }
    }
  }
  if (recording != nullptr && !recording->waitForPipelines()) {
    RNSkLogger::logToConsole(
        "Canvas: a pipeline of the frame failed to compile.");
  }
  const auto epoch = finishFrame(target, std::move(recording), start);
  if (!epoch.has_value()) {
    return false;
  }
  _readyWaiters.ready(start.revision, *epoch);
  return true;
}

std::optional<uint64_t> RNSkGraphiteProducer::finishFrame(
    const std::shared_ptr<RNSkGraphiteTarget> &target,
    std::shared_ptr<RNSkGraphiteRecording> recording, RNSkFrameStart start) {
  std::lock_guard<std::mutex> lock(_mutex);
  auto outcome = RNSkFrameOutcome::kNotRecorded;
  if (recording != nullptr) {
    // Queued even if the view was bound to another target while this job
    // ran: a target never drops a recording, a later one on the same
    // recorder may depend on resources this one uploads. Submitted under the
    // lock: a frame presented in between (a redraw replaying the last one)
    // would otherwise free a queue slot before the recording is even queued.
    target->submit(std::move(recording));
    // A frame that went to the previous target leaves the new one with none
    // yet.
    outcome = target == _target ? RNSkFrameOutcome::kQueued
                                : RNSkFrameOutcome::kQueuedOnRetiredTarget;
  }
  // Values that arrived during the replay start the next job right away,
  // while this frame waits for its vsync, as long as the gate allows it. A
  // frame that did not reach the current target keeps the content dirty so
  // that the next request (a surface, a resize) records it; a request that
  // landed while this job ran (binding a new target is one) starts the next
  // job now.
  if (_schedule.frameFinished(outcome, start)) {
    kickLocked();
  }
  if (outcome != RNSkFrameOutcome::kQueued) {
    return std::nullopt;
  }
  return _readyWaiters.epoch();
}

void RNSkGraphiteProducer::renderInto(SkCanvas *canvas, float pixelDensity) {
  std::shared_ptr<Recorder> recorder;
  sk_sp<SkPicture> picture;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    recorder = _recorder;
    picture = _picture;
  }
  drawContent(canvas, recorder.get(), picture, pixelDensity);
}

} // namespace RNSkia
