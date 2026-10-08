#pragma once

#include <algorithm>
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

/**
 The bookkeeping of a producer's frames: which content is the newest, whether
 a frame is being recorded, and whether a submitted frame still waits to be
 presented. It holds no lock; its owner calls it under its own mutex.
 */
class RNSkFrameSchedule {
public:
  /**
   Whether a frame may start recording. One frame waits to be presented at a
   time, so that recording keeps pace with the display. A view without a
   surface presents nothing, though: there, newer content does not wait for
   the unpresented frame and queues behind it, while redraws of the same
   content still wait, so that the queue only grows by one frame per React
   commit.
   */
  bool canStart(bool hasTarget, bool hasContent,
                const std::function<bool()> &hasSurface) const {
    if (_inFlight || !hasTarget || !hasContent) {
      return false;
    }
    if (!_presentPending) {
      return true;
    }
    return _generation > _submittedGeneration && !hasSurface();
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
   The frame finished recording. A frame that recorded nothing keeps its
   content dirty, so that the next request (a surface, a resize) records it.
   Returns whether content changed or a redraw was asked for while the frame
   was recorded: those requests were refused while the frame was in flight,
   so the owner starts the next frame now if the gate allows it.
   */
  bool frameFinished(bool submitted, uint64_t generation) {
    _inFlight = false;
    const bool requested = _dirty;
    if (submitted) {
      _presentPending = true;
      _submittedGeneration = std::max(_submittedGeneration, generation);
    } else {
      _dirty = true;
    }
    return requested;
  }

  /**
   The submitted frame is on screen. Returns whether the content is dirty
   and the next frame should start.
   */
  bool framePresented() {
    _presentPending = false;
    return _dirty;
  }

  /**
   The revision a caller waiting for what the view shows now waits for: the
   current content with its animated values as they are. A view that never
   had content waits for its first.
   */
  uint64_t awaitedRevision() const {
    return std::max<uint64_t>(_revision, 1);
  }

  bool isPresentPending() const { return _presentPending; }

private:
  bool _dirty = false;
  bool _inFlight = false;
  bool _presentPending = false;
  uint64_t _generation = 0;
  uint64_t _submittedGeneration = 0;
  uint64_t _revision = 0;
};

} // namespace RNSkia
