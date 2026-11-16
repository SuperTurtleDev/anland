package com.anland.consumer;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.SharedPreferences;
import android.graphics.Typeface;
import android.util.Log;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import com.anland.consumer.hud.HudAction;
import com.anland.consumer.theme.M3;

import org.json.JSONArray;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

/**
 * Editor for the 12 thumb-wing keys of the split-arc virtual keyboard.
 *
 * Lives in its own class because the same preference is now edited from two
 * places: Settings (键盘与按键) and the floating-control editor, which shows the
 * keyboard as its own layer. Both write {@link VirtualKeyboardView#KEY_SPLIT_ARC_TOP_WINGS}.
 */
public final class KeyboardWingsDialog {

    private static final String TAG = "KeyboardWings";
    private static final String PREFS_NAME = "anland_settings";
    private static final String KEY_WINGS = VirtualKeyboardView.KEY_SPLIT_ARC_TOP_WINGS;

    private static final String[] LEFT_WING_DEFAULTS =
            {"ESC", "TAB", "CTRL", "ALT", "SHIFT", "SUPER"};
    private static final String[] RIGHT_WING_DEFAULTS =
            {"DEL", "▲", "PGUP", "◀", "▼", "▶"};

    private KeyboardWingsDialog() {}

    /** One selectable action for a wing slot. */
    private static final class Preset {
        final String label;
        final int scanCode;
        final boolean modifier;

        Preset(String label, int scanCode, boolean modifier) {
            this.label = label;
            this.scanCode = scanCode;
            this.modifier = modifier;
        }
    }

    /**
     * Show the wing editor.
     *
     * @param onChanged run after any slot is saved or reset, so a live keyboard
     *                  view (the one shown in the floating-control editor) can
     *                  pick the change up immediately. May be null.
     */
    public static void show(Activity activity, Runnable onChanged) {
        LinearLayout content = new LinearLayout(activity);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(activity, 16), dp(activity, 8), dp(activity, 16), dp(activity, 8));

        final String[] labels = new String[12];
        System.arraycopy(LEFT_WING_DEFAULTS, 0, labels, 0, LEFT_WING_DEFAULTS.length);
        System.arraycopy(RIGHT_WING_DEFAULTS, 0, labels, 6, RIGHT_WING_DEFAULTS.length);
        String saved = prefs(activity).getString(KEY_WINGS, null);
        if (saved != null) {
            try {
                JSONArray slots = new JSONArray(saved);
                for (int i = 0; i < slots.length(); i++) {
                    JSONObject slot = slots.optJSONObject(i);
                    if (slot == null) continue;
                    int index = slot.optInt("index", -1);
                    if (index >= 0 && index < labels.length)
                        labels[index] = slot.optString("label", labels[index]);
                }
            } catch (Exception e) {
                Log.w(TAG, "Ignoring invalid keyboard wing settings", e);
            }
        }

        TextView header = new TextView(activity);
        header.setText(R.string.keyboard_wings_title);
        header.setTextSize(18);
        header.setTypeface(Typeface.DEFAULT_BOLD);
        header.setTextColor(M3.COLOR_TEXT_PRIMARY);
        header.setPadding(0, 0, 0, dp(activity, 12));
        content.addView(header);

        addSection(activity, content, R.string.keyboard_wings_left, 0, labels, onChanged);
        addSection(activity, content, R.string.keyboard_wings_right, 6, labels, onChanged);

