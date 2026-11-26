#pragma once

#include <optional>
#include <utility>

#include <webgpu/webgpu_cpp.h>

#include "RNDawnFeatureLevel.h"

namespace RNSkia {

/** The level as Dawn's adapter request names it. */
inline wgpu::FeatureLevel toWGPUFeatureLevel(RNSkFeatureLevel level) {
  switch (level) {
  case RNSkFeatureLevel::Core:
    return wgpu::FeatureLevel::Core;
  case RNSkFeatureLevel::Compatibility:
    return wgpu::FeatureLevel::Compatibility;
  }
  return wgpu::FeatureLevel::Core;
}

/**
 The limits a device at `level` requires, or nullopt for none. A core device
 requires none, and `readAdapterLimits` is not called for it, so a core
 device is requested exactly as before the compatibility level existed. A
 compatibility device requires its adapter's color attachment limit (see
 requiredColorAttachments); every other limit keeps the level's default.
 */
template <typename ReadAdapterLimits>
std::optional<wgpu::Limits>
requiredDeviceLimits(RNSkFeatureLevel level,
                     ReadAdapterLimits &&readAdapterLimits) {
  if (level != RNSkFeatureLevel::Compatibility) {
    return std::nullopt;
  }
  const wgpu::Limits adapterLimits =
      std::forward<ReadAdapterLimits>(readAdapterLimits)();
  auto colorAttachments =
      requiredColorAttachments(level, adapterLimits.maxColorAttachments);
  if (!colorAttachments) {
    return std::nullopt;
  }
  wgpu::Limits required{};
  required.maxColorAttachments = *colorAttachments;
  return required;
}

} // namespace RNSkia
