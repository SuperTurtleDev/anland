package com.anland.consumer.hud;

import com.anland.consumer.R;
import com.anland.consumer.theme.M3;
import android.content.Context;
import android.graphics.Color;
import android.graphics.drawable.GradientDrawable;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/**
 * Free-form combo builder. The user picks up to three keys and the view
 * emits a HudAction.combo(int[]...) when the user accepts. The active
 * slot is highlighted; pressing a key fills the slot and advances to
 * the next empty one.
 *
 * Strict no-emoji rule: no glyph, all labels are plain Latin text.
 */
public final class ComboBuilderView {

    public interface OnComboBuiltListener {
        void onComboBuilt(HudAction action);
    }

    private static final int MAX_SLOTS = 3;

    // The keycode -> human label map. We keep it tiny on purpose: the
    // builder is a fallback for the predefined macros in
    // HudKeyPickerDialog, so the common keys are enough.
    private static final int[] MODIFIER_CODES = {
            29,   // CTRL
            56,   // ALT
            125,  // SUPER
            42,   // SHIFT
    };
    private static final String[] MODIFIER_LABELS = {"CTRL", "ALT", "SUPER", "SHIFT"};

    private final int[] mSlotCodes = new int[MAX_SLOTS];
    private int mActiveSlot = 0;
    private final TextView[] mSlotLabels = new TextView[MAX_SLOTS];

    // All selectable keycodes for the second / third rows. Modifier
    // keys go in the first column; letters + digits in the middle;
    // function keys at the end.
    private final List<Integer> mKeycodes = new ArrayList<>();
    private final List<String> mKeyLabels = new ArrayList<>();

