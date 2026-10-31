package com.anland.consumer.hud;

import org.json.JSONException;
import org.json.JSONObject;

/**
 * Root profile managing separate Portrait and Landscape layouts, persistence, and default factory presets.
 */
public final class HudLayoutProfile {
    public int version = 2;
    public String profileName = "Niri WM Profile";
    public boolean firstTimeNoticeShown = false;

    public HudLayout portraitLayout = new HudLayout();
    public HudLayout landscapeLayout = new HudLayout();
    /**
     * Second HUD layer that shows while the virtual keyboard is up. Same kind
     * of widgets, separate layout, and deliberately empty by default: nothing
     * appears until the user puts something there.
     */
    public HudLayout keyboardPortraitLayout = new HudLayout();
    public HudLayout keyboardLandscapeLayout = new HudLayout();

    public HudLayoutProfile() {
        initDefaults();
    }

    private void initDefaults() {
        portraitLayout = createDefaultPortraitLayout();
        landscapeLayout = createSteamDeckLayout();
    }

    /**
     * User's actual live Portrait Layout:
     * - Right pill Super Gesture widget (Super+Left, Super+Right, Super+F, Toggle IME)
     * - Quick action buttons: Q (Super+Q), OV (Super+O / Overview), CP (Ctrl+Shift+V / Copy-Paste)
     */
    public static HudLayout createDefaultPortraitLayout() {
        HudLayout layout = new HudLayout();
        layout.dockEnabled = false;

        // 1. Super Gesture Bar (Round Pill on right side)
        HudButton gesture = new HudButton();
        gesture.widgetType = HudButton.WIDGET_SUPER_GESTURE;
        gesture.label = "";
        gesture.posXPercent = 0.817f;
        gesture.posYPercent = 0.397f;
        gesture.widthDp = 120;
        gesture.heightDp = 30;
        gesture.cornerRadiusDp = 29;
        gesture.opacity = 0.10f;
        gesture.bgColor = 0xE62A2B3D;
        gesture.textColor = 0xFFFFFFFF;
        gesture.activeColor = 0xFF80DEEA;
        gesture.action = HudAction.key(0);
        gesture.swipeLeftAction = HudAction.combo(125, 105);   // Super + Left
        gesture.swipeRightAction = HudAction.combo(125, 106);  // Super + Right
        gesture.swipeUpAction = HudAction.combo(125, 33);      // Super + F (Fullscreen)
        gesture.swipeDownAction = HudAction.system("toggle_ime"); // Toggle Soft Keyboard
        gesture.swipeThresholdDp = 30;
        layout.floatingButtons.add(gesture);

        // 2. Q button (Super + Q / Settings)
        HudButton btnQ = new HudButton();
        btnQ.label = "Q";
        btnQ.posXPercent = 0.919f;
        btnQ.posYPercent = 0.437f;
        btnQ.widthDp = 36;
        btnQ.heightDp = 36;
        btnQ.cornerRadiusDp = 8;
        btnQ.opacity = 0.10f;
        btnQ.bgColor = 0xE62A2B3D;
        btnQ.textColor = 0xFFFFFFFF;
        btnQ.activeColor = 0xFF80DEEA;
        btnQ.action = HudAction.combo(125, 16); // Super + Q (Close window)
        btnQ.popupAction = HudAction.system("open_settings");
        layout.floatingButtons.add(btnQ);

        // 3. OV button (Super + O / Enter)
        HudButton btnOv = new HudButton();
        btnOv.label = "OV";
        btnOv.posXPercent = 0.812f;
        btnOv.posYPercent = 0.437f;
        btnOv.widthDp = 36;
        btnOv.heightDp = 36;
        btnOv.cornerRadiusDp = 8;
        btnOv.opacity = 0.10f;
        btnOv.bgColor = 0xE62A2B3D;
        btnOv.textColor = 0xFFFFFFFF;
        btnOv.activeColor = 0xFF80DEEA;
        btnOv.action = HudAction.combo(125, 24); // Super + O (Overview)
        btnOv.popupAction = HudAction.key(28);   // Enter
        layout.floatingButtons.add(btnOv);

        // 4. CP button (Ctrl+Shift+V / Ctrl+Shift+C)
        HudButton btnCp = new HudButton();
        btnCp.label = "CP";
        btnCp.posXPercent = 0.715f;
        btnCp.posYPercent = 0.437f;
        btnCp.widthDp = 36;
        btnCp.heightDp = 36;
        btnCp.cornerRadiusDp = 8;
        btnCp.opacity = 0.10f;
        btnCp.bgColor = 0xE62A2B3D;
        btnCp.textColor = 0xFFFFFFFF;
        btnCp.activeColor = 0xFF80DEEA;
        btnCp.action = HudAction.combo(29, 42, 47);      // Ctrl + Shift + V (Paste)
        btnCp.popupAction = HudAction.combo(29, 42, 46); // Ctrl + Shift + C (Copy)
        layout.floatingButtons.add(btnCp);

        return layout;
    }

