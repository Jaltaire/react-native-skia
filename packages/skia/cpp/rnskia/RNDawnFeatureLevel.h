#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <utility>

namespace RNSkia {

/**
 The WebGPU feature levels Skia asks Dawn for an adapter at, in order. Core is
 asked first, and a device that has a core adapter renders on it exactly as
 before. Compatibility is asked only when there is none: on drivers that fall
 short of a core limit or feature but meet the compatibility ones (for
 example, a 2D texture limit of 4096 against core's 8192, as PowerVR Rogue
 GE8300, GE8320, and GE8322 report on vulkan.gpuinfo.org). Graphite reads the
 device's limits and takes its texture limit from them.
 */
enum class RNSkFeatureLevel { Core, Compatibility };

inline constexpr std::array<RNSkFeatureLevel, 2> kRNSkFeatureLevels = {
    RNSkFeatureLevel::Core, RNSkFeatureLevel::Compatibility};

/** The level's name as Skia.gpu reports it. */
inline const char *featureLevelName(RNSkFeatureLevel level) {
  switch (level) {
  case RNSkFeatureLevel::Core:
    return "core";
  case RNSkFeatureLevel::Compatibility:
    return "compatibility";
  }
  return "";
}

/**
 The level a device runs at, from whether it has CoreFeaturesAndLimits: Dawn
 enables that feature on every device created from a core adapter, and on no
 other.
 */
inline RNSkFeatureLevel featureLevelOfDevice(bool hasCoreFeaturesAndLimits) {
  return hasCoreFeaturesAndLimits ? RNSkFeatureLevel::Core
                                  : RNSkFeatureLevel::Compatibility;
}

/**
 The color attachment limit to request for a device at `level`, or nullopt to
 keep the level's default. At the compatibility level the default is 4, while
 Dawn's own MSAA expand-resolve pipeline declares kDawnExpandResolveColorTargets
 (8) targets, so every pass that loads an MSAA attachment fails validation
 there; the adapter's own limit is requested instead. An adapter that allows
 fewer than 8 also gives up TransientAttachments (see
 requestsTransientAttachments). At the core level the default is kept, so a
 core device is created exactly as before.
 */
inline std::optional<uint32_t>
requiredColorAttachments(RNSkFeatureLevel level,
                         uint32_t adapterMaxColorAttachments) {
  switch (level) {
  case RNSkFeatureLevel::Core:
    return std::nullopt;
  case RNSkFeatureLevel::Compatibility:
    return adapterMaxColorAttachments;
  }
  return std::nullopt;
}

/**
 The color targets Dawn's own MSAA expand-resolve pipeline declares: one per
 possible attachment (on Vulkan, GetOrCreateColorBlitPipeline in
 ResolveTextureLoadingUtilsVk.cpp), whatever the pass attaches.
 */
inline constexpr uint32_t kDawnExpandResolveColorTargets = 8;

/**
 Whether a device at `level` requests TransientAttachments. With it, Graphite
 loads MSAA attachments through Dawn's expand-resolve pipeline, which a device
 allowing fewer than kDawnExpandResolveColorTargets color attachments refuses
 whenever Dawn validates (debug builds, and after a device loss or an
 out-of-memory error). Without it, Graphite emulates the load and store
 resolve and never asks Dawn for that pipeline. A core device allows eight
 and keeps the feature.
 */
inline bool requestsTransientAttachments(RNSkFeatureLevel level,
                                         uint32_t adapterMaxColorAttachments) {
  switch (level) {
  case RNSkFeatureLevel::Core:
    return true;
  case RNSkFeatureLevel::Compatibility:
    return adapterMaxColorAttachments >= kDawnExpandResolveColorTargets;
  }
  return true;
}

/**
 The adapter `find` returns at the first level, in kRNSkFeatureLevels order,
 where it returns one, with that level; nullopt when it returns none at any
 level. `find` is not asked at a lower level once a higher one has an adapter.
 */
template <typename Adapter, typename Find>
std::optional<std::pair<Adapter, RNSkFeatureLevel>> chooseAdapter(Find &&find) {
  for (auto level : kRNSkFeatureLevels) {
    std::optional<Adapter> adapter = find(level);
    if (adapter) {
      return std::make_pair(std::move(*adapter), level);
    }
  }
  return std::nullopt;
}

} // namespace RNSkia
