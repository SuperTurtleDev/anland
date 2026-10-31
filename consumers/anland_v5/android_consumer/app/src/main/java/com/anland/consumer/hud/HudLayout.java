package com.anland.consumer.hud;

import org.json.JSONArray;
import org.json.JSONException;
import org.json.JSONObject;

import java.util.ArrayList;
import java.util.List;

/**
 * Encapsulates an orientation layout (Portrait vs Landscape) containing floating buttons and a dock strip.
 */
public final class HudLayout {
    public boolean dockEnabled = true;
    public boolean dockScrollable = false;
    public int dockHeightDp = 40;
    public int dockBgColor = 0xCC11111B;
    public List<HudButton> dockItems = new ArrayList<>();
    public List<HudButton> floatingButtons = new ArrayList<>();

    public HudLayout() {}

    public JSONObject toJSON() throws JSONException {
        JSONObject obj = new JSONObject();
        obj.put("dock_enabled", dockEnabled);
        obj.put("dock_scrollable", dockScrollable);
        obj.put("dock_height_dp", dockHeightDp);
        obj.put("dock_bg_color", dockBgColor);

        JSONArray dockArr = new JSONArray();
        for (HudButton b : dockItems) dockArr.put(b.toJSON());
        obj.put("dock_items", dockArr);

        JSONArray floatArr = new JSONArray();
        for (HudButton b : floatingButtons) floatArr.put(b.toJSON());
        obj.put("floating_buttons", floatArr);

        return obj;
    }

    public static HudLayout fromJSON(JSONObject obj) {
        if (obj == null) return new HudLayout();
        HudLayout l = new HudLayout();
        l.dockEnabled = obj.optBoolean("dock_enabled", true);
        l.dockScrollable = obj.optBoolean("dock_scrollable", false);
        l.dockHeightDp = obj.optInt("dock_height_dp", 40);
        l.dockBgColor = obj.optInt("dock_bg_color", 0xCC11111B);

        JSONArray dockArr = obj.optJSONArray("dock_items");
        if (dockArr != null) {
            for (int i = 0; i < dockArr.length(); i++) {
                HudButton b = HudButton.fromJSON(dockArr.optJSONObject(i));
                if (b != null) l.dockItems.add(b);
            }
        }

        JSONArray floatArr = obj.optJSONArray("floating_buttons");
        if (floatArr != null) {
            for (int i = 0; i < floatArr.length(); i++) {
                HudButton b = HudButton.fromJSON(floatArr.optJSONObject(i));
                if (b != null) l.floatingButtons.add(b);
            }
        }
        return l;
    }
}