    /**
     * Authentic Steam Deck Handheld Console Preset (Based on user-refined ergonomics):
     * - Left: keyboard D-pad, EXIT and Settings.
     * - Right: horizontal scrollbar, touchpad and keyboard toggle.
     */
    /* Always the landscape handheld layout: this preset has no portrait
     * variant, so it takes no orientation argument. */
    public static HudLayout createSteamDeckLayout() {
        HudLayout layout = new HudLayout();
        layout.dockEnabled = false;

        // ===== Left Grip =====

        // 4. D-Pad (Sliding 3D Cross)
        HudButton dpad = new HudButton();
        dpad.widgetType = HudButton.WIDGET_DPAD;
        dpad.label = "D-PAD";
        dpad.widthDp = 124;
        dpad.heightDp = 124;
        dpad.cornerRadiusDp = 62;
        dpad.posXPercent = 0.07046875f;
        dpad.posYPercent = 0.6173611f;
        layout.floatingButtons.add(dpad);

        // 5. Bottom Left: EXIT and Settings
        HudButton btnExit = new HudButton();
        btnExit.label = "EXIT";
        btnExit.action = HudAction.system("exit_steam_deck");
        btnExit.bgColor = 0xE6882222;
        btnExit.widthDp = 44;
        btnExit.heightDp = 30;
        btnExit.cornerRadiusDp = 8;
        btnExit.posXPercent = 0.039375f;
        btnExit.posYPercent = 0.9309028f;
        layout.floatingButtons.add(btnExit);

        HudButton btnSettings = new HudButton();
        btnSettings.label = "⚙";
        btnSettings.action = HudAction.system("open_settings");
        btnSettings.widthDp = 42;
        btnSettings.heightDp = 30;
        btnSettings.cornerRadiusDp = 8;
        btnSettings.posXPercent = 0.1104688f;
        btnSettings.posYPercent = 0.9309028f;
        layout.floatingButtons.add(btnSettings);

        // ===== Right Grip =====

        // 4. Horizontal ScrollBar Widget (placed directly on top of Touchpad)
        HudButton deckScroll = new HudButton();
        deckScroll.widgetType = HudButton.WIDGET_SCROLLBAR;
        deckScroll.label = "☰";
        deckScroll.trackpointMode = "horizontal";
        deckScroll.trackpointSensitivity = 1.5f;
        deckScroll.trackpointDeadzoneDp = 4;
        deckScroll.widthDp = 92;
        deckScroll.heightDp = 22;
        deckScroll.cornerRadiusDp = 11;
        deckScroll.posXPercent = 0.9200000f;
        deckScroll.posYPercent = 0.6173611f;
        layout.floatingButtons.add(deckScroll);

        // 5. Dedicated Steam Deck Haptic Trackpad
        HudButton deckTrackpad = new HudButton();
        deckTrackpad.widgetType = HudButton.WIDGET_TOUCHPAD;
        deckTrackpad.label = "TOUCHPAD";
        deckTrackpad.trackpointMode = "mouse";
        deckTrackpad.trackpointSensitivity = 1.4f;
        deckTrackpad.trackpointDeadzoneDp = 4;
        deckTrackpad.widthDp = 92;
        deckTrackpad.heightDp = 92;
        deckTrackpad.cornerRadiusDp = 18;
        deckTrackpad.posXPercent = 0.9200000f;
        deckTrackpad.posYPercent = 0.7729167f;
        layout.floatingButtons.add(deckTrackpad);

        // 6. Bottom Right: KB and MENU
        HudButton btnVk = new HudButton();
        btnVk.label = "KB";
        btnVk.action = HudAction.system("toggle_vk");
        btnVk.widthDp = 42;
        btnVk.heightDp = 30;
        btnVk.cornerRadiusDp = 8;
        btnVk.posXPercent = 0.8926563f;
        btnVk.posYPercent = 0.9333333f;
        layout.floatingButtons.add(btnVk);

        return layout;
    }

