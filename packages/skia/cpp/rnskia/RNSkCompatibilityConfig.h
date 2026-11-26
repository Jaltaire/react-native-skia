#pragma once

#include <optional>
#include <string>

namespace RNSkia {

/**
 How the app configures rendering at the WebGPU compatibility level, read by
 the platform before the GPU is probed (see
 RNSkPlatformContext::getCompatibilityConfig).
 */
struct RNSkCompatibilityConfig {
  /**
   Whether the app's build lets Skia render at the compatibility level on a
   device without a core adapter: the Android manifest's
   com.reactnative.skia.COMPATIBILITY_FEATURE_LEVEL meta-data, or the
   Info.plist's RNSkiaCompatibilityFeatureLevel key. True when the key is
   absent.
   */
  bool allowed = true;
};

/**
 Why Skia does not ask for an adapter at the compatibility level, or nullopt
 when it may. Asked only on a device without a core adapter.
 */
inline std::optional<std::string>
compatibilityRefusal(const RNSkCompatibilityConfig &config) {
  if (!config.allowed) {
    return std::string("The WebGPU compatibility level is turned off in this "
                       "app's build settings.");
  }
  return std::nullopt;
}

} // namespace RNSkia
