// Activity of the NFS Shift loader.
//
// It only adds two things to org.libsdl.app.SDLActivity:
//
//   * the Wi-Fi MulticastLock (and a partial WakeLock) that the LAN
//     multiplayer needs.  Without the MulticastLock the Wi-Fi firmware filters
//     out every frame that is not addressed to our own unicast address, which
//     kills the UDP broadcast discovery on port 45470 (see docs/DESIGN_LAN.md
//     section 2.1).
//   * command line arguments from the launching intent, so that options can be
//     tried without rebuilding:
//       adb shell am start -n org.nfsshift.loader/.ShiftActivity --es args "--height 720"
//
// Why Java and not JNI from C++?  The JNI version needs FindClass/GetMethodID
// with hand-written signature strings that only fail at run time (the inner
// class is "Landroid/net/wifi/WifiManager$MulticastLock;" -- a missed dollar
// sign is a NoSuchMethodError on the device), a NewGlobalRef so the lock
// survives the return into Java, and it must run on a thread attached to the
// VM.  Here javac checks everything, the lock follows the activity lifecycle
// (it is released even when the native thread dies), and we need a subclass
// anyway -- for the launcher label/icon, and later for the .deb import dialog.
package org.nfsshift.loader;

import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.os.Build;
import android.os.Bundle;
import android.net.wifi.WifiManager;
import android.os.PowerManager;
import android.util.Log;

import java.io.File;
import java.io.PrintWriter;
import java.io.BufferedReader;
import java.io.FileReader;

import org.libsdl.app.SDLActivity;

public class ShiftActivity extends SDLActivity {
    private static final String TAG = "nfsshift";

    private WifiManager.MulticastLock multicastLock;
    private PowerManager.WakeLock wakeLock;

    // Two files next to the game data, in the app's own external directory
    // (no permission needed, reachable over USB):
    //   nfsshift-start.txt  written at every start -- if it is missing after a
    //                       failed launch, Java never even got this far
    //   nfsshift-crash.txt  stack trace of an uncaught exception
    // Why the effort: on a device without adb a crash before SDL_main says
    // nothing at all -- the app is simply gone again, no dialog, no log. The
    // trace is shown on the NEXT start, which is the only moment there is
    // still a window to show it in.
    private static final String START_FILE = "nfsshift-start.txt";
    private static final String CRASH_FILE = "nfsshift-crash.txt";

    private File stateFile(String name) {
        File dir = getExternalFilesDir(null);
        if (dir == null) dir = getFilesDir();
        return dir != null ? new File(dir, name) : null;
    }

    private void writeStartMarker() {
        File f = stateFile(START_FILE);
        if (f == null) return;
        try (PrintWriter w = new PrintWriter(f)) {
            w.println("start " + new java.util.Date());
            w.println("device " + Build.MANUFACTURER + " " + Build.MODEL);
            w.println("android " + Build.VERSION.RELEASE + " (SDK " + Build.VERSION.SDK_INT + ")");
            w.println("abis " + java.util.Arrays.toString(Build.SUPPORTED_ABIS));
            w.println("page size " + android.system.Os.sysconf(android.system.OsConstants._SC_PAGESIZE));
        } catch (Exception e) {
            Log.e(TAG, "start marker not written", e);
        }
    }

    private void installCrashHandler() {
        final Thread.UncaughtExceptionHandler previous = Thread.getDefaultUncaughtExceptionHandler();
        Thread.setDefaultUncaughtExceptionHandler((thread, error) -> {
            try {
                File f = stateFile(CRASH_FILE);
                if (f != null) {
                    try (PrintWriter w = new PrintWriter(f)) {
                        w.println(new java.util.Date() + " on thread " + thread.getName());
                        w.println(Build.MANUFACTURER + " " + Build.MODEL + ", Android "
                                  + Build.VERSION.RELEASE);
                        error.printStackTrace(w);
                    }
                }
            } catch (Throwable ignored) {
                // Never let the reporting swallow the original error.
            }
            Log.e(TAG, "uncaught exception", error);
            if (previous != null) previous.uncaughtException(thread, error);
        });
    }

