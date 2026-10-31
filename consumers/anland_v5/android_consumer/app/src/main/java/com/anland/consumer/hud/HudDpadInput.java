package com.anland.consumer.hud;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;

/** Held keyboard actions for one sliding cross, including overlapping diagonal bindings. */
public final class HudDpadInput {
    public interface Sender {
        void sendKey(int action, int code);
    }

    private final Sender sender;
    private final Map<Integer, List<Integer>> directions = new LinkedHashMap<>();
    private final Map<Integer, Integer> references = new LinkedHashMap<>();

    public HudDpadInput(Sender sender) {
        this.sender = sender;
    }

    public static boolean isHeldAction(HudAction action) {
        return HudAction.TYPE_KEY.equals(action.type)
                || HudAction.TYPE_MODIFIER.equals(action.type)
                || HudAction.TYPE_COMBO.equals(action.type);
    }

    public void update(int direction, HudAction action, boolean pressed) {
        if (pressed) {
            if (directions.containsKey(direction)) return;
            LinkedHashSet<Integer> keys = new LinkedHashSet<>();
            if (action != null) {
                if (HudAction.TYPE_COMBO.equals(action.type)) keys.addAll(action.comboKeys);
                else if (isHeldAction(action)) keys.add(action.code);
            }
            keys.removeIf(code -> code <= 0);
            List<Integer> held = new ArrayList<>(keys);
            directions.put(direction, held);
            for (int code : held) {
                int count = references.containsKey(code) ? references.get(code) : 0;
                references.put(code, count + 1);
                if (count == 0) sender.sendKey(0, code);
            }
        } else {
            // Release the action that was pressed, even if the binding has since changed.
            List<Integer> held = directions.remove(direction);
            if (held == null) return;
            for (int i = held.size() - 1; i >= 0; i--) {
                int code = held.get(i);
                int count = references.get(code) - 1;
                if (count == 0) {
                    references.remove(code);
                    sender.sendKey(1, code);
                } else references.put(code, count);
            }
        }
    }
}
