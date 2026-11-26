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
  /**
   Where Skia records an attempt at the compatibility level until its first
   frame is presented (see RNSkCompatibilityGuard): a directory of the app's
   that is kept out of backups. Empty when the platform has none, and then
   the level is not tried.
   */
  std::string guardDirectory;
  /**
   The app's version and build, which an attempt is recorded under: an
   update tries a level that failed on the version before.
   */
  std::string appVersion;
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
