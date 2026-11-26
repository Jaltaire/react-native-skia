---
id: installation
title: Installation
sidebar_label: Installation
slug: /getting-started/installation
---

React Native Skia brings the [Skia Graphics Library](https://skia.org/) to React Native.
Skia serves as the graphics engine for Google Chrome and Chrome OS, Android, Flutter, Mozilla Firefox, Firefox OS, and many other products.

:::info[React Native Skia v3]

This is the documentation of React Native Skia v3, which renders with Skia Graphite and is published on npm as `react-native-skia`.

- Upgrading from v2 or from `@shopify/react-native-skia`? Follow the [migration guide](/docs/getting-started/migration).
- v2 is still maintained. Its documentation is available at [wcandillon.github.io/react-native-skia/v2](https://wcandillon.github.io/react-native-skia/v2/).

:::

```sh
yarn add react-native-skia
# or
npm install react-native-skia
```

The Skia prebuilt binaries are delivered as regular npm dependencies (`react-native-skia-graphite-android`, `react-native-skia-graphite-apple-*` and `react-native-webgpu-dawn` for the WebGPU implementation Skia renders with) and are resolved automatically by the native build systems (CocoaPods on iOS/macOS/tvOS, Gradle on Android). No `postinstall` script is required, so there is nothing to allow or configure: `trustedDependencies` (Bun) or `enableScripts` (Yarn Berry) settings are not needed.

## Requirements

| | Minimum version |
|:--|:--|
| React Native | `0.79`, with the New Architecture |
| React | `19` |
| iOS | `15.1` |
| Android | API level 26 (`minSdkVersion = 26`) |
| Reanimated (optional) | `react-native-reanimated@>=4.0.0` with `react-native-worklets@>=0.7.0` |

React Native Skia runs on iOS, Android, macOS, Mac Catalyst, tvOS, and [the Web](/docs/getting-started/web).

Android TV is not supported by v3. It remain supported by [v2](https://wcandillon.github.io/react-native-skia/v2/docs/getting-started/installation/).
For `react-native@<=0.78` and `react@<=18`, you need to use `@shopify/react-native-skia` version `1.12.4` or below.

## iOS

Run `pod install` on the `ios/` directory.

## Android

React Native Skia requires Android API level 26 or above and renders with Vulkan.
React Native projects default to a lower `minSdkVersion`, so raise it in `android/build.gradle`:

```groovy
buildscript {
    ext {
        minSdkVersion = 26
    }
}
```

### Vulkan

Skia renders with Vulkan 1.1, which every 64-bit device shipped with Android 10 or later supports.
The library's manifest declares it as a required feature, so the Play Store does not serve your app to devices without it.
This does not affect the emulator (use an API 29 or later image with hardware graphics acceleration).

Skia asks Dawn for a Vulkan adapter at the WebGPU core feature level first.
On a device whose driver falls short of a core requirement but meets the compatibility ones (for example, a 2D texture limit of 4096, as on PowerVR GE8300, GE8320, and GE8322 GPUs), it renders at the compatibility level instead, and `Skia.gpu.featureLevel` is `"compatibility"`.
The device `Skia.getNativeDevice()` hands to react-native-webgpu is then a compatibility device too: WebGPU code that uses a core-only feature (such as cube array textures or per-attachment blend state) must check `Skia.gpu.featureLevel` first, because release builds skip Dawn's validation and such a call reaches the driver unchecked.

The compatibility level is on unless the app turns it off.
To render as before on such devices (no device, `Skia.gpu.isAvailable` false, blank canvases), set the build setting to false.
On Android, add this inside `<application>` in the app's manifest:

```xml
<meta-data android:name="com.reactnative.skia.COMPATIBILITY_FEATURE_LEVEL" android:value="false" />
```

On iOS, add `RNSkiaCompatibilityFeatureLevel` to `Info.plist` with the Boolean value `NO`.
In an Expo app, add the library's config plugin with the option, which writes both:

```json
{ "plugins": [["@shopify/react-native-skia", { "compatibilityFeatureLevel": false }]] }
```

The plugin stops the build when the option is not `true` or `false`, or when it is given an option it does not have.
At run time, a value Skia cannot read as true or false (`"0"`, `"off"`) is logged once as an error naming the key and the value, and the level stays on, as when the key is absent.
`Skia.gpu.error` then says that the level is turned off in the app's build settings.

A driver can still fail on that level in a way no app can catch, such as a crash or a hang inside the driver.
Skia therefore counts an attempt at the compatibility level while the driver is exercised for the first time in a process:
- while the device and its Graphite context are created;
- while it compiles again, on a thread of its own, the pipelines an earlier launch used;
- and from the first canvas window made (whose swapchain the driver creates) or the first drawing snapped for the GPU (where the driver starts compiling its pipelines), whichever comes first, until a canvas window's frame has finished: submitted, presented on a device that was not lost, and reported done by the GPU.

An offscreen surface's drawing that the GPU reports done first closes that window only while no canvas window has been made and no window frame snapped, and does not end the attempt: the first canvas window made, or window frame snapped, after it opens the window again, so that the window's swapchain, first image, and first frame are counted whatever offscreen drawing finished before them.
A window frame submitted while an offscreen drawing is still the one being watched is not watched itself, and the next window frame is: a canvas that draws exactly one frame in that gap leaves a healthy launch counted until a later frame finishes.
A texture upload, a readback, or a surface's first clear never finishes that window, and a frame whose drawing was rejected neither finishes it nor counts again.
It keeps the count in a directory of the app's that is kept out of backups, writes it only when its value changes, and removes it once a window frame has finished and every counted window has closed.
If two launches in a row on the same app version end inside those windows, Skia stops trying the level on that version: the device renders as it did before the level existed (`Skia.gpu.isAvailable` false, blank canvases), and `Skia.gpu.error` says why.
The next app version tries the level again.
An app that only ever draws offscreen never finishes a window frame, so its count is never removed: a launch that ended inside a window stays counted until the app's version changes, and one more such launch on that version stops the level.
A fault after that first frame, on a later frame or a heavier screen, is not caught: the count is removed at every launch that gets that far.
Neither is a fault in a texture upload or a readback made before the first drawing, nor in the buffers and textures Graphite creates while a drawing is recorded, before it is snapped.
For such a fault, the build setting above is the backstop.
A launch that makes no canvas window and snaps no drawing neither counts nor resets the count, unless it starts with pipelines stored by an earlier launch: their compile is counted from the device's creation on, so a background start killed while it runs (a tenth of a second or so) counts.
A launch that makes a canvas window or snaps a drawing and then ends before its first frame finishes counts, even when the user, not the driver, ended it.
If the count cannot be written when the device is created, Skia does not try the level; if a later write fails, Skia stops counting for that launch and leaves the count as it was, never below what the launch found.
A count whose content cannot be read as one is replaced by this launch's own, as its app version cannot be known; a count the file system fails to read is left as it is, and that launch does not try the level: for as long as the failure lasts, a new app version included.
An app whose Skia loads in two processes can also lose a launch's attempt: one process removing stale temporary files can remove the one the other is writing, and that process then does not try the level on that launch, or stops counting and leaves the count as it was.
The count's last removal, once the first window frame has finished, can run on the main thread: one file removal and one directory flush, once per process, on a device without a core adapter only.
Devices with a core adapter never read or write the count, and never read the app's build setting, directories, or version: Skia reads them only after it found no core adapter.
If react-native-webgpu asks for the GPU before Skia's module is installed, Skia has none of them yet, and does not try the level in that process.

On a device without Vulkan the app still runs: `Skia.gpu.isAvailable` is false, `Skia.gpu.error` says why, and canvases stay blank.
An app that has to render on such devices uses [v2](https://wcandillon.github.io/react-native-skia/v2/docs/getting-started/installation/), which renders with OpenGL ES.
To ship to those devices anyway, remove the declaration from your app's manifest and check the flag before rendering with Skia:

```xml
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
          xmlns:tools="http://schemas.android.com/tools">
  <uses-feature android:name="android.hardware.vulkan.version" tools:node="remove" />
</manifest>
```

If another dependency already declares `android.hardware.vulkan.version` with a different version, keep one declaration and mark it `tools:replace="android:version"`.

Currently, you will need Android NDK to be installed.
If you have Android Studio installed, make sure `$ANDROID_NDK` is available.
`ANDROID_NDK=/Users/username/Library/Android/sdk/ndk/<version>` for instance.

If the NDK is not installed, you can install it via Android Studio by going to the menu _File > Project Structure_

And then the _SDK Location_ section. It will show you the NDK path, or the option to download it if you don't have it installed.

### TroubleShooting

For error **_CMake 'X.X.X' was not found in SDK, PATH, or by cmake.dir property._**

open _Tools > SDK Manager_, switch to the _SDK Tools_ tab.
Find `CMake` and click _Show Package Details_ and download compatiable version **'X.X.X'**, and apply to install.

## Expo

React Native Skia v3 requires a [development build](https://docs.expo.dev/develop/development-builds/introduction/).
It does not run in Expo Go, which bundles the native code of v2.

```sh
npx expo install react-native-skia expo-build-properties
```

Use [expo-build-properties](https://docs.expo.dev/versions/latest/sdk/build-properties/) to set the Android `minSdkVersion` in your app config:

```json
{
  "expo": {
    "plugins": [
      ["expo-build-properties", { "android": { "minSdkVersion": 26 } }]
    ]
  }
}
```

Then create the development build with `npx expo run:ios` and `npx expo run:android`.

## Web

To use this library in the browser, see [these instructions](/docs/getting-started/web).

## Debugging

We recommend using React Native DevTools to debug your JS code: see the [React Native docs](https://reactnative.dev/docs/debugging). Alternatively, you can debug both JS and platform code in VS Code and via native IDEs. If using VS Code, we recommend [Expo Tools](https://github.com/expo/vscode-expo), [Radon IDE](https://ide.swmansion.com/), or Microsoft's [React Native Tools](https://marketplace.visualstudio.com/items?itemName=msjsdiag.vscode-react-native#debugging-react-native-applications).

## Testing with Jest

React Native Skia test mocks use a web implementation that depends on loading CanvasKit.

The very first step is to make sure that your Skia files are not being transformed by jest, for instance, we can add it the `transformIgnorePatterns` directive:
```js
"transformIgnorePatterns": [
  "node_modules/(?!(react-native|react-native.*|@react-native.*|@?react-navigation.*|react-native-skia)/)"
]
```

You also need to add the following to your `jest.config.js` file:

```js
// jest.config.js
module.exports = {
  // Other values
  testEnvironment: "react-native-skia/jestEnv.js",
  setupFilesAfterEnv: [
    "react-native-skia/jestSetup.js",
  ],
};
```

The `jestEnv.js` will load CanvasKit for you and `jestSetup.js` mocks React Native Skia.
You can also have a look at the [example app](https://github.com/wcandillon/react-native-skia/tree/main/apps/example) to see how Jest tests are enabled there.

## Playground

We have example projects you can play with [here](https://github.com/wcandillon/react-native-skia/tree/main/apps).
To run them, follow the [contributing guide](https://github.com/wcandillon/react-native-skia/blob/main/packages/skia/CONTRIBUTING.md).
