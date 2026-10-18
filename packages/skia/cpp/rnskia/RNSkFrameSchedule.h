#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace RNSkia {

enum class RNSkFrameOutcome {
  kQueued,
  kQueuedOnRetiredTarget,
  kNotRecorded,
};

/**
 The bookkeeping of a producer's frames: which content is the newest, whether
 a frame is being recorded, and how many submitted frames still wait to be
 presented. It holds no lock; its owner calls it under its own mutex.
 */
class RNSkFrameSchedule {
public:
  // Recordings submitted to the current target and not presented yet. A job
  // starts while there is room for its recording in the queue.
  static constexpr size_t kMaxQueued = 2;

  /**
   Whether a frame may start recording. Up to kMaxQueued frames wait to be
   presented at a time, so that recording keeps pace with the display. A
   view that cannot present (it has no surface) presents nothing, though:
   there, newer content does not wait for the unpresented frames and queues
   behind them, while redraws of the same content still wait, so that the
   queue only grows by one frame per React commit.
   */
  bool canStart(bool hasTarget, bool hasContent,
                const std::function<bool()> &canPresent) const {
    if (_inFlight || !hasTarget || !hasContent) {
      return false;
    }
    if (_queued < kMaxQueued) {
      return true;
    }
    return _generation > _submittedGeneration && !canPresent();
  }

  /**
   Whether a frame may start recording on the UI thread, in the step of the
   Reanimated mapper. A step whose window presents right after it records
   presents the whole queue in one batch, so it may record while the queue
   is full: the queue then holds one frame more for the length of the step,
   never more, also when the window cannot take the batch. Otherwise the
   gate of canStart() applies.
   */
  bool canStartInStep(bool hasTarget, bool hasContent, bool presentsNow,
                      const std::function<bool()> &canPresent) const {
    if (!presentsNow) {
      return canStart(hasTarget, hasContent, canPresent);
    }
    return !_inFlight && hasTarget && hasContent && _queued <= kMaxQueued;
  }

  /**
   New content replaced the old one. Content that is not dirty, such as a
   cleared view, has nothing to record.
   */
  void contentReplaced(bool dirty) {
    _generation++;
    _dirty = dirty;
  }

  /**
   A redraw of the current content was asked for.
   */
  void redrawRequested() { _dirty = true; }

  /**
   The view was bound to another target. The frames queued on the previous
   one are not coming through this view anymore: none is waited for, and the
   content is recorded again for the new target.
   */
  void targetReplaced() {
    _queued = 0;
    _dirty = true;
  }

  /**
   A frame was scheduled; no other frame starts until it finishes.
   */
  void frameScheduled() { _inFlight = true; }

  /**
   The scheduled frame took its snapshot of the content. Returns the
   generation it records.
   */
  uint64_t frameBegan() {
    _dirty = false;
    return _generation;
  }

  /**
   The frame finished recording. A frame that did not reach the current
   target keeps the content dirty, so that the next request (a surface, a
   resize, the new target) records it. Returns whether content changed or a
   redraw was asked for while the frame was recorded: those requests were
   refused while the frame was in flight, so the owner starts the next frame
   now if the gate allows it.
   */
  bool frameFinished(RNSkFrameOutcome outcome, uint64_t generation) {
    _inFlight = false;
    const bool requested = _dirty;
    switch (outcome) {
    case RNSkFrameOutcome::kQueued:
      _queued++;
      _submittedGeneration = std::max(_submittedGeneration, generation);
      break;
    case RNSkFrameOutcome::kQueuedOnRetiredTarget:
    case RNSkFrameOutcome::kNotRecorded:
      _dirty = true;
      break;
    }
    return requested;
  }

  /**
   The view presented `presented` recordings, every one it had taken from
   the queue: as many frames are on screen. The view may present recordings
   that are not this producer's (a client recording, the last frame shown
   again), hence the clamp. Returns whether the content is dirty and the
   next frame should start.
   */
  bool framesPresented(size_t presented) {
    _queued = presented >= _queued ? 0 : _queued - presented;
    return _dirty;
  }

  /**
   The generation a caller waiting for the current content waits for. A view
   that never had content waits for its first.
   */
  uint64_t awaitedGeneration() const {
    return std::max<uint64_t>(_generation, 1);
  }

  size_t queued() const { return _queued; }

  bool isDirty() const { return _dirty; }

private:
  bool _dirty = false;
  bool _inFlight = false;
  size_t _queued = 0;
  uint64_t _generation = 0;
  uint64_t _submittedGeneration = 0;
};

} // namespace RNSkia
