package com.reactnative.skia;

import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;

import com.facebook.jni.HybridData;
import com.facebook.proguard.annotations.DoNotStrip;
import com.facebook.react.bridge.ReactContext;

import java.io.BufferedInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.MalformedURLException;
import java.net.URI;
import java.net.URISyntaxException;
import java.net.URL;
import java.net.URLConnection;

public class PlatformContext {
    @DoNotStrip
    private final HybridData mHybridData;

    private final ReactContext mContext;

    private final String TAG = "PlatformContext";

    private final Handler mainHandler = new Handler(Looper.getMainLooper());

    /**
     * The manifest meta-data that turns off rendering at the WebGPU compatibility level on
     * devices without a core adapter. Absent, the level is allowed.
     */
    static final String COMPATIBILITY_FEATURE_LEVEL_KEY =
            "com.reactnative.skia.COMPATIBILITY_FEATURE_LEVEL";

    public PlatformContext(ReactContext reactContext) {
        mContext = reactContext;
        mHybridData = initHybrid(reactContext.getResources().getDisplayMetrics().density,
                reactContext.getCacheDir().getAbsolutePath(),
                compatibilityFeatureLevelAllowed(reactContext),
                reactContext.getNoBackupFilesDir().getAbsolutePath(),
                appVersion(reactContext));
    }

    /**
     * The app's version name and version code, which an attempt at the compatibility level is
     * recorded under; empty when the package cannot be read.
     */
    static String appVersion(ReactContext context) {
        try {
            PackageInfo info = context.getPackageManager().getPackageInfo(context.getPackageName(), 0);
            long code = Build.VERSION.SDK_INT >= Build.VERSION_CODES.P
                    ? info.getLongVersionCode()
                    : info.versionCode;
            return info.versionName + " (" + code + ")";
        } catch (PackageManager.NameNotFoundException e) {
            return "";
        }
    }

    static boolean compatibilityFeatureLevelAllowed(ReactContext context) {
        try {
            ApplicationInfo info = context.getPackageManager().getApplicationInfo(
                    context.getPackageName(), PackageManager.GET_META_DATA);
            Bundle metaData = info.metaData;
            return metaData == null || metaData.getBoolean(COMPATIBILITY_FEATURE_LEVEL_KEY, true);
        } catch (PackageManager.NameNotFoundException e) {
            return true;
        }
    }

    private byte[] getStreamAsBytes(InputStream is) throws IOException {
        ByteArrayOutputStream buffer = new ByteArrayOutputStream();
        int nRead;
        byte[] data = new byte[4 * 0x400];
        while ((nRead = is.read(data, 0, data.length)) != -1) {
            buffer.write(data, 0, nRead);
        }
        return buffer.toByteArray();
    }

    @DoNotStrip
    public byte[] getJniStreamFromSource(String sourceUri) throws IOException {
        // First try loading the input as a resource directly
        int resourceId = mContext.getResources().getIdentifier(sourceUri, "drawable", mContext.getPackageName());

        // Test to see if we have a raw resource (for SVG)
        if (resourceId == 0) {
            resourceId = mContext.getResources().getIdentifier(sourceUri, "raw", mContext.getPackageName());
        }

        if (resourceId != 0) {
            // We can just return the input stream directly
            return getStreamAsBytes(mContext.getResources().openRawResource(resourceId));
        }

        // We should try to open a connection and return a stream to download this
        // object
        URI uri = null;
        try {
            uri = new URI(sourceUri);

            String scheme = uri.getScheme();

            if (scheme == null)
                throw new Exception("Invalid URI scheme");

            // TODO: Base64??

            URL url = uri.toURL();
            URLConnection connection = url.openConnection();
            connection.connect();

            BufferedInputStream b = new BufferedInputStream(url.openStream(), 8192);
            return getStreamAsBytes(b);

        } catch (URISyntaxException e) {
            e.printStackTrace();
        } catch (MalformedURLException e) {
            e.printStackTrace();
        } catch (IOException e) {
            e.printStackTrace();
        } catch (Exception e) {
            e.printStackTrace();
        }
        return null;
    }

    @Override
    protected void finalize() throws Throwable {
        mHybridData.resetNative();
        super.finalize();
    }

    @DoNotStrip
    public void raise(final String message) {
        mainHandler.post(new Runnable() {
            @Override
            public void run() {
                mContext.handleException(new Exception(message));
            }
        });
    }

    @DoNotStrip
    public void notifyTaskReady() {
        if (Looper.myLooper() == Looper.getMainLooper()) {
            notifyTaskReadyNative();
        } else {
            mainHandler.post(this::notifyTaskReadyNative);
        }
    }

    // Private c++ native methods
    private native HybridData initHybrid(float pixelDensity, String cacheDirectory,
            boolean compatibilityFeatureLevelAllowed, String compatibilityGuardDirectory,
            String appVersion);

    private native void notifyTaskReadyNative();
}
