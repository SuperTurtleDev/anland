package com.anland.consumer.hud;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

/**
 * Model representing a single customizable on-screen widget (Button, TrackPoint, Super Gesture).
 */
public final class HudButton {
    public static final String WIDGET_STANDARD = "button";
    public static final String WIDGET_SUPER_GESTURE = "super_gesture";
    public static final String WIDGET_TRACKPOINT = "trackpoint";
    public static final String WIDGET_TOUCHPAD = "touchpad";
    public static final String WIDGET_SCROLLBAR = "scrollbar";
    public static final String WIDGET_DPAD = "dpad";
    /** Container that holds child keys and moves them as one unit. */
    public static final String WIDGET_GROUP = "group";

    // Strongly-typed mirror of WIDGET_* above, used by code that switches
    // over the widget kind (e.g. the toolbar's add-widget chooser).
    public enum WidgetKind {
        KEY,           // WIDGET_STANDARD
        SUPER_GESTURE, // WIDGET_SUPER_GESTURE
        TRACKPOINT,    // WIDGET_TRACKPOINT
        TOUCHPAD,      // WIDGET_TOUCHPAD
        SCROLLBAR,     // WIDGET_SCROLLBAR
        DPAD,          // WIDGET_DPAD
        GROUP          // WIDGET_GROUP
    }

    public String id = UUID.randomUUID().toString();
    public String widgetType = WIDGET_STANDARD;
    public String label = "BTN";

    // Position as screen percentages [0.0 - 1.0] for resolution/rotation independence
    public float posXPercent = 0.5f;
    public float posYPercent = 0.5f;

    // Size in density-independent pixels (dp)
    public int widthDp = 64;
    public int heightDp = 40;
    /** Inner padding for WIDGET_GROUP, in dp. */
    public int padDp = 8;
    public int cornerRadiusDp = 8;
    public float opacity = 0.85f;

    // Styling colors (ARGB hex)
    public int bgColor = 0xE62A2B3D;
    public int textColor = 0xFFFFFFFF;
    public int activeColor = 0xFF80DEEA;

    // Action mappings
    public HudAction action = new HudAction();

    /**
     * Children of a WIDGET_GROUP container. They are drawn by HudGroupView,
     * which arranges them in a centred grid inside the container, so moving the
     * group moves them all as one unit. Their posXPercent / posYPercent are
     * ignored for group children and only kept so old profiles still load.
     */
    public List<HudButton> children = new ArrayList<>();
    public HudAction popupAction = null; // optional swipe-up secondary action

    // Super gesture specific mappings
    public HudAction swipeLeftAction = HudAction.combo(125, 105);   // Super + Left
    public HudAction swipeRightAction = HudAction.combo(125, 106);  // Super + Right
    public HudAction swipeUpAction = HudAction.combo(125, 24);      // Super + O (Overview)
    public HudAction swipeDownAction = HudAction.combo(125, 57);    // Super + Space (Launcher)
    public int swipeThresholdDp = 30;

    // TrackPoint specific mappings
    public float trackpointSensitivity = 1.5f;
    public int trackpointDeadzoneDp = 4;
    public static final String MODE_MOUSE = "mouse";
    public static final String MODE_SCROLL = "scroll";
    public String trackpointMode = MODE_MOUSE;

    // Per-button haptic feedback toggle (default: enabled)
    public boolean hapticEnabled = true;

    // Independent held actions for the sliding cross; Linux keyboard arrows by default.
    public HudAction dpadUpAction = HudAction.key(103);
    public HudAction dpadDownAction = HudAction.key(108);
    public HudAction dpadLeftAction = HudAction.key(105);
    public HudAction dpadRightAction = HudAction.key(106);

    public HudButton() {}

    public HudAction directionalAction(int slot) {
        boolean dpad = WIDGET_DPAD.equals(widgetType);
        switch (slot) {
            case 2: return dpad ? dpadLeftAction : swipeLeftAction;
            case 3: return dpad ? dpadRightAction : swipeRightAction;
            case 4: return dpad ? dpadUpAction : swipeUpAction;
            case 5: return dpad ? dpadDownAction : swipeDownAction;
            default: throw new IllegalArgumentException("Invalid direction slot");
        }
    }

