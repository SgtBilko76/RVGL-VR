package com.bilko.rvglvr;

import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;

import org.libsdl.app.SDLActivity;

/**
 * RVGL on Meta Quest. The engine (libmain.so) is the official RVGL Android build;
 * the VR host lives in our libSDL2.so (jni/questxr).
 */
public class RVGLVRActivity extends SDLActivity {
    private static final String TAG = "RVGLQuest";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // RVGL keeps its data in /sdcard/RVGL: needs all-files access on Android 11+.
        if (Build.VERSION.SDK_INT >= 30 && !Environment.isExternalStorageManager()) {
            Log.i(TAG, "requesting all-files access");
            try {
                Intent i = new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                        Uri.parse("package:" + getPackageName()));
                startActivity(i);
            } catch (Exception e) {
                Log.e(TAG, "cannot open all-files access settings", e);
            }
        }
        super.onCreate(savedInstanceState);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {
            "openxr_loader",
            "SDL2",
            "SDL2_image",
            "openal",
            "mpg123",
            "sndfile",
            "unistring",
            "main"
        };
    }

    @Override
    protected void onDestroy() {
        // SDL joins its thread without a timeout; make sure the process really ends.
        Thread watchdog = new Thread(() -> {
            try { Thread.sleep(3000); } catch (InterruptedException ignored) { }
            android.os.Process.killProcess(android.os.Process.myPid());
        });
        watchdog.setDaemon(true);
        watchdog.start();
        super.onDestroy();
        android.os.Process.killProcess(android.os.Process.myPid());
    }
}
