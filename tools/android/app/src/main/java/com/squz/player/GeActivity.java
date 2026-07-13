package com.squz.player;

import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import org.libsdl.app.SDLActivity;

public class GeActivity extends SDLActivity {
    // Holds the stream_addr (or legacy ged_addr) intent extra so native code
    // can retrieve it via JNI. Set before SDL's native thread starts; cleared
    // after first read.
    private static volatile String sStreamAddr = null;
    // Stream server name registered with spyder (e.g. "tiltbuggy").
    private static volatile String sServerName = null;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Read intent extras before super.onCreate() loads native libraries
        // and starts the SDL thread — available when SDL_main runs.
        Intent intent = getIntent();
        if (intent != null) {
            String addr = intent.getStringExtra("stream_addr");
            if (addr == null || addr.isEmpty()) {
                addr = intent.getStringExtra("ged_addr"); // legacy alias
            }
            if (addr != null && !addr.isEmpty()) {
                sStreamAddr = addr;
            }
            String name = intent.getStringExtra("server_name");
            if (name == null || name.isEmpty()) {
                name = intent.getStringExtra("name");
            }
            if (name != null && !name.isEmpty()) {
                sServerName = name;
            }
        }
        super.onCreate(savedInstanceState);
    }

    // Called from native (JNI) to retrieve the intent-supplied relay address.
    // Returns e.g. "192.168.1.100:3030" or null if absent.
    // Clears after first read so it does not persist across Activity restarts.
    public static String getStreamAddr() {
        String addr = sStreamAddr;
        sStreamAddr = null;
        return addr;
    }

    /** Stream server name (e.g. "tiltbuggy"), or null if not supplied. */
    public static String getServerName() {
        String name = sServerName;
        sServerName = null;
        return name;
    }

    /** @deprecated Use {@link #getStreamAddr()}; kept for any old native stubs. */
    public static String getGedAddr() {
        return getStreamAddr();
    }

    /**
     * Force Activity orientation from native {@code playerForceOrientation}.
     * Values match {@code ge::wire::kOrientation*} / SDL_DisplayOrientation:
     * 1=Landscape, 2=LandscapeFlipped, 3=Portrait, 4=PortraitFlipped,
     * 0xFE=AnyLandscape (sensor landscape). 0 = no-op.
     */
    public static void forceOrientation(int orientation) {
        final SDLActivity act = mSingleton;
        if (act == null || orientation == 0) return;

        final int mode;
        switch (orientation) {
            case 0xFE: // kOrientationAnyLandscape
                mode = ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE;
                break;
            case 1: // SDL_ORIENTATION_LANDSCAPE → reverse of device left tilt
                mode = ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE;
                break;
            case 2: // SDL_ORIENTATION_LANDSCAPE_FLIPPED
                mode = ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE;
                break;
            case 3: // SDL_ORIENTATION_PORTRAIT
                mode = ActivityInfo.SCREEN_ORIENTATION_PORTRAIT;
                break;
            case 4: // SDL_ORIENTATION_PORTRAIT_FLIPPED
                mode = ActivityInfo.SCREEN_ORIENTATION_REVERSE_PORTRAIT;
                break;
            default:
                return;
        }
        act.runOnUiThread(() -> act.setRequestedOrientation(mode));
    }

    @Override
    protected String[] getLibraries() {
        return new String[]{"SDL3", "main"};
    }
}