    /** Show what the previous run died of -- and only then throw the file away. */
    private void reportPreviousCrash() {
        File f = stateFile(CRASH_FILE);
        if (f == null || !f.exists()) return;
        StringBuilder text = new StringBuilder();
        try (BufferedReader r = new BufferedReader(new FileReader(f))) {
            for (String line = r.readLine(); line != null && text.length() < 4000; line = r.readLine())
                text.append(line).append('\n');
        } catch (Exception e) {
            text.append("(").append(f).append(" not readable: ").append(e).append(")");
        }
        final String message = text.toString();
        f.delete();
        runOnUiThread(() -> new AlertDialog.Builder(this)
                .setTitle("NFS Shift: letzter Start abgestürzt")
                .setMessage(message)
                .setPositiveButton("OK", null)
                .show());
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        // Before super: SDLActivity loads the native libraries in its onCreate,
        // and a failure there is one of the things this is meant to catch.
        installCrashHandler();
        writeStartMarker();
        super.onCreate(savedInstanceState);
        reportPreviousCrash();
    }

    /** libSDL2.so first, then libmain.so -- SDLActivity dlsym()s SDL_main there. */
    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "main" };
    }

    /** Extra "args" of the start intent, split on whitespace. */
    @Override
    protected String[] getArguments() {
        Intent intent = getIntent();
        String args = intent != null ? intent.getStringExtra("args") : null;
        if (args == null || args.trim().isEmpty()) {
            return new String[0];
        }
        Log.i(TAG, "arguments: " + args);
        return args.trim().split("\\s+");
    }

    @Override
    protected void onResume() {
        super.onResume();
        acquireLocks();
    }

    @Override
    protected void onPause() {
        releaseLocks();
        super.onPause();
    }

    @Override
    protected void onDestroy() {
        releaseLocks();
        super.onDestroy();
    }

    private void acquireLocks() {
        // The application context, not the activity: getSystemService() on an
        // activity keeps a reference to it alive for as long as the lock.
        Context context = getApplicationContext();
        try {
            if (multicastLock == null) {
                WifiManager wifi = (WifiManager) context.getSystemService(Context.WIFI_SERVICE);
                if (wifi != null) {
                    multicastLock = wifi.createMulticastLock("nfsshift-lan");
                    multicastLock.setReferenceCounted(false);
                }
            }
            if (multicastLock != null && !multicastLock.isHeld()) {
                multicastLock.acquire();
                Log.i(TAG, "MulticastLock held: " + multicastLock.isHeld());
            }
        } catch (Exception e) {
            // Missing permission or a vendor specific failure: the game still
            // runs, only discovery by broadcast may not work.
            Log.e(TAG, "MulticastLock not acquired", e);
        }
        try {
            if (wakeLock == null) {
                PowerManager power = (PowerManager) context.getSystemService(Context.POWER_SERVICE);
                if (power != null) {
                    wakeLock = power.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "nfsshift:lan");
                    wakeLock.setReferenceCounted(false);
                }
            }
            if (wakeLock != null && !wakeLock.isHeld()) {
                wakeLock.acquire();
            }
        } catch (Exception e) {
            Log.e(TAG, "WakeLock not acquired", e);
        }
    }

    private void releaseLocks() {
        try {
            if (multicastLock != null && multicastLock.isHeld()) {
                multicastLock.release();
            }
        } catch (Exception e) {
            Log.e(TAG, "MulticastLock not released", e);
        }
        try {
            if (wakeLock != null && wakeLock.isHeld()) {
                wakeLock.release();
            }
        } catch (Exception e) {
            Log.e(TAG, "WakeLock not released", e);
        }
    }
}
