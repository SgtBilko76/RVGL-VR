package com.bilko.rvglvr;

import android.content.Intent;
import android.os.Bundle;
import android.util.Log;

import org.libsdl.app.SDLActivity;

/**
 * RVGL on Meta Quest. The engine (libmain.so) is the official RVGL Android build;
 * the VR host lives in our libSDL2.so (jni/questxr).
 */
public class RVGLVRActivity extends SDLActivity {
    private static final String TAG = "RVGLQuest";

    private boolean toSetup;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Missing file access or game data: hand over to the 2D setup panel (own process),
        // which downloads the data and starts this activity again when done.
        toSetup = !SetupActivity.dataReady();
        super.onCreate(savedInstanceState);
        if (toSetup) {
            Log.i(TAG, "game data missing, starting setup");
            Intent i = new Intent(this, SetupActivity.class);
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
            startActivity(i);
            finish();
        }
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
        if (toSetup) {
            super.onDestroy();
            android.os.Process.killProcess(android.os.Process.myPid());
            return;
        }
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
