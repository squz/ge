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
import android.media.AudioAttributes;
import android.media.AudioFocusRequest;
import android.media.AudioManager;
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

    // 🎯T43: Android audio focus. Requested in onResume, abandoned in onPause.
    // Changes are forwarded to native via nativeOnAudioFocusChange.
    private AudioManager audioManager;
    private AudioFocusRequest audioFocusRequest;
    private AudioManager.OnAudioFocusChangeListener audioFocusListener;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        // 🎯T119: bridge spyder's launch-Intent string-extras into the process
        // environment before the native main thread reads getenv(). Must run
        // after super.onCreate (native libs loaded → nativeSetenv resolvable)
        // and before SDL starts the native thread.
        passLaunchEnv();
        getWindow().getDecorView().setOnApplyWindowInsetsListener((v, ins) -> {
            Insets c = ins.getInsets(WindowInsets.Type.displayCutout());
            cutoutInsets = new int[]{c.left, c.right, c.top, c.bottom};
            return ins;
        });
        sensorManager = (SensorManager) getSystemService(Context.SENSOR_SERVICE);
        gameRotationVector = sensorManager == null ? null
            : sensorManager.getDefaultSensor(Sensor.TYPE_GAME_ROTATION_VECTOR);

        // 🎯T43: set up the audio focus listener once. The actual
        // requestAudioFocus / abandonAudioFocus cycle happens in
        // onResume / onPause so we don't hold focus while backgrounded.
        audioManager = (AudioManager) getSystemService(Context.AUDIO_SERVICE);
        audioFocusListener = focusChange -> nativeOnAudioFocusChange(focusChange);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
            audioFocusRequest = new AudioFocusRequest.Builder(
                    AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(new AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_GAME)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build())
                .setOnAudioFocusChangeListener(audioFocusListener)
                .build();
        }

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
        // 🎯T43: request audio focus when coming to the foreground.
        // On success (or if focus is already ours) the listener fires
        // AUDIOFOCUS_GAIN which pumpEvents drains on the game thread.
        if (audioManager != null) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                if (audioFocusRequest != null) {
                    audioManager.requestAudioFocus(audioFocusRequest);
                }
            } else {
                //noinspection deprecation
                audioManager.requestAudioFocus(audioFocusListener,
                    AudioManager.STREAM_MUSIC, AudioManager.AUDIOFOCUS_GAIN);
            }
        }
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (sensorManager != null) sensorManager.unregisterListener(this);
        // 🎯T43: release audio focus while backgrounded.
        if (audioManager != null) {
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                if (audioFocusRequest != null) {
                    audioManager.abandonAudioFocusRequest(audioFocusRequest);
                }
            } else {
                //noinspection deprecation
                audioManager.abandonAudioFocus(audioFocusListener);
            }
        }
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

    // 🎯T119: lift spyder's launch-Intent string-extras (SPYDER_APP_CHANNEL,
    // GE_IAP_MODE, …) into the process environment via setenv, so native
    // getenv() sees them on Android the same way iOS receives the process env.
    // spyder switches from `monkey` to `am start --es KEY VALUE` when env is
    // supplied; Intent extras aren't environment variables, so the app bridges
    // them before the native thread reads getenv(). nativeSetenv is a no-op in
    // release builds, so a shipped app ignores launch extras entirely.
    private void passLaunchEnv() {
        if (getIntent() == null) return;
        Bundle extras = getIntent().getExtras();
        if (extras == null) return;
        for (String key : extras.keySet()) {
            Object v = extras.get(key);
            if (key != null && v != null) nativeSetenv(key, String.valueOf(v));
        }
    }

    // JNI exports live in src/render/DirectRenderHost.mm.
    // All are async-safe — they only touch atomics; the engine
    // dispatches the actual game callback on the game thread next pump.
    private static native boolean nativeOnBackPressed();
    private static native void nativeOnTrimMemory(int level);
    // 🎯T43: audio focus change forwarded from OnAudioFocusChangeListener.
    // focusChange mirrors AudioManager.AUDIOFOCUS_* constants.
    private static native void nativeOnAudioFocusChange(int focusChange);
    // 🎯T119: bridge launch-Intent string-extras → process env (debug-only on
    // the native side). Called from passLaunchEnv() in onCreate.
    private static native void nativeSetenv(String key, String value);

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
