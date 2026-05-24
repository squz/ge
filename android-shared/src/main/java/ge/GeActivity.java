// Copyright 2026 Marcelo Cantos
// SPDX-License-Identifier: Apache-2.0
//
// Canonical Android Activity for ge consumers.
//
// Apps reference ge.GeActivity directly from their AndroidManifest.xml
// (android:name="ge.GeActivity"); no per-app subclass is required.
// Gradle source-includes this file from the engine submodule via
// sourceSets.main.java.srcDirs, so the Activity bumps in lockstep with
// the engine and consumer apps inherit new engine-side hooks (sensor
// listeners, lifecycle callbacks, future plumbing) just by updating
// the ge submodule pointer — no Java drift.
//
// Contains:
//   * getLibraries() — returns {"SDL3", "main"}; ge always builds the
//     consumer app's native code as libmain.so.
//   * Display-cutout insets — drives Context::drawSafeRect via JNI to
//     getDisplayCutoutInsets().
//   * Immersive mode — applyImmersive() called from native when
//     SessionHostConfig.immersive is set.
//   * Sensor-fused attitude — registers TYPE_GAME_ROTATION_VECTOR
//     while resumed, exposes the latest quaternion via getAttitude()
//     for the parallax pipeline.
//
// Apps that genuinely need to customise Activity behaviour subclass
// GeActivity; the supported default is zero-customisation.
package ge;

import android.content.ComponentCallbacks2;
import android.content.Context;
import android.graphics.Insets;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Build;
import android.os.Bundle;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.window.OnBackInvokedDispatcher;

import org.libsdl.app.SDLActivity;

public class GeActivity extends SDLActivity implements SensorEventListener {
    @Override
    protected String[] getLibraries() {
        return new String[]{"SDL3", "main"};
    }

    // Display-cutout-only insets (left, right, top, bottom in pixels),
    // updated by an OnApplyWindowInsetsListener attached in onCreate.
    // Read by native (CutoutInsets_android.cpp) so the engine can
    // distinguish drawSafeRect (cutouts only) from uiSafeRect (full
    // input-safe). Volatile because Java updates on the UI thread and
    // native reads on the game thread.
    private volatile int[] cutoutInsets = new int[]{0, 0, 0, 0};

    // Latest sensor-fused attitude as a unit quaternion (x, y, z, w),
    // updated by the SensorEventListener and read from native via
    // getAttitude() for the parallax pipeline. Identity until the
    // first onSensorChanged fires.
    private volatile float[] attitude = new float[]{0f, 0f, 0f, 1f};
    private SensorManager sensorManager;
    private Sensor gameRotationVector;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().getDecorView().setOnApplyWindowInsetsListener((v, ins) -> {
            Insets c = ins.getInsets(WindowInsets.Type.displayCutout());
            cutoutInsets = new int[]{c.left, c.right, c.top, c.bottom};
            return ins;
        });
        sensorManager = (SensorManager) getSystemService(Context.SENSOR_SERVICE);
        gameRotationVector = sensorManager == null ? null
            : sensorManager.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR);

        // 🎯T44: predictive-back gesture (API 33+). The callback only
        // fires when nativeOnBackPressed returns true — i.e. when the
        // game has registered RunConfig.onBackPressed. Otherwise we
        // don't register and the OS default (Activity.finish) fires.
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            getOnBackInvokedDispatcher().registerOnBackInvokedCallback(
                OnBackInvokedDispatcher.PRIORITY_DEFAULT,
                () -> nativeOnBackPressed());
        }
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (sensorManager != null && gameRotationVector != null) {
            sensorManager.registerListener(this, gameRotationVector,
                SensorManager.SENSOR_DELAY_GAME);
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (sensorManager != null) sensorManager.unregisterListener(this);
    }

    @Override
    public void onSensorChanged(SensorEvent ev) {
        if (ev.sensor.getType() != Sensor.TYPE_GAME_ROTATION_VECTOR) return;
        float qx = ev.values[0], qy = ev.values[1], qz = ev.values[2];
        float qw;
        if (ev.values.length > 3) {
            qw = ev.values[3];
        } else {
            float ss = qx * qx + qy * qy + qz * qz;
            qw = ss < 1f ? (float) Math.sqrt(1f - ss) : 0f;
        }
        attitude = new float[]{qx, qy, qz, qw};
    }

    @Override
    public void onAccuracyChanged(Sensor sensor, int accuracy) { }

    // Called from native (CutoutInsets_android.cpp) each frame to
    // populate Context::drawSafeRect on Android.
    public int[] getDisplayCutoutInsets() { return cutoutInsets; }

    // Called from native (Attitude_android.cpp) each frame to populate
    // Context::parallax(). Returns the current quaternion as a four-
    // element float array (x, y, z, w).
    public float[] getAttitude() { return attitude; }

    // 🎯T44: legacy back-press handler (pre-API 33). On API 33+ the
    // OnBackInvokedDispatcher path takes precedence and this is not
    // called. nativeOnBackPressed returns true if the engine consumed
    // the event; otherwise we fall through to super for the OS default.
    @Override
    public void onBackPressed() {
        if (nativeOnBackPressed()) return;
        super.onBackPressed();
    }

    // 🎯T45: OS memory-pressure warning. The native side maps the
    // graded TRIM_MEMORY_* constant onto MemoryPressureLevel and
    // forwards on the next pumpEvents on the game thread.
    @Override
    public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        nativeOnTrimMemory(level);
    }

    // JNI exports live in src/render/DirectRenderHost.mm.
    // Both are async-safe — they only touch atomics; the engine
    // dispatches the actual game callback on the game thread next
    // pump.
    private static native boolean nativeOnBackPressed();
    private static native void nativeOnTrimMemory(int level);

    // 🎯T63: High-refresh-rate during press.
    // Called from native (RefreshRateBoost_android.cpp) when a pointer
    // press starts or ends. On API 30+ requests max display refresh rate
    // via Surface.setFrameRate so VRR/ProMotion displays stay at high
    // refresh during touch interaction. No-op on API < 30.
    public void setFrameRateBoost(final boolean active) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) return;  // API 30+

        // SDL3's main surface view. SDLActivity exposes mSurface (the
        // SurfaceView) as a protected field. We access it via the class
        // hierarchy since SDL's SurfaceHolder gives us the Surface.
        runOnUiThread(() -> {
            try {
                // Get the Surface from the SurfaceView's SurfaceHolder.
                SurfaceHolder holder = mSurface.getHolder();
                if (holder == null) return;
                Surface surface = holder.getSurface();
                if (surface == null || !surface.isValid()) return;

                float targetFps = active
                    ? getDisplay().getMode().getRefreshRate()
                    : 0.0f;
                // FRAME_RATE_COMPATIBILITY_DEFAULT = 0
                surface.setFrameRate(targetFps, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
            } catch (Exception e) {
                // Best-effort; VRR boost is a non-critical optimisation.
            }
        });
    }

    // Called from native (Immersive_android.cpp) when the app's
    // SessionHostConfig.immersive flag changes. Hides or restores
    // status + navigation bars (the gesture / tappable insets persist
    // either way and continue to drive uiSafeRect). Must run on UI thread.
    public void applyImmersive(final boolean enabled) {
        runOnUiThread(() -> {
            getWindow().setDecorFitsSystemWindows(!enabled);
            WindowInsetsController c = getWindow().getInsetsController();
            if (c == null) return;
            if (enabled) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            } else {
                c.show(WindowInsets.Type.systemBars());
            }
        });
    }
}