    /**
     * Ergonomic Productivity & Niri Desktop Preset (10% Margin):
     * - Left side: TrackPoint Mouse Nub, ESC, TAB, CTRL, ALT.
     * - Right side: Super Gesture Bar, Q (Super+Q), OV (Super+O), CP (Copy-Paste), ENTER, BKSP, KB, Settings.
     */
    public static HudLayout createProductivityLayout(boolean isLandscape) {
        HudLayout layout = new HudLayout();
        layout.dockEnabled = false;

        float leftX = isLandscape ? 0.13f : 0.18f;
        float rightX = isLandscape ? 0.87f : 0.82f;

        // Left Side Controls (10% Margin)
        HudButton mouseNub = new HudButton();
        mouseNub.widgetType = HudButton.WIDGET_TRACKPOINT;
        mouseNub.label = "MOUSE";
        mouseNub.widthDp = 62;
        mouseNub.heightDp = 62;
        mouseNub.cornerRadiusDp = 31;
        mouseNub.posXPercent = isLandscape ? 0.15f : 0.20f;
        mouseNub.posYPercent = 0.68f;
        layout.floatingButtons.add(mouseNub);

        HudButton btnEsc = new HudButton();
        btnEsc.label = "ESC";
        btnEsc.action = HudAction.key(1);
        btnEsc.widthDp = 48;
        btnEsc.heightDp = 34;
        btnEsc.cornerRadiusDp = 8;
        btnEsc.posXPercent = leftX;
        btnEsc.posYPercent = 0.16f;
        layout.floatingButtons.add(btnEsc);

        HudButton btnTab = new HudButton();
        btnTab.label = "TAB";
        btnTab.action = HudAction.key(15);
        btnTab.widthDp = 48;
        btnTab.heightDp = 34;
        btnTab.cornerRadiusDp = 8;
        btnTab.posXPercent = leftX;
        btnTab.posYPercent = 0.28f;
        layout.floatingButtons.add(btnTab);

        HudButton btnCtrl = new HudButton();
        btnCtrl.label = "CTRL";
        btnCtrl.action = HudAction.modifier(29);
        btnCtrl.widthDp = 48;
        btnCtrl.heightDp = 34;
        btnCtrl.cornerRadiusDp = 8;
        btnCtrl.posXPercent = leftX;
        btnCtrl.posYPercent = 0.40f;
        layout.floatingButtons.add(btnCtrl);

        HudButton btnAlt = new HudButton();
        btnAlt.label = "ALT";
        btnAlt.action = HudAction.modifier(56);
        btnAlt.widthDp = 48;
        btnAlt.heightDp = 34;
        btnAlt.cornerRadiusDp = 8;
        btnAlt.posXPercent = leftX + (isLandscape ? 0.07f : 0.12f);
        btnAlt.posYPercent = 0.40f;
        layout.floatingButtons.add(btnAlt);

        // Right Side Controls (10% Margin)
        HudButton superGesture = new HudButton();
        superGesture.widgetType = HudButton.WIDGET_SUPER_GESTURE;
        superGesture.label = "SUPER";
        superGesture.widthDp = 120;
        superGesture.heightDp = 32;
        superGesture.cornerRadiusDp = 29;
        superGesture.posXPercent = isLandscape ? 0.85f : 0.80f;
        superGesture.posYPercent = 0.16f;
        superGesture.swipeLeftAction = HudAction.combo(125, 105);
        superGesture.swipeRightAction = HudAction.combo(125, 106);
        superGesture.swipeUpAction = HudAction.combo(125, 33);
        superGesture.swipeDownAction = HudAction.system("toggle_ime");
        layout.floatingButtons.add(superGesture);

        HudButton btnQ = new HudButton();
        btnQ.label = "Q";
        btnQ.action = HudAction.combo(125, 16);
        btnQ.popupAction = HudAction.system("open_settings");
        btnQ.widthDp = 38;
        btnQ.heightDp = 38;
        btnQ.cornerRadiusDp = 8;
        btnQ.posXPercent = rightX;
        btnQ.posYPercent = 0.36f;
        layout.floatingButtons.add(btnQ);

        HudButton btnOv = new HudButton();
        btnOv.label = "OV";
        btnOv.action = HudAction.combo(125, 24);
        btnOv.popupAction = HudAction.key(28);
        btnOv.widthDp = 38;
        btnOv.heightDp = 38;
        btnOv.cornerRadiusDp = 8;
        btnOv.posXPercent = rightX - (isLandscape ? 0.06f : 0.10f);
        btnOv.posYPercent = 0.36f;
        layout.floatingButtons.add(btnOv);

        HudButton btnCp = new HudButton();
        btnCp.label = "CP";
        btnCp.action = HudAction.combo(29, 42, 47);
        btnCp.popupAction = HudAction.combo(29, 42, 46);
        btnCp.widthDp = 38;
        btnCp.heightDp = 38;
        btnCp.cornerRadiusDp = 8;
        btnCp.posXPercent = rightX - (isLandscape ? 0.12f : 0.20f);
        btnCp.posYPercent = 0.36f;
        layout.floatingButtons.add(btnCp);

        HudButton btnEnter = new HudButton();
        btnEnter.label = "ENTER";
        btnEnter.action = HudAction.key(28);
        btnEnter.widthDp = 58;
        btnEnter.heightDp = 38;
        btnEnter.cornerRadiusDp = 8;
        btnEnter.posXPercent = rightX;
        btnEnter.posYPercent = 0.65f;
        layout.floatingButtons.add(btnEnter);

        HudButton btnBksp = new HudButton();
        btnBksp.label = "BKSP";
        btnBksp.action = HudAction.key(14);
        btnBksp.widthDp = 58;
        btnBksp.heightDp = 38;
        btnBksp.cornerRadiusDp = 8;
        btnBksp.posXPercent = rightX - (isLandscape ? 0.09f : 0.15f);
        btnBksp.posYPercent = 0.65f;
        layout.floatingButtons.add(btnBksp);

        HudButton btnVk = new HudButton();
        btnVk.label = "KB";
        btnVk.action = HudAction.system("toggle_vk");
        btnVk.widthDp = 58;
        btnVk.heightDp = 34;
        btnVk.cornerRadiusDp = 8;
        btnVk.posXPercent = rightX;
        btnVk.posYPercent = 0.85f;
        layout.floatingButtons.add(btnVk);

        HudButton btnSet = new HudButton();
        btnSet.label = "⚙";
        btnSet.action = HudAction.system("open_settings");
        btnSet.widthDp = 58;
        btnSet.heightDp = 34;
        btnSet.cornerRadiusDp = 8;
        btnSet.posXPercent = rightX - (isLandscape ? 0.09f : 0.15f);
        btnSet.posYPercent = 0.85f;
        layout.floatingButtons.add(btnSet);

        return layout;
    }