    public void setDirectionalAction(int slot, HudAction value) {
        if (WIDGET_DPAD.equals(widgetType)) {
            switch (slot) {
                case 2: dpadLeftAction = value; break;
                case 3: dpadRightAction = value; break;
                case 4: dpadUpAction = value; break;
                case 5: dpadDownAction = value; break;
                default: throw new IllegalArgumentException("Invalid direction slot");
            }
        } else {
            switch (slot) {
                case 2: swipeLeftAction = value; break;
                case 3: swipeRightAction = value; break;
                case 4: swipeUpAction = value; break;
                case 5: swipeDownAction = value; break;
                default: throw new IllegalArgumentException("Invalid direction slot");
            }
        }
    }

    private static Object actionJSON(HudAction value) throws JSONException {
        return value == null ? JSONObject.NULL : value.toJSON();
    }

    private static HudAction copyAction(HudAction value) {
        return value == null ? null : value.copy();
    }

    public JSONObject toJSON() throws JSONException {
        JSONObject obj = new JSONObject();
        obj.put("id", id);
        obj.put("widget_type", widgetType);
        obj.put("label", label);
        obj.put("pos_x_pct", posXPercent);
        obj.put("pos_y_pct", posYPercent);
        obj.put("width_dp", widthDp);
        obj.put("height_dp", heightDp);
        obj.put("corner_radius_dp", cornerRadiusDp);
        obj.put("opacity", opacity);
        obj.put("bg_color", bgColor);
        obj.put("text_color", textColor);
        obj.put("active_color", activeColor);
        obj.put("action", action.toJSON());
        if (popupAction != null) {
            obj.put("popup_action", actionJSON(popupAction));
        }

        if (WIDGET_SUPER_GESTURE.equals(widgetType)) {
            obj.put("swipe_left", actionJSON(swipeLeftAction));
            obj.put("swipe_right", actionJSON(swipeRightAction));
            obj.put("swipe_up", actionJSON(swipeUpAction));
            obj.put("swipe_down", actionJSON(swipeDownAction));
            obj.put("swipe_threshold_dp", swipeThresholdDp);
        } else if (WIDGET_TRACKPOINT.equals(widgetType) || WIDGET_TOUCHPAD.equals(widgetType) || WIDGET_SCROLLBAR.equals(widgetType)) {
            obj.put("trackpoint_sensitivity", trackpointSensitivity);
            obj.put("trackpoint_deadzone_dp", trackpointDeadzoneDp);
            obj.put("trackpoint_mode", trackpointMode != null ? trackpointMode : MODE_MOUSE);
        }
        if (WIDGET_DPAD.equals(widgetType)) {
            obj.put("dpad_up", actionJSON(dpadUpAction));
            obj.put("dpad_down", actionJSON(dpadDownAction));
            obj.put("dpad_left", actionJSON(dpadLeftAction));
            obj.put("dpad_right", actionJSON(dpadRightAction));
        }
        obj.put("haptic_enabled", hapticEnabled);
        if (WIDGET_GROUP.equals(widgetType)) {
            obj.put("pad_dp", padDp);
            JSONArray kids = new JSONArray();
            for (HudButton child : children) kids.put(child.toJSON());
            obj.put("children", kids);
        }
        return obj;
    }

