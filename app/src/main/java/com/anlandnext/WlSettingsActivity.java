package com.anlandnext;

import android.app.Activity;
import android.os.Bundle;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.TextView;

/** Window behavior settings.
 *  IME mode → SharedPreferences (APK-local, read by WlWindowActivity);
 *  display zoom → daemon config.json (the daemon is the single source of
 *  truth, read/written over binder, #31).
 *  (The exit-behavior setting is gone: swiping away / killing the background
 *  is fixed to minimize-and-keep-alive, and the only close entry is the
 *  window-list long-press menu — all decisions live in the daemon, the APK
 *  has no policy left to configure.) */
public class WlSettingsActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setTitle(R.string.settings_title);

        android.widget.LinearLayout root = new android.widget.LinearLayout(this);
        root.setOrientation(android.widget.LinearLayout.VERTICAL);
        root.setPadding(48, 64, 48, 48);

        /* ---- IME display mode ---- */
        TextView imeTip = new TextView(this);
        imeTip.setText(R.string.ime_mode_tip);
        root.addView(imeTip);

        RadioGroup imeRg = new RadioGroup(this);
        RadioButton inset = new RadioButton(this);
        inset.setId(3);
        inset.setText(R.string.ime_mode_inset);
        RadioButton overlay = new RadioButton(this);
        overlay.setId(4);
        overlay.setText(R.string.ime_mode_overlay);

        imeRg.addView(inset);
        imeRg.addView(overlay);
        int imeMode = getSharedPreferences("awl", MODE_PRIVATE).getInt("ime_mode", 0);
        (imeMode != 0 ? overlay : inset).setChecked(true);
        imeRg.setOnCheckedChangeListener((g, checkedId) ->
                getSharedPreferences("awl", MODE_PRIVATE).edit()
                        .putInt("ime_mode", checkedId == 4 ? 1 : 0)
                        .apply());
        root.addView(imeRg);

        /* ---- Display zoom (arbitrary ratio, daemon-side; #31) ---- */
        android.widget.Space gap2 = new android.widget.Space(this);
        gap2.setMinimumHeight(64);
        root.addView(gap2);

        TextView zoomTip = new TextView(this);
        zoomTip.setText(R.string.zoom_tip);
        root.addView(zoomTip);

        TextView zoomVal = new TextView(this);
        zoomVal.setTextSize(20);
        root.addView(zoomVal);

        int got = WlBinder.configGet("zoom");
        final int[] cur = {(got < 50 || got > 300) ? 100 : got};   /* daemon down / unknown → 100 */

        android.widget.SeekBar seek = new android.widget.SeekBar(this);
        seek.setMax(250);          /* progress = pct - 50 → any value in 50..300 */
        seek.setProgress(cur[0] - 50);
        root.addView(seek);

        /* 200ms drag debounce + immediate on release/preset — applies
         * dynamically without bombarding per tick (each set = a
         * preferred_scale broadcast + re-configure of every window) */
        android.os.Handler h = new android.os.Handler(getMainLooper());
        final Runnable[] pending = new Runnable[1];
        Runnable apply = () -> WlBinder.configSet("zoom", cur[0]);
        java.util.function.IntConsumer showZoom = pct ->
                zoomVal.setText(pct + "%  =  " + (pct / 100.0) + "×");

        java.util.function.IntConsumer setZoom = pct -> {
            cur[0] = pct;
            seek.setProgress(pct - 50);
            showZoom.accept(pct);
            if (pending[0] != null) h.removeCallbacks(pending[0]);
            h.post(apply);   /* preset: immediate */
        };
        seek.setOnSeekBarChangeListener(new android.widget.SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(android.widget.SeekBar sb, int prog, boolean fromUser) {
                if (!fromUser) return;
                cur[0] = prog + 50;
                showZoom.accept(cur[0]);
                if (pending[0] != null) h.removeCallbacks(pending[0]);
                int pct = cur[0];
                pending[0] = () -> WlBinder.configSet("zoom", pct);
                h.postDelayed(pending[0], 200);
            }
            @Override public void onStartTrackingTouch(android.widget.SeekBar sb) { }
            @Override public void onStopTrackingTouch(android.widget.SeekBar sb) {
                if (pending[0] != null) h.removeCallbacks(pending[0]);
                pending[0] = null;
                h.post(apply);   /* release: immediate */
            }
        });
        showZoom.accept(cur[0]);

        android.widget.LinearLayout presets = new android.widget.LinearLayout(this);
        presets.setOrientation(android.widget.LinearLayout.HORIZONTAL);
        int[] ratios = {100, 150, 175, 200, 250};
        for (int r : ratios) {
            android.widget.Button btn = new android.widget.Button(this);
            btn.setText(r + "%");
            btn.setOnClickListener(v -> setZoom.accept(r));
            android.widget.LinearLayout.LayoutParams lp =
                    new android.widget.LinearLayout.LayoutParams(
                            0, android.widget.LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
            presets.addView(btn, lp);
        }
        root.addView(presets);

        /* ---- Initial window size (first-frame configure placeholder; #33,
         *      daemon config init_w/init_h — new windows only) ---- */
        android.widget.Space gap3 = new android.widget.Space(this);
        gap3.setMinimumHeight(64);
        root.addView(gap3);

        TextView sizeTip = new TextView(this);
        sizeTip.setText(R.string.init_size_tip);
        root.addView(sizeTip);

        final TextView sizeVal = new TextView(this);
        sizeVal.setTextSize(20);
        root.addView(sizeVal);

        /* pickers bounded to the daemon's accepted domain (a set outside it
         * is rejected — keep the UI from producing one) */
        final android.widget.NumberPicker wp = new android.widget.NumberPicker(this);
        final android.widget.NumberPicker hp = new android.widget.NumberPicker(this);
        android.widget.LinearLayout pickers = new android.widget.LinearLayout(this);
        pickers.setOrientation(android.widget.LinearLayout.HORIZONTAL);
        TextView wl = new TextView(this);
        wl.setText(R.string.init_size_w);
        android.widget.LinearLayout.LayoutParams plp =
                new android.widget.LinearLayout.LayoutParams(
                        0, android.widget.LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        android.widget.LinearLayout.LayoutParams tlp =
                new android.widget.LinearLayout.LayoutParams(
                        android.widget.LinearLayout.LayoutParams.WRAP_CONTENT,
                        android.widget.LinearLayout.LayoutParams.WRAP_CONTENT);
        pickers.addView(wl, tlp);
        pickers.addView(wp, plp);
        TextView hl = new TextView(this);
        hl.setText(R.string.init_size_h);
        pickers.addView(hl, tlp);
        pickers.addView(hp, plp);
        root.addView(pickers);

        int gw = WlBinder.configGet("init_w");
        int gh = WlBinder.configGet("init_h");
        final int[] sz = {(gw >= 100 && gw <= 7680) ? gw : 800,   /* daemon down → defaults */
                          (gh >= 100 && gh <= 4320) ? gh : 600};

        /* debounce like zoom (a scroll fires many changes); presets apply
         * immediately */
        final Runnable[] pendSz = new Runnable[1];
        final Runnable applySz = () -> {
            WlBinder.configSet("init_w", sz[0]);
            WlBinder.configSet("init_h", sz[1]);
        };
        final Runnable showSz = () -> sizeVal.setText(sz[0] + " × " + sz[1]);
        final Runnable changedSz = () -> {
            showSz.run();
            if (pendSz[0] != null) h.removeCallbacks(pendSz[0]);
            int w = sz[0], ht = sz[1];
            pendSz[0] = () -> {
                WlBinder.configSet("init_w", w);
                WlBinder.configSet("init_h", ht);
            };
            h.postDelayed(pendSz[0], 300);
        };
        android.widget.NumberPicker.OnValueChangeListener ncl = (p, o, n) -> {
            if (p == wp) sz[0] = n; else sz[1] = n;
            changedSz.run();
        };
        wp.setMinValue(100);
        wp.setMaxValue(7680);
        wp.setWrapSelectorWheel(false);
        wp.setValue(sz[0]);
        wp.setOnValueChangedListener(ncl);
        hp.setMinValue(100);
        hp.setMaxValue(4320);
        hp.setWrapSelectorWheel(false);
        hp.setValue(sz[1]);
        hp.setOnValueChangedListener(ncl);

        android.widget.LinearLayout sizes = new android.widget.LinearLayout(this);
        sizes.setOrientation(android.widget.LinearLayout.HORIZONTAL);
        int[][] presets2 = {{800, 600}, {1024, 768}, {1280, 720}, {1920, 1080}};
        for (int[] s : presets2) {
            android.widget.Button btn = new android.widget.Button(this);
            btn.setText(s[0] + "×" + s[1]);
            btn.setOnClickListener(v -> {
                if (pendSz[0] != null) h.removeCallbacks(pendSz[0]);
                pendSz[0] = null;
                sz[0] = s[0];
                sz[1] = s[1];
                wp.setValue(s[0]);
                hp.setValue(s[1]);
                showSz.run();
                h.post(applySz);
            });
            sizes.addView(btn, lp1());
        }
        root.addView(sizes);
        showSz.run();

        setContentView(root);
    }

    private static android.widget.LinearLayout.LayoutParams lp1() {
        return new android.widget.LinearLayout.LayoutParams(
                0, android.widget.LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
    }
}
