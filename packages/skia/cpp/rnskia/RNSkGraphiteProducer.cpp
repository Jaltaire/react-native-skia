// Everything that needs a complete Recorder lives here rather than in
// RNSkGraphiteProducer.h (which the view headers include on their own).
#include "RNSkGraphiteProducer.h"

#include <algorithm>
#include <memory>
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
  std::lock_guard<std::mutex> lock(_mutex);
  _target = std::move(target);
  _schedule.redrawRequested();
  kickLocked();
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

void RNSkGraphiteProducer::clear() {
  replaceContent(nullptr, nullptr, /* dirty= */ false);
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

bool RNSkGraphiteProducer::produceNow() {
  {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!canStartFrameLocked()) {
      return false;
    }
    _schedule.frameScheduled();
  }
  produce();
  std::lock_guard<std::mutex> lock(_mutex);
  return _schedule.isPresentPending();
}

void RNSkGraphiteProducer::whenFrameReady(std::function<void()> callback) {
  uint64_t revision;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    revision = _schedule.awaitedRevision();
  }
  _readyWaiters.wait(revision, std::move(callback));
}

bool RNSkGraphiteProducer::requestFrame() {
  std::lock_guard<std::mutex> lock(_mutex);
  _schedule.redrawRequested();
  kickLocked();
  return _target != nullptr && (_recorder != nullptr || _picture != nullptr);
}

void RNSkGraphiteProducer::onFramePresented() {
  std::lock_guard<std::mutex> lock(_mutex);
  if (_schedule.framePresented()) {
    kickLocked();
  }
}

bool RNSkGraphiteProducer::canStartFrameLocked() {
  const auto target = _target;
  return _schedule.canStart(target != nullptr,
                            _recorder != nullptr || _picture != nullptr,
                            [&target]() { return target->hasSurface(); });
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

void RNSkGraphiteProducer::produce() {
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
  const bool submitted = recording != nullptr;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    // Submitted under the lock: a frame presented in between (a redraw
    // replaying the last one) would otherwise clear the pending present
    // before the recording is even queued.
    const bool requested =
        _schedule.frameFinished(submitted, start.generation);
    if (submitted) {
      target->submit(std::move(recording));
    }
    if (requested) {
      kickLocked();
    }
  }
  if (submitted) {
    _readyWaiters.ready(start.revision);
  }
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
