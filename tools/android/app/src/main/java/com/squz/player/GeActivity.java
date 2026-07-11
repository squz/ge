package com.squz.player;

import android.content.Intent;
import android.os.Bundle;
import org.libsdl.app.SDLActivity;

public class GeActivity extends SDLActivity {
    // Holds the stream_addr (or legacy ged_addr) intent extra so native code
    // can retrieve it via JNI. Set before SDL's native thread starts; cleared
    // after first read.
    private static volatile String sStreamAddr = null;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Read intent extra before super.onCreate() loads native libraries
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

    /** @deprecated Use {@link #getStreamAddr()}; kept for any old native stubs. */
    public static String getGedAddr() {
        return getStreamAddr();
    }

    @Override
    protected String[] getLibraries() {
        return new String[]{"SDL3", "main"};
    }
}
