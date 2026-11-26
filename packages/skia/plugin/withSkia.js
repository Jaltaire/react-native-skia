/**
 * The Expo config plugin of React Native Skia. Its one option,
 * `compatibilityFeatureLevel`, writes the build setting that turns the WebGPU
 * compatibility level off (false) or keeps it on (true): the Android manifest's
 * meta-data and the Info.plist key the library reads. Without the option the
 * app's native projects are left as they are, and the level is on.
 */
const COMPATIBILITY_FEATURE_LEVEL_ANDROID_KEY =
  "com.reactnative.skia.COMPATIBILITY_FEATURE_LEVEL";
const COMPATIBILITY_FEATURE_LEVEL_IOS_KEY = "RNSkiaCompatibilityFeatureLevel";
const OPTIONS = ["compatibilityFeatureLevel"];

/**
 * The plugin's options, checked when the app's native projects are generated:
 * an unknown option or a value that is not a boolean stops the build with an
 * error that names it, rather than leaving the level on without a word.
 */
function readOptions(options) {
  if (options === undefined || options === null) {
    return {};
  }
  if (typeof options !== "object" || Array.isArray(options)) {
    throw new Error(
      `React Native Skia's config plugin takes an object of options; got ${JSON.stringify(options)}.`
    );
  }
  for (const name of Object.keys(options)) {
    if (!OPTIONS.includes(name)) {
      throw new Error(
        `React Native Skia's config plugin has no option "${name}"; its options are ${OPTIONS.map((option) => `"${option}"`).join(", ")}.`
      );
    }
  }
  const value = options.compatibilityFeatureLevel;
  if (value !== undefined && typeof value !== "boolean") {
    throw new Error(
      `React Native Skia's config plugin option "compatibilityFeatureLevel" must be true or false; got ${JSON.stringify(value)}.`
    );
  }
  return { compatibilityFeatureLevel: value };
}

/** Writes the Android setting into the manifest's application element. */
function applyAndroidCompatibilityFeatureLevel(
  AndroidConfig,
  manifest,
  allowed
) {
  const application =
    AndroidConfig.Manifest.getMainApplicationOrThrow(manifest);
  AndroidConfig.Manifest.addMetaDataItemToMainApplication(
    application,
    COMPATIBILITY_FEATURE_LEVEL_ANDROID_KEY,
    String(allowed)
  );
  return manifest;
}

/** Writes the Apple setting into Info.plist. */
function applyIosCompatibilityFeatureLevel(infoPlist, allowed) {
  infoPlist[COMPATIBILITY_FEATURE_LEVEL_IOS_KEY] = allowed;
  return infoPlist;
}

// The config plugin helpers: Expo's own re-export, which an app has, and the
// package itself where expo is not installed (this repository's tests).
function configPlugins() {
  try {
    return require("expo/config-plugins");
  } catch (error) {
    return require("@expo/config-plugins");
  }
}

function withSkia(config, options) {
  const { compatibilityFeatureLevel } = readOptions(options);
  if (compatibilityFeatureLevel === undefined) {
    return config;
  }
  const { AndroidConfig, withAndroidManifest, withInfoPlist } =
    configPlugins();
  config = withAndroidManifest(config, (modConfig) => {
    applyAndroidCompatibilityFeatureLevel(
      AndroidConfig,
      modConfig.modResults,
      compatibilityFeatureLevel
    );
    return modConfig;
  });
  config = withInfoPlist(config, (modConfig) => {
    applyIosCompatibilityFeatureLevel(
      modConfig.modResults,
      compatibilityFeatureLevel
    );
    return modConfig;
  });
  return config;
}

module.exports = withSkia;
module.exports.readOptions = readOptions;
module.exports.applyAndroidCompatibilityFeatureLevel =
  applyAndroidCompatibilityFeatureLevel;
module.exports.applyIosCompatibilityFeatureLevel =
  applyIosCompatibilityFeatureLevel;
module.exports.COMPATIBILITY_FEATURE_LEVEL_ANDROID_KEY =
  COMPATIBILITY_FEATURE_LEVEL_ANDROID_KEY;
module.exports.COMPATIBILITY_FEATURE_LEVEL_IOS_KEY =
  COMPATIBILITY_FEATURE_LEVEL_IOS_KEY;
