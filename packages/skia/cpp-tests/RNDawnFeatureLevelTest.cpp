#include "RNDawnFeatureLevel.h"
#include "RNSkCompatibilityConfig.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

using RNSkia::chooseAdapter;
using RNSkia::compatibilityRefusal;
using RNSkia::RNSkCompatibilityConfig;
using RNSkia::featureLevelName;
using RNSkia::featureLevelOfDevice;
using RNSkia::requestsTransientAttachments;
using RNSkia::requiredColorAttachments;
using RNSkia::RNSkFeatureLevel;

namespace {

int failures = 0;

void expect(bool condition, const char *description) {
  if (!condition) {
    std::fprintf(stderr, "FAILED: %s\n", description);
    failures++;
  }
}

/**
 A device as Dawn's adapter enumeration sees it: the adapter it has at each
 feature level, if any, and the levels it was asked at, in order.
 */
struct Device {
  std::optional<std::string> core;
  std::optional<std::string> compatibility;
  std::vector<RNSkFeatureLevel> asked;

  int gateCalls = 0;

  std::optional<std::pair<std::string, RNSkFeatureLevel>> choose() {
    return chooseAdapter<std::string>([this](RNSkFeatureLevel level) {
      asked.push_back(level);
      return level == RNSkFeatureLevel::Core ? core : compatibility;
    });
  }

