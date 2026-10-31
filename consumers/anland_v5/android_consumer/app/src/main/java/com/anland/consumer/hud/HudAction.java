package com.anland.consumer.hud;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

/**
 * Encapsulates an action triggered by a HUD widget (single key, combo macro, text, or system command).
 */
public final class HudAction {
    public static final String TYPE_KEY = "key";
    public static final String TYPE_MODIFIER = "modifier";
    public static final String TYPE_COMBO = "combo";
    public static final String TYPE_TEXT = "text";
    public static final String TYPE_SYSTEM = "system";

    public String type = TYPE_KEY;
    public int code = 0; // evdev scancode for key / modifier
    public boolean repeat = false;
    public String text = "";
    public String systemCommand = ""; // e.g. "toggle_ime", "toggle_vk", "open_settings", "mouse_left", "mouse_right"
    public List<Integer> comboKeys = new ArrayList<>();

    public HudAction() {}

    public static HudAction key(int evdevCode) {
        HudAction a = new HudAction();
        a.type = TYPE_KEY;
        a.code = evdevCode;
        return a;
    }

    public static HudAction modifier(int evdevCode) {
        HudAction a = new HudAction();
        a.type = TYPE_MODIFIER;
        a.code = evdevCode;
        return a;
    }

    public static HudAction combo(int... evdevCodes) {
        HudAction a = new HudAction();
        a.type = TYPE_COMBO;
        for (int c : evdevCodes) a.comboKeys.add(c);
        return a;
    }

    public static HudAction text(String text) {
        HudAction a = new HudAction();
        a.type = TYPE_TEXT;
        a.text = text;
        return a;
    }

    public static HudAction system(String command) {
        HudAction a = new HudAction();
        a.type = TYPE_SYSTEM;
        a.systemCommand = command;
        return a;
    }

    public JSONObject toJSON() throws JSONException {
        JSONObject obj = new JSONObject();
        obj.put("type", type);
        if (TYPE_KEY.equals(type) || TYPE_MODIFIER.equals(type)) {
            obj.put("code", code);
            obj.put("repeat", repeat);
        } else if (TYPE_COMBO.equals(type)) {
            JSONArray arr = new JSONArray();
            for (int k : comboKeys) arr.put(k);
            obj.put("combo_keys", arr);
        } else if (TYPE_TEXT.equals(type)) {
            obj.put("text", text);
        } else if (TYPE_SYSTEM.equals(type)) {
            obj.put("command", systemCommand);
        }
        return obj;
    }

    public static HudAction fromJSON(JSONObject obj) {
        if (obj == null) return key(0);
        HudAction a = new HudAction();
        a.type = obj.optString("type", TYPE_KEY);
        if ("gamepad".equals(a.type)) return key(0);
        a.code = obj.optInt("code", 0);
        a.repeat = obj.optBoolean("repeat", false);
        a.text = obj.optString("text", "");
        a.systemCommand = obj.optString("command", "");
        JSONArray arr = obj.optJSONArray("combo_keys");
        if (arr != null) {
            for (int i = 0; i < arr.length(); i++) {
                a.comboKeys.add(arr.optInt(i));
            }
        }
        return a;
    }

    public HudAction copy() {
        HudAction c = new HudAction();
        c.type = this.type;
        c.code = this.code;
        c.repeat = this.repeat;
        c.text = this.text;
        c.systemCommand = this.systemCommand;
        c.comboKeys.addAll(this.comboKeys);
        return c;
    }
}
