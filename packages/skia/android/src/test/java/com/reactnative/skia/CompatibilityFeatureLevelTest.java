package com.reactnative.skia;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNull;

import org.junit.Test;

public class CompatibilityFeatureLevelTest {
    @Test
    public void anAbsentSettingLeavesTheLevelOn() {
        assertEquals(Boolean.TRUE, CompatibilityFeatureLevel.parse(null));
    }

    @Test
    public void aBooleanSetsIt() {
        assertEquals(Boolean.FALSE, CompatibilityFeatureLevel.parse(Boolean.FALSE));
        assertEquals(Boolean.TRUE, CompatibilityFeatureLevel.parse(Boolean.TRUE));
    }

    @Test
    public void theStringsTrueAndFalseSetItInAnyCase() {
        assertEquals(Boolean.FALSE, CompatibilityFeatureLevel.parse("false"));
        assertEquals(Boolean.FALSE, CompatibilityFeatureLevel.parse(" FALSE "));
        assertEquals(Boolean.TRUE, CompatibilityFeatureLevel.parse("True"));
    }

    @Test
    public void anythingElseIsReported() {
        assertNull(CompatibilityFeatureLevel.parse("0"));
        assertNull(CompatibilityFeatureLevel.parse("off"));
        assertNull(CompatibilityFeatureLevel.parse(0));
        assertNull(CompatibilityFeatureLevel.parse(1.0f));
    }
}
