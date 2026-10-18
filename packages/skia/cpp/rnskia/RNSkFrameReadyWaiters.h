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

  void ready(uint64_t generation) {
    std::vector<std::function<void()>> ready;
    {
      std::lock_guard<std::mutex> lock(_mutex);
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
  std::vector<std::pair<uint64_t, std::function<void()>>> _waiters;
};

} // namespace RNSkia
