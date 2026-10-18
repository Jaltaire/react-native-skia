#pragma once

#include <cstddef>
#include <vector>

namespace RNSkia {

/**
 Settles a batch of recordings a view took from its target's queue, once it
 tried to present them: the first `done` went through (presented, or
 skipped as unpresentable), and the window could not take the one after
 them (no drawable). That one and the ones behind it go back to the front
 of the queue. The producer is told how many are done on every attempt,
 also when the window declined the ones behind them: each frees a slot of
 its queue, and a slot left taken by a recording that is gone would stop
 the producer once two of them add up, with nothing queued to present (see
 RNSkFrameSchedule::framesPresented). Returns whether the whole batch went
 through. Holds no Graphite dependencies, so that the producer and the view
 are tested together.
 */
template <typename Recording, typename Requeue, typename Acknowledge>
bool settlePresentedBatch(const std::vector<Recording> &recordings, size_t done,
                          Requeue &&requeue, Acknowledge &&acknowledge) {
  if (done < recordings.size()) {
    requeue(
        std::vector<Recording>(recordings.begin() + done, recordings.end()));
  }
  if (done > 0) {
    acknowledge(done);
  }
  return done == recordings.size();
}

} // namespace RNSkia
