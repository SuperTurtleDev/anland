package com.anlandnext.test;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import com.anlandnext.awl.Awl;

import java.util.List;

/**
 * anland third-party-path test harness:
 *   Launch  — Awl.getWaylandFd() (socketpair + binder T_CONNECT, all inside
 *             libawl) → Awl.spawnClient: fork+exec of the packaged pure shm
 *             client (nativeLibraryDir/libawlshm.so) with WAYLAND_SOCKET
 *             inherited. The client's wayland credentials = THIS app's uid,
 *             so everything below is scoped to us server-side.
 *   List    — Awl.getWindows() snapshot (server filters to our uid).
 *   Events  — Awl.registerCallback: created/attached/detached/destroyed
 *             lines in the log as the client runs.
 *   Tap a window row — Awl.attachWindow: hosted by this app's own
 *             AwlWindowActivity (identity = this app, not the anland APK).
 *   Long-press a row — Awl.closeWindow (xdg close → client exits cleanly).
 */
public class MainActivity extends Activity {
    private LinearLayout rows;
    private TextView status;
    private TextView log;
    private int logLines;

    private final Awl.Callback events = new Awl.Callback() {
        @Override public void onWindowCreated(long id, String title) {
            logLine("event: created " + id + " '" + title + "'");
            refresh();
        }
        @Override public void onWindowDestroyed(long id) {
            logLine("event: destroyed " + id);
            refresh();
        }
        @Override public void onWindowAttached(long id) {
            logLine("event: attached " + id);
            refresh();
        }
        @Override public void onWindowDetached(long id) {
            logLine("event: detached " + id);
            refresh();
        }
    };

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(32, 48, 32, 48);

        status = new TextView(this);
        status.setTextSize(14);
        root.addView(status);

        LinearLayout btns = new LinearLayout(this);
        btns.setOrientation(LinearLayout.HORIZONTAL);
        Button launch = new Button(this);
        launch.setText("Launch wayland client");
        launch.setOnClickListener(v -> launchClient());
        btns.addView(launch, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        Button refreshBtn = new Button(this);
        refreshBtn.setText("Refresh");
        refreshBtn.setOnClickListener(v -> refresh());
        btns.addView(refreshBtn, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        root.addView(btns);

        ScrollView scRows = new ScrollView(this);
        rows = new LinearLayout(this);
        rows.setOrientation(LinearLayout.VERTICAL);
        scRows.addView(rows);
        root.addView(scRows, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        ScrollView scLog = new ScrollView(this);
        log = new TextView(this);
        log.setTextSize(12);
        log.setTypeface(android.graphics.Typeface.MONOSPACE);
        scLog.addView(log);
        root.addView(scLog, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        setContentView(root);
    }

    @Override
    protected void onResume() {
        super.onResume();
        Awl.registerCallback(events);
        refresh();
    }

    @Override
    protected void onPause() {
        Awl.unregisterCallback(events);
        super.onPause();
    }

    /** wayland fd over binder (Awl) → fork+exec of the pure shm client with
     *  WAYLAND_SOCKET inherited; its stdio pipes drain into the on-screen
     *  log (all plumbing inside libawl) */
    private void launchClient() {
        final String exe = getApplicationInfo().nativeLibraryDir + "/libawlshm.so";
        logLine("spawning " + exe);
        new Thread(() -> {
            java.io.FileDescriptor fd = Awl.getWaylandFd();
            if (fd == null) {
                logLine("no wayland fd (daemon down / refused)");
                return;
            }
            Awl.ClientProcess p = Awl.spawnClient(fd, exe);
            if (p == null) {
                logLine("spawn failed");
                return;
            }
            logLine("client pid=" + p.pid);
            drain(p.stdout, "out");
            drain(p.stderr, "err");
        }, "awlshm-exec").start();
    }

    /** stream one of the child's pipes into the on-screen log AND logcat
     *  (external debugging: adb logcat -s awlshm-out awlshm-err); EOF = child exited */
    private void drain(android.os.ParcelFileDescriptor pfd, String tag) {
        new Thread(() -> {
            try (java.io.BufferedReader r = new java.io.BufferedReader(
                    new java.io.InputStreamReader(
                            new java.io.FileInputStream(pfd.getFileDescriptor())))) {
                String line;
                while ((line = r.readLine()) != null) {
                    android.util.Log.i("awlshm-" + tag, line);
                    logLine(tag + "| " + line);
                }
                android.util.Log.i("awlshm-" + tag, "(eof)");
                logLine(tag + "| (eof)");
            } catch (Exception e) {
                logLine(tag + "| drain: " + e);
            }
        }, "awlshm-" + tag).start();
    }

    private void refresh() {
        rows.removeAllViews();
        List<Awl.WlWindow> wins = Awl.getWindows();
        if (wins == null) {
            status.setText("daemon unreachable");
            return;
        }
        status.setText("windows (own uid): " + wins.size());
        for (final Awl.WlWindow w : wins) {
            TextView t = new TextView(this);
            t.setText("#" + w.id + "  " + (w.title == null || w.title.isEmpty()
                    ? "(untitled)" : w.title)
                    + (w.attached ? "  [attached]" : ""));
            t.setTextSize(15);
            t.setPadding(16, 20, 16, 20);
            t.setTextColor(w.attached ? Color.rgb(0, 128, 0) : Color.rgb(160, 96, 0));
            t.setOnClickListener(v -> {
                logLine("attachWindow " + w.id);
                Awl.attachWindow(MainActivity.this, w.id, w.title);
            });
            t.setOnLongClickListener(v -> {
                logLine("closeWindow " + w.id);
                Awl.closeWindow(w.id);
                return true;
            });
            rows.addView(t);
        }
        if (wins.isEmpty()) {
            TextView empty = new TextView(this);
            empty.setText("none — tap Launch");
            empty.setGravity(Gravity.CENTER);
            empty.setPadding(0, 48, 0, 0);
            rows.addView(empty);
        }
    }

    /** append on the main thread; keep the tail bounded */
    private void logLine(String s) {
        runOnUiThread(() -> {
            log.append(s + "\n");
            if (++logLines > 200) {
                int cut = log.getLayout() != null
                        ? log.getLayout().getLineStart(logLines / 2) : 0;
                if (cut > 0) log.getEditableText().delete(0, cut);
                logLines /= 2;
            }
        });
    }
}
