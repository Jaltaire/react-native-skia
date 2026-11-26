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
 Why Skia does not use the compatibility level, or nullopt when it may: the
 end of a sentence that compatibilityRefusedMessage begins. Asked only on a
 device without a core adapter.
 */
inline std::optional<std::string>
compatibilityRefusal(const RNSkCompatibilityConfig &config) {
  if (!config.allowed) {
    return std::string(
        "the level is turned off in this app's build settings "
        "(com.reactnative.skia.COMPATIBILITY_FEATURE_LEVEL in the Android "
        "manifest, RNSkiaCompatibilityFeatureLevel in Info.plist). It is "
        "tried again once the setting is removed or set to true.");
  }
  return std::nullopt;
}

/**
 What Skia.gpu.error says when a device has an adapter at the compatibility
 level only and Skia did not use it: the adapter found, and `reason` (see
 compatibilityRefusal, compatibilityStopReason).
 */
inline std::string compatibilityRefusedMessage(const std::string &backend,
                                               const std::string &device,
                                               const std::string &description,
                                               const std::string &reason) {
  std::string adapter = backend + " adapter " + device;
  if (!description.empty()) {
    adapter += " (" + description + ")";
  }
  return "React Native Skia found the " + adapter +
         " only at the WebGPU compatibility feature level, and did not use "
         "it: " +
         reason;
}

} // namespace RNSkia
