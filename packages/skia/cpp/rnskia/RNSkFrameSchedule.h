#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>

namespace RNSkia {

/**
 What a frame records: the content it replays (its generation) and the
 revision of the view at that point, which also counts animated value
 updates.
 */
struct RNSkFrameStart {
  uint64_t generation;
  uint64_t revision;
};

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
   behind them, and so do animated values a caller waits for, while other
   redraws of the same content still wait, so that the queue only grows by
   one frame per React commit or wait.
   */
  bool canStart(bool hasTarget, bool hasContent,
                const std::function<bool()> &canPresent) const {
    if (_inFlight || !hasTarget || !hasContent) {
      return false;
    }
    if (_queued < kMaxQueued) {
      return true;
    }
    const bool newer = _generation > _submittedGeneration ||
                       _waitedRevision > _submittedRevision;
    return newer && !canPresent();
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
    _revision++;
    _dirty = dirty;
  }

  /**
   Animated values of the current content changed: a frame recorded from now
   on draws them. The content itself stays the same generation, so that a view
   without a surface still queues one frame per React commit rather than one
   per animation step.
   */
  void valuesUpdated() { _revision++; }

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
   generation and the revision it records.
   */
  RNSkFrameStart frameBegan() {
    _dirty = false;
    return {_generation, _revision};
  }

  /**
   The frame finished recording. A frame that did not reach the current
   target keeps the content dirty, so that the next request (a surface, a
   resize, the new target) records it. Returns whether content changed or a
   redraw was asked for while the frame was recorded: those requests were
   refused while the frame was in flight, so the owner starts the next frame
   now if the gate allows it.
   */
  bool frameFinished(RNSkFrameOutcome outcome, RNSkFrameStart start) {
    _inFlight = false;
    const bool requested = _dirty;
    switch (outcome) {
    case RNSkFrameOutcome::kQueued:
      _queued++;
      _submittedGeneration = std::max(_submittedGeneration, start.generation);
      _submittedRevision = std::max(_submittedRevision, start.revision);
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
   The revision a caller waiting for what the view shows now waits for: the
   current content with its animated values as they are. A view that never
   had content waits for its first.
   */
  uint64_t awaitedRevision() const { return std::max<uint64_t>(_revision, 1); }

  /**
   A caller waits for a frame of the revision. Returns whether no submitted
   frame covers it yet, in which case the owner starts one if the gate
   allows it: the wait lets a view without a surface queue that frame.
   */
  bool waitRequested(uint64_t revision) {
    _waitedRevision = std::max(_waitedRevision, revision);
    return revision > _submittedRevision;
  }

  size_t queued() const { return _queued; }

  bool isDirty() const { return _dirty; }

private:
  bool _dirty = false;
  bool _inFlight = false;
  size_t _queued = 0;
  uint64_t _generation = 0;
  uint64_t _submittedGeneration = 0;
  uint64_t _revision = 0;
  uint64_t _submittedRevision = 0;
  uint64_t _waitedRevision = 0;
};

} // namespace RNSkia