  std::optional<std::pair<std::string, RNSkFeatureLevel>>
  chooseWithGate(bool mayTryCompatibility) {
    return chooseAdapter<std::string>(
        [this](RNSkFeatureLevel level) {
          asked.push_back(level);
          return level == RNSkFeatureLevel::Core ? core : compatibility;
        },
        [this, mayTryCompatibility]() {
          gateCalls++;
          return mayTryCompatibility;
        });
  }
};

void coreAdapterNeverConsultsTheCompatibilityGate() {
  Device device{"Adreno 740", "Adreno 740", {}};
  auto chosen = device.chooseWithGate(false);
  expect(chosen && chosen->second == RNSkFeatureLevel::Core,
         "a device with a core adapter renders at the core level whatever "
         "the gate would say");
  expect(device.gateCalls == 0,
         "a device with a core adapter never consults the compatibility gate");
}

void refusedCompatibilityIsNeverAskedFor() {
  Device device{std::nullopt, "PowerVR Rogue GE8320", {}};
  auto chosen = device.chooseWithGate(false);
  expect(!chosen.has_value(),
         "a device without a core adapter has none when compatibility is "
         "refused");
  expect(device.gateCalls == 1, "the gate is consulted once");
  expect(device.asked == std::vector<RNSkFeatureLevel>{RNSkFeatureLevel::Core},
         "a refused compatibility level is never asked for");
}

void allowedCompatibilityIsAskedForAfterCore() {
  Device device{std::nullopt, "PowerVR Rogue GE8320", {}};
  auto chosen = device.chooseWithGate(true);
  expect(chosen && chosen->second == RNSkFeatureLevel::Compatibility,
         "an allowed compatibility level is chosen when core has none");
  expect(device.gateCalls == 1, "the gate is consulted once");
}

void buildSettingsAllowCompatibilityUnlessTurnedOff() {
  expect(!compatibilityRefusal(RNSkCompatibilityConfig{}).has_value(),
         "the compatibility level is allowed when the app sets nothing");
  RNSkCompatibilityConfig off;
  off.allowed = false;
  auto refusal = compatibilityRefusal(off);
  expect(refusal.has_value() &&
             refusal->find("turned off in this app's build settings") !=
                 std::string::npos,
         "the compatibility level is refused, and says why, when the app's "
         "build turns it off");
}

void coreAdapterIsChosenWithoutAskingForCompatibility() {
  Device device{"Adreno 740", "Adreno 740", {}};
  auto chosen = device.choose();
  expect(chosen.has_value(), "a device with a core adapter has an adapter");
  expect(chosen && chosen->first == "Adreno 740",
         "the core adapter is the one chosen");
  expect(chosen && chosen->second == RNSkFeatureLevel::Core,
         "a device with a core adapter renders at the core level");
  expect(device.asked == std::vector<RNSkFeatureLevel>{RNSkFeatureLevel::Core},
         "compatibility is never asked for when core has an adapter");
}

void compatibilityAdapterIsChosenWhenCoreHasNone() {
  Device device{std::nullopt, "PowerVR Rogue GE8320", {}};
  auto chosen = device.choose();
  expect(chosen.has_value(),
         "a device with only a compatibility adapter has an adapter");
  expect(chosen && chosen->first == "PowerVR Rogue GE8320",
         "the compatibility adapter is the one chosen");
  expect(chosen && chosen->second == RNSkFeatureLevel::Compatibility,
         "a device without a core adapter renders at the compatibility level");
  expect(device.asked ==
             std::vector<RNSkFeatureLevel>{RNSkFeatureLevel::Core,
                                           RNSkFeatureLevel::Compatibility},
         "core is asked first, then compatibility");
}

void noAdapterAtEitherLevelChoosesNone() {
  Device device{std::nullopt, std::nullopt, {}};
  auto chosen = device.choose();
  expect(!chosen.has_value(),
         "a device with no adapter at either level has none");
  expect(device.asked ==
             std::vector<RNSkFeatureLevel>{RNSkFeatureLevel::Core,
                                           RNSkFeatureLevel::Compatibility},
         "both levels are asked before giving up");
}

void levelsAreNamedAsSkiaGpuReportsThem() {
  expect(std::string(featureLevelName(RNSkFeatureLevel::Core)) == "core",
         "the core level is named \"core\"");
  expect(std::string(featureLevelName(RNSkFeatureLevel::Compatibility)) ==
             "compatibility",
         "the compatibility level is named \"compatibility\"");
}

void deviceLevelFollowsCoreFeaturesAndLimits() {
  expect(featureLevelOfDevice(true) == RNSkFeatureLevel::Core,
         "a device with CoreFeaturesAndLimits runs at the core level");
  expect(featureLevelOfDevice(false) == RNSkFeatureLevel::Compatibility,
         "a device without CoreFeaturesAndLimits runs at the compatibility "
         "level");
}

void coreKeepsTheDefaultColorAttachmentLimit() {
  expect(!requiredColorAttachments(RNSkFeatureLevel::Core, 8).has_value(),
         "a core device keeps the level's default color attachment limit");
}

void compatibilityRequestsTheAdapterColorAttachmentLimit() {
  auto limit = requiredColorAttachments(RNSkFeatureLevel::Compatibility, 8);
  expect(limit.has_value() && *limit == 8,
         "a compatibility device requests the adapter's 8 color attachments, "
         "which Dawn's MSAA expand-resolve pipeline declares");
  auto lower = requiredColorAttachments(RNSkFeatureLevel::Compatibility, 4);
  expect(lower.has_value() && *lower == 4,
         "a compatibility device requests no more color attachments than its "
         "adapter reports");
}

void coreKeepsTransientAttachments() {
  expect(requestsTransientAttachments(RNSkFeatureLevel::Core, 8),
         "a core device requests TransientAttachments");
  expect(requestsTransientAttachments(RNSkFeatureLevel::Core, 4),
         "a core device requests TransientAttachments whatever the adapter "
         "reports");
}

void compatibilityKeepsTransientAttachmentsOnlyWithEightTargets() {
  expect(requestsTransientAttachments(RNSkFeatureLevel::Compatibility, 8),
         "a compatibility device whose adapter allows the expand-resolve "
         "pipeline's 8 targets requests TransientAttachments");
  expect(!requestsTransientAttachments(RNSkFeatureLevel::Compatibility, 7),
         "a compatibility device whose adapter allows 7 color attachments "
         "does not, so Graphite emulates the load and store resolve");
  expect(!requestsTransientAttachments(RNSkFeatureLevel::Compatibility, 4),
         "a compatibility device whose adapter allows 4 color attachments "
         "does not, so Graphite emulates the load and store resolve");
}

} // namespace

int main() {
  coreAdapterIsChosenWithoutAskingForCompatibility();
  compatibilityAdapterIsChosenWhenCoreHasNone();
  noAdapterAtEitherLevelChoosesNone();
  coreAdapterNeverConsultsTheCompatibilityGate();
  refusedCompatibilityIsNeverAskedFor();
  allowedCompatibilityIsAskedForAfterCore();
  buildSettingsAllowCompatibilityUnlessTurnedOff();
  levelsAreNamedAsSkiaGpuReportsThem();
  deviceLevelFollowsCoreFeaturesAndLimits();
  coreKeepsTheDefaultColorAttachmentLimit();
  compatibilityRequestsTheAdapterColorAttachmentLimit();
  coreKeepsTransientAttachments();
  compatibilityKeepsTransientAttachmentsOnlyWithEightTargets();
  if (failures > 0) {
    std::fprintf(stderr, "%d expectation(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("RNDawnFeatureLevelTest: all expectations met\n");
  return EXIT_SUCCESS;
}
