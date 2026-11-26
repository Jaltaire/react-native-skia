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
 example, a 2D texture limit of 4096 against core's 8192, as on PowerVR Rogue
 GE8300, GE8320, and GE8322). Graphite reads the device's limits and adapts to
 the compatibility ones.
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
 The color attachment limit to request for a device at `level`, or nullopt to
 keep the level's default. At the compatibility level the default is 4, while
 Dawn's own MSAA expand-resolve pipeline (BlitColorToColorWithDraw) declares
 one target per possible attachment, 8, so every pass that loads an MSAA
 attachment fails validation there; the adapter's own limit (8 on the drivers
 this level is for) is requested instead. At the core level the default is
 kept, so a core device is created exactly as before.
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
