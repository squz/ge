// OrientProbe (Android) — orientation/letterbox/accel probe.
// Four activities differing only in manifest android:screenOrientation:
//   MainActivity: unspecified   LandActivity: landscape
//   SensorLandActivity: sensorLandscape   PortActivity: portrait
// Paints: green body, red bar = view TOP edge, blue bar = view LEFT edge,
// yellow corners, and a live metrics overlay (also logged to logcat tag "OrientProbe").
package com.squz.orientprobe;

import android.app.Activity;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.hardware.Sensor;
import android.hardware.SensorEvent;
import android.hardware.SensorEventListener;
import android.hardware.SensorManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.Display;
import android.view.View;
import android.view.WindowManager;
import android.view.WindowMetrics;

public class Probe {
    public static class BaseActivity extends Activity implements SensorEventListener {
        static final String TAG = "OrientProbe";
        float[] accel = new float[3];
        ProbeView view;
        SensorManager sm;
        final Handler handler = new Handler(Looper.getMainLooper());
        final Runnable tick = new Runnable() { public void run() { report(); view.invalidate(); handler.postDelayed(this, 1000); } };

        @Override protected void onCreate(Bundle b) {
            super.onCreate(b);
            view = new ProbeView(this);
            setContentView(view);
            sm = (SensorManager) getSystemService(Context.SENSOR_SERVICE);
        }
        @Override protected void onResume() {
            super.onResume();
            Sensor a = sm.getDefaultSensor(Sensor.TYPE_ACCELEROMETER);
            if (a != null) sm.registerListener(this, a, SensorManager.SENSOR_DELAY_UI);
            handler.post(tick);
        }
        @Override protected void onPause() { super.onPause(); sm.unregisterListener(this); handler.removeCallbacks(tick); }
        @Override public void onSensorChanged(SensorEvent e) { accel = e.values.clone(); }
        @Override public void onAccuracyChanged(Sensor s, int a) {}

        String metrics() {
            Display d = getDisplay();
            int rot = d != null ? d.getRotation() : -1;
            WindowManager wm = getWindowManager();
            WindowMetrics cur = wm.getCurrentWindowMetrics();
            WindowMetrics max = wm.getMaximumWindowMetrics();
            Rect cb = cur.getBounds(), mb = max.getBounds();
            return String.format(
                "%s\nrot=%d multiWin=%b\nwin=%dx%d display=%dx%d\nview=%dx%d\naccel=%.1f,%.1f,%.1f",
                getClass().getSimpleName(), rot, isInMultiWindowMode(),
                cb.width(), cb.height(), mb.width(), mb.height(),
                view.getWidth(), view.getHeight(), accel[0], accel[1], accel[2]);
        }
        void report() { Log.i(TAG, metrics().replace('\n', ' ')); }
    }

    public static class MainActivity extends BaseActivity {}
    public static class LandActivity extends BaseActivity {}
    public static class SensorLandActivity extends BaseActivity {}
    public static class PortActivity extends BaseActivity {}

    static class ProbeView extends View {
        final BaseActivity host;
        final Paint p = new Paint();
        ProbeView(BaseActivity a) { super(a); host = a; }
        @Override protected void onDraw(Canvas c) {
            int w = getWidth(), h = getHeight();
            p.setStyle(Paint.Style.FILL);
            p.setColor(Color.rgb(46, 204, 84)); c.drawRect(0, 0, w, h, p);          // green body
            p.setColor(Color.rgb(255, 59, 48)); c.drawRect(0, 0, w, 100, p);        // red top
            p.setColor(Color.rgb(0, 122, 255)); c.drawRect(0, 0, 100, h, p);        // blue left
            p.setColor(Color.rgb(255, 204, 0));                                     // yellow corners
            c.drawRect(0, 0, 100, 100, p); c.drawRect(w - 100, 0, w, 100, p);
            c.drawRect(0, h - 100, 100, h, p); c.drawRect(w - 100, h - 100, w, h, p);
            p.setColor(Color.WHITE); c.drawRect(w * 0.2f, h * 0.40f, w * 0.8f, h * 0.62f, p);
            p.setColor(Color.BLACK); p.setTextSize(40); p.setFakeBoldText(true);
            float y = h * 0.40f + 50;
            for (String line : host.metrics().split("\n")) { c.drawText(line, w * 0.2f + 20, y, p); y += 48; }
        }
    }
}