    public ComboBuilderView() {
        // 0. Modifiers (CTRL, ALT, SUPER, SHIFT) - included in normal grid!
        for (int m = 0; m < MODIFIER_CODES.length; m++) {
            final int code = MODIFIER_CODES[m];
            final String lbl = MODIFIER_LABELS[m];
            mKeycodes.add(code);
            mKeyLabels.add(lbl);
        }
        // 1. Navigation & Arrow keys (evdev scancodes)
        int[] navCodes = {103, 108, 105, 106, 102, 107, 104, 109};
        String[] navLabels = {"UP", "DOWN", "LEFT", "RIGHT", "HOME", "END", "PGUP", "PGDN"};
        for (int i = 0; i < navCodes.length; i++) {
            mKeycodes.add(navCodes[i]);
            mKeyLabels.add(navLabels[i]);
        }
        // 2. Common editing keys (evdev scancodes)
        int[] editCodes = {1, 15, 28, 57, 14, 111};
        String[] editLabels = {"ESC", "TAB", "ENTER", "SPACE", "BKSP", "DEL"};
        for (int i = 0; i < editCodes.length; i++) {
            mKeycodes.add(editCodes[i]);
            mKeyLabels.add(editLabels[i]);
        }
        // 3. Letters A-Z (evdev scancodes: 30=A, 48=B, etc.)
        int[] letterCodes = {30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
        for (int i = 0; i < 26; i++) {
            mKeycodes.add(letterCodes[i]);
            mKeyLabels.add(String.valueOf((char) ('A' + i)));
        }
        // 4. Numbers 0-9 (evdev scancodes: 11=0, 2=1..10=9)
        int[] numCodes = {11, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        for (int d = 0; d <= 9; d++) {
            mKeycodes.add(numCodes[d]);
            mKeyLabels.add(String.valueOf(d));
        }
        // 5. F1-F12 (evdev scancodes: 59-68, 87, 88)
        int[] fCodes = {59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88};
        for (int i = 0; i < fCodes.length; i++) {
            mKeycodes.add(fCodes[i]);
            mKeyLabels.add("F" + (i + 1));
        }
        // 6. Symbols (evdev scancodes)
        int[] symCodes = {12, 13, 26, 27, 43, 39, 40, 51, 52, 53, 41};
        String[] symLabels = {"-", "=", "[", "]", "\\", ";", "'", ",", ".", "/", "`"};
        for (int i = 0; i < symCodes.length; i++) {
            mKeycodes.add(symCodes[i]);
            mKeyLabels.add(symLabels[i]);
        }
    }

    /** Reset all slots to empty. */
    public void clear() {
        for (int i = 0; i < MAX_SLOTS; i++) {
            mSlotCodes[i] = 0;
            if (mSlotLabels[i] != null) mSlotLabels[i].setText(slotLabel(mSlotLabels[i].getContext(), i + 1));
        }
        mActiveSlot = 0;
        refreshActiveSlot();
    }

    /** Clear a single slot by index. */
    public void clearSlot(int index) {
        if (index < 0 || index >= MAX_SLOTS) return;
        mSlotCodes[index] = 0;
        if (mSlotLabels[index] != null) mSlotLabels[index].setText(slotLabel(mSlotLabels[index].getContext(), index + 1));
        mActiveSlot = index;
        refreshActiveSlot();
    }

    /** Set a slot's keycode by index. */
    public void setSlot(int index, int keycode) {
        if (index < 0 || index >= MAX_SLOTS) return;
        mSlotCodes[index] = keycode;
        if (mSlotLabels[index] != null) mSlotLabels[index].setText(labelFor(keycode));
        // Auto-advance to the next empty slot.
        int next = -1;
        for (int i = 0; i < MAX_SLOTS; i++) {
            if (mSlotCodes[i] == 0) { next = i; break; }
        }
        mActiveSlot = next >= 0 ? next : (index + 1) % MAX_SLOTS;
        refreshActiveSlot();
    }

    /** Mark a slot as the user's current focus. */
    public void setActiveSlot(int index) {
        if (index < 0 || index >= MAX_SLOTS) return;
        mActiveSlot = index;
        refreshActiveSlot();
    }

    private void refreshActiveSlot() {
        for (int i = 0; i < MAX_SLOTS; i++) {
            if (mSlotLabels[i] == null) continue;
            Context ctx = mSlotLabels[i].getContext();
            if (i == mActiveSlot) {
                GradientDrawable d = new GradientDrawable();
                d.setCornerRadius(dp(ctx, 8));
                d.setColor(0xFFA39CB8); // Lilac pastel
                mSlotLabels[i].setBackground(d);
                mSlotLabels[i].setTextColor(Color.WHITE);
                mSlotLabels[i].setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
            } else if (mSlotCodes[i] != 0) {
                GradientDrawable d = new GradientDrawable();
                d.setCornerRadius(dp(ctx, 8));
                d.setColor(0xFF484848);
                d.setStroke(dp(ctx, 1), 0xFFA39CB8);
                mSlotLabels[i].setBackground(d);
                mSlotLabels[i].setTextColor(Color.WHITE);
                mSlotLabels[i].setTypeface(android.graphics.Typeface.DEFAULT);
            } else {
                GradientDrawable d = new GradientDrawable();
                d.setCornerRadius(dp(ctx, 8));
                d.setColor(0xFF2E2E2E);
                d.setStroke(dp(ctx, 1), 0x33FFFFFF);
                mSlotLabels[i].setBackground(d);
                mSlotLabels[i].setTextColor(0xFF888888);
                mSlotLabels[i].setTypeface(android.graphics.Typeface.DEFAULT);
            }
        }
    }

    private String labelFor(int keycode) {
        Context context = mSlotLabels[0] != null ? mSlotLabels[0].getContext()
                : (mPreview != null ? mPreview.getContext() : null);
        if (keycode == 0) {
            return slotLabel(context, mActiveSlot + 1);
        }
        return HudPropertyInspectorView.labelForEvdev(context, keycode);
    }

    private static String slotLabel(Context context, int slot) {
        return context.getString(R.string.hud_combo_slot, slot);
    }

    private void commit() {
        // Collapse to only the filled slots, in order.
        int[] arr = new int[MAX_SLOTS];
        int n = 0;
        for (int i = 0; i < MAX_SLOTS; i++) {
            if (mSlotCodes[i] != 0) arr[n++] = mSlotCodes[i];
        }
        if (n == 0) return;
        int[] packed = new int[n];
        System.arraycopy(arr, 0, packed, 0, n);
        HudAction action = HudAction.combo(packed);
        mListener.onComboBuilt(action);
    }

    private OnComboBuiltListener mListener;
    private TextView mPreview;

    /**
     * The combo builder body is appended to {@code host}. The caller
     * dismisses its own outer dialog (e.g. the HudKeyPickerDialog) by
     * calling the onComboBuilt callback.
     */
    public void showInline(Context ctx, ViewGroup host, OnComboBuiltListener listener) {
        showInline(ctx, host, null, listener);
    }

    public void showInline(Context ctx, ViewGroup host, LinearLayout slotContainer, OnComboBuiltListener listener) {
        mListener = listener;
        host.removeAllViews();
        host.setBackgroundColor(Color.TRANSPARENT);

        if (slotContainer != null) {
            slotContainer.removeAllViews();
            slotContainer.addView(buildSlotRow(ctx), new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }

        // On short screens, let the whole builder scroll rather than squeezing
        // its weighted key grid down to an unusable sliver.
        ScrollView scroll = new ScrollView(ctx);
        scroll.setFillViewport(true);
        LinearLayout content = buildContent(ctx);
        content.setMinimumHeight(dp(ctx, 320));
        scroll.addView(content, new ScrollView.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        host.addView(scroll, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
    }

    private LinearLayout buildSlotRow(final Context ctx) {
        boolean isLandscape = ctx.getResources().getConfiguration().orientation
                == android.content.res.Configuration.ORIENTATION_LANDSCAPE;
        LinearLayout slotRow = new LinearLayout(ctx);
        slotRow.setOrientation(LinearLayout.HORIZONTAL);
        slotRow.setGravity(Gravity.CENTER_VERTICAL);

        for (int i = 0; i < MAX_SLOTS; i++) {
            TextView slot = new TextView(ctx);
            slot.setText(slotLabel(ctx, i + 1));
            slot.setTextSize(isLandscape ? 11f : 12f);
            slot.setAllCaps(false);
            slot.setTextColor(0xFF888888);
            slot.setGravity(Gravity.CENTER_VERTICAL | Gravity.START);
            slot.setPadding(dp(ctx, 10), 0, dp(ctx, 6), 0);
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, dp(ctx, 32), 1f);
            if (i > 0) lp.leftMargin = dp(ctx, 6);
            slotRow.addView(slot, lp);
            mSlotLabels[i] = slot;
            final int slotIndex = i;
            slot.setOnClickListener(v -> {
                if (mActiveSlot == slotIndex && mSlotCodes[slotIndex] != 0) {
                    clearSlot(slotIndex);
                } else {
                    setActiveSlot(slotIndex);
                }
            });
        }
        refreshActiveSlot();
        return slotRow;
    }

    // Build the dialog body.
    private LinearLayout buildContent(final Context ctx) {
        boolean isLandscape = ctx.getResources().getConfiguration().orientation
                == android.content.res.Configuration.ORIENTATION_LANDSCAPE;
        LinearLayout root = new LinearLayout(ctx);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(Color.TRANSPARENT);
        root.setPadding(0, dp(ctx, 4), 0, dp(ctx, 4));
        // KEY PICKER GRID INSIDE FLEX SCROLLVIEW
        ScrollView keyScroll = new ScrollView(ctx);
        keyScroll.setVerticalScrollBarEnabled(true);
        keyScroll.setScrollbarFadingEnabled(false);
        keyScroll.setScrollBarStyle(View.SCROLLBARS_INSIDE_INSET);
        keyScroll.setBackgroundColor(Color.TRANSPARENT); // Transparent background

        GradientDrawable thumb = new GradientDrawable();
        thumb.setShape(GradientDrawable.RECTANGLE);
        thumb.setCornerRadius(dp(ctx, 4));
        thumb.setColor(0xFFA39CB8); // Lilac pastel
        thumb.setSize(dp(ctx, 4), dp(ctx, 36));
        keyScroll.setVerticalScrollbarThumbDrawable(thumb);

        LinearLayout keyGrid = new LinearLayout(ctx);
        keyGrid.setOrientation(LinearLayout.VERTICAL);
        keyGrid.setPadding(dp(ctx, 2), dp(ctx, 2), dp(ctx, 12), dp(ctx, 4)); // Right padding 12dp to make room for scrollbar

        final int keyCols = isLandscape ? 5 : 3; // Standard 5 columns
        final int hSpacing = dp(ctx, isLandscape ? 4 : 6);
        final int vSpacing = dp(ctx, isLandscape ? 4 : 6);
        for (int i = 0; i < mKeycodes.size(); i += keyCols) {
            LinearLayout row = new LinearLayout(ctx);
            row.setOrientation(LinearLayout.HORIZONTAL);
            row.setBaselineAligned(false);
            LinearLayout.LayoutParams rowLp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            if (i > 0) rowLp.topMargin = vSpacing;
            keyGrid.addView(row, rowLp);
            for (int col = 0; col < keyCols; col++) {
                final int idx = i + col;
                if (idx >= mKeycodes.size()) {
                    ViewGroup.LayoutParams sp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
                    row.addView(new android.view.View(ctx), sp);
                    continue;
                }
                final int code = mKeycodes.get(idx);
                final String lbl = mKeyLabels.get(idx);
                Button k = new Button(ctx, null, android.R.attr.buttonBarButtonStyle);
                k.setText(lbl);
                k.setTextSize(isLandscape ? 11f : 12f);
                k.setAllCaps(false);
                k.setTextColor(Color.WHITE);
                GradientDrawable kBg = new GradientDrawable();
                kBg.setCornerRadius(dp(ctx, 8));
                kBg.setColor(0xFF484848);
                kBg.setStroke(dp(ctx, 1), 0x33FFFFFF);
                k.setBackground(kBg);
                k.setMinHeight(dp(ctx, isLandscape ? 30 : 38));
                k.setPadding(dp(ctx, 4), dp(ctx, 2), dp(ctx, 4), dp(ctx, 2));
                k.setSingleLine(true);
                k.setEllipsize(android.text.TextUtils.TruncateAt.END);
                k.setGravity(Gravity.CENTER);
                k.setIncludeFontPadding(false);
                LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
                if (col > 0) blp.leftMargin = hSpacing;
                k.setLayoutParams(blp);
                k.setOnClickListener(v -> {
                    setSlot(mActiveSlot, code);
                });
                row.addView(k);
            }
        }
        keyScroll.addView(keyGrid, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        // Weight = 1f ensures the key grid takes the flex space and never pushes actions off screen!
        LinearLayout.LayoutParams scrollLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f);
        scrollLp.setMargins(0, 0, 0, dp(ctx, 6));
        root.addView(keyScroll, scrollLp);

        // Action row: Clear All + Apply Combo (always pinned at the bottom)
        LinearLayout actions = new LinearLayout(ctx);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        actions.setPadding(0, dp(ctx, 4), 0, 0);

        Button btnClear = new Button(ctx, null, android.R.attr.buttonBarButtonStyle);
        btnClear.setText(ctx.getString(R.string.hud_combo_clear_all));
        btnClear.setTextSize(12);
        btnClear.setAllCaps(true);
        btnClear.setTextColor(Color.WHITE);
        GradientDrawable clrBg = new GradientDrawable();
        clrBg.setCornerRadius(dp(ctx, 12));
        clrBg.setColor(0xFF878787); // Grey button
        btnClear.setBackground(clrBg);
        btnClear.setMinHeight(dp(ctx, 32));
        btnClear.setOnClickListener(v -> clear());
        actions.addView(btnClear, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f));

        Button btnApply = new Button(ctx, null, android.R.attr.buttonBarButtonStyle);
        btnApply.setText(ctx.getString(R.string.hud_combo_apply));
        btnApply.setTextSize(12);
        btnApply.setAllCaps(true);
        btnApply.setTextColor(Color.WHITE);
        btnApply.setTypeface(android.graphics.Typeface.DEFAULT_BOLD);
        GradientDrawable appBg = new GradientDrawable();
        appBg.setCornerRadius(dp(ctx, 12));
        appBg.setColor(0xFFA39CB8); // Lilac pastel
        btnApply.setBackground(appBg);
        btnApply.setMinHeight(dp(ctx, 32));
        btnApply.setOnClickListener(v -> commit());
        LinearLayout.LayoutParams alp = new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
        alp.leftMargin = dp(ctx, 8);
        actions.addView(btnApply, alp);

        root.addView(actions);

        refreshActiveSlot();

        return root;
    }

    private void updatePreview() {
    }

    private static int dp(Context ctx, int v) {
        return Math.round(v * ctx.getResources().getDisplayMetrics().density);
    }
}