    public static HudButton fromJSON(JSONObject obj) {
        if (obj == null) return null;
        // Old gamepad-only buttons have no desktop action and must not remain as dead keys.
        JSONObject oldAction = obj.optJSONObject("action");
        if (WIDGET_STANDARD.equals(obj.optString("widget_type", WIDGET_STANDARD))
                && oldAction != null && "gamepad".equals(oldAction.optString("type"))) return null;
        HudButton b = new HudButton();
        b.id = obj.optString("id", UUID.randomUUID().toString());
        b.widgetType = obj.optString("widget_type", WIDGET_STANDARD);
        b.label = obj.optString("label", "BTN");
        b.posXPercent = (float) obj.optDouble("pos_x_pct", 0.5);
        b.posYPercent = (float) obj.optDouble("pos_y_pct", 0.5);
        b.widthDp = obj.optInt("width_dp", 64);
        b.heightDp = obj.optInt("height_dp", 40);
        b.cornerRadiusDp = obj.optInt("corner_radius_dp", 8);
        b.opacity = (float) obj.optDouble("opacity", 0.85);
        b.bgColor = obj.optInt("bg_color", 0xE62A2B3D);
        b.textColor = obj.optInt("text_color", 0xFFFFFFFF);
        b.activeColor = obj.optInt("active_color", 0xFF80DEEA);
        b.action = HudAction.fromJSON(obj.optJSONObject("action"));
        if (obj.has("popup_action")) {
            b.popupAction = HudAction.fromJSON(obj.optJSONObject("popup_action"));
        }

        if (obj.has("swipe_left")) b.swipeLeftAction = HudAction.fromJSON(obj.optJSONObject("swipe_left"));
        if (obj.has("swipe_right")) b.swipeRightAction = HudAction.fromJSON(obj.optJSONObject("swipe_right"));
        if (obj.has("swipe_up")) b.swipeUpAction = HudAction.fromJSON(obj.optJSONObject("swipe_up"));
        if (obj.has("swipe_down")) b.swipeDownAction = HudAction.fromJSON(obj.optJSONObject("swipe_down"));
        if (obj.has("dpad_up")) b.dpadUpAction = HudAction.fromJSON(obj.optJSONObject("dpad_up"));
        if (obj.has("dpad_down")) b.dpadDownAction = HudAction.fromJSON(obj.optJSONObject("dpad_down"));
        if (obj.has("dpad_left")) b.dpadLeftAction = HudAction.fromJSON(obj.optJSONObject("dpad_left"));
        if (obj.has("dpad_right")) b.dpadRightAction = HudAction.fromJSON(obj.optJSONObject("dpad_right"));
        b.swipeThresholdDp = obj.optInt("swipe_threshold_dp", 30);

        b.trackpointSensitivity = (float) obj.optDouble("trackpoint_sensitivity", 1.5);
        b.trackpointDeadzoneDp = obj.optInt("trackpoint_deadzone_dp", 4);
        // Default to mouse mode when an old profile does not record a mode.
        b.trackpointMode = obj.optString("trackpoint_mode", MODE_MOUSE);
        if ("stick_left".equals(b.trackpointMode) || "stick_right".equals(b.trackpointMode))
            b.trackpointMode = MODE_MOUSE;
        b.hapticEnabled = obj.optBoolean("haptic_enabled", true);

        if (WIDGET_GROUP.equals(b.widgetType)) {
            b.padDp = obj.optInt("pad_dp", 8);
            JSONArray kids = obj.optJSONArray("children");
            if (kids != null) {
                for (int i = 0; i < kids.length(); i++) {
                    HudButton child = HudButton.fromJSON(kids.optJSONObject(i));
                    if (child != null) b.children.add(child);
                }
            }
        }

        return b;
    }

    /** True for the on-screen KB key: tapping it toggles the virtual keyboard. */
    public boolean isVirtualKeyboardToggle() {
        return action != null && HudAction.TYPE_SYSTEM.equals(action.type)
                && "toggle_vk".equals(action.systemCommand);
    }

    public HudButton duplicate() {
        HudButton d = new HudButton();
        d.id = UUID.randomUUID().toString();
        d.widgetType = this.widgetType;
        d.label = this.label;
        d.posXPercent = Math.min(0.95f, this.posXPercent + 0.04f);
        d.posYPercent = Math.min(0.95f, this.posYPercent + 0.04f);
        d.widthDp = this.widthDp;
        d.heightDp = this.heightDp;
        d.cornerRadiusDp = this.cornerRadiusDp;
        d.opacity = this.opacity;
        d.bgColor = this.bgColor;
        d.textColor = this.textColor;
        d.activeColor = this.activeColor;
        d.action = this.action.copy();
        if (this.popupAction != null) d.popupAction = copyAction(this.popupAction);
        d.swipeLeftAction = copyAction(this.swipeLeftAction);
        d.swipeRightAction = copyAction(this.swipeRightAction);
        d.swipeUpAction = copyAction(this.swipeUpAction);
        d.swipeDownAction = copyAction(this.swipeDownAction);
        d.dpadUpAction = copyAction(this.dpadUpAction);
        d.dpadDownAction = copyAction(this.dpadDownAction);
        d.dpadLeftAction = copyAction(this.dpadLeftAction);
        d.dpadRightAction = copyAction(this.dpadRightAction);
        d.swipeThresholdDp = this.swipeThresholdDp;
        d.trackpointSensitivity = this.trackpointSensitivity;
        d.trackpointDeadzoneDp = this.trackpointDeadzoneDp;
        d.trackpointMode = this.trackpointMode;
        d.hapticEnabled = this.hapticEnabled;
        d.padDp = this.padDp;
        for (HudButton child : this.children) d.children.add(child.duplicate());
        return d;
    }
}
