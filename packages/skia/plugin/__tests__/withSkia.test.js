const { AndroidConfig } = require("@expo/config-plugins");

const withSkia = require("../withSkia");

const {
  readOptions,
  applyAndroidCompatibilityFeatureLevel,
  applyIosCompatibilityFeatureLevel,
  COMPATIBILITY_FEATURE_LEVEL_ANDROID_KEY,
  COMPATIBILITY_FEATURE_LEVEL_IOS_KEY,
} = withSkia;

const manifest = () => ({
  manifest: {
    $: { "xmlns:android": "http://schemas.android.com/apk/res/android" },
    application: [{ $: { "android:name": ".MainApplication" } }],
  },
});

describe("React Native Skia's config plugin", () => {
  it("leaves the app as it is without the option", () => {
    const config = { name: "app", slug: "app" };
    expect(withSkia(config)).toBe(config);
    expect(withSkia(config, {})).toBe(config);
    expect(readOptions(undefined)).toEqual({});
  });

  it("accepts true and false", () => {
    expect(readOptions({ compatibilityFeatureLevel: false })).toEqual({
      compatibilityFeatureLevel: false,
    });
    expect(readOptions({ compatibilityFeatureLevel: true })).toEqual({
      compatibilityFeatureLevel: true,
    });
  });

  it("rejects a value that is not a boolean, naming it", () => {
    for (const value of ["false", 0, "off", null, {}]) {
      expect(() => readOptions({ compatibilityFeatureLevel: value })).toThrow(
        `"compatibilityFeatureLevel" must be true or false; got ${JSON.stringify(value)}.`
      );
    }
  });

  it("rejects an unknown option and options that are not an object", () => {
    expect(() => readOptions({ compatibilityLevel: false })).toThrow(
      'has no option "compatibilityLevel"'
    );
    expect(() => readOptions(false)).toThrow("takes an object of options");
    expect(() => readOptions([false])).toThrow("takes an object of options");
  });

  it("adds the manifest and Info.plist mods with the option", () => {
    const config = withSkia(
      { name: "app", slug: "app" },
      { compatibilityFeatureLevel: false }
    );
    expect(typeof config.mods.android.manifest).toBe("function");
    expect(typeof config.mods.ios.infoPlist).toBe("function");
  });

  it("writes the Android meta-data the library reads", () => {
    const written = applyAndroidCompatibilityFeatureLevel(
      AndroidConfig,
      manifest(),
      false
    );
    const application = written.manifest.application[0];
    expect(application["meta-data"]).toEqual([
      {
        $: {
          "android:name": COMPATIBILITY_FEATURE_LEVEL_ANDROID_KEY,
          "android:value": "false",
        },
      },
    ]);
  });

  it("writes the Info.plist key the library reads", () => {
    expect(applyIosCompatibilityFeatureLevel({}, false)).toEqual({
      [COMPATIBILITY_FEATURE_LEVEL_IOS_KEY]: false,
    });
  });
});
