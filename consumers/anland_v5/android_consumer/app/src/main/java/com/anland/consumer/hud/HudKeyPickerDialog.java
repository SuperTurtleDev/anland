package com.anland.consumer.hud;

import com.anland.consumer.R;
import com.anland.consumer.theme.M3;
import android.app.AlertDialog;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.EditText;
import android.widget.HorizontalScrollView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/**
 * Categorized Tabbed Dialog for picking keys, WM macros, standard
 * alphanumeric keys, and system actions. Strictly adheres to Anland
 * design language without emojis.
 *
 * The dialog has three tabs:
 *   - KEYS         alphanumeric + F-keys, four-column dense grid.
 *   - MODS & SYS   modifiers + nav / editing + system actions, three-column.
 *   - COMBO        the ComboBuilderView rendered inline (no nested dialog).
 */
public final class HudKeyPickerDialog {

    public interface OnActionSelectedListener {
        void onActionSelected(HudAction action, String displayLabel);
        // Optional: lets the combo builder deliver a custom-built combo
        // without changing the regular picker. Implementations can just
        // forward to onActionSelected if they do not care.
        default void onComboBuilt(Context context, HudAction action) {
            onActionSelected(action, formatComboLabel(context, action));
        }
    }

    // Render a combo's keycodes as "CTRL + C" / "SUPER + L" / "F12" style
    // text. The "None" branch is taken when the slot is empty.
    public static String formatComboLabel(Context context, HudAction action) {
        if (action == null || action.comboKeys == null || action.comboKeys.isEmpty()) {
            return context.getString(R.string.hud_none);
        }
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < action.comboKeys.size(); i++) {
            if (i > 0) sb.append(" + ");
            sb.append(HudPropertyInspectorView.labelForEvdev(context, action.comboKeys.get(i)));
        }
        return sb.toString();
    }

    private static final class KeyEntry {
        final String label;
        final HudAction action;
        KeyEntry(String label, HudAction action) {
            this.label = label;
            this.action = action;
        }
    }

    public static void show(Context context, OnActionSelectedListener listener) {
        show(context, listener, null);
    }

    public static void show(Context context, OnActionSelectedListener listener, Runnable onDismiss) {
        AlertDialog.Builder builder = new AlertDialog.Builder(context, AlertDialog.THEME_DEVICE_DEFAULT_DARK);

        LinearLayout root = new LinearLayout(context);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(dp(context, 16), dp(context, 16), dp(context, 16), dp(context, 16));
        root.setBackground(M3.shape(context, M3.RADIUS_CARD, M3.COLOR_SURFACE, M3.COLOR_BORDER_STRONG, 1.0f));

        // Header Row: Back Button on left + Title
        LinearLayout headerRow = new LinearLayout(context);
        headerRow.setOrientation(LinearLayout.HORIZONTAL);
        headerRow.setGravity(Gravity.CENTER_VERTICAL);
        headerRow.setPadding(0, 0, 0, dp(context, 10));

        AlertDialog dialog = builder.setView(root).create();
        if (dialog.getWindow() != null) {
            dialog.getWindow().setBackgroundDrawable(new android.graphics.drawable.ColorDrawable(Color.TRANSPARENT));
        }

        LinearLayout btnBack = new LinearLayout(context);
        btnBack.setOrientation(LinearLayout.HORIZONTAL);
        btnBack.setGravity(Gravity.CENTER_VERTICAL);
        btnBack.setBackground(M3.createRippleDrawable(context, M3.RADIUS_CHIP, M3.COLOR_SURFACE_HIGHEST, 0x44FFFFFF, M3.COLOR_BORDER_SUBTLE));
        btnBack.setPadding(dp(context, 8), dp(context, 4), dp(context, 10), dp(context, 4));
        btnBack.setClickable(true);
        btnBack.setOnClickListener(v -> dialog.dismiss());

        M3.BackArrowView backArrow = new M3.BackArrowView(context);
        btnBack.addView(backArrow);

        TextView backText = new TextView(context);
        backText.setText(context.getString(R.string.hud_picker_back));
        backText.setTextSize(11.5f);
        backText.setTypeface(Typeface.DEFAULT_BOLD);
        backText.setTextColor(M3.COLOR_PRIMARY);
        backText.setPadding(dp(context, 6), 0, 0, 0);
        btnBack.addView(backText);

        headerRow.addView(btnBack);

        TextView title = new TextView(context);
        title.setText(context.getString(R.string.hud_picker_title));
        title.setTextColor(M3.COLOR_TEXT_PRIMARY);
        title.setTextSize(16);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        title.setPadding(dp(context, 10), 0, 0, 0);
        headerRow.addView(title);
        root.addView(headerRow);

        // Tab Navigation Bar (wrapped in HorizontalScrollView so the labels
        // never get squeezed on narrow portrait phones).
        HorizontalScrollView tabScroller = new HorizontalScrollView(context);
        tabScroller.setHorizontalScrollBarEnabled(false);
        tabScroller.setFillViewport(true);
        LinearLayout tabRow = new LinearLayout(context);
        tabRow.setOrientation(LinearLayout.HORIZONTAL);
        tabRow.setGravity(Gravity.CENTER_VERTICAL);
        tabRow.setPadding(dp(context, 4), 0, dp(context, 4), dp(context, 8));
        tabScroller.addView(tabRow, new HorizontalScrollView.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        root.addView(tabScroller, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));

        // Search filter input
        EditText searchInput = new EditText(context);
        M3.styleInput(searchInput);
        searchInput.setHint(context.getString(R.string.hud_picker_search));
        searchInput.setHintTextColor(0xFF888888);
        searchInput.setSingleLine(true);
        LinearLayout.LayoutParams searchLp = new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        searchLp.setMargins(0, 0, 0, dp(context, 8));
        searchInput.setLayoutParams(searchLp);
        root.addView(searchInput);

        boolean isLandscape = context.getResources().getConfiguration().orientation
                == android.content.res.Configuration.ORIENTATION_LANDSCAPE;

        int screenW = context.getResources().getDisplayMetrics().widthPixels;
        int screenH = context.getResources().getDisplayMetrics().heightPixels;
        final int dialogW = isLandscape ? Math.min(dp(context, 540), screenW - dp(context, 32))
                                        : Math.min(dp(context, 360), screenW - dp(context, 20));
        final int dialogH = isLandscape ? Math.min(dp(context, 330), screenH - dp(context, 24))
                                        : Math.min(dp(context, 520), screenH - dp(context, 64));
        root.setLayoutParams(new ViewGroup.LayoutParams(dialogW, dialogH));

        // Content Container
        LinearLayout contentContainer = new LinearLayout(context);
        contentContainer.setOrientation(LinearLayout.VERTICAL);
        root.addView(contentContainer, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        Button btnTabKeys = createTabButton(context, context.getString(R.string.hud_tab_keys));
        Button btnTabMods = createTabButton(context, context.getString(R.string.hud_tab_mods_system));
        Button btnTabCombo = createTabButton(context, context.getString(R.string.hud_tab_combo));

        tabRow.addView(btnTabKeys);
        tabRow.addView(btnTabMods);
        tabRow.addView(btnTabCombo);

        // KEYS is dense 4-column (6 in landscape); MODS & SYS is 3-column (4 in landscape)
        final int colsKeys = isLandscape ? 6 : computeColumnCount(context, 4);
        final int colsLong = isLandscape ? 4 : 3;

        final int[] activeTab = {0};
        final Runnable refreshCurrentTab = () -> {
            String query = searchInput.getText() != null ? searchInput.getText().toString().trim().toLowerCase() : "";
            List<KeyEntry> source;
            if (activeTab[0] == 0) source = getStandardKeys();
            else source = getModsAndSystemKeys(context);
            List<KeyEntry> filtered = new ArrayList<>();
            if (query.isEmpty()) {
                filtered.addAll(source);
            } else {
                for (KeyEntry e : source) {
                    if (e.label.toLowerCase().contains(query)) {
                        filtered.add(e);
                    }
                }
            }
            int cols = (activeTab[0] == 0) ? colsKeys : colsLong;
            showGrid(context, contentContainer, filtered, listener, dialog, cols);
        };
        searchInput.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) {}
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {}
            @Override public void afterTextChanged(Editable s) {
                if (activeTab[0] != 2) refreshCurrentTab.run();
            }
        });

        Runnable[] selectTab = new Runnable[3];
        selectTab[0] = () -> {
            activeTab[0] = 0;
            searchInput.setVisibility(View.VISIBLE);
            highlightTab(tabRow, 0);
            refreshCurrentTab.run();
        };
        selectTab[1] = () -> {
            activeTab[0] = 1;
            searchInput.setVisibility(View.VISIBLE);
            highlightTab(tabRow, 1);
            refreshCurrentTab.run();
        };
        // The COMBO tab inlines the combo builder so the user does not get a
        // second nested dialog. Tapping APPLY calls onComboBuilt which
        // dismisses the outer dialog and forwards the new HudAction.combo
        // to the caller's onActionSelected (via the default in
        // OnActionSelectedListener).
        selectTab[2] = () -> {
            activeTab[0] = 2;
            searchInput.setVisibility(View.GONE);
            highlightTab(tabRow, 2);
            showComboTab(context, contentContainer, listener, dialog);
        };

        btnTabKeys.setOnClickListener(v -> selectTab[0].run());
        btnTabMods.setOnClickListener(v -> selectTab[1].run());
        btnTabCombo.setOnClickListener(v -> selectTab[2].run());

        dialog.setOnDismissListener(d -> {
            if (onDismiss != null) onDismiss.run();
        });

        selectTab[0].run();
        dialog.show();
        if (dialog.getWindow() != null) {
            dialog.getWindow().setLayout(dialogW, dialogH);
        }
    }

    // COMBO tab body: build a ComboBuilderView and place its content inside
    // the contentContainer. The builder's APPLY handler dismisses the outer
    // dialog (so the user is not stuck inside two layers) and reports the
    // chosen action through onComboBuilt.
    private static void showComboTab(final Context ctx, final LinearLayout container,
                                    final OnActionSelectedListener listener,
                                    final AlertDialog dialog) {
        container.removeAllViews();
        new ComboBuilderView().showInline(ctx, container, combo -> {
            dialog.dismiss();
            if (listener != null) listener.onComboBuilt(ctx, combo);
        });
    }

    private static Button createTabButton(Context ctx, String text) {
        Button b = new Button(ctx, null, android.R.attr.buttonBarButtonStyle);
        b.setText(text);
        b.setTextSize(12);
        b.setTextColor(0xFF888888);
        b.setPadding(dp(ctx, 14), dp(ctx, 6), dp(ctx, 14), dp(ctx, 6));
        b.setBackgroundColor(Color.TRANSPARENT);
        b.setAllCaps(true);
        return b;
    }

    private static void highlightTab(LinearLayout tabRow, int activeIdx) {
        for (int i = 0; i < tabRow.getChildCount(); i++) {
            Button b = (Button) tabRow.getChildAt(i);
            if (i == activeIdx) {
                b.setTextColor(M3.COLOR_PRIMARY);
                b.setTypeface(Typeface.DEFAULT_BOLD);
                b.setBackground(M3.shape(b.getContext(), M3.RADIUS_CHIP, M3.COLOR_PRIMARY_CONTAINER, M3.COLOR_PRIMARY, 1.0f));
            } else {
                b.setTextColor(M3.COLOR_TEXT_MUTED);
                b.setTypeface(Typeface.DEFAULT);
                b.setBackgroundColor(Color.TRANSPARENT);
            }
        }
    }

    private static void showGrid(
            Context ctx,
            LinearLayout container,
            List<KeyEntry> entries,
            OnActionSelectedListener listener,
            AlertDialog dialog,
            int columns
    ) {
        container.removeAllViews();

        // Explicit vertical LinearLayout of horizontal rows. The GridView
        // attempt earlier only rendered the first row on this dialog size;
        // this layout is dumb but always correct and reacts well to long
        // labels because each cell is given equal share of the width via
        // LayoutParams weight=1.
        ScrollView scroll = new ScrollView(ctx);
        LinearLayout grid = new LinearLayout(ctx);
        grid.setOrientation(LinearLayout.VERTICAL);
        int hSpacing = dp(ctx, 6);
        int vSpacing = dp(ctx, 6);
        int sidePad = dp(ctx, 4);
        grid.setPadding(sidePad, dp(ctx, 6), sidePad, dp(ctx, 8));

        for (int row = 0; row < entries.size(); row += columns) {
            LinearLayout rowView = new LinearLayout(ctx);
            rowView.setOrientation(LinearLayout.HORIZONTAL);
            rowView.setBaselineAligned(false);
            LinearLayout.LayoutParams rowLp = new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT);
            if (row > 0) rowLp.topMargin = vSpacing;
            grid.addView(rowView, rowLp);

            for (int col = 0; col < columns; col++) {
                int idx = row + col;
                if (idx >= entries.size()) {
                    View spacer = new View(ctx);
                    LinearLayout.LayoutParams spacerLp = new LinearLayout.LayoutParams(
                            0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
                    rowView.addView(spacer, spacerLp);
                    continue;
                }
                final KeyEntry entry = entries.get(idx);
                Button btn = new Button(ctx, null, android.R.attr.buttonBarButtonStyle);
                btn.setText(entry.label);
                btn.setTextColor(Color.WHITE);
                btn.setBackground(M3.createRippleDrawable(ctx, M3.RADIUS_CHIP, M3.COLOR_SURFACE_HIGHEST, 0x44FFFFFF, M3.COLOR_BORDER_SUBTLE));
                btn.setPadding(dp(ctx, 4), dp(ctx, 6), dp(ctx, 4), dp(ctx, 6));
                btn.setMinHeight(dp(ctx, 44));
                btn.setMaxLines(2);
                btn.setSingleLine(false);
                btn.setEllipsize(android.text.TextUtils.TruncateAt.END);
                btn.setGravity(Gravity.CENTER);
                btn.setIncludeFontPadding(false);
                if (entry.label.length() > 9) {
                    btn.setTextSize(10.5f);
                } else {
                    btn.setTextSize(12.5f);
                }
                LinearLayout.LayoutParams btnLp = new LinearLayout.LayoutParams(
                        0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f);
                if (col > 0) btnLp.leftMargin = hSpacing;
                btn.setLayoutParams(btnLp);
                btn.setOnClickListener(v -> {
                    dialog.dismiss();
                    if (listener != null) {
                        listener.onActionSelected(entry.action.copy(), entry.label);
                    }
                });
                rowView.addView(btn);
            }
        }

        scroll.addView(grid, new ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        container.addView(scroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));
    }

    private static int computeColumnCount(Context ctx, int minColumns) {
        int widthDp = ctx.getResources().getConfiguration().screenWidthDp;
        if (widthDp <= 0) widthDp = 360;
        int cols = widthDp / 80;
        if (cols < minColumns) cols = minColumns;
        if (cols > minColumns + 2) cols = minColumns + 2;
        return cols;
    }

    // Modifiers, nav / editing keys, and system actions all share the
    // 3-column grid in the "MODS & SYS" tab so the user does not have to
    // switch tabs just to set an action on a swipe.
    private static List<KeyEntry> getModsAndSystemKeys(Context context) {
        List<KeyEntry> list = new ArrayList<>();
        // Modifiers
        list.add(new KeyEntry("SUPER", HudAction.modifier(125)));
        list.add(new KeyEntry("CTRL", HudAction.modifier(29)));
        list.add(new KeyEntry("ALT", HudAction.modifier(56)));
        list.add(new KeyEntry("SHIFT", HudAction.modifier(42)));
        // Common editing keys
        list.add(new KeyEntry("ESC", HudAction.key(1)));
        list.add(new KeyEntry("TAB", HudAction.key(15)));
        list.add(new KeyEntry("ENTER", HudAction.key(28)));
        list.add(new KeyEntry("BKSP", HudAction.key(14)));
        list.add(new KeyEntry("SPACE", HudAction.key(57)));
        list.add(new KeyEntry("DEL", HudAction.key(111)));
        list.add(new KeyEntry("HOME", HudAction.key(102)));
        list.add(new KeyEntry("END", HudAction.key(107)));
        list.add(new KeyEntry("PGUP", HudAction.key(104)));
        list.add(new KeyEntry("PGDN", HudAction.key(109)));
        // Arrow keys
        list.add(new KeyEntry("UP", HudAction.key(103)));
        list.add(new KeyEntry("DOWN", HudAction.key(108)));
        list.add(new KeyEntry("LEFT", HudAction.key(105)));
        list.add(new KeyEntry("RIGHT", HudAction.key(106)));
        // System actions - clear, concise titles that fit comfortably on any screen
        list.add(new KeyEntry(context.getString(R.string.hud_system_soft_keyboard), HudAction.system("toggle_ime")));
        list.add(new KeyEntry(context.getString(R.string.hud_system_virtual_keyboard), HudAction.system("toggle_vk")));
        list.add(new KeyEntry(context.getString(R.string.hud_system_settings), HudAction.system("open_settings")));
        list.add(new KeyEntry(context.getString(R.string.hud_system_mouse_left), HudAction.system("mouse_left")));
        list.add(new KeyEntry(context.getString(R.string.hud_system_mouse_right), HudAction.system("mouse_right")));
        list.add(new KeyEntry(context.getString(R.string.hud_system_mouse_middle), HudAction.system("mouse_middle")));
        list.add(new KeyEntry(context.getString(R.string.hud_system_scroll_up), HudAction.system("mouse_scroll_up")));
        list.add(new KeyEntry(context.getString(R.string.hud_system_scroll_down), HudAction.system("mouse_scroll_down")));
        list.add(new KeyEntry(context.getString(R.string.hud_system_steam_deck_mode), HudAction.system("toggle_steam_deck")));
        list.add(new KeyEntry(context.getString(R.string.hud_system_exit_deck_mode), HudAction.system("exit_steam_deck")));
        return list;
    }

    private static List<KeyEntry> getStandardKeys() {
        List<KeyEntry> list = new ArrayList<>();
        // Letters A-Z
        int[] letterCodes = {30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50, 49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44};
        for (int i = 0; i < 26; i++) {
            char ch = (char) ('A' + i);
            list.add(new KeyEntry(String.valueOf(ch), HudAction.key(letterCodes[i])));
        }
        // Numbers 0-9
        int[] numCodes = {11, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        for (int i = 0; i <= 9; i++) {
            list.add(new KeyEntry(String.valueOf(i), HudAction.key(numCodes[i])));
        }
        // F1-F12
        int[] fCodes = {59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 87, 88};
        for (int i = 1; i <= 12; i++) {
            list.add(new KeyEntry("F" + i, HudAction.key(fCodes[i - 1])));
        }

        // ---- Punctuation (unshifted symbols) ----
        addKeys(list, new String[]{
                "`", "-", "=", "[", "]", "\\", ";", "'", ",", ".", "/"},
                new int[]{41, 12, 13, 26, 27, 43, 39, 40, 51, 52, 53});

        // ---- Shifted symbols: sent as Shift + base key combos ----
        // evdev pairs are {shift, base}; the picker shows the resulting glyph.
        String[] shifted = {"~", "!", "@", "#", "$", "%", "^", "&", "*",
                "(", ")", "_", "+", "{", "}", "|", ":", "\"", "<", ">", "?"};
        int[][] shiftedCodes = {
                {42, 41}, {42, 2}, {42, 3}, {42, 4}, {42, 5}, {42, 6}, {42, 7},
                {42, 8}, {42, 9}, {42, 10}, {42, 11}, {42, 12}, {42, 13},
                {42, 26}, {42, 27}, {42, 43}, {42, 39}, {42, 40}, {42, 51},
                {42, 52}, {42, 53}};
        for (int i = 0; i < shifted.length; i++) {
            list.add(new KeyEntry(shifted[i], combo(shiftedCodes[i][0], shiftedCodes[i][1])));
        }

        // ---- Editing / locking keys ----
        addKeys(list, new String[]{"INSERT", "MENU", "CAPS", "NUMLOCK", "PRTSC", "SCROLL", "PAUSE"},
                new int[]{110, 139, 58, 69, 99, 70, 119});
        // Right-hand modifiers, distinct from the generic ones in MODS & SYS.
        list.add(new KeyEntry("SHIFT (R)", HudAction.modifier(54)));
        list.add(new KeyEntry("CTRL (R)", HudAction.modifier(97)));
        list.add(new KeyEntry("ALT (R)", HudAction.modifier(100)));

        // ---- Numeric keypad ----
        addKeys(list, new String[]{"KP0", "KP1", "KP2", "KP3", "KP4", "KP5", "KP6", "KP7", "KP8", "KP9"},
                new int[]{82, 79, 80, 81, 75, 76, 77, 71, 72, 73});
        addKeys(list, new String[]{"KP.", "KP*", "KP-", "KP+", "KPENTER", "KP/"},
                new int[]{83, 55, 74, 78, 96, 98});

        // ---- Multimedia / brightness ----
        addKeys(list, new String[]{"MEDIA PLAY", "MEDIA NEXT", "MEDIA PREV", "MEDIA STOP",
                        "MUTE", "VOL-", "VOL+", "BRI-", "BRI+"},
                new int[]{164, 163, 165, 166, 113, 114, 115, 224, 225});
        return list;
    }

    /** Build a two-key (modifier + key) combo action. */
    private static HudAction combo(int modifier, int key) {
        HudAction a = new HudAction();
        a.type = HudAction.TYPE_COMBO;
        a.comboKeys = new java.util.ArrayList<>();
        a.comboKeys.add(modifier);
        a.comboKeys.add(key);
        return a;
    }

    private static void addKeys(List<KeyEntry> list, String[] labels, int[] codes) {
        for (int i = 0; i < labels.length && i < codes.length; i++) {
            list.add(new KeyEntry(labels[i], HudAction.key(codes[i])));
        }
    }

    private static int dp(Context ctx, int dpVal) {
        return Math.round(dpVal * ctx.getResources().getDisplayMetrics().density);
    }
}
