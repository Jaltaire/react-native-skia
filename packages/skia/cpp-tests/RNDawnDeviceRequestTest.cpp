#include "RNDawnDeviceRequest.h"

#include <cstdio>
#include <cstdlib>

using RNSkia::requiredDeviceLimits;
using RNSkia::RNSkFeatureLevel;
using RNSkia::toWGPUFeatureLevel;

namespace {

int failures = 0;

void expect(bool condition, const char *description) {
  if (!condition) {
    std::fprintf(stderr, "FAILED: %s\n", description);
    failures++;
  }
}

/** An adapter's limits as Dawn reports them, and how often they were read. */
struct Adapter {
  uint32_t maxColorAttachments;
  int reads = 0;

  wgpu::Limits limits() {
    reads++;
    wgpu::Limits limits{};
    limits.maxColorAttachments = maxColorAttachments;
    limits.maxTextureDimension2D = 4096;
    return limits;
  }
};

void coreDeviceRequiresNoLimitsAndNeverReadsTheAdapter() {
  Adapter adapter{8};
  auto limits = requiredDeviceLimits(RNSkFeatureLevel::Core,
                                     [&adapter]() { return adapter.limits(); });
  expect(!limits.has_value(),
         "a core device is requested without required limits");
  expect(adapter.reads == 0,
         "the adapter's limits are not read for a core device");
}

void compatibilityDeviceRequiresTheAdapterColorAttachmentsOnly() {
  Adapter adapter{8};
  auto limits = requiredDeviceLimits(RNSkFeatureLevel::Compatibility,
                                     [&adapter]() { return adapter.limits(); });
  expect(adapter.reads == 1,
         "the adapter's limits are read once for a compatibility device");
  expect(limits.has_value() && limits->maxColorAttachments == 8,
         "a compatibility device requires its adapter's 8 color attachments");
  expect(limits.has_value() &&
             limits->maxTextureDimension2D == WGPU_LIMIT_U32_UNDEFINED,
         "every other limit of a compatibility device keeps the level's "
         "default, the adapter's texture limit included");
}

void compatibilityDeviceRequiresNoMoreThanTheAdapterReports() {
  Adapter adapter{4};
  auto limits = requiredDeviceLimits(RNSkFeatureLevel::Compatibility,
                                     [&adapter]() { return adapter.limits(); });
  expect(limits.has_value() && limits->maxColorAttachments == 4,
         "a compatibility device requires no more color attachments than its "
         "adapter reports");
}

void levelsMapToDawnsFeatureLevels() {
  expect(toWGPUFeatureLevel(RNSkFeatureLevel::Core) == wgpu::FeatureLevel::Core,
         "the core level is asked for as Dawn's core feature level");
  expect(toWGPUFeatureLevel(RNSkFeatureLevel::Compatibility) ==
             wgpu::FeatureLevel::Compatibility,
         "the compatibility level is asked for as Dawn's compatibility "
         "feature level");
}

} // namespace

int main() {
  coreDeviceRequiresNoLimitsAndNeverReadsTheAdapter();
  compatibilityDeviceRequiresTheAdapterColorAttachmentsOnly();
  compatibilityDeviceRequiresNoMoreThanTheAdapterReports();
  levelsMapToDawnsFeatureLevels();
  if (failures > 0) {
    std::fprintf(stderr, "%d expectation(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  std::printf("RNDawnDeviceRequestTest: all expectations met\n");
  return EXIT_SUCCESS;
}
