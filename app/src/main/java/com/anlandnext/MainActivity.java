package com.anlandnext;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.view.Gravity;
import android.view.Menu;
import android.view.View;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.PopupMenu;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.Locale;

/**
 * Window list (launcher entry): shows the wayland windows the daemon holds
 * and their attach state.
 * Tap → BRING (bring-to-front for attached ones / re-attach for detached
 * ones). Long-press → dropdown menu (the long-press itself does nothing,
 * guarding against accidental triggers):
 *   Close — the only close entry (daemon T_CLOSE → the client exits
 *   cleanly);
 *   Window info — id/title/attach state.
 */
public class MainActivity extends Activity {
    private static final int MENU_CLOSE = 1;
    private static final int MENU_INFO = 2;

    private LinearLayout list;
    private TextView status;

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
        refresh();
    }

    private void refresh() {
        list.removeAllViews();
        ArrayList<WlBinder.WinInfo> wins = WlBinder.list();
        if (wins == null) {
            status.setText(R.string.status_daemon_unreachable);
            return;
        }
        status.setText(getString(R.string.status_window_count, wins.size()));
        for (WlBinder.WinInfo w : wins) list.addView(row(w));
        if (wins.isEmpty()) {
            TextView empty = new TextView(this);
            empty.setText(R.string.status_empty);
            empty.setGravity(Gravity.CENTER);
            empty.setPadding(0, 96, 0, 0);
            list.addView(empty);
        }
    }

    private View row(WlBinder.WinInfo w) {
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
            WlBinder.bring(w.id);
            refresh();
        });
        r.setOnLongClickListener(v -> {
            showRowMenu(v, w);
            return true;
        });
        return r;
    }

    /** Row long-press dropdown: Close (the only close entry) / Window info */
    private void showRowMenu(View anchor, WlBinder.WinInfo w) {
        PopupMenu menu = new PopupMenu(this, anchor);
        menu.getMenu().add(Menu.NONE, MENU_CLOSE, Menu.NONE, R.string.menu_close);
        menu.getMenu().add(Menu.NONE, MENU_INFO, Menu.NONE, R.string.menu_window_info);
        menu.setOnMenuItemClickListener(item -> {
            if (item.getItemId() == MENU_CLOSE) {
                WlBinder.close(w.id);   /* daemon decides: the client exits cleanly */
                refresh();
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