    public JSONObject toJSON() throws JSONException {
        JSONObject obj = new JSONObject();
        obj.put("version", version);
        obj.put("profile_name", profileName);
        obj.put("first_time_notice_shown", firstTimeNoticeShown);
        obj.put("portrait_layout", portraitLayout.toJSON());
        obj.put("landscape_layout", landscapeLayout.toJSON());
        obj.put("keyboard_portrait_layout", keyboardPortraitLayout.toJSON());
        obj.put("keyboard_landscape_layout", keyboardLandscapeLayout.toJSON());
        return obj;
    }

    public static HudLayoutProfile fromJSON(String jsonStr) {
        if (jsonStr == null || jsonStr.trim().isEmpty()) {
            return new HudLayoutProfile();
        }
        try {
            JSONObject obj = new JSONObject(jsonStr);
            HudLayoutProfile p = new HudLayoutProfile();
            p.version = obj.optInt("version", 2);
            p.profileName = obj.optString("profile_name", "Niri WM Profile");
            p.firstTimeNoticeShown = obj.optBoolean("first_time_notice_shown", false);
            if (obj.has("portrait_layout")) {
                p.portraitLayout = HudLayout.fromJSON(obj.optJSONObject("portrait_layout"));
            }
            if (obj.has("landscape_layout")) {
                p.landscapeLayout = HudLayout.fromJSON(obj.optJSONObject("landscape_layout"));
            }
            if (p.portraitLayout.floatingButtons.isEmpty()) {
                p.portraitLayout = createDefaultPortraitLayout();
            }
            if (p.landscapeLayout.floatingButtons.isEmpty()) {
                p.landscapeLayout = createSteamDeckLayout();
            }
            // The keyboard layer never falls back to a preset: empty is a
            // perfectly valid layout there, and that is the default.
            if (obj.has("keyboard_portrait_layout")) {
                HudLayout kb = HudLayout.fromJSON(obj.optJSONObject("keyboard_portrait_layout"));
                if (kb != null) p.keyboardPortraitLayout = kb;
            }
            if (obj.has("keyboard_landscape_layout")) {
                HudLayout kb = HudLayout.fromJSON(obj.optJSONObject("keyboard_landscape_layout"));
                if (kb != null) p.keyboardLandscapeLayout = kb;
            }
            return p;
        } catch (Exception e) {
            return new HudLayoutProfile();
        }
    }
}
