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

        setContentView(root);
    }
}
