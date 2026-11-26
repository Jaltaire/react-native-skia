package com.reactnative.skia;

/** The build setting that turns the WebGPU compatibility level off, as the manifest gives it. */
final class CompatibilityFeatureLevel {
    /** The manifest meta-data. Absent, the level is allowed. */
    static final String KEY = "com.reactnative.skia.COMPATIBILITY_FEATURE_LEVEL";

    private CompatibilityFeatureLevel() {}

    /**
     * The setting's value: absent (null) leaves the level on, a Boolean or the strings "true" and
     * "false" (any case) set it, and anything else is null, which the caller reports.
     */
    static Boolean parse(Object value) {
        if (value == null) {
            return Boolean.TRUE;
        }
        if (value instanceof Boolean) {
            return (Boolean) value;
        }
        if (value instanceof String) {
            String text = ((String) value).trim();
            if (text.equalsIgnoreCase("true")) {
                return Boolean.TRUE;
            }
            if (text.equalsIgnoreCase("false")) {
                return Boolean.FALSE;
            }
        }
        return null;
    }
}
