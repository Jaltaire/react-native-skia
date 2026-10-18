#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <mutex>
#include <utility>
#include <vector>

namespace RNSkia {

class RNSkFrameReadyWaiters {
public:
  void wait(uint64_t generation, std::function<void()> callback) {
    {
      std::lock_guard<std::mutex> lock(_mutex);
      if (_ready < generation) {
        _waiters.emplace_back(generation, std::move(callback));
        return;
      }
    }
    callback();
  }

  /**
   A frame of the generation was queued on the view's target while it was
   bound in the given epoch (see epoch()). A frame queued on a target the
   view has been unbound from since resolves nothing.
   */
  void ready(uint64_t generation, uint64_t epoch) {
    std::vector<std::function<void()>> ready;
    {
      std::lock_guard<std::mutex> lock(_mutex);
      if (epoch != _epoch) {
        return;
      }
      _ready = std::max(_ready, generation);
      auto keep = std::stable_partition(
          _waiters.begin(), _waiters.end(),
          [this](const auto &waiter) { return waiter.first > _ready; });
      for (auto it = keep; it != _waiters.end(); ++it) {
        ready.push_back(std::move(it->second));
      }
      _waiters.erase(keep, _waiters.end());
    }
    for (auto &callback : ready) {
      callback();
    }
  }

  /**
   The view was bound to another target. A frame that was queued on the
   previous one, but whose readiness is only reported now, resolves nothing:
   the frame is not coming through this view anymore. The owner reads the
   epoch of a frame (see epoch()) under the same lock it calls this under,
   so that the check of ready() is atomic with the replacement.
   */
  void targetReplaced() {
    std::lock_guard<std::mutex> lock(_mutex);
    _epoch++;
  }

  /** The number of target replacements so far. */
  uint64_t epoch() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _epoch;
  }

  uint64_t readyGeneration() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _ready;
  }

  size_t pendingCount() {
    std::lock_guard<std::mutex> lock(_mutex);
    return _waiters.size();
  }

private:
  std::mutex _mutex;
  uint64_t _ready = 0;
  uint64_t _epoch = 0;
  std::vector<std::pair<uint64_t, std::function<void()>>> _waiters;
};

} // namespace RNSkia
