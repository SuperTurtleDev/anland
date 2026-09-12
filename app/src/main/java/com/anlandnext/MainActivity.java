package com.anlandnext;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Gravity;
import android.view.Menu;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.PopupMenu;
import android.widget.ScrollView;
import android.widget.TextView;

import com.anlandnext.awl.Awl;

import java.util.List;
import java.util.Locale;

/**
 * Window list (launcher entry), implemented on the libawl client library —
 * the same API third-party consumer apps use (Awl.getWindows /
 * registerCallback / attachWindow / closeWindow). The daemon scopes by
 * caller identity: this APK is package-authenticated, so it manages every
 * window; a consumer app only ever sees its own uid's.
 * Refreshed from daemon window events (create/destroy/attach/detach) while
 * resumed, plus a snapshot pull on resume (events are live-edge only).
 * Tap → attachWindow (bring-to-front / re-attach). Long-press → dropdown
 * menu (the long-press itself does nothing, guarding against accidental
 * triggers):
 *   Close — the only close entry (daemon T_CLOSE → the client exits
 *   cleanly);
 *   Window info — id/title/attach state.
 */
public class MainActivity extends Activity {
    private static final int MENU_CLOSE = 1;
    private static final int MENU_INFO = 2;

    private LinearLayout list;
    private TextView status;

    private final Handler handler = new Handler(Looper.getMainLooper());
    private final Runnable refresher = this::refresh;

    /** daemon window events → one coalesced snapshot pull (a re-attach bursts
     *  detach+attach; a window going away bursts destroy+detach) */
    private final Awl.Callback events = new Awl.Callback() {
        @Override public void onWindowCreated(long id, String title) { scheduleRefresh(); }
        @Override public void onWindowDestroyed(long id) { scheduleRefresh(); }
        @Override public void onWindowAttached(long id) { scheduleRefresh(); }
        @Override public void onWindowDetached(long id) { scheduleRefresh(); }
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

        Button settings = new Button(this);
        settings.setText(R.string.settings_title);
        settings.setOnClickListener(v ->
                startActivity(new Intent(this, WlSettingsActivity.class)));
        root.addView(settings);

        ScrollView sc = new ScrollView(this);
        list = new LinearLayout(this);
        list.setOrientation(LinearLayout.VERTICAL);
        sc.addView(list);
        root.addView(sc, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, 0, 1f));

        setContentView(root);
    }

    @Override
    protected void onResume() {
        super.onResume();
        Awl.registerCallback(events);   /* live list updates while resumed (daemon disconnects a paused subscriber) */
        refresh();
    }

    @Override
    protected void onPause() {
        handler.removeCallbacks(refresher);
        Awl.unregisterCallback(events);   /* last callback out → subscription dropped */
        super.onPause();
    }

    /** event-burst coalescing: one snapshot pull per burst */
    private void scheduleRefresh() {
        handler.removeCallbacks(refresher);
        handler.postDelayed(refresher, 60);
    }

    private void refresh() {
        list.removeAllViews();
        List<Awl.WlWindow> wins = Awl.getWindows();
        if (wins == null) {
            status.setText(R.string.status_daemon_unreachable);
            return;
        }
        Awl.ensureSubscribed();   /* daemon restarted under us → re-subscribe the event stream */
        status.setText(getString(R.string.status_window_count, wins.size()));
        for (Awl.WlWindow w : wins) list.addView(row(w));
        if (wins.isEmpty()) {
            TextView empty = new TextView(this);
            empty.setText(R.string.status_empty);
            empty.setGravity(Gravity.CENTER);
            empty.setPadding(0, 96, 0, 0);
            list.addView(empty);
        }
    }

    private View row(Awl.WlWindow w) {
        LinearLayout r = new LinearLayout(this);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(24, 28, 24, 28);

        TextView t = new TextView(this);
        t.setText(w.title == null || w.title.isEmpty()
                ? getString(R.string.window_fallback_title, w.id) : w.title);
        t.setTextSize(16);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        r.addView(t, new LinearLayout.LayoutParams(0,
                LinearLayout.LayoutParams.WRAP_CONTENT, 1f));

        TextView st = new TextView(this);
        st.setText(w.attached ? "Attached" : "Detached");
        st.setTextSize(13);
        st.setTextColor(w.attached ? Color.rgb(0, 128, 0) : Color.rgb(160, 96, 0));
        r.addView(st);

        r.setOnClickListener(v -> {
            /* libawl hosting: AwlWindowActivity (merged from the library) —
             * the daemon evicts any previous holder of this window */
            Awl.attachWindow(MainActivity.this, w.id, w.title);
        });
        r.setOnLongClickListener(v -> {
            showRowMenu(v, w);
            return true;
        });
        return r;
    }

    /** Row long-press dropdown: Close (the only close entry) / Window info */
    private void showRowMenu(View anchor, Awl.WlWindow w) {
        PopupMenu menu = new PopupMenu(this, anchor);
        menu.getMenu().add(Menu.NONE, MENU_CLOSE, Menu.NONE, R.string.menu_close);
        menu.getMenu().add(Menu.NONE, MENU_INFO, Menu.NONE, R.string.menu_window_info);
        menu.setOnMenuItemClickListener(item -> {
            if (item.getItemId() == MENU_CLOSE) {
                Awl.closeWindow(w.id);   /* daemon decides: the client exits cleanly */
                scheduleRefresh();
            } else if (item.getItemId() == MENU_INFO) {
                new AlertDialog.Builder(this)
                        .setTitle(R.string.menu_window_info)
                        .setMessage(String.format(Locale.US,
                                getString(R.string.window_info_format),
                                w.id,
                                w.title == null || w.title.isEmpty()
                                        ? getString(R.string.title_none) : w.title,
                                w.attached ? getString(R.string.state_attached_foreground)
                                           : getString(R.string.state_detached_kept)))
                        .setPositiveButton(R.string.dialog_ok, null)
                        .show();
            }
            return true;
        });
        menu.show();
    }
}
