package com.bilko.rvglvr;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.os.StatFs;
import android.provider.Settings;
import android.util.Log;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.io.RandomAccessFile;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.util.Enumeration;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;

/**
 * First-launch setup (plain 2D panel, own process): gets all-files access, then downloads the
 * Re-Volt game data, the soundtrack and the RV25 pack from their official sources and unpacks
 * them into /sdcard/RVGL. Nothing of the game data is shipped in the APK.
 */
public class SetupActivity extends Activity {
    private static final String TAG = "RVGLQuestSetup";

    /** One downloadable package: where it comes from and where it is unpacked to. */
    static final class Pack {
        final String name, url, sha256, dest, check;
        final long size;
        Pack(String name, String url, long size, String sha256, String dest, String check) {
            this.name = name; this.url = url; this.size = size; this.sha256 = sha256;
            this.dest = dest; this.check = check;
        }
    }

    static final Pack[] PACKS = {
        new Pack("Re-Volt game files", "https://distribute.re-volt.io/packs/game_files.zip",
                 57370269L, "6ce4033dd436e37fdd37f02991ae5f5c4427a40f58e87a8ff0cb787ebb667f9f",
                 "", "levels/nhood1/nhood1.w"),
        new Pack("Re-Volt soundtrack", "https://distribute.re-volt.io/packs/soundtrack.zip",
                 108541343L, "472259107349e7af549ff859cc7ffcf347f56786e5e63ecf7e1ffa761a152bf8",
                 "", "redbook/track02.ogg"),
        new Pack("RV 25th Anniversary pack",
                 "https://www.revoltworld.net/dl/rv25-pack/?ind=1728162910361&filename=rv25.zip&wpdmdl=41528",
                 58950019L, "dc52af4080a5e5a7d7592747198696d11215d85d88701f00ec4352f6972cf3c3",
                 "packs/rv25/", "packs/rv25/levels"),
    };

    static File root() {
        return new File(Environment.getExternalStorageDirectory(), "RVGL");
    }

    private static File marker() {
        return new File(root(), ".rvglvr-install-incomplete");
    }

    static boolean hasStorageAccess() {
        return Build.VERSION.SDK_INT < 30 || Environment.isExternalStorageManager();
    }

    /** True when every pack is unpacked and no install was interrupted. */
    static boolean dataReady() {
        if (!hasStorageAccess() || marker().exists()) return false;
        for (Pack p : PACKS)
            if (!new File(root(), p.check).exists()) return false;
        return true;
    }

    private TextView status;
    private ProgressBar bar;
    private Button action, quit;
    private volatile boolean running;

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        LinearLayout l = new LinearLayout(this);
        l.setOrientation(LinearLayout.VERTICAL);
        l.setPadding(60, 50, 60, 50);
        l.setBackgroundColor(Color.rgb(16, 18, 28));

        TextView title = new TextView(this);
        title.setText("RVGL VR - first start");
        title.setTextSize(30);
        title.setTextColor(Color.WHITE);
        l.addView(title);

        TextView info = new TextView(this);
        info.setText("RVGL VR needs the Re-Volt game data. It is downloaded once from the official "
                + "community sources and installed to /sdcard/RVGL (about 225 MB download, "
                + "500 MB on the headset):\n\n"
                + "  - Re-Volt game files and soundtrack: Re-Volt I/O (re-volt.io)\n"
                + "  - RV 25th Anniversary pack: Re-Volt World (revoltworld.net)\n\n"
                + "Re-Volt is (c) Acclaim / WeGo Interactive. RVGL is made by the RVGL team "
                + "(rvgl.org). This VR port is unofficial.\n\n"
                + "Already have RVGL data? Copy it to /sdcard/RVGL instead; files that are present "
                + "are not downloaded again.");
        info.setTextSize(18);
        info.setTextColor(Color.rgb(210, 214, 224));
        info.setPadding(0, 24, 0, 24);
        l.addView(info);