        ScrollView scroll = new ScrollView(activity);
        scroll.addView(content);
        AlertDialog dialog = new AlertDialog.Builder(activity, AlertDialog.THEME_DEVICE_DEFAULT_DARK)
                .setView(scroll)
                .setNeutralButton(R.string.keyboard_wings_reset, (d, which) -> {
                    prefs(activity).edit().remove(KEY_WINGS).apply();
                    if (onChanged != null) onChanged.run();
                })
                .setPositiveButton(R.string.keyboard_wings_done, null)
                .create();
        dialog.show();
        if (dialog.getWindow() != null) {
            int width = Math.min(dp(activity, 440),
                    activity.getResources().getDisplayMetrics().widthPixels - dp(activity, 24));
            int height = Math.min(dp(activity, 520),
                    activity.getResources().getDisplayMetrics().heightPixels - dp(activity, 40));
            dialog.getWindow().setBackgroundDrawable(M3.shape(activity, M3.RADIUS_CARD,
                    M3.COLOR_SURFACE, M3.COLOR_BORDER_STRONG, 1f));
            dialog.getWindow().setLayout(width, height);
        }
        Button done = dialog.getButton(AlertDialog.BUTTON_POSITIVE);
        if (done != null) {
            done.setTextColor(M3.COLOR_PRIMARY);
            done.setAllCaps(false);
        }
        Button reset = dialog.getButton(AlertDialog.BUTTON_NEUTRAL);
        if (reset != null) {
            reset.setTextColor(M3.COLOR_TEXT_MUTED);
            reset.setAllCaps(false);
        }
    }

    private static void addSection(Activity activity, LinearLayout parent, int titleRes,
                                   int baseIndex, String[] labels, Runnable onChanged) {
        TextView title = new TextView(activity);
        title.setText(titleRes);
        title.setTextSize(14);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setPadding(0, dp(activity, 8), 0, dp(activity, 4));
        parent.addView(title);

        for (int row = 0; row < 2; row++) {
            LinearLayout rowView = new LinearLayout(activity);
            rowView.setOrientation(LinearLayout.HORIZONTAL);
            LinearLayout.LayoutParams rowParams = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
            if (row > 0) rowParams.topMargin = dp(activity, 4);
            parent.addView(rowView, rowParams);

            for (int col = 0; col < 3; col++) {
                int index = baseIndex + row * 3 + col;
                Button button = new Button(activity);
                button.setText(labels[index]);
                button.setAllCaps(false);
                button.setTextSize(12);
                button.setTextColor(M3.COLOR_TEXT_PRIMARY);
                button.setBackground(M3.createRippleDrawable(activity, M3.RADIUS_CHIP,
                        M3.COLOR_SURFACE_HIGHEST, 0x44FFFFFF, M3.COLOR_BORDER_SUBTLE));
                button.setMinHeight(dp(activity, 44));
                button.setOnClickListener(v ->
                        showPicker(activity, index, button, onChanged));
                LinearLayout.LayoutParams buttonParams = new LinearLayout.LayoutParams(
                        0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
                if (col > 0) buttonParams.leftMargin = dp(activity, 4);
                rowView.addView(button, buttonParams);
            }
        }
    }

    private static void showPicker(Activity activity, int index, Button targetButton,
                                   Runnable onChanged) {
        List<Preset> presets = presets();
        String[] labels = new String[presets.size() + 1];
        labels[0] = activity.getString(R.string.keyboard_wings_use_default);
        for (int i = 0; i < presets.size(); i++) labels[i + 1] = presets.get(i).label;

        new AlertDialog.Builder(activity)
                .setTitle(R.string.keyboard_wings_picker_title)
                .setItems(labels, (dialog, which) -> {
                    if (which == 0) {
                        clear(activity, index);
                        targetButton.setText(defaultLabel(index));
                    } else {
                        Preset preset = presets.get(which - 1);
                        HudAction action = preset.modifier
                                ? HudAction.modifier(preset.scanCode)
                                : HudAction.key(preset.scanCode);
                        action.repeat = isRepeatable(preset.scanCode);
                        save(activity, index, preset.label, action);
                        targetButton.setText(preset.label);
                    }
                    if (onChanged != null) onChanged.run();
                })
                .show();
    }

    private static String defaultLabel(int index) {
        return index < 6 ? LEFT_WING_DEFAULTS[index] : RIGHT_WING_DEFAULTS[index - 6];
    }

    private static List<Preset> presets() {
        List<Preset> presets = new ArrayList<>();
        presets.add(new Preset("CTRL", 29, true));
        presets.add(new Preset("ALT", 56, true));
        presets.add(new Preset("SHIFT", 42, true));
        presets.add(new Preset("SUPER", 125, true));
        String[] specialLabels = {"ESC", "TAB", "ENTER", "BKSP", "SPACE", "DEL", "HOME", "END",
                "PGUP", "PGDN", "UP", "DOWN", "LEFT", "RIGHT"};
        int[] specialCodes = {1, 15, 28, 14, 57, 111, 102, 107, 104, 109, 103, 108, 105, 106};
        for (int i = 0; i < specialLabels.length; i++)
            presets.add(new Preset(specialLabels[i], specialCodes[i], false));

        String letters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
        int[] letterCodes = {30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
                49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
        for (int i = 0; i < letters.length(); i++)
            presets.add(new Preset(String.valueOf(letters.charAt(i)), letterCodes[i], false));

        int[] numberCodes = {2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
        for (int i = 0; i < numberCodes.length; i++)
            presets.add(new Preset(String.valueOf((i + 1) % 10), numberCodes[i], false));

        int[] functionCodes = {59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88};
        for (int i = 0; i < functionCodes.length; i++)
            presets.add(new Preset("F" + (i + 1), functionCodes[i], false));
        return presets;
    }

    private static boolean isRepeatable(int scanCode) {
        switch (scanCode) {
            case 14: case 15: case 102: case 103: case 104: case 105:
            case 106: case 107: case 108: case 109: case 111:
                return true;
            default:
                return false;
        }
    }

    private static void save(Activity activity, int index, String label, HudAction action) {
        try {
            SharedPreferences prefs = prefs(activity);
            JSONArray slots = read(prefs.getString(KEY_WINGS, null));
            JSONObject replacement = new JSONObject();
            replacement.put("index", index);
            replacement.put("label", label);
            replacement.put("action", action.toJSON());
            for (int i = slots.length() - 1; i >= 0; i--) {
                JSONObject slot = slots.optJSONObject(i);
                if (slot != null && slot.optInt("index", -1) == index) slots.remove(i);
            }
            slots.put(replacement);
            prefs.edit().putString(KEY_WINGS, slots.toString()).apply();
        } catch (Exception e) {
            Log.e(TAG, "Unable to save keyboard wing", e);
        }
    }

    private static void clear(Activity activity, int index) {
        SharedPreferences prefs = prefs(activity);
        JSONArray slots = read(prefs.getString(KEY_WINGS, null));
        JSONArray kept = new JSONArray();
        for (int i = 0; i < slots.length(); i++) {
            JSONObject slot = slots.optJSONObject(i);
            if (slot != null && slot.optInt("index", -1) != index) kept.put(slot);
        }
        SharedPreferences.Editor editor = prefs.edit();
        if (kept.length() == 0) editor.remove(KEY_WINGS);
        else editor.putString(KEY_WINGS, kept.toString());
        editor.apply();
    }

    private static JSONArray read(String value) {
        if (value != null && !value.isEmpty()) {
            try {
                return new JSONArray(value);
            } catch (Exception e) {
                Log.w(TAG, "Resetting invalid keyboard wing settings", e);
            }
        }
        return new JSONArray();
    }

    private static SharedPreferences prefs(Activity activity) {
        return activity.getSharedPreferences(PREFS_NAME, Activity.MODE_PRIVATE);
    }

    private static int dp(Activity activity, int value) {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }
}
