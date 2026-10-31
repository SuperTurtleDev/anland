package com.anland.consumer;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.graphics.Color;
import android.os.Bundle;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.ScrollView;
import android.widget.Switch;
import android.widget.TextView;

import com.anland.consumer.hud.HudOverlayView;
import com.anland.consumer.theme.M3;

/**
 * Standalone HUD layout editor.
 *
 * This activity intentionally has no SurfaceView and never creates a Native
 * transport. The editor therefore opens on its own black canvas instead of
 * temporarily starting the Anland desktop and placing the editor over it.
 */
public final class HudEditorActivity extends Activity {
    private static final String PREFS_NAME = "anland_settings";

    private HudOverlayView mHudOverlay;

    // The floating keyboard is the other half of the layout: it shares the
    // screen with the HUD, so both are edited here and only one is shown at a
    // time (they are mutually exclusive at runtime as well).
    private VirtualKeyboardView mKeyboard;
    private Button mLayerButton;
    private Button mKeyboardSettingsButton;
    private boolean mKeyboardLayer = false;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        configureFullscreenWindow();

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);
        root.setClipChildren(false);
        root.setClipToPadding(false);

        SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);
        mHudOverlay = new HudOverlayView(this, prefs, new HudOverlayView.HudHost() {
            @Override public void sendKey(int action, int evdevCode) {}
            @Override public void sendTextInput(byte[] utf8) {}
            @Override public void sendMouseMotion(float dx, float dy) {}
            @Override public void sendMouseButton(int button, boolean pressed) {}
            @Override public void sendMouseScroll(int axis, float value) {}
            @Override public void toggleSystemKeyboard() {}
            @Override public void toggleVirtualKeyboard() {}

            @Override public void openSettings() {
                startActivity(new Intent(HudEditorActivity.this, SettingsActivity.class));
            }

            @Override public void toggleSteamDeckMode() {}
            @Override public void exitSteamDeckMode() {}
            @Override public void onLayoutSaved() {}
            @Override public boolean isNativeReady() { return false; }

            @Override public void onEditModeExited() {
                finish();
            }
        });
        root.addView(mHudOverlay, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT,
                FrameLayout.LayoutParams.MATCH_PARENT));

        // The keyboard layer sits above the HUD but below the layer switch, and
        // is hidden until the user asks for it.
        mKeyboard = new VirtualKeyboardView(this);
        // The layout editor is the only place where the two halves of the
        // split keyboard may be dragged around.
        mKeyboard.setSplitDragEnabled(true);
        mKeyboard.setVisibility(View.GONE);
        root.addView(mKeyboard, new FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        setContentView(root);
        hideSystemBars();

        // Enter edit mode only after the view is attached. This keeps the
        // editor toolbar and the first-use notice on the standalone canvas.
        mHudOverlay.post(() -> {
            if (mHudOverlay != null && !isFinishing()) {
                mHudOverlay.setEditMode(true);
                // The layer switch lives in the editor toolbar so it stays
                // reachable while the HUD widgets themselves are hidden.
                mLayerButton = mHudOverlay.addToolbarButton(
                        getString(R.string.keyboard_layer_to_keyboard),
                        () -> setKeyboardLayer(!mKeyboardLayer));
                mKeyboardSettingsButton = mHudOverlay.addToolbarButton(
                        getString(R.string.keyboard_layer_settings),
                        this::showKeyboardSettings);
                updateLayerBar();
            }
        });
    }

    private void updateLayerBar() {
        if (mLayerButton != null) {
            mLayerButton.setText(mKeyboardLayer
                    ? R.string.keyboard_layer_to_hud
                    : R.string.keyboard_layer_to_keyboard);
        }
        if (mKeyboardSettingsButton != null) {
            mKeyboardSettingsButton.setVisibility(mKeyboardLayer ? View.VISIBLE : View.GONE);
        }
    }

    /**
     * Show exactly one floating layer. This mirrors the runtime rule that the
     * keyboard and the HUD are never visible together.
     */
    private void setKeyboardLayer(boolean keyboardLayer) {
        mKeyboardLayer = keyboardLayer;
        if (mKeyboard == null) return;
        if (keyboardLayer) {
            // The keyboard owns a second HUD layer: same widgets, own layout.
            mHudOverlay.setLayer(HudOverlayView.LAYER_KEYBOARD);
            mKeyboard.setVisibility(View.VISIBLE);
            mKeyboard.refreshKeyboardPreferences();
            positionKeyboard();
        } else {
            mKeyboard.setVisibility(View.GONE);
            mHudOverlay.setLayer(HudOverlayView.LAYER_HUD);
            mHudOverlay.rebuildActiveLayout();
        }
        updateLayerBar();
    }

    /**
     * Rotation is handled in place (the manifest declares the orientation /
     * screenSize config changes), so nothing recreates the views for us: the
     * keyboard layer has to be re-laid-out by hand. Its split-arc flag and its
     * saved floating position are both derived from the canvas size, so without
     * this the keyboard kept the previous orientation's size and off-screen
     * translation and the layer switch looked broken after rotating.
     */
    @Override
    public void onConfigurationChanged(android.content.res.Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        if (mKeyboard != null && mKeyboard.getVisibility() == View.VISIBLE) {
            mKeyboard.post(() -> {
                if (mKeyboard == null || mKeyboard.getVisibility() != View.VISIBLE) return;
                mKeyboard.refreshKeyboardPreferences();
                positionKeyboard();
            });
        }
        updateLayerBar();
    }

    /** Split-arc keyboards fill the screen; floating ones sit at their saved spot. */
    private void positionKeyboard() {
        if (mKeyboard == null) return;
        mKeyboard.updateScreenSize();
        ViewGroup.LayoutParams lp = mKeyboard.getLayoutParams();
        if (lp == null) return;
        if (mKeyboard.isSplitArcMode()) {
            lp.width = ViewGroup.LayoutParams.MATCH_PARENT;
            lp.height = ViewGroup.LayoutParams.MATCH_PARENT;
            mKeyboard.setLayoutParams(lp);
            mKeyboard.setTranslationX(0f);
            mKeyboard.setTranslationY(0f);
        } else {
            lp.width = ViewGroup.LayoutParams.WRAP_CONTENT;
            lp.height = ViewGroup.LayoutParams.WRAP_CONTENT;
            mKeyboard.setLayoutParams(lp);
            mKeyboard.post(() -> {
                if (mKeyboard != null && mKeyboard.getVisibility() == View.VISIBLE)
                    mKeyboard.setInitialPosition();
            });
        }
    }

    /**
     * Keyboard appearance / split / handedness / wings, without leaving the
     * editor. Styled with the shared M3 tokens so it does not fall back to the
     * platform's light default widgets.
     */
    private void showKeyboardSettings() {
        final SharedPreferences prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE);

        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(20), dp(16), dp(20), dp(8));

        TextView header = new TextView(this);
        header.setText(R.string.keyboard_layer_settings);
        header.setTextSize(18);
        header.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        header.setTextColor(M3.COLOR_TEXT_PRIMARY);
        header.setPadding(0, 0, 0, dp(12));
        content.addView(header);

        Switch split = new Switch(this);
        M3.styleSwitch(split);
        split.setText(R.string.landscape_split_keyboard);
        split.setChecked(prefs.getBoolean(VirtualKeyboardView.KEY_LANDSCAPE_SPLIT_ARC, true));
        split.setOnCheckedChangeListener((v, checked) -> {
            saveBool(VirtualKeyboardView.KEY_LANDSCAPE_SPLIT_ARC, checked);
            onKeyboardStyleChanged();
        });
        content.addView(split);

        content.addView(sectionLabel(R.string.keyboard_handedness));
        Spinner handedness = createMd3Spinner(R.array.keyboard_handedness_options,
                "right".equals(prefs.getString(VirtualKeyboardView.KEY_KEYBOARD_HANDEDNESS, "left")) ? 1 : 0);
        handedness.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> parent, View view, int pos, long id) {
                getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                        .putString(VirtualKeyboardView.KEY_KEYBOARD_HANDEDNESS,
                                pos == 1 ? "right" : "left").apply();
                onKeyboardStyleChanged();
            }
            @Override public void onNothingSelected(AdapterView<?> parent) {}
        });
        content.addView(handedness);

        content.addView(sectionLabel(R.string.keyboard_theme));
        Spinner theme = createMd3Spinner(R.array.keyboard_theme_options,
                Math.max(VirtualKeyboardView.THEME_FILLED,
                        Math.min(VirtualKeyboardView.THEME_TRANSPARENT_FULL,
                                prefs.getInt(VirtualKeyboardView.KEY_KEYBOARD_THEME,
                                        VirtualKeyboardView.THEME_FILLED))));
        theme.setOnItemSelectedListener(new AdapterView.OnItemSelectedListener() {
            @Override public void onItemSelected(AdapterView<?> parent, View view, int pos, long id) {
                getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit()
                        .putInt(VirtualKeyboardView.KEY_KEYBOARD_THEME, pos).apply();
                onKeyboardStyleChanged();
            }
            @Override public void onNothingSelected(AdapterView<?> parent) {}
        });
        content.addView(theme);

        Switch haptic = new Switch(this);
        M3.styleSwitch(haptic);
        haptic.setText(R.string.keyboard_haptic_feedback);
        haptic.setChecked(prefs.getBoolean(VirtualKeyboardView.KEY_HAPTIC_FEEDBACK, true));
        haptic.setOnCheckedChangeListener((v, checked) ->
                saveBool(VirtualKeyboardView.KEY_HAPTIC_FEEDBACK, checked));
        content.addView(haptic);

        Button wings = createMd3Button(R.string.customize_keyboard_wings, false,
                () -> KeyboardWingsDialog.show(this, this::onKeyboardStyleChanged));
        LinearLayout.LayoutParams wingsLp = new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        wingsLp.topMargin = dp(12);
        content.addView(wings, wingsLp);

        ScrollView scroll = new ScrollView(this);
        scroll.addView(content);

        AlertDialog dialog = new AlertDialog.Builder(this, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
                .setView(scroll)
                .setPositiveButton(R.string.keyboard_wings_done, null)
                .create();
        dialog.show();
        Window dialogWindow = dialog.getWindow();
        if (dialogWindow != null) {
            dialogWindow.setBackgroundDrawable(M3.shape(this, M3.RADIUS_CARD,
                    M3.COLOR_SURFACE, M3.COLOR_BORDER_STRONG, 1f));
            int width = Math.min(dp(460),
                    getResources().getDisplayMetrics().widthPixels - dp(32));
            dialogWindow.setLayout(width, ViewGroup.LayoutParams.WRAP_CONTENT);
        }
        Button done = dialog.getButton(AlertDialog.BUTTON_POSITIVE);
        if (done != null) {
            done.setTextColor(M3.COLOR_PRIMARY);
            done.setAllCaps(false);
        }
    }

    /** M3-styled spinner: tonal field with a dark rounded popup. */
    private Spinner createMd3Spinner(int arrayRes, int selection) {
        String[] items = getResources().getStringArray(arrayRes);
        Spinner spinner = new Spinner(this);
        ArrayAdapter<String> adapter =
                new ArrayAdapter<String>(this, android.R.layout.simple_spinner_item, items) {
            @Override public View getView(int position, View convertView, ViewGroup parent) {
                TextView tv = (TextView) super.getView(position, convertView, parent);
                tv.setTextColor(M3.COLOR_TEXT_PRIMARY);
                tv.setTextSize(14);
                return tv;
            }

            @Override public View getDropDownView(int position, View convertView, ViewGroup parent) {
                TextView tv = (TextView) super.getDropDownView(position, convertView, parent);
                tv.setTextColor(M3.COLOR_TEXT_PRIMARY);
                tv.setTextSize(14);
                tv.setPadding(dp(16), dp(12), dp(16), dp(12));
                tv.setBackgroundColor(M3.COLOR_SURFACE);
                return tv;
            }
        };
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinner.setAdapter(adapter);
        spinner.setSelection(selection);
        // Keep the platform's Material field background (it carries the
        // dropdown chevron); only the popup gets the dark M3 surface.
        spinner.setMinimumHeight(dp(48));
        spinner.setPopupBackgroundDrawable(M3.shape(this, M3.RADIUS_CHIP,
                M3.COLOR_SURFACE, M3.COLOR_BORDER_STRONG, 1f));
        return spinner;
    }

    /** M3 tonal / text button with a ripple and a hairline border. */
    private Button createMd3Button(int textRes, boolean primary, Runnable onClick) {
        Button b = new Button(this);
        b.setText(textRes);
        b.setAllCaps(false);
        b.setTextSize(14);
        b.setTextColor(primary ? M3.COLOR_PRIMARY : M3.COLOR_TEXT_PRIMARY);
        b.setBackground(M3.createRippleDrawable(this, M3.RADIUS_BUTTON,
                primary ? M3.COLOR_PRIMARY_CONTAINER : M3.COLOR_SURFACE_HIGHEST,
                0x44FFFFFF, M3.COLOR_BORDER_SUBTLE));
        b.setMinHeight(dp(44));
        b.setPadding(dp(16), dp(8), dp(16), dp(8));
        b.setOnClickListener(v -> onClick.run());
        return b;
    }

    private TextView sectionLabel(int textRes) {
        TextView tv = new TextView(this);
        tv.setText(textRes);
        tv.setTextSize(13);
        tv.setPadding(0, dp(10), 0, dp(2));
        return tv;
    }

    private void saveBool(String key, boolean value) {
        getSharedPreferences(PREFS_NAME, MODE_PRIVATE).edit().putBoolean(key, value).apply();
    }

    /** Re-layout the visible keyboard after an appearance or layout change. */
    private void onKeyboardStyleChanged() {
        if (mKeyboard == null) return;
        mKeyboard.refreshKeyboardPreferences();
        positionKeyboard();
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void configureFullscreenWindow() {
        Window window = getWindow();
        window.setFlags(WindowManager.LayoutParams.FLAG_FULLSCREEN,
                WindowManager.LayoutParams.FLAG_FULLSCREEN);
        window.setStatusBarColor(Color.BLACK);
        window.setNavigationBarColor(Color.BLACK);
        window.setNavigationBarDividerColor(Color.BLACK);
        window.setDecorFitsSystemWindows(false);
        // hideSystemBars() deliberately runs after setContentView(): the
        // insets controller is reached through the DecorView, which does not
        // exist yet while onCreate() is still configuring the window.
    }

    private void hideSystemBars() {
        Window window = getWindow();
        // Touching the DecorView first also covers callers that arrive before
        // setContentView(), so this can never dereference a null DecorView.
        View decor = window.getDecorView();
        WindowInsetsController controller = window.getInsetsController();
        if (controller != null) {
            controller.hide(WindowInsets.Type.statusBars() | WindowInsets.Type.navigationBars());
            controller.setSystemBarsBehavior(
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        } else {
            decor.setSystemUiVisibility(
                    View.SYSTEM_UI_FLAG_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY
                            | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                            | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                            | View.SYSTEM_UI_FLAG_LAYOUT_STABLE);
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) hideSystemBars();
    }

    @Override
    public void onBackPressed() {
        // Back walks out of the keyboard layer first, then out of the editor.
        if (mKeyboardLayer) {
            setKeyboardLayer(false);
            return;
        }
        if (mHudOverlay != null && mHudOverlay.isEditMode()) {
            mHudOverlay.requestExit();
            return;
        }
        super.onBackPressed();
    }
}