        bar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        bar.setMax(1000);
        bar.setVisibility(View.INVISIBLE);
        l.addView(bar, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 40));

        status = new TextView(this);
        status.setTextSize(18);
        status.setTextColor(Color.rgb(255, 210, 90));
        status.setPadding(0, 12, 0, 24);
        l.addView(status);

        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.END);
        action = new Button(this);
        action.setTextSize(20);
        quit = new Button(this);
        quit.setText("Quit");
        quit.setTextSize(20);
        quit.setOnClickListener(v -> finishAndRemoveTask());
        row.addView(quit);
        row.addView(action);
        l.addView(row);

        setContentView(l);
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (running) return;
        if (!hasStorageAccess()) {
            status.setText("Step 1: allow RVGL VR to access all files (RVGL keeps its data in /sdcard/RVGL).");
            action.setText("Allow file access");
            action.setOnClickListener(v -> openStorageSettings());
        } else if (dataReady()) {
            startGame();
        } else {
            status.setText("");
            action.setText("Download and install");
            action.setOnClickListener(v -> startInstall());
        }
    }

    private void openStorageSettings() {
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            try {
                startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
            } catch (Exception e2) {
                // some headset OS builds have no such page: fall back to the app's settings
                startActivity(new Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                        Uri.parse("package:" + getPackageName())));
            }
        }
    }

    private void startGame() {
        Intent i = new Intent(this, RVGLVRActivity.class);
        i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(i);
        finishAndRemoveTask();
    }

    private void ui(String text, int permille) {
        runOnUiThread(() -> {
            status.setText(text);
            if (permille >= 0) {
                bar.setVisibility(View.VISIBLE);
                bar.setProgress(permille);
            }
        });
    }

    private void startInstall() {
        running = true;
        action.setEnabled(false);
        new Thread(() -> {
            try {
                install();
                running = false;
                runOnUiThread(this::startGame);
            } catch (Exception e) {
                Log.e(TAG, "install failed", e);
                running = false;
                runOnUiThread(() -> {
                    status.setText("Installation failed: " + e.getMessage()
                            + "\nCheck the Wi-Fi connection and try again; finished downloads are kept.");
                    action.setText("Try again");
                    action.setEnabled(true);
                });
            }
        }, "rvgl-setup").start();
    }

    private void install() throws Exception {
        File root = root();
        File tmp = new File(root, ".download");
        if (!tmp.isDirectory() && !tmp.mkdirs()) throw new IOException("cannot create " + tmp);

        long need = 0;
        for (Pack p : PACKS)
            if (!new File(root, p.check).exists()) need += p.size * 3;
        long free = new StatFs(root.getPath()).getAvailableBytes();
        if (free < need + (100L << 20))
            throw new IOException("not enough free space (" + (free >> 20) + " MB free, "
                    + ((need >> 20) + 100) + " MB needed)");

        new FileOutputStream(marker()).close();
        for (int n = 0; n < PACKS.length; n++) {
            Pack p = PACKS[n];
            String step = "(" + (n + 1) + "/" + PACKS.length + ") ";
            if (new File(root, p.check).exists()) {
                Log.i(TAG, p.name + " already present");
                continue;
            }
            File zip = new File(tmp, "pack" + n + ".zip");
            if (!(zip.exists() && zip.length() == p.size && p.sha256.equals(sha256(zip))))
                download(p, zip, step);
            unpack(p, zip, new File(root, p.dest), step);
            if (!zip.delete()) Log.w(TAG, "cannot delete " + zip);
        }

        File packs = new File(root, "packs/default.txt");
        if (!packs.exists()) {
            try (OutputStream o = new FileOutputStream(packs)) {
                o.write("default\nrv25\nlocal\n".getBytes("UTF-8"));
            }
        }
        tmp.delete();
        if (!marker().delete()) throw new IOException("cannot remove install marker");
        ui("Done. Starting RVGL VR...", 1000);
        Log.i(TAG, "install complete");
    }

    private void download(Pack p, File zip, String step) throws Exception {
        File part = new File(zip.getPath() + ".part");
        for (int attempt = 0; ; attempt++) {
            long have = part.exists() ? part.length() : 0;
            if (have > p.size) { part.delete(); have = 0; }
            HttpURLConnection c = (HttpURLConnection) new URL(p.url).openConnection();
            c.setInstanceFollowRedirects(true);
            c.setConnectTimeout(20000);
            c.setReadTimeout(30000);
            c.setRequestProperty("User-Agent", "RVGL-VR (Quest)");
            if (have > 0 && have < p.size) c.setRequestProperty("Range", "bytes=" + have + "-");
            try {
                int code = c.getResponseCode();
                boolean append = code == 206;
                if (code != 200 && code != 206) throw new IOException("HTTP " + code + " from " + new URL(p.url).getHost());
                if (!append) have = 0;
                try (InputStream in = c.getInputStream();
                     RandomAccessFile out = new RandomAccessFile(part, "rw")) {
                    out.setLength(have);
                    out.seek(have);
                    byte[] buf = new byte[1 << 16];
                    long last = 0;
                    int r;
                    while (have < p.size && (r = in.read(buf)) > 0) {
                        out.write(buf, 0, r);
                        have += r;
                        long now = System.currentTimeMillis();
                        if (now - last > 250) {
                            last = now;
                            ui(step + "Downloading " + p.name + ": " + (have >> 20) + " / " + (p.size >> 20) + " MB",
                               (int) (have * 1000 / p.size));
                        }
                    }
                }
            } catch (IOException e) {
                if (attempt >= 4) throw e;
                Log.w(TAG, "download interrupted, retrying: " + e);
                Thread.sleep(3000);
                continue;
            } finally {
                c.disconnect();
            }
            if (part.length() != p.size) {
                if (attempt >= 4) throw new IOException(p.name + ": incomplete download");
                continue;
            }
            ui(step + "Checking " + p.name + "...", 1000);
            if (!p.sha256.equals(sha256(part))) {
                part.delete();
                throw new IOException(p.name + ": checksum mismatch (download damaged or changed on the server)");
            }
            if (!part.renameTo(zip)) throw new IOException("cannot rename " + part);
            return;
        }
    }

    private void unpack(Pack p, File zip, File dest, String step) throws IOException {
        String base = dest.getCanonicalPath() + File.separator;
        try (ZipFile z = new ZipFile(zip)) {
            int total = z.size(), done = 0;
            byte[] buf = new byte[1 << 16];
            for (Enumeration<? extends ZipEntry> en = z.entries(); en.hasMoreElements(); ) {
                ZipEntry e = en.nextElement();
                done++;
                String name = e.getName().replace('\\', '/');
                String leaf = name.substring(name.lastIndexOf('/') + 1);
                if (leaf.equalsIgnoreCase("Thumbs.db") || leaf.equalsIgnoreCase("desktop.ini")) continue;
                File f = new File(dest, name);
                if (!(f.getCanonicalPath() + (e.isDirectory() ? File.separator : "")).startsWith(base))
                    throw new IOException("bad path in archive: " + name);
                if (e.isDirectory()) { f.mkdirs(); continue; }
                File dir = f.getParentFile();
                if (dir != null && !dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot create " + dir);
                try (InputStream in = z.getInputStream(e); OutputStream out = new FileOutputStream(f)) {
                    int r;
                    while ((r = in.read(buf)) > 0) out.write(buf, 0, r);
                }
                if (done % 20 == 0)
                    ui(step + "Installing " + p.name + ": " + done + " / " + total + " files", done * 1000 / total);
            }
        }
        Log.i(TAG, p.name + " unpacked to " + dest);
    }

    private static String sha256(File f) throws Exception {
        MessageDigest md = MessageDigest.getInstance("SHA-256");
        try (InputStream in = new FileInputStream(f)) {
            byte[] buf = new byte[1 << 16];
            int r;
            while ((r = in.read(buf)) > 0) md.update(buf, 0, r);
        }
        StringBuilder s = new StringBuilder();
        for (byte x : md.digest()) s.append(String.format("%02x", x));
        return s.toString();
    }
}
