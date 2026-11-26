#pragma once

#include <fbjni/fbjni.h>
#include <jsi/jsi.h>

#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <utility>

#include "RNSkPlatformContext.h"

class SkStreamAsset;
namespace RNSkia {

namespace jsi = facebook::jsi;
namespace jni = facebook::jni;

class JniPlatformContext : public jni::HybridClass<JniPlatformContext> {
public:
  static auto constexpr kJavaDescriptor =
      "Lcom/reactnative/skia/PlatformContext;";

  static jni::local_ref<jhybriddata>
  initHybrid(jni::alias_ref<jhybridobject> jThis, const float,
             std::string cacheDirectory, bool compatibilityFeatureLevelAllowed,
             std::string compatibilityGuardDirectory, std::string appVersion);

  static void registerNatives();

  void performStreamOperation(
      const std::string &sourceUri,
      const std::function<void(std::unique_ptr<SkStreamAsset>)> &op);

  void raiseError(const std::exception &err);

  void notifyTaskReadyExternal();

  void runTaskOnMainThread(std::function<void()> task);

  void notifyTaskReadyNative();

  float getPixelDensity() { return _pixelDensity; }

  const std::string &getCacheDirectory() const { return _cacheDirectory; }

  bool compatibilityFeatureLevelAllowed() const {
    return _compatibilityFeatureLevelAllowed;
  }

  const std::string &getCompatibilityGuardDirectory() const {
    return _compatibilityGuardDirectory;
  }

  const std::string &getAppVersion() const { return _appVersion; }

private:
  friend HybridBase;
  jni::global_ref<JniPlatformContext::javaobject> javaPart_;

  float _pixelDensity;
  std::string _cacheDirectory;
  bool _compatibilityFeatureLevelAllowed;
  std::string _compatibilityGuardDirectory;
  std::string _appVersion;
  std::mutex _mainThreadTasksMutex;
  std::queue<std::function<void()>> _mainThreadTasks;
  bool _mainThreadDispatchScheduled = false;

  explicit JniPlatformContext(
      jni::alias_ref<JniPlatformContext::jhybridobject> jThis,
      const float pixelDensity, std::string cacheDirectory,
      bool compatibilityFeatureLevelAllowed,
      std::string compatibilityGuardDirectory, std::string appVersion)
      : javaPart_(jni::make_global(jThis)), _pixelDensity(pixelDensity),
        _cacheDirectory(std::move(cacheDirectory)),
        _compatibilityFeatureLevelAllowed(compatibilityFeatureLevelAllowed),
        _compatibilityGuardDirectory(std::move(compatibilityGuardDirectory)),
        _appVersion(std::move(appVersion)) {}
};
} // namespace RNSkia
